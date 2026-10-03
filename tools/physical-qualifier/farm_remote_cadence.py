"""Join Farm32's designated producer's Luau Remote offer/completion trace.

All durations use one client's os.clock() domain. A GameSession receive/handled
counter is not a Luau handler observation and no cross-host latency is inferred.
"""

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path


PHASES = ("baseline", "load", "resident", "evict", "reload")
PREFIX = "[Qualification:RemoteCadence] "
READY_PREFIX = "[Qualification:Client] event=ready "
METRICS_PREFIX = "[Qualification:Producer] event=phase_metrics "
FIELDS = re.compile(r"([a-z_]+)=([^\s]+)")
RUN_ID = re.compile(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}")
RPC_ROW = re.compile(r"([0-9]+):([0-9]+):([0-9]+):([01])")
EVENT_ROW = re.compile(r"([0-9]+):([0-9]+):([0-9]+)")
LOG_LIMIT = 4_194_304
RPC_LIMIT_NS = (150_000_000, 250_000_000, 500_000_000)
EVENT_LIMIT_NS = 250_000_000


def Require(Condition, Message):
    if not Condition:
        raise ValueError("[Qualification:RemoteCadence] " + Message)


def Number(Row, Name):
    Require(Name in Row and re.fullmatch(r"[0-9]+", Row[Name]) is not None,
            "invalid " + Name)
    return int(Row[Name])


def ReadIndexed(IndexPath, Slot):
    IndexPath = Path(IndexPath)
    Require(IndexPath.name == "evidence-sha256.json" and IndexPath.is_file() and
            not IndexPath.is_symlink() and 0 < IndexPath.stat().st_size <= 65_536,
            "client evidence index missing or oversized")
    Index = json.loads(IndexPath.read_text(encoding="utf-8"))
    Require(isinstance(Index, dict) and Index.get("Role") == "Clients" and
            isinstance(Index.get("RunId"), str) and RUN_ID.fullmatch(Index["RunId"]) and
            isinstance(Index.get("Files"), list) and len(Index["Files"]) <= 128,
            "client evidence index identity invalid")
    Name = f"client-{Slot:02d}.stdout.log"
    Entries = [Entry for Entry in Index["Files"]
               if isinstance(Entry, dict) and Entry.get("Name") == Name]
    Require(len(Entries) == 1, "producer log missing or duplicated in index")
    Entry = Entries[0]
    Require(set(Entry) == {"Name", "Bytes", "Sha256"} and
            type(Entry["Bytes"]) is int and 0 < Entry["Bytes"] <= LOG_LIMIT and
            isinstance(Entry["Sha256"], str) and
            re.fullmatch(r"[0-9a-f]{64}", Entry["Sha256"]),
            "producer log index member invalid")
    Log = IndexPath.parent / Name
    Require(Log.is_file() and not Log.is_symlink() and Log.stat().st_size == Entry["Bytes"] and
            hashlib.sha256(Log.read_bytes()).hexdigest() == Entry["Sha256"],
            "producer log hash or size mismatch")
    return Index["RunId"], Log.read_text(encoding="utf-8", errors="strict").splitlines(), Entry["Sha256"]


def Percentile(Values, Numerator):
    Ordered = sorted(Values)
    return Ordered[((len(Ordered) - 1) * Numerator) // 100]


def ParseLines(Lines, RunId, Slot=0):
    Require(RUN_ID.fullmatch(RunId) is not None and type(Slot) is int and 0 <= Slot < 32,
            "invalid run or producer slot")
    Ready = []
    Metrics = {}
    Summaries = {}
    Chunks = {}
    SeenPhases = []
    for LineNumber, Line in enumerate(Lines):
        if Line.startswith(READY_PREFIX):
            Row = dict(FIELDS.findall(Line[len(READY_PREFIX):]))
            if Row.get("run_id") == RunId and Row.get("slot") == str(Slot):
                Ready.append((LineNumber, Row))
        elif Line.startswith(METRICS_PREFIX):
            Row = dict(FIELDS.findall(Line[len(METRICS_PREFIX):]))
            if Row.get("run_id") == RunId and Row.get("slot") == str(Slot):
                Phase = Row.get("phase")
                Require(Phase in PHASES and Phase not in Metrics,
                        "duplicate or invalid producer phase metrics")
                Metrics[Phase] = (LineNumber, Row)
                SeenPhases.append(Phase)
        elif Line.startswith(PREFIX):
            Row = dict(FIELDS.findall(Line[len(PREFIX):]))
            Require(Row.get("version") == "1" and Row.get("phase") in PHASES and
                    Row.get("kind") in ("rpc", "event"), "invalid trace identity")
            Key = (Row["phase"], Row["kind"])
            if Row.get("event") == "summary":
                Require(set(Row) == {"event", "version", "kind", "phase", "records", "chunks"} and
                        Key not in Summaries, "duplicate or malformed trace summary")
                Summaries[Key] = (LineNumber, Number(Row, "records"), Number(Row, "chunks"))
            elif Row.get("event") == "chunk":
                Require(set(Row) == {"event", "version", "kind", "phase", "index", "records"},
                        "malformed trace chunk")
                Chunks.setdefault(Key, []).append((LineNumber, Number(Row, "index"), Row["records"]))
            else:
                Require(False, "unknown trace event")
    Require(len(Ready) == 1 and Number(Ready[0][1], "nonce") > 0,
            "designated producer ready identity absent or duplicated")
    Require(SeenPhases == list(PHASES) and set(Metrics) == set(PHASES),
            "producer phases missing or out of order")
    Require(set(Summaries) == {(Phase, Kind) for Phase in PHASES for Kind in ("rpc", "event")} and
            set(Chunks) == set(Summaries), "Remote trace kinds or phases missing")

    PhaseResults = {}
    PreviousEventSequence = None
    for Phase in PHASES:
        Samples = {}
        RowsByKind = {}
        for Kind in ("rpc", "event"):
            Key = (Phase, Kind)
            SummaryLine, Count, ChunkCount = Summaries[Key]
            Require((Count == 100 if Kind == "rpc" else 0 < Count <= 1200) and
                    ChunkCount == (Count + 31) // 32 and
                    SummaryLine < Metrics[Phase][0], "trace count or phase order invalid")
            Pieces = Chunks[Key]
            Require(len(Pieces) == ChunkCount and
                    [Piece[1] for Piece in Pieces] == list(range(1, ChunkCount + 1)) and
                    all(SummaryLine < Piece[0] < Metrics[Phase][0] for Piece in Pieces),
                    "trace chunks missing, reordered, or outside phase")
            Rows = []
            for Position, (_, _, Encoded) in enumerate(Pieces):
                TextRows = Encoded.split(",")
                Expected = min(32, Count - Position * 32)
                Require(len(TextRows) == Expected, "trace chunk record count invalid")
                for Text in TextRows:
                    Match = (RPC_ROW if Kind == "rpc" else EVENT_ROW).fullmatch(Text)
                    Require(Match is not None, "malformed Remote trace record")
                    Values = tuple(map(int, Match.groups()))
                    Require(0 <= Values[1] <= Values[2] <= 500_000_000_000,
                            "Remote trace local timestamp invalid")
                    Rows.append(Values)
            RowsByKind[Kind] = Rows
            Samples[Kind] = [Row[2] - Row[1] for Row in Rows]
        Rpc = RowsByKind["rpc"]
        Events = RowsByKind["event"]
        Require([Row[0] for Row in Rpc] == list(range(1, 101)),
                "RPC invocation identity missing or repeated")
        Require(all(Rpc[Index][1] >= Rpc[Index - 1][2] for Index in range(1, 100)),
                "sequential RPC workload overlapped")
        Require(all(Events[Index][0] == Events[Index - 1][0] + 1 and
                    Events[Index][1] >= Events[Index - 1][2]
                    for Index in range(1, len(Events))),
                "Event offer identity or one-outstanding policy invalid")
        if PreviousEventSequence is not None:
            Require(Events[0][0] == PreviousEventSequence + 1,
                    "Event identity gap across phases")
        PreviousEventSequence = Events[-1][0]
        Row = Metrics[Phase][1]
        Require(Number(Row, "remote_samples") == len(Rpc) and
                Number(Row, "event_offers") == len(Events) and
                Number(Row, "event_acks") == len(Events) and
                Number(Row, "event_outstanding") == 0,
                "Luau trace and typed producer counts disagree")
        RpcP95 = Percentile(Samples["rpc"], 95)
        RpcP99 = Percentile(Samples["rpc"], 99)
        RpcMaximum = max(Samples["rpc"])
        EventRttMaximum = max(Samples["event"])
        EventAckGapMaximum = max((Events[Index][2] - Events[Index - 1][2]
                                  for Index in range(1, len(Events))), default=0)
        Healthy = (all(Item[3] == 1 for Item in Rpc) and
                   RpcP95 <= RPC_LIMIT_NS[0] and RpcP99 <= RPC_LIMIT_NS[1] and
                   RpcMaximum <= RPC_LIMIT_NS[2] and EventRttMaximum <= EVENT_LIMIT_NS and
                   EventAckGapMaximum <= EVENT_LIMIT_NS)
        PhaseResults[Phase] = {"Status": "PASS" if Healthy else "FAIL",
                               "RpcSamples": len(Rpc), "RpcP95Ns": RpcP95,
                               "RpcP99Ns": RpcP99, "RpcMaxNs": RpcMaximum,
                               "EventOffers": len(Events), "EventAcks": len(Events),
                               "EventMaxRttNs": EventRttMaximum,
                               "EventMaxAckGapNs": EventAckGapMaximum}
    return {"Format": "GargantuanFarmRemoteLuauCadence", "Version": 1,
            "RunId": RunId, "ProducerSlot": Slot, "ClockDomain": "producer_luau_local",
            "OtherClientsScaleRemoteRecipientService": "NOT_MEASURED",
            "CrossHostOneWayLatency": "NOT_MEASURED", "Phases": PhaseResults,
            "Status": "MEASURED_PASS" if all(Row["Status"] == "PASS" for Row in PhaseResults.values())
            else "MEASURED_FAIL"}


def Analyze(IndexPath, Slot=0):
    RunId, Lines, Digest = ReadIndexed(IndexPath, Slot)
    Result = ParseLines(Lines, RunId, Slot)
    Result["SourceSha256"] = Digest
    return Result


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--clients-index", required=True)
    Parser.add_argument("--producer-slot", type=int, default=0)
    Arguments = Parser.parse_args()
    try:
        Result = Analyze(Arguments.clients_index, Arguments.producer_slot)
        print(json.dumps(Result, sort_keys=True))
        return 0 if Result["Status"] == "MEASURED_PASS" else 1
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as Error:
        print(str(Error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(Main())
