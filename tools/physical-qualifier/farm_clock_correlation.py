"""Read-only Farm32 clock consistency check for already reconciled evidence.

Native steady clocks and supervisor Stopwatch clocks are independent domains.
This checks order and elapsed durations within each domain. A nonce join proves
causal connection identity across hosts, never a one-way time or clock offset.
Run PhysicalGameSessionFarmReconcile.ps1 first for the full evidence contract.
"""

import argparse
import csv
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import sys


FIELD = re.compile(r"(?:^|\s)([a-z][a-z0-9_]*)=([^\s]+)")
PREFIX = "[Qualification:"
MAX_FILE = 16 * 1024 * 1024
PHASES = ("baseline", "load", "resident", "evict", "reload")


def Require(Condition, Message):
    if not Condition:
        raise ValueError("[Qualification:FarmClock] " + Message)


def Number(Value, Name):
    Require(isinstance(Value, str) and re.fullmatch(r"(0|[1-9][0-9]*)", Value) is not None,
            "invalid " + Name)
    return int(Value)


def Utc(Value):
    try:
        Parsed = datetime.fromisoformat(Value.replace("Z", "+00:00"))
    except (AttributeError, ValueError) as Error:
        raise ValueError("[Qualification:FarmClock] invalid UTC timestamp") from Error
    Require(Parsed.tzinfo is not None, "timestamp lacks UTC offset")
    return Parsed.timestamp()


def ReadIndexed(Root, RunId, Name):
    Root = Path(Root).resolve(strict=True)
    Index = Root / "evidence-sha256.json"
    Require(Index.is_file() and Index.stat().st_size <= 65536, "missing bounded evidence index")
    Row = json.loads(Index.read_text(encoding="utf-8"))
    Require(Row.get("RunId") == RunId and isinstance(Row.get("Files"), list),
            "evidence index run mismatch")
    Entries = [Item for Item in Row["Files"] if Item.get("Name") == Name]
    Require(len(Entries) == 1, "missing or duplicate indexed " + Name)
    Entry = Entries[0]
    File = Root / Name
    Require(File.is_file() and not File.is_symlink() and 0 < File.stat().st_size <= MAX_FILE and
            File.stat().st_size == Entry.get("Bytes"), "indexed file size mismatch: " + Name)
    Require(hashlib.sha256(File.read_bytes()).hexdigest() == str(Entry.get("Sha256", "")).lower(),
            "indexed file hash mismatch: " + Name)
    return File


def Records(File, Kind):
    Expected = PREFIX + Kind + "] "
    Rows = []
    for Line in File.read_text(encoding="utf-8").splitlines():
        if Line.startswith(Expected):
            Fields = dict(FIELD.findall(Line))
            Require("event" in Fields, "untyped " + Kind + " record")
            Rows.append(Fields)
    return Rows


def Unique(Rows, Event, Identity):
    Found = [Row for Row in Rows if Row.get("event") == Event]
    Require(len(Found) == 1, Identity + " lacks one " + Event + " record")
    return Found[0]


def Samples(File, RunId, Labels):
    with File.open(newline="", encoding="utf-8-sig") as Stream:
        Rows = list(csv.DictReader(Stream))
    Require(0 < len(Rows) <= 20000, "resource sample count invalid")
    ByLabel = {Label: [] for Label in Labels}
    HostFrequencies = set()
    for Row in Rows:
        Label = Row.get("Label")
        Require(Row.get("RunId") == RunId and Label in ByLabel,
                "resource sample run or label mismatch")
        ByLabel[Label].append(Row)
    for Label, Group in ByLabel.items():
        Require(Group, "missing process samples for " + Label)
        Pids = {Number(Row.get("Pid"), "PID") for Row in Group}
        Frequencies = {Number(Row.get("MonotonicFrequency"), "frequency") for Row in Group}
        HostFrequencies.update(Frequencies)
        Require(len(Pids) == 1 and next(iter(Pids)) > 0 and len(Frequencies) == 1 and
                next(iter(Frequencies)) > 0, "process or clock identity changed for " + Label)
        Previous = None
        for Row in Group:
            Tick = Number(Row.get("MonotonicTicks"), "sample tick")
            Elapsed = Number(Row.get("SupervisorElapsedMilliseconds"), "elapsed")
            Wall = Utc(Row.get("Utc"))
            if Previous:
                PriorTick, PriorElapsed, PriorWall = Previous
                Require(Tick > PriorTick and Elapsed >= PriorElapsed,
                        "nonmonotonic resource series for " + Label)
                ClockSeconds = (Tick - PriorTick) / next(iter(Frequencies))
                ElapsedSeconds = (Elapsed - PriorElapsed) / 1000
                Require(abs(ClockSeconds - ElapsedSeconds) <= 2.0,
                        "Stopwatch/elapsed drift for " + Label)
                Require(abs(ClockSeconds - (Wall - PriorWall)) <= 5.0,
                        "UTC/monotonic drift for " + Label)
            Previous = Tick, Elapsed, Wall
    Require(len(HostFrequencies) == 1, "same-host Stopwatch frequency changed")
    return ByLabel


def Correlate(ServerRoot, ClientRoot, RunId):
    Require(re.fullmatch(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}",
                         RunId) is not None, "invalid run UUID")
    ClientLabels = [f"client-{Slot:02d}" for Slot in range(32)]
    ServerResult = json.loads(ReadIndexed(ServerRoot, RunId, "result.json").read_text())
    ClientResult = json.loads(ReadIndexed(ClientRoot, RunId, "result.json").read_text())
    for Role, Result in (("Server", ServerResult), ("Clients", ClientResult)):
        Require(Result.get("RunId") == RunId and Result.get("Role") == Role and
                Result.get("Status") == "PASS", Role + " result mismatch")
        Require(Utc(Result.get("StartedUtc")) <= Utc(Result.get("CompletedUtc")),
                Role + " lifecycle UTC order invalid")
    ClientPids = ClientResult.get("Pids")
    Require(isinstance(ClientPids, list) and len(ClientPids) == 32 and
            len(set(ClientPids)) == 32, "32 independent client PIDs absent")
    ClientSamples = Samples(ReadIndexed(ClientRoot, RunId, "process-resources.csv"),
                            RunId, ClientLabels)
    ServerSamples = Samples(ReadIndexed(ServerRoot, RunId, "process-resources.csv"),
                            RunId, ["server"])
    Require(all(int(ClientSamples[Label][0]["Pid"]) == ClientPids[Slot]
                for Slot, Label in enumerate(ClientLabels)),
            "client sample PID/slot assignment differs from owned result")
    Require(ServerResult.get("Pids") == [int(ServerSamples["server"][0]["Pid"])],
            "server sample PID differs from owned result")

    Nonces = set()
    NativeDurations = []
    for Slot, Label in enumerate(ClientLabels):
        Rows = Records(ReadIndexed(ClientRoot, RunId, Label + ".stdout.log"), "Client")
        Timeline = [Unique(Rows, Event, Label) for Event in ("start", "ready", "result")]
        for Row in Timeline:
            Require(Row.get("run_id") == RunId and Row.get("slot") == str(Slot) and
                    Row.get("nonce") == Timeline[0].get("nonce"),
                    "client native run/slot/nonce mismatch for " + Label)
        Require(Timeline[0].get("nonce") not in Nonces, "duplicate client nonce")
        Nonces.add(Timeline[0]["nonce"])
        Times = [Number(Row.get("steady_ns"), "native steady_ns") for Row in Timeline]
        Require(0 < Times[0] < Times[1] < Times[2], "client native clock order invalid for " + Label)
        Duration = (Times[2] - Times[0]) / 1_000_000_000
        RoleSeconds = Utc(ClientResult["CompletedUtc"]) - Utc(ClientResult["StartedUtc"])
        Require(Duration <= RoleSeconds + 5, "client native duration exceeds role lifecycle")
        NativeDurations.append(Duration)

    ServerLog = ReadIndexed(ServerRoot, RunId, "server.stdout.log")
    Ready = [Row for Row in Records(ServerLog, "Server") if Row.get("event") == "ready"]
    Require(len(Ready) == 32 and {Row.get("nonce") for Row in Ready} == Nonces,
            "server/client nonce causal join incomplete")
    ReadyTimes = [Number(Row.get("monotonic_us"), "server ready time") for Row in Ready]
    Require(all(ReadyTimes[Index] <= ReadyTimes[Index + 1] for Index in range(31)),
            "server ready monotonic order invalid")
    Scale = Records(ServerLog, "Scale")
    PreviousEnd = 0
    FirstStart = None
    for Phase in PHASES:
        Starts = [Row for Row in Scale if Row.get("event") == "phase_start" and
                  Row.get("phase") == Phase]
        Ends = [Row for Row in Scale if Row.get("event") == "phase_end" and
                Row.get("phase") == Phase]
        Require(len(Starts) == len(Ends) == 1 and Starts[0].get("run") == RunId and
                Ends[0].get("run") == RunId, "missing server phase clock anchors")
        Start = Number(Starts[0].get("monotonic_us"), "phase start")
        End = Number(Ends[0].get("monotonic_us"), "phase end")
        if FirstStart is None:
            FirstStart = Start
        Require(PreviousEnd < Start < End and End - Start > 13_000_000,
                "server phase monotonic order or duration invalid")
        PreviousEnd = End
    RoleSeconds = Utc(ServerResult["CompletedUtc"]) - Utc(ServerResult["StartedUtc"])
    Require((PreviousEnd - FirstStart) / 1_000_000 <= RoleSeconds + 5,
            "server native duration exceeds role lifecycle")

    Marker = json.loads(ReadIndexed(ServerRoot, RunId, "server-ready.json").read_text())
    Require(Marker.get("RunId") == RunId and Marker.get("Role") == "Server" and
            Marker.get("Pid") == ServerResult["Pids"][0], "server lifecycle marker identity mismatch")
    MarkerUtc = Utc(Marker.get("ReadyUtc"))
    Require(Utc(ServerResult["StartedUtc"]) - 1 <= MarkerUtc <=
            Utc(ServerResult["CompletedUtc"]) + 1, "server lifecycle marker outside role run")
    return {
        "Format": "GargantuanFarm32ClockCorrelation", "Version": 1, "RunId": RunId,
        "Status": "PARTIAL_CLOCK_CORRELATION", "NativeClientProcesses": 32,
        "NativeClientDurationSecondsMax": round(max(NativeDurations), 3),
        "ServerReadyCausalNonceJoins": 32, "ServerOrderedPhases": 5,
        "SameHostSampleClocks": "DURATION_AND_ORDER_CONSISTENT",
        "LifecycleUtc": "ROLE_LOCAL_ORDER_CONSISTENT",
        "NativeToSupervisorEpoch": "NOT_MEASURED",
        "CoordinatorLifecycleChronology": "NOT_MEASURED",
        "CrossHostClockOffset": "NOT_MEASURED",
        "CrossHostChronology": "NONCE_CAUSAL_ORDER_ONLY",
    }


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("ServerEvidenceRoot")
    Parser.add_argument("ClientEvidenceRoot")
    Parser.add_argument("RunId")
    Args = Parser.parse_args()
    print(json.dumps(Correlate(Args.ServerEvidenceRoot, Args.ClientEvidenceRoot, Args.RunId),
                     indent=2, sort_keys=True))


if __name__ == "__main__":
    try:
        Main()
    except (ValueError, OSError, KeyError, TypeError, json.JSONDecodeError) as Error:
        print(str(Error), file=sys.stderr)
        raise SystemExit(1)
