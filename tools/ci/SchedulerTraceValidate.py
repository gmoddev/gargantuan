"""Offline loss and clock-window validation. Never attributes a stall or changes latency."""
import argparse
import csv
import json
from pathlib import Path

CSV_FIELDS = 'Qpc,Processor,Opcode,Version,HeaderPid,HeaderTid,NewTid,OldTid,TargetTid,TargetPid,OldWaitReason,OldWaitMode,OldState,ReadyAdjustReason,ReadyAdjustIncrement,ReadyFlags'.split(',')


def Validate(Metadata, Stdout, Case="Full", CsvPath=None):
    Errors = []

    def Require(Condition, Message):
        if not Condition:
            Errors.append(Message)

    Require(Metadata.get("Format") == "GargantuanSchedulerTrace" and Metadata.get("Version") == 1, "unknown trace metadata")
    Require(Metadata.get("WorkloadCase") == Case or (Case == "Full" and "WorkloadCase" not in Metadata),
            "wrong workload metadata")
    for Name in ("StartStatus", "QueryStatus", "StopStatus", "EventsLost", "LogBuffersLost", "RealTimeBuffersLost",
                 "HeaderEventsLost", "HeaderBuffersLost", "DecodeOpenStatus", "DecodeProcessStatus", "UnsupportedEvents"):
        Require(type(Metadata.get(Name)) is int and Metadata[Name] == 0, Name + " is nonzero or unmeasured")
    for Name, Expected in (("DiagnosticComplete", True), ("ChildResumed", True), ("ChildTreeReaped", True),
                           ("TimedOut", False), ("ChildLogCapped", False), ("ChildLogFailed", False), ("CsvCapped", False), ("DecodeTimedOut", False)):
        Require(type(Metadata.get(Name)) is bool and Metadata[Name] is Expected, Name + " invalid")
    Require(type(Metadata.get("ChildLaunchAttempts")) is int and Metadata["ChildLaunchAttempts"] == 1, "expected one workload launch")
    Require(type(Metadata.get("ClockType")) is int and Metadata["ClockType"] == 1, "trace is not QPC")
    Frequency = Metadata.get("QpcFrequency")
    Require(type(Frequency) is int and Frequency > 0 and Frequency == Metadata.get("ControllerQpcFrequency"), "QPC frequency mismatch")
    Names = ("ControllerStartQpc", "AfterTraceStartQpc", "BeforeChildResumeQpc", "AfterChildExitQpc", "ControllerEndQpc")
    Ticks = [Metadata.get(Name) for Name in Names]
    Require(all(type(Value) is int and Value > 0 for Value in Ticks) and Ticks == sorted(Ticks, key=lambda V: V if type(V) is int else -1), "controller QPC order invalid")
    Require(type(Metadata.get("HeaderStartFileTime")) is int and type(Metadata.get("HeaderEndFileTime")) is int and
            0 < Metadata["HeaderStartFileTime"] <= Metadata["HeaderEndFileTime"], "ETL FILETIME boundaries absent")
    Anchors = []
    for Line in Stdout.splitlines():
        if not Line.startswith("[Qualification:ClockAnchor] "):
            continue
        try:
            Parts = [Part.split("=", 1) for Part in Line.split()[1:]]
            Fields = dict(Parts)
            Require(len(Fields) == len(Parts), "duplicate fixture anchor field")
            for Key in ("pid", "native_tid", "native_valid", "steady_ns", "qpc_before", "qpc_after", "qpc_frequency"):
                Fields[Key] = int(Fields[Key])
            Require(Fields["native_valid"] == 1 and Fields["pid"] == Metadata.get("ChildPid") and
                    Fields["native_tid"] == Metadata.get("ChildMainTid") and Fields["qpc_frequency"] == Frequency,
                    "fixture native identity/clock mismatch")
            Require(Fields.get("profile") == ("ACK_STATS" if Case == "AckStats" else "FULL_RESERVATION"), "unexpected fixture profile")
            Require(Metadata["BeforeChildResumeQpc"] <= Fields["qpc_before"] <= Fields["qpc_after"] <= Metadata["AfterChildExitQpc"], "fixture anchor outside child interval")
            Require(Metadata["MainFirstQpc"] <= Fields["qpc_before"] <= Fields["qpc_after"] <= Metadata["MainLastQpc"], "target thread events do not bracket fixture anchor")
            Anchors.append(Fields)
        except (ValueError, KeyError, TypeError):
            Errors.append("malformed fixture clock anchor")
    Require(bool(Anchors), "no fixture anchors")
    ByCase = {}
    for Anchor in Anchors:
        ByCase.setdefault(Anchor.get("case"), []).append(Anchor)
    for CaseName, Pair in ByCase.items():
        Require(len(Pair) == 2 and [Value.get("boundary") for Value in Pair] == ["BEGIN", "END"], "missing or duplicate anchor pair: " + str(CaseName))
        if len(Pair) == 2:
            Require(Pair[0]["qpc_after"] <= Pair[1]["qpc_before"] and Pair[0]["steady_ns"] <= Pair[1]["steady_ns"], "case clock order invalid")
    Extra = {}
    if Case == "AckStats":
        try:
            Require(set(ByCase) in ({"ackstats-control"}, {"ackstats-control", "ackstats-prompt"}), "unexpected stats arms")
            Require(type(Metadata.get("ChildExitCode")) is int and
                    (Metadata["ChildExitCode"] != 0 or set(ByCase) == {"ackstats-control", "ackstats-prompt"}),
                    "successful stats workload must retain both arms")
            Require(type(Metadata.get("UnsupportedLifecycleEvents")) is int and Metadata["UnsupportedLifecycleEvents"] == 0,
                    "service-thread lifecycle schema incomplete")
            Extra = ValidateStats(Metadata, Stdout, CsvPath, ByCase)
        except (OSError, ValueError, KeyError, TypeError, UnicodeError) as Error:
            Errors.append("stats coverage: " + str(Error))
    return {"Format": "GargantuanSchedulerTraceCoverage", "Version": 1,
            "State": "LOSS_FREE_ANCHOR_WINDOW_RETAINED" if not Errors else "INCOMPLETE",
            "CausalVerdict": "NOT_CLAIMED", "Errors": Errors, "FixtureAnchorCount": len(Anchors),
            "Note": "Window retention does not prove a scheduler cause; inspect the exact CSwitch/ReadyThread chain. Unknown lifecycle versions limit other-thread identity attribution.", **Extra}


def ValidateStats(Metadata, Stdout, CsvPath, Arms):
    def Need(Value, Message):
        if not Value:
            raise ValueError(Message)
    def Fields(Line):
        Parts = [Part.split("=", 1) for Part in Line.split()[1:]]
        Need(all(len(Part) == 2 for Part in Parts), "malformed stats marker")
        Value = dict(Parts)
        Need(len(Value) == len(Parts), "duplicate stats marker field")
        return Value
    Services, Snapshots, Summaries = [], [], []
    for Line in Stdout.splitlines():
        Need(len(Line) <= 8192, "stats log line cap")
        if Line.startswith("[Qualification:AckStatsServiceThread] "):
            Services.append(Fields(Line))
        elif Line.startswith("[Qualification:AckStatsNativeSnapshot] "):
            Snapshots.append(Fields(Line))
        elif Line.startswith("[Qualification:AckStatsDiagnostic] "):
            Summaries.append(Fields(Line))
        Need(len(Services) <= 16 and len(Snapshots) <= 16 and len(Summaries) <= 1, "stats evidence cap")
    Need(Services and len(Summaries) == 1 and int(Summaries[0]["valid"]) == 1 and
         int(Summaries[0]["thread_count"]) == len(Services), "service callback missing/overflow")
    Need([int(Row['sequence']) for Row in Services] == list(range(len(Services))), "service callback sequence")
    Need(set(Row['case'] for Row in Services) == set(Arms), "service callback/arm mismatch")
    if 'ackstats-prompt' in Arms:
        Need(Arms['ackstats-control'][-1]['qpc_after'] <= Arms['ackstats-prompt'][0]['qpc_before'], "stats arm order")
    for Row in Services + Snapshots:
        Need(Row['case'] in Arms and len(Arms[Row['case']]) == 2, "unbracketed stats arm")
        Begin, End = Arms[Row['case']]
        Need(int(Row['native_valid']) == 1 and int(Row['pid']) == Metadata['ChildPid'] and
             int(Row['qpc_frequency']) == Metadata['QpcFrequency'] and
             Begin['qpc_before'] <= int(Row['qpc_before']) <= int(Row['qpc_after']) <= End['qpc_after'],
             "stats identity/clock outside arm")
    for Row in Snapshots:
        Need(int(Row['native_tid']) == Metadata['ChildMainTid'] and int(Row['available']) == 1 and
             int(Row['native_us']) > 0 and int(Row['token']) >= 0 and Row['side'] in ('sender', 'receiver') and
             Row['stage'] in ('after-quiet', 'terminal', 'grant-failure'), "native snapshot identity/value")
    Need(Snapshots, "native snapshot brackets absent")
    Need(CsvPath is not None and CsvPath.is_file() and CsvPath.stat().st_size <= 512 * 1024 * 1024, "bounded scheduler CSV absent")
    Identities = {int(Row['native_tid']): [] for Row in Services}
    Count, Last = 0, 0
    with CsvPath.open('r', encoding='ascii', newline='') as Stream:
        csv.field_size_limit(1024)
        Reader = csv.DictReader(Stream)
        Need(Reader.fieldnames == CSV_FIELDS, "unexpected scheduler CSV schema")
        for Row in Reader:
            Count += 1
            Need(Count <= 5_000_000 and None not in Row and all(Value is not None for Value in Row.values()), "CSV cap/schema")
            Tick = int(Row['Qpc'])
            Need(Tick > 0 and Tick >= Last, "unordered scheduler CSV")
            Last = Tick
            if int(Row['Opcode']) not in (1, 2, 3, 4) or not Row['TargetTid'] or int(Row['TargetTid']) not in Identities:
                continue
            Need(int(Row['Version']) in (2, 3), "unsupported service lifecycle schema")
            Item = Identities[int(Row['TargetTid'])]
            Item.append((Tick, int(Row['Opcode']), int(Row['TargetPid'])))
            Need(len(Item) <= 64, "service lifecycle cap")
    Need(Count == Metadata['DecodedRows'], "CSV count mismatch")
    Used = set()
    for Service in Services:
        Events = Identities[int(Service['native_tid'])]
        Before = [Row for Row in Events if Row[0] <= int(Service['qpc_before'])]
        After = [Row for Row in Events if Row[0] >= int(Service['qpc_after'])]
        Need(Before and After, "service lifecycle not retained")
        Start, End = Before[-1], After[0]
        Identity = (int(Service['native_tid']), Start[0], End[0])
        Need(Identity not in Used and Start[1] in (1, 3) and End[1] in (2, 4) and
             Start[2] == End[2] == Metadata['ChildPid'] and
             Metadata['BeforeChildResumeQpc'] <= Start[0] < End[0] <= Metadata['AfterChildExitQpc'],
             "service lifecycle conflicting/reused")
        Used.add(Identity)
    return {'ServiceThreadCount': len(Services), 'NativeSnapshotCount': len(Snapshots),
            'NativeClockMapping': 'SNAPSHOT_BRACKETS_ONLY_NO_GLOBAL_OFFSET',
            'FiniteGrantTimestampResolutionNs': 1000}


def Main():
    Parser = argparse.ArgumentParser()
    Parser.add_argument("--root", type=Path, required=True)
    Parser.add_argument("--case", choices=("Full", "AckStats"), default="Full")
    Args = Parser.parse_args()
    Result = {"State": "INCOMPLETE", "CausalVerdict": "NOT_CLAIMED"}
    try:
        MetadataPath = Args.root / "metadata.json"
        LogPath = Args.root / "workload.stdout.txt"
        if MetadataPath.stat().st_size > 65536 or LogPath.stat().st_size > 32 * 1024 * 1024:
            raise ValueError("raw metadata/log size exceeds fixed bound")
        Result = Validate(json.loads(MetadataPath.read_text(encoding="utf-8")), LogPath.read_text(encoding="utf-8", errors="strict"),
                          Args.case, Args.root / "scheduler.csv")
    except (OSError, ValueError, TypeError) as Error:
        Result["Errors"] = [str(Error)]
    with (Args.root / "coverage.json").open("x", encoding="utf-8") as Output:
        json.dump(Result, Output, indent=2)
    return 0 if Result["State"] == "LOSS_FREE_ANCHOR_WINDOW_RETAINED" else 125


if __name__ == "__main__":
    raise SystemExit(Main())
