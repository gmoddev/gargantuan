"""Bounded four-timestamp join for Farm32's existing ScaleFunction echo.

The timestamps are local steady-clock GNS submission/receive boundaries, not
packet-wire timestamps. The result is a causal cross-host offset interval at
each probe, not an exact one-way latency or a phase-long drift guarantee.
"""

from collections import defaultdict
import argparse
import hashlib
import json
from pathlib import Path
import re
import stat
import sys

NATIVE_PREFIX = "[Qualification:FarmClock] event=native "
TERMINAL_PREFIX = "[Qualification:FarmClock] event=terminal "
COMPLETE_PREFIX = "[Qualification:FarmClock] event=calibration_complete "
SERVER_READY_PREFIX = "[Qualification:Server] event=ready "
CLIENT_READY_PREFIX = "[Qualification:Client] event=ready "
PHASE_START_PREFIX = "[Qualification:Scale] event=phase_start "
PHASE_END_PREFIX = "[Qualification:Scale] event=phase_end "
CALIBRATION_START_PREFIX = "[Qualification:FarmClock] event=calibration_start "
QUIESCE_COMPLETE_PREFIX = "[Qualification:FarmClock] event=quiesce_complete "
PHASE_NAMES = ("baseline", "load", "resident", "evict", "reload")
FIELDS = re.compile(r"([A-Za-z_]+)=([^\s]+)")


def Require(Condition, Message):
    if not Condition:
        raise ValueError("[Qualification:FarmClock] " + Message)


def Fields(Line, Prefix):
    return dict(FIELDS.findall(Line[len(Prefix):])) if Line.startswith(Prefix) else None


def Number(Row, Key):
    try:
        Value = int(Row[Key])
    except (KeyError, TypeError, ValueError) as Error:
        raise ValueError("[Qualification:FarmClock] invalid " + Key) from Error
    Require(Value >= 0, "negative " + Key)
    return Value


def OffsetInterval(T1, T2, T3, T4):
    """Return inclusive client-minus-server nanosecond bounds.

    T1/T4 are from one client; T2/T3 are from the server. No path-symmetry
    assumption is needed. The bound includes native queue and polling time.
    """
    Require(T1 < T4 and T2 <= T3, "native timestamp order invalid")
    Lower = T1 - T2
    Upper = T4 - T3
    Require(Lower <= Upper, "causal clock interval is empty")
    return Lower, Upper


def Analyze(ServerLines, ClientLinesBySlot, RunId, Peers=32, Epochs=5, Probes=4):
    Require(Peers > 0 and Epochs > 0 and Probes > 0, "invalid expected dimensions")
    Require(set(ClientLinesBySlot) == set(range(Peers)), "client slot set incomplete")
    ServerConnections = {}
    ServerNonces = {}
    ServerRows = []
    PhaseStarts = {}
    PhaseEnds = {}
    CalibrationStarts = {}
    QuiescedEpochs = {}
    CompleteEpochs = set()
    ServerTerminal = []
    for Line in ServerLines:
        if (Row := Fields(Line, SERVER_READY_PREFIX)) is not None and Row.get("run") == RunId:
            Nonce = Number(Row, "nonce")
            Slot = (Nonce & 0xffffffff) - 1
            Require(0 <= Slot < Peers, "server nonce slot invalid")
            Key = (Number(Row, "connection_slot"), Number(Row, "connection_generation"))
            Require(Key not in ServerConnections and Slot not in ServerConnections.values(),
                    "duplicate server peer identity")
            ServerConnections[Key] = Slot
            ServerNonces[Slot] = Nonce
        elif (Row := Fields(Line, PHASE_START_PREFIX)) is not None and Row.get("run") == RunId:
            Phase = Row.get("phase")
            Require(Phase in PHASE_NAMES[:Epochs] and Phase not in PhaseStarts,
                    "duplicate or unexpected phase start")
            PhaseStarts[Phase] = Number(Row, "monotonic_us") * 1000
        elif (Row := Fields(Line, PHASE_END_PREFIX)) is not None and Row.get("run") == RunId:
            Phase = Row.get("phase")
            Require(Phase in PHASE_NAMES[:Epochs] and Phase not in PhaseEnds,
                    "duplicate or unexpected phase end")
            PhaseEnds[Phase] = Number(Row, "monotonic_us") * 1000
        elif (Row := Fields(Line, CALIBRATION_START_PREFIX)) is not None and Row.get("run") == RunId:
            Epoch = Number(Row, "epoch")
            Require(1 <= Epoch <= Epochs and Epoch not in CalibrationStarts and
                    Row.get("next_phase") == PHASE_NAMES[Epoch - 1],
                    "calibration start identity invalid")
            CalibrationStarts[Epoch] = Number(Row, "monotonic_us") * 1000
        elif (Row := Fields(Line, QUIESCE_COMPLETE_PREFIX)) is not None and Row.get("run") == RunId:
            Epoch = Number(Row, "epoch")
            Require(1 <= Epoch <= Epochs and Epoch not in QuiescedEpochs and
                    Number(Row, "count") == Peers, "quiescence barrier invalid")
            QuiescedEpochs[Epoch] = Number(Row, "monotonic_us") * 1000
        elif (Row := Fields(Line, NATIVE_PREFIX)) is not None and Row.get("run") == RunId:
            Require(Row.get("role") == "server" and Row.get("slot") == "-1",
                    "server native role invalid")
            ServerRows.append(Row)
        elif (Row := Fields(Line, COMPLETE_PREFIX)) is not None and Row.get("run") == RunId:
            Epoch = Number(Row, "epoch")
            Require(Epoch not in CompleteEpochs and Number(Row, "count") == Peers,
                    "calibration completion invalid")
            CompleteEpochs.add(Epoch)
        elif (Row := Fields(Line, TERMINAL_PREFIX)) is not None and Row.get("run") == RunId:
            ServerTerminal.append(Row)
    Require(len(ServerConnections) == Peers, "server ready identities incomplete")
    Require(set(PhaseStarts) == set(PHASE_NAMES[:Epochs]), "phase starts incomplete")
    Require(set(PhaseEnds) == set(PHASE_NAMES[:Epochs]) and
            set(CalibrationStarts) == set(range(1, Epochs + 1)) and
            set(QuiescedEpochs) == set(range(1, Epochs + 1)),
            "phase or calibration boundaries incomplete")
    for Epoch in range(1, Epochs + 1):
        Phase = PHASE_NAMES[Epoch - 1]
        Require(CalibrationStarts[Epoch] < PhaseStarts[Phase] < PhaseEnds[Phase],
                "phase calibration boundary order invalid")
        Require(CalibrationStarts[Epoch] <= QuiescedEpochs[Epoch] < PhaseStarts[Phase],
                "quiescence preceded calibration or entered phase")
        if Epoch > 1:
            Require(PhaseEnds[PHASE_NAMES[Epoch - 2]] <= CalibrationStarts[Epoch],
                    "calibration overlapped prior measured phase")
    Require(CompleteEpochs == set(range(1, Epochs + 1)),
            "all-peer calibration barrier incomplete")
    Require(len(ServerTerminal) == 1 and Number(ServerTerminal[0], "records") == len(ServerRows) and
            Number(ServerTerminal[0], "overflow") == 0 and
            Number(ServerTerminal[0], "invalid_decode") == 0, "server native trace incomplete")

    ClientRows = []
    for Slot, Lines in ClientLinesBySlot.items():
        Ready = []
        Terminal = []
        for Line in Lines:
            if (Row := Fields(Line, CLIENT_READY_PREFIX)) is not None and Row.get("run_id") == RunId:
                Ready.append(Row)
            elif (Row := Fields(Line, NATIVE_PREFIX)) is not None and Row.get("run") == RunId:
                Require(Row.get("role") == "client" and Number(Row, "slot") == Slot,
                        "client native role or slot invalid")
                ClientRows.append((Slot, Row))
            elif (Row := Fields(Line, TERMINAL_PREFIX)) is not None and Row.get("run") == RunId:
                Terminal.append(Row)
        Require(len(Ready) == 1 and Number(Ready[0], "slot") == Slot and
                Number(Ready[0], "nonce") == ServerNonces[Slot],
                "client ready identity invalid")
        Count = sum(1 for ItemSlot, _ in ClientRows if ItemSlot == Slot)
        Require(len(Terminal) == 1 and Number(Terminal[0], "slot") == Slot and
                Number(Terminal[0], "records") == Count and
                Number(Terminal[0], "overflow") == 0 and
                Number(Terminal[0], "invalid_decode") == 0, "client native trace incomplete")

    Samples = defaultdict(dict)

    def Add(Slot, Row):
        Epoch, Sequence = Number(Row, "epoch"), Number(Row, "sequence")
        Require(1 <= Epoch <= Epochs and 1 <= Sequence <= Probes,
                "probe epoch or sequence invalid")
        Request = Number(Row, "request")
        Require(Request > 0, "request identity invalid")
        Kind, Stage = Number(Row, "kind"), Row.get("stage")
        Require(Kind in (3, 4) and Stage in ("GnsBefore", "GnsQueued", "GnsReceive"),
                "native stage or kind invalid")
        Key = (Slot, Epoch, Sequence, Request)
        Label = ("request" if Kind == 3 else "response") + ":" + Stage
        Require(Label not in Samples[Key], "duplicate native probe event")
        Samples[Key][Label] = (Number(Row, "monotonic_ns"), Number(Row, "result")
                               if Stage == "GnsQueued" else None)

    for Row in ServerRows:
        Key = (Number(Row, "connection_slot"), Number(Row, "connection_generation"))
        Require(Key in ServerConnections, "server native connection has no ready identity")
        Add(ServerConnections[Key], Row)
    for Slot, Row in ClientRows:
        Add(Slot, Row)

    ExpectedLabels = {"request:GnsBefore", "request:GnsQueued", "request:GnsReceive",
                      "response:GnsBefore", "response:GnsQueued", "response:GnsReceive"}
    Require(len(Samples) == Peers * Epochs * Probes, "native probe count incomplete")
    Result = []
    RequestIds = defaultdict(set)
    for (Slot, Epoch, Sequence, Request), Events in sorted(Samples.items()):
        Require(Request not in RequestIds[Slot], "client request identity reused")
        RequestIds[Slot].add(Request)
        Require(set(Events) == ExpectedLabels, "native four-timestamp path incomplete")
        T1 = Events["request:GnsBefore"][0]
        T2 = Events["request:GnsReceive"][0]
        T3 = Events["response:GnsBefore"][0]
        T4 = Events["response:GnsReceive"][0]
        Require(Events["request:GnsQueued"][1] == 1 and
                Events["response:GnsQueued"][1] == 1,
                "native send was not accepted")
        Require(T1 <= Events["request:GnsQueued"][0] < T4 and
                T2 <= T3 <= Events["response:GnsQueued"][0],
                "per-host native event order invalid")
        Require(T2 < PhaseStarts[PHASE_NAMES[Epoch - 1]] and
                T3 < PhaseStarts[PHASE_NAMES[Epoch - 1]],
                "clock probe entered measured phase")
        Require(QuiescedEpochs[Epoch] <= T2 <= T3,
                "clock probe preceded all-client quiescence")
        Lower, Upper = OffsetInterval(T1, T2, T3, T4)
        Result.append({"Slot": Slot, "Epoch": Epoch, "Sequence": Sequence,
                       "Request": Request, "LowerNs": Lower, "UpperNs": Upper,
                       "WidthNs": Upper - Lower})
    return {"Format": "GargantuanFarm32NativeClockExchange", "Version": 1,
            "RunId": RunId, "Status": "BOUNDED_AT_PROBE", "Samples": Result,
            "PhaseLongOffset": "NOT_MEASURED", "OneWayLatency": "NOT_MEASURED"}


# Keep the mathematical/native join above independent from filesystem custody.
# The role supervisor permits at most 16 MiB per log and 128 indexed members.
LOG_LIMIT = 16 * 1024 * 1024
INDEX_LIMIT = 1024 * 1024
RETAINED_LINES = 4096 + 128  # Native trace cap plus readiness/phase/barrier metadata.
CLOCK_PREFIX = "[Qualification:FarmClock] "
RELEVANT_PREFIXES = (CLOCK_PREFIX, SERVER_READY_PREFIX, CLIENT_READY_PREFIX,
                     PHASE_START_PREFIX, PHASE_END_PREFIX)


def RegularFile(File, Maximum):
    File = Path(File)
    for Part in (File, *File.parents):
        Info = Part.lstat()
        Require(not stat.S_ISLNK(Info.st_mode) and
                not (getattr(Info, "st_file_attributes", 0) & 0x400),
                "redirected evidence path")
    Info = File.stat()
    Require(stat.S_ISREG(Info.st_mode) and 0 <= Info.st_size <= Maximum,
            "evidence file missing or oversized")
    return File


def ReadJsonFile(File):
    with RegularFile(File, INDEX_LIMIT).open("rb") as Stream:
        Data = Stream.read(INDEX_LIMIT + 1)
    Require(len(Data) <= INDEX_LIMIT, "JSON evidence grew beyond bound")
    return json.loads(Data.decode("utf-8-sig")), hashlib.sha256(Data).hexdigest()


def ReadRole(IndexPath, RunId, Role, Names):
    IndexPath = Path(IndexPath)
    Require(IndexPath.name == "evidence-sha256.json", "unexpected evidence index name")
    Index, IndexHash = ReadJsonFile(IndexPath)
    Require(isinstance(Index, dict) and Index.get("RunId") == RunId and
            Index.get("Role") == Role and isinstance(Index.get("Files"), list) and
            0 < len(Index["Files"]) <= 128, "evidence index identity invalid")
    Entries = {}
    for Entry in Index["Files"]:
        Require(isinstance(Entry, dict) and isinstance(Entry.get("Name"), str) and
                re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}", Entry["Name"]) and
                Entry["Name"].lower() not in Entries, "duplicate or invalid indexed member")
        Entries[Entry["Name"].lower()] = Entry
    LinesByName, Sources = {}, []
    for Name in Names:
        Entry = Entries.get(Name)
        Require(Entry is not None and Entry.get("Name") == Name and
                set(Entry) == {"Name", "Bytes", "Sha256"} and
                type(Entry["Bytes"]) is int and 0 <= Entry["Bytes"] <= LOG_LIMIT and
                isinstance(Entry["Sha256"], str) and
                re.fullmatch(r"[a-fA-F0-9]{64}", Entry["Sha256"]),
                "required clock log missing or invalid")
        File = RegularFile(IndexPath.parent / Name, LOG_LIMIT)
        Require(File.stat().st_size == Entry["Bytes"], "clock log size mismatch")
        Digest, Count, Lines = hashlib.sha256(), 0, []
        with File.open("rb") as Stream:
            for Data in iter(lambda: Stream.readline(LOG_LIMIT + 1), b""):
                Count += len(Data)
                Require(Count <= Entry["Bytes"], "clock log grew during read")
                Digest.update(Data)
                Line = Data.decode("utf-8", errors="strict")
                if Line.startswith(RELEVANT_PREFIXES):
                    Require(len(Data) <= 65536 and len(Lines) < RETAINED_LINES,
                            "clock metadata bound exceeded")
                    if Line.startswith(CLOCK_PREFIX):
                        Require(Fields(Line, CLOCK_PREFIX).get("run") == RunId,
                                "foreign run in clock metadata")
                    Lines.append(Line)
        Require(Count == Entry["Bytes"] and Digest.hexdigest() == Entry["Sha256"].lower(),
                "clock log hash or size mismatch")
        LinesByName[Name] = Lines
        Sources.append({"Role": Role, "Name": Name, "Bytes": Count,
                        "Sha256": Digest.hexdigest()})
    return LinesByName, Sources, IndexHash


def AnalyzeIndexed(ServerIndexPath, ClientIndexPath, ManifestPath):
    Manifest, ManifestHash = ReadJsonFile(ManifestPath)
    Require(isinstance(Manifest, dict) and
            Manifest.get("Format") == "GargantuanPhysicalFarmEndpoint" and
            Manifest.get("Version") == 1 and Manifest.get("ScaleWorkload") is True and
            isinstance(Manifest.get("RunId"), str) and
            re.fullmatch(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}", Manifest["RunId"]),
            "clock manifest identity invalid")
    Nonces = Manifest.get("Nonces")
    Require(isinstance(Nonces, list) and len(Nonces) == 32 and
            all(isinstance(Value, str) and re.fullmatch(r"[1-9][0-9]{0,19}", Value)
                for Value in Nonces), "clock manifest nonces invalid")
    Values = list(map(int, Nonces))
    Require(all(Value <= (1 << 64) - 1 and Value >> 32 == Values[0] >> 32 and
                (Value & 0xffffffff) == Slot + 1 for Slot, Value in enumerate(Values)),
            "clock manifest nonce identities invalid")
    RunId = Manifest["RunId"]
    Server, ServerSources, ServerHash = ReadRole(ServerIndexPath, RunId, "Server", ["server.stdout.log"])
    Names = [f"client-{Slot:02d}.stdout.log" for Slot in range(32)]
    Clients, ClientSources, ClientHash = ReadRole(ClientIndexPath, RunId, "Clients", Names)
    ServerLines = Server["server.stdout.log"]
    ClientLines = {Slot: Clients[Name] for Slot, Name in enumerate(Names)}
    Present = any(Line.startswith(CLOCK_PREFIX) for Lines in [ServerLines, *ClientLines.values()]
                  for Line in Lines)
    if Present:
        for Slot, Lines in ClientLines.items():
            Ready = [Fields(Line, CLIENT_READY_PREFIX) for Line in Lines
                     if Line.startswith(CLIENT_READY_PREFIX)]
            Require(len(Ready) == 1 and Ready[0].get("run_id") == RunId and
                    Ready[0].get("nonce") == Nonces[Slot], "clock manifest/ready nonce mismatch")
        Result = Analyze(ServerLines, ClientLines, RunId)
        Require({(Row["Slot"], Row["Epoch"], Row["Sequence"]) for Row in Result["Samples"]} ==
                {(Slot, Epoch, Sequence) for Slot in range(32) for Epoch in range(1, 6)
                 for Sequence in range(1, 5)}, "clock probe matrix coverage incomplete")
    else:
        Result = {"Format": "GargantuanFarm32NativeClockExchange", "Version": 1,
                  "RunId": RunId, "Status": "NOT_MEASURED", "Samples": [],
                  "Reason": "all indexed role logs lack native clock metadata",
                  "PhaseLongOffset": "NOT_MEASURED", "OneWayLatency": "NOT_MEASURED"}
    Result.update({"Clients": 32, "Epochs": 5, "ProbesPerClientEpoch": 4,
                   "ServerIndexSha256": ServerHash, "ClientIndexSha256": ClientHash,
                   "RunManifestSha256": ManifestHash, "Sources": ServerSources + ClientSources,
                   "AnalyzerSha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()})
    return Result


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("ServerIndex")
    Parser.add_argument("ClientIndex")
    Parser.add_argument("RunManifest")
    Args = Parser.parse_args()
    print(json.dumps(AnalyzeIndexed(Args.ServerIndex, Args.ClientIndex, Args.RunManifest),
                     separators=(",", ":")))


if __name__ == "__main__":
    try:
        Main()
    except (ValueError, OSError) as Error:
        print(str(Error), file=sys.stderr)
        raise SystemExit(1)
