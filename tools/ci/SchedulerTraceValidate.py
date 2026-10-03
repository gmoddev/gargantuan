"""Offline loss and clock-window validation. Never attributes a stall or changes latency."""
import argparse
import json
from pathlib import Path


def Validate(Metadata, Stdout):
    Errors = []

    def Require(Condition, Message):
        if not Condition:
            Errors.append(Message)

    Require(Metadata.get("Format") == "GargantuanSchedulerTrace" and Metadata.get("Version") == 1, "unknown trace metadata")
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
            Fields = dict(Part.split("=", 1) for Part in Line.split()[1:])
            for Key in ("pid", "native_tid", "native_valid", "steady_ns", "qpc_before", "qpc_after", "qpc_frequency"):
                Fields[Key] = int(Fields[Key])
            Require(Fields["native_valid"] == 1 and Fields["pid"] == Metadata.get("ChildPid") and
                    Fields["native_tid"] == Metadata.get("ChildMainTid") and Fields["qpc_frequency"] == Frequency,
                    "fixture native identity/clock mismatch")
            Require(Fields.get("profile") == "FULL_RESERVATION", "unexpected fixture profile")
            Require(Metadata["BeforeChildResumeQpc"] <= Fields["qpc_before"] <= Fields["qpc_after"] <= Metadata["AfterChildExitQpc"], "fixture anchor outside child interval")
            Require(Metadata["MainFirstQpc"] <= Fields["qpc_before"] <= Fields["qpc_after"] <= Metadata["MainLastQpc"], "target thread events do not bracket fixture anchor")
            Anchors.append(Fields)
        except (ValueError, KeyError, TypeError):
            Errors.append("malformed fixture clock anchor")
    Require(bool(Anchors), "no fixture anchors")
    ByCase = {}
    for Anchor in Anchors:
        ByCase.setdefault(Anchor.get("case"), []).append(Anchor)
    for Case, Pair in ByCase.items():
        Require(len(Pair) == 2 and [Value.get("boundary") for Value in Pair] == ["BEGIN", "END"], "missing or duplicate anchor pair: " + str(Case))
        if len(Pair) == 2:
            Require(Pair[0]["qpc_after"] <= Pair[1]["qpc_before"] and Pair[0]["steady_ns"] <= Pair[1]["steady_ns"], "case clock order invalid")
    return {"Format": "GargantuanSchedulerTraceCoverage", "Version": 1,
            "State": "LOSS_FREE_ANCHOR_WINDOW_RETAINED" if not Errors else "INCOMPLETE",
            "CausalVerdict": "NOT_CLAIMED", "Errors": Errors, "FixtureAnchorCount": len(Anchors),
            "Note": "Window retention does not prove a scheduler cause; inspect the exact CSwitch/ReadyThread chain. Unknown lifecycle versions limit other-thread identity attribution."}


def Main():
    Parser = argparse.ArgumentParser()
    Parser.add_argument("--root", type=Path, required=True)
    Args = Parser.parse_args()
    Result = {"State": "INCOMPLETE", "CausalVerdict": "NOT_CLAIMED"}
    try:
        MetadataPath = Args.root / "metadata.json"
        LogPath = Args.root / "workload.stdout.txt"
        if MetadataPath.stat().st_size > 65536 or LogPath.stat().st_size > 32 * 1024 * 1024:
            raise ValueError("raw metadata/log size exceeds fixed bound")
        Result = Validate(json.loads(MetadataPath.read_text(encoding="utf-8")), LogPath.read_text(encoding="utf-8", errors="strict"))
    except (OSError, ValueError, TypeError) as Error:
        Result["Errors"] = [str(Error)]
    with (Args.root / "coverage.json").open("x", encoding="utf-8") as Output:
        json.dump(Result, Output, indent=2)
    return 0 if Result["State"] == "LOSS_FREE_ANCHOR_WINDOW_RETAINED" else 125


if __name__ == "__main__":
    raise SystemExit(Main())
