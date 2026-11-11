"""Summarize source-bound Runtime 6 comparison receipts without making a verdict.

Only successful invocations contribute performance samples. Each invocation and
its original ABBA position remains in the JSON, even when it failed.
"""

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import statistics


SCHEMAS = {
    "gargantuan_runtime_foundation6_benchmark": {
        "facets": ("Scenario", "Elements", "GroupWidth", "RendererOnly", "ProfileEnabled",
                   "CFrameProfileEnabled", "ExpectedUpdates", "WritesPerObject"),
        "stages": ("DirtyAccumulationMs", "PublicationGenerationMs", "ProjectionApplyMs",
                   "DirtyCaptureMs", "DirtyExpansionMs", "FinalStateExtractionMs",
                   "PublicationConstructionMs", "PublisherReconciliationMs", "JournalMs",
                   "ClassificationMs", "AccumulatorMs", "JournalRecords", "CFrameCommittedWrites",
                   "CFrameValidationNs", "CFrameMetadataLookupNs", "CFrameEncodingNs",
                   "CFrameJournalNs", "CFrameRevisionNs", "CFrameSignalNs"),
        "derived": {"FrameStagesMs": ("DirtyAccumulationMs", "PublicationGenerationMs",
                                       "ProjectionApplyMs")},
        "sample": True,
    },
    "gargantuan_runtime6_allocation_benchmark": {
        "facets": ("Scenario", "Elements", "GroupWidth", "RendererOnly", "ProfileEnabled",
                   "CFrameProfileEnabled", "ExpectedUpdates", "WritesPerObject"),
        "stages": ("DirtyAccumulationMs", "PublicationGenerationMs", "ProjectionApplyMs",
                   "DirtyCaptureMs", "DirtyExpansionMs", "FinalStateExtractionMs",
                   "PublicationConstructionMs", "PublisherReconciliationMs", "JournalMs",
                   "ClassificationMs", "AccumulatorMs", "JournalRecords", "FrameCppNewCalls",
                   "FrameCppNewBytes", "CFrameCommittedWrites", "CFrameValidationNs",
                   "CFrameMetadataLookupNs", "CFrameEncodingNs", "CFrameJournalNs",
                   "CFrameRevisionNs", "CFrameSignalNs"),
        "derived": {"FrameStagesMs": ("DirtyAccumulationMs", "PublicationGenerationMs",
                                       "ProjectionApplyMs")},
        "sample": True,
    },
    "gargantuan_runtime6_physics_sync_benchmark": {
        "facets": ("static_count", "dynamic_count", "control_density", "lifecycle_count",
                   "contacts_disabled", "publication_enabled"),
        "stages": ("create_ms", "control_ms", "step_ms", "publish_ms",
                   "projection_ms", "validation_ms", "frame_ms"),
        "derived": {"MeasuredStagesMs": ("create_ms", "control_ms", "step_ms",
                                          "publish_ms", "projection_ms", "validation_ms")},
        "sample": False,
    },
    "gargantuan_runtime6_extraction_alternatives": {
        "facets": ("count", "changed", "lifecycle", "order", "signal_profile_enabled"),
        "requiredFacets": ("count", "changed", "lifecycle", "order"),
        "stages": ("setter_ms", "capture_ms", "direct_ms", "soa_sync_ms",
                   "soa_consume_ms", "snapshot_sync_ms", "snapshot_consume_ms", "verify_ms",
                   "signal_calls", "signal_deferred_calls", "signal_connection_visits",
                   "signal_native_callbacks", "signal_script_callbacks", "signal_total_ns",
                   "signal_snapshot_ns", "signal_callback_ns", "signal_cleanup_ns"),
        "requiredStages": ("setter_ms", "capture_ms", "direct_ms", "soa_sync_ms",
                           "soa_consume_ms", "snapshot_sync_ms", "snapshot_consume_ms", "verify_ms"),
        "derived": {"SoaTotalMs": ("soa_sync_ms", "soa_consume_ms"),
                    "SnapshotTotalMs": ("snapshot_sync_ms", "snapshot_consume_ms"),
                    "DirectEndToEndMs": ("setter_ms", "capture_ms", "direct_ms"),
                    "SoaEndToEndMs": ("setter_ms", "capture_ms", "soa_sync_ms", "soa_consume_ms"),
                    "SnapshotEndToEndMs": ("setter_ms", "capture_ms", "snapshot_sync_ms",
                                             "snapshot_consume_ms")},
        "sample": False,
    },
    "gargantuan_runtime6_realistic_publication_probe": {
        "facets": ("mode", "phase", "objects", "changed"),
        "stages": ("source_ms", "produce_ms", "apply_ms", "publish_ms", "projection_ms"),
        "derived": {"FrameStagesMs": ("source_ms", "produce_ms", "apply_ms",
                                       "publish_ms", "projection_ms")},
        "sample": False,
    },
    "gargantuan_replication_benchmark": {
        "facets": ("Workload", "Objects", "Bytes"),
        "stages": ("GenerateMs", "EncodeMs", "DecodeMs", "SchedulerTransportMs", "ApplyMs"),
        "derived": {"MeasuredStagesMs": ("GenerateMs", "EncodeMs", "DecodeMs",
                                          "SchedulerTransportMs", "ApplyMs")},
        "sample": False,
    },
}


def Stats(Values):
    Ordered = sorted(Values)
    if not Ordered:
        return None
    return {"n": len(Ordered), "mean": statistics.fmean(Ordered),
            "p50": Ordered[math.ceil(0.50 * len(Ordered)) - 1],
            "p95": Ordered[math.ceil(0.95 * len(Ordered)) - 1],
            "p99": Ordered[math.ceil(0.99 * len(Ordered)) - 1]}


def Number(Value):
    try:
        Result = float(Value)
    except (TypeError, ValueError):
        return None
    return Result if math.isfinite(Result) else None


def ReadRows(Text, Binary):
    Lines = [Line for Line in Text.splitlines() if Line.strip()]
    if Binary == "gargantuan_asset_foundation_benchmark":
        return [{"Scenario": Match.group(1), "ValueMs": Match.group(2)}
                for Line in Lines if (Match := re.fullmatch(r"METRIC\s+(\S+_ms)\s+(\S+)", Line))]
    Schema = SCHEMAS.get(Binary)
    if Schema is None:
        return []
    # Some benchmark stdout contains progress messages as well as CSV. A row
    # is admitted only when its width matches the selected header exactly.
    Headers = []
    Rows = []
    for Line in Lines:
        Fields = next(csv.reader([Line]))
        if (Fields and all(Field in Fields for Field in Schema.get("requiredFacets", Schema["facets"]))
                and all(Field in Fields for Field in Schema.get("requiredStages", Schema["stages"]))):
            Headers = Fields
        elif Headers and len(Fields) == len(Headers):
            Row = dict(zip(Headers, Fields))
            if Binary == "gargantuan_runtime6_physics_sync_benchmark":
                if Row.get("kind") != "frame" or Row.get("phase") != "sample":
                    continue
            if Binary == "gargantuan_replication_benchmark" and Row.get("Workload") == "Workload":
                continue
            Rows.append(Row)
    return Rows


def ReadSampleRows(File):
    with File.open(newline="", encoding="utf-8") as Handle:
        return list(csv.DictReader(Handle))


def ReportedOnly(Text, Binary):
    """Retain benchmark-owned summaries without treating them as frame samples."""
    if Binary == "gargantuan_soft_body_physics_benchmark":
        Records = []
        for Line in Text.splitlines():
            try:
                Value = json.loads(Line)
            except json.JSONDecodeError:
                continue
            if isinstance(Value, dict) and "Scenario" in Value:
                Records.append(Value)
        return Records
    if Binary == "gargantuan_animation_foundation_benchmark":
        Records = []
        for Line in Text.splitlines():
            if not Line.startswith("[Animation:") or "Ms=" not in Line:
                continue
            Fields = dict(re.findall(r"([A-Za-z][A-Za-z0-9]*)=([^\s]+)", Line))
            if Fields:
                Records.append({"prefix": Line.split("]", 1)[0] + "]", "fields": Fields})
        return Records
    return []


def GroupRows(Rows, Binary):
    if Binary == "gargantuan_asset_foundation_benchmark":
        FacetNames, StageNames, Derived = ("Scenario",), ("ValueMs",), {}
    else:
        Schema = SCHEMAS.get(Binary)
        if Schema is None:
            return []
        FacetNames, StageNames, Derived = Schema["facets"], Schema["stages"], Schema["derived"]
    Groups = {}
    for Row in Rows:
        Facets = {Name: Row.get(Name, "") for Name in FacetNames}
        Key = tuple(Facets.items())
        Group = Groups.setdefault(Key, {"scenario": Facets, "rowCount": 0,
                                        "values": {Name: [] for Name in (*StageNames, *Derived)}})
        Group["rowCount"] += 1
        for Name in StageNames:
            Value = Number(Row.get(Name))
            if Value is not None:
                Group["values"][Name].append(Value)
        for Name, Members in Derived.items():
            Values = [Number(Row.get(Member)) for Member in Members]
            if all(Value is not None for Value in Values):
                Group["values"][Name].append(sum(Values))
    return [{"scenario": Group["scenario"], "rowCount": Group["rowCount"],
             "stages": {Name: Stats(Values) for Name, Values in Group["values"].items()
                        if Values}}
            for Group in Groups.values()]


def FileByBasename(Comparison, Name):
    File = Comparison / str(Name).replace("\\", "/").rsplit("/", 1)[-1]
    return File if File.is_file() else None


def ExecutableName(Command):
    Name = str(Command).replace("\\", "/").rsplit("/", 1)[-1]
    return Name[:-4] if Name.lower().endswith(".exe") else Name


def Summarize(Evidence, ComparisonDirectory="comparison"):
    Comparison = Evidence / ComparisonDirectory
    Receipts = json.loads((Comparison / "receipts.json").read_text(encoding="utf-8"))
    Sources = json.loads((Comparison / "sources.json").read_text(encoding="utf-8"))
    Runs = []
    Combined = {}
    Failures = {}
    for Index, Receipt in enumerate(Receipts):
        Arm, Label = Receipt["arm"], Receipt["label"]
        Source = Sources[Arm]["source"]
        if Receipt["source"] != Source:
            raise ValueError(f"[Runtime6:Summarize] Source mismatch at receipt {Index}")
        Binary = ExecutableName(Receipt["command"][0])
        Status = "timeout" if Receipt["timeout"] else ("ok" if Receipt["exit"] == 0 else "failed")
        Run = {"receiptIndex": Index, "label": Label, "arm": Arm, "source": Source,
               "binary": Binary, "command": Receipt["command"], "status": Status,
               "exit": Receipt["exit"], "elapsedSeconds": Receipt.get("elapsedSeconds"),
               "resources": {}, "scenarios": []}
        ResourceFile = FileByBasename(Comparison, Receipt.get("resources", ""))
        if ResourceFile:
            Match = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)",
                              ResourceFile.read_text(encoding="utf-8", errors="replace"))
            if Match:
                Run["resources"]["maxResidentKiB"] = int(Match.group(1))
        if "memoryStatus" in Receipt:
            Run["resources"]["memoryStatus"] = Receipt["memoryStatus"]
        PeakBytes = Receipt.get("peakWorkingSetBytes")
        if (Receipt.get("memoryStatus") == "measured" and isinstance(PeakBytes, (int, float))
                and not isinstance(PeakBytes, bool) and math.isfinite(PeakBytes) and PeakBytes > 0):
            Run["resources"]["peakWorkingSetKiB"] = PeakBytes / 1024.0
        if Status != "ok":
            Failures[(Label, Arm, Status)] = Failures.get((Label, Arm, Status), 0) + 1
            Runs.append(Run)
            continue
        OutputFile = Comparison / f"{Index:03}-{Label}-{Arm}.stdout"
        if not OutputFile.is_file():
            raise FileNotFoundError(OutputFile)
        Content = OutputFile.read_bytes()
        if hashlib.sha256(Content).hexdigest() != Receipt["stdoutSha256"]:
            raise ValueError(f"[Runtime6:Summarize] Stdout digest mismatch at receipt {Index}")
        Schema = SCHEMAS.get(Binary)
        Text = Content.decode("utf-8", errors="replace")
        if Schema and Schema["sample"]:
            SampleArgument = next((Value for Value in Receipt["command"] if Value.startswith("--samples=")), None)
            if SampleArgument is None:
                raise ValueError(f"[Runtime6:Summarize] Missing samples argument at receipt {Index}")
            SampleFile = FileByBasename(Comparison, SampleArgument.split("=", 1)[1])
            if SampleFile is None:
                raise FileNotFoundError(f"[Runtime6:Summarize] Missing fresh sample for receipt {Index}")
            Run["sampleFile"] = SampleFile.name
            Rows = ReadSampleRows(SampleFile)
        else:
            Rows = ReadRows(Text, Binary)
        Run["scenarios"] = GroupRows(Rows, Binary)
        Reported = ReportedOnly(Text, Binary)
        if Reported:
            Run["reportedOnly"] = Reported
        if not Run["scenarios"]:
            Run["note"] = "No raw numeric frame rows; benchmark summaries, if any, are reportedOnly"
        for Scenario in Run["scenarios"]:
            Key = (Label, Arm, Binary, json.dumps(Scenario["scenario"], sort_keys=True))
            Combined.setdefault(Key, {"label": Label, "arm": Arm, "binary": Binary,
                                      "scenario": Scenario["scenario"], "receiptIndices": [],
                                      "values": {}})
            Group = Combined[Key]
            Group["receiptIndices"].append(Index)
            # Reuse the exact per-run frame rows to aggregate raw samples.
            # This deliberately keeps distinct scenario facets in separate keys.
        for Row in Rows:
            if Binary == "gargantuan_asset_foundation_benchmark":
                Facets, Names, Derived = {"Scenario": Row["Scenario"]}, ("ValueMs",), {}
            elif Schema:
                Facets = {Name: Row.get(Name, "") for Name in Schema["facets"]}
                Names, Derived = Schema["stages"], Schema["derived"]
            else:
                continue
            Key = (Label, Arm, Binary, json.dumps(Facets, sort_keys=True))
            Group = Combined[Key]
            for Name in Names:
                Value = Number(Row.get(Name))
                if Value is not None:
                    Group["values"].setdefault(Name, []).append(Value)
            for Name, Members in Derived.items():
                Values = [Number(Row.get(Member)) for Member in Members]
                if all(Value is not None for Value in Values):
                    Group["values"].setdefault(Name, []).append(sum(Values))
        Runs.append(Run)
    Aggregates = [{"label": Group["label"], "arm": Group["arm"], "binary": Group["binary"],
                   "scenario": Group["scenario"], "receiptIndices": Group["receiptIndices"],
                   "stages": {Name: Stats(Values) for Name, Values in Group["values"].items()}}
                  for Group in Combined.values()]
    Memory = {}
    for Run in Runs:
        if Run["status"] != "ok":
            continue
        for Metric in ("maxResidentKiB", "peakWorkingSetKiB"):
            Value = Run["resources"].get(Metric)
            if Value is None:
                continue
            Group = Memory.setdefault((Run["label"], Run["arm"], Metric),
                                      {"receiptIndices": [], "values": []})
            Group["receiptIndices"].append(Run["receiptIndex"])
            Group["values"].append(Value)
    Abba = []
    for Start in range(len(Runs) - 3):
        Quartet = Runs[Start:Start + 4]
        if [Run["arm"] for Run in Quartet] != ["baseline", "candidate", "candidate", "baseline"]:
            continue
        if len({Run["label"] for Run in Quartet}) != 1:
            continue
        Drift = []
        ByRun = [{json.dumps(Item["scenario"], sort_keys=True): Item for Item in Run["scenarios"]}
                 for Run in Quartet]
        for Key in sorted(set.intersection(*(set(Items) for Items in ByRun))):
            Items = [Run[Key] for Run in ByRun]
            for Stage in sorted(set.intersection(*(set(Item["stages"]) for Item in Items))):
                Means = [Item["stages"][Stage]["mean"] for Item in Items]
                Drift.append({"scenario": Items[0]["scenario"], "stage": Stage,
                              "orderedMeans": Means, "baselineLastMinusFirst": Means[3] - Means[0],
                              "candidateSecondMinusFirst": Means[2] - Means[1]})
        Abba.append({"label": Quartet[0]["label"], "receiptIndices": [Run["receiptIndex"] for Run in Quartet],
                     "arms": [Run["arm"] for Run in Quartet],
                     "sources": [Run["source"] for Run in Quartet],
                     "statuses": [Run["status"] for Run in Quartet], "drift": Drift})
    return {"format": "runtime6-summary-v1", "sources": Sources, "receiptCount": len(Receipts),
            "runs": Runs, "aggregates": Aggregates,
            "memoryKiB": [{"label": Label, "arm": Arm, "metric": Metric,
                           "receiptIndices": Group["receiptIndices"], "stats": Stats(Group["values"])}
                          for (Label, Arm, Metric), Group in sorted(Memory.items())],
            "failures": [{"label": Label, "arm": Arm, "status": Status, "count": Count}
                         for (Label, Arm, Status), Count in sorted(Failures.items())], "abba": Abba}


def Main():
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--evidence", type=Path, required=True)
    Parser.add_argument("--comparison-directory", default="comparison")
    Parser.add_argument("--output", type=Path, help="JSON path (default: comparison/summary.json)")
    Args = Parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", Args.comparison_directory):
        Parser.error("comparison directory must be a simple basename")
    Result = Summarize(Args.evidence.resolve(), Args.comparison_directory)
    Output = Args.output or Args.evidence / Args.comparison_directory / "summary.json"
    Output.parent.mkdir(parents=True, exist_ok=True)
    Output.write_text(json.dumps(Result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"[Runtime6:Summarize] {Output}: {Result['receiptCount']} receipts, "
          f"{len(Result['aggregates'])} aggregate groups, {sum(Item['count'] for Item in Result['failures'])} failures")


if __name__ == "__main__":
    Main()
