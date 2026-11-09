"""Reduce native/Engine timing receipts without making a quality-winner claim.

Example:
  python ReducePerformance.py --root .. --inputs B-Provider-20261109T060849 \
      B-Engine-20261109T060849 Group512-20261109T064817

The input list can later include C-Provider and C-Engine folders with the
same Receipts.json schema. No compilation, simulation, or network access.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import pathlib
import re
import statistics
from collections import defaultdict


PROFILE = {
    "jolt_world_updates": 2,
    "jolt_collision_steps_per_update": 8,
    "jolt_soft_iterations_per_collision_step": 5,
    "jolt_constraint_sweeps_per_iteration": 2,
    "jolt_soft_integrations_per_frame": 80,
    "jolt_full_update_sleep_clock": 1,
}
PREFIXES = {
    "[Physics:ProviderBenchmark] ": "provider",
    "[Physics:EngineBenchmark] ": "engine",
}


def Require(Condition: bool, Message: str) -> None:
    if not Condition:
        raise ValueError(Message)


def Number(Value: object | None) -> float | None:
    if Value is None or Value == "":
        return None
    return float(Value)


def Integer(Value: object | None) -> int | None:
    if Value is None or Value == "":
        return None
    return int(Value)


def Kv(Text: str) -> dict[str, str]:
    return dict(Token.split("=", 1) for Token in Text.split() if "=" in Token)


def ReadCache(Folder: pathlib.Path) -> dict[str, str]:
    Cache = Folder / "CMakeCache.txt"
    if not Cache.exists():
        return {}
    return dict(
        Line.split("=", 1)
        for Line in Cache.read_text(errors="replace").splitlines()
        if "=" in Line and not Line.startswith(("#", "//"))
    )


def ValidateCache(Folder: pathlib.Path) -> bool:
    Cache = ReadCache(Folder)
    for Key in ("JPH_USE_EXTERNAL_PROFILE:BOOL", "PHYSICS_EVALUATION_NATIVE_PROFILE:BOOL",
                "PROFILER_IN_DEBUG_AND_RELEASE:BOOL"):
        if Key in Cache:
            Require(Cache[Key] == "OFF", f"{Folder}: profiled CMake cache {Key}={Cache[Key]}")
    return bool(Cache) and all(Cache.get(Key) == "OFF" for Key in (
        "JPH_USE_EXTERNAL_PROFILE:BOOL", "PHYSICS_EVALUATION_NATIVE_PROFILE:BOOL"))


def CheckReceipt(Folder: pathlib.Path, Stdout: pathlib.Path, Receipt: dict,
                 Stderr: pathlib.Path | None) -> bytes:
    Require(Receipt.get("ExitCode") == 0 and not Receipt.get("TimedOut", False),
            f"{Stdout}: process failed or timed out")
    Data = Stdout.read_bytes()
    Digest = hashlib.sha256(Data).hexdigest()
    Require(Digest == Receipt["StdoutSha256"], f"{Stdout}: stdout digest mismatch")
    if Stderr is not None and Stderr.exists() and Receipt.get("StderrSha256"):
        Require(hashlib.sha256(Stderr.read_bytes()).hexdigest() == Receipt["StderrSha256"],
                f"{Stderr}: stderr digest mismatch")
    return Data


def Percentile(Values: list[float], Fraction: float) -> float:
    return sorted(Values)[math.ceil(Fraction * len(Values)) - 1]


def ParseOutput(Data: bytes, Label: str, CacheUnprofiled: bool) -> dict:
    Header: dict[str, str] | None = None
    Summary: dict[str, str] | None = None
    Lifecycle: dict | None = None
    Frames: list[dict[str, str]] = []
    Warmup: list[dict[str, str]] = []
    StateHashes: list[int] = []
    Kind: str | None = None
    for Line in Data.decode("utf-8", errors="strict").splitlines():
        Match = next(((Prefix, Name) for Prefix, Name in PREFIXES.items()
                      if Line.startswith(Prefix)), None)
        if Match is None:
            continue
        Prefix, Name = Match
        Require(Kind is None or Kind == Name, f"{Label}: mixed benchmark types")
        Kind = Name
        Body = Line[len(Prefix):]
        if Body.startswith("summary "):
            Require(Summary is None, f"{Label}: duplicate summary")
            Summary = Kv(Body[8:])
        elif Body.startswith("lifecycle "):
            Require(Lifecycle is None, f"{Label}: duplicate lifecycle")
            Lifecycle = json.loads(Body[10:])
        elif Body.startswith("frame="):
            Frames.append(Kv(Body))
        elif Body.startswith("warmup_frame="):
            Warmup.append(Kv(Body))
        elif Body.startswith("state_frame="):
            StateHashes.append(int(Kv(Body)["soft_trajectory_hash"]))
        elif Body.startswith("backend="):
            Require(Header is None, f"{Label}: duplicate header")
            Header = Kv(Body)
    Require(Kind in ("provider", "engine") and Header is not None and Summary is not None,
            f"{Label}: missing header/summary")
    Require(len(Frames) == int(Summary["frames"]), f"{Label}: measured frame count mismatch")
    ExpectedWarmup = int(Header.get("warmup_frames", "0"))
    Require(len(Warmup) == ExpectedWarmup, f"{Label}: warmup frame count mismatch")
    if StateHashes:
        Require(len(StateHashes) == len(Frames) + len(Warmup),
                f"{Label}: state hash count mismatch")
        Require(StateHashes[-1] == int(Summary["soft_trajectory_hash"]),
                f"{Label}: terminal state hash mismatch")
    for Key in ("backend", "scene", "coupling", "workers_requested"):
        Require(Header.get(Key) == Summary.get(Key), f"{Label}: inconsistent {Key}")
    if Kind == "provider":
        Require(Header.get("timing_instrumented") == "0"
                and Summary.get("timing_instrumented") == "0"
                and Lifecycle is not None and Lifecycle.get("TimingInstrumented") is False,
                f"{Label}: provider timing instrumentation not verified off")
    else:
        Require(CacheUnprofiled, f"{Label}: Engine timing instrumentation unverified by CMake cache")
    if Header["backend"] == "jolt":
        for Key, Expected in PROFILE.items():
            Require(int(Header[Key]) == Expected and int(Summary[Key]) == Expected,
                    f"{Label}: native profile {Key} differs from {Expected}")

    Metric = "total_ms" if Kind == "provider" else "physics_step_wall_ms"
    Values = [float(Frame[Metric]) for Frame in Frames]
    P50 = statistics.median(Values)
    P95 = Percentile(Values, 0.95)
    P99 = Percentile(Values, 0.99)
    MedianKey = "median_total_ms" if Kind == "provider" else "median_physics_step_wall_ms"
    Require(abs(P50 - float(Summary[MedianKey])) <= 0.0011,
            f"{Label}: raw-frame median differs from summary")
    Run = (Lifecycle or {}).get("RunResources", {})
    Whole = (Lifecycle or {}).get("WholeProcessResources", {})
    Return = (Lifecycle or {}).get("RetireResources", {})
    Row = {
        "Benchmark": Kind,
        "Backend": Header["backend"],
        "Scene": Header["scene"],
        "Coupling": Header["coupling"],
        "Workers": int(Header["workers_requested"]),
        "Frames": len(Frames),
        "WarmupFrames": len(Warmup),
        "TimingVerifiedUninstrumented": True,
        "Metric": Metric,
        "MeanMs": statistics.fmean(Values),
        "P50Ms": P50,
        "P95Ms": P95,
        "P99Ms": P99,
        "MaxMs": max(Values),
        "AdmissionMs": Number((Lifecycle or {}).get("StartupAdmissionMs", Summary.get("startup_admission_ms"))),
        "TeardownMs": Number((Lifecycle or {}).get("TeardownMs")),
        "EngineSetupMs": Number(Summary.get("setup_ms")),
        "EngineSemanticDestroyMs": Number(Summary.get("semantic_destroy_ms")),
        "EngineOwnerReleaseMs": Number(Summary.get("owner_release_ms")),
        "RunCpuSeconds": Number(Run.get("ProcessCpuSeconds")),
        "WholeProcessCpuSeconds": Number(Whole.get("ProcessCpuSeconds")),
        "RunCpuScope": "AfterAdmission to AfterRun, including warmup and quality/logging work" if Kind == "provider" else None,
        "RunWorkingSetBeforeBytes": Integer(Run.get("WorkingSetBeforeBytes")),
        "RunWorkingSetAfterBytes": Integer(Run.get("WorkingSetAfterBytes")),
        "ProcessLifetimePeakWorkingSetBytes": Integer(Whole.get("PeakWorkingSetProcessLifetimeBytes")),
        "PostRetireWorkingSetBytes": Integer((Lifecycle or {}).get("PostRetireWorkingSetBytes")),
        "RetireWorkingSetAfterBytes": Integer(Return.get("WorkingSetAfterBytes")),
        "StateHash": StateHashes[-1] if StateHashes else Integer(Summary.get("soft_trajectory_hash")),
        "MaxPinError": Number(Summary.get("max_pin_error")),
        "MaxEdgeStrain": Number(Summary.get("max_edge_strain")),
        "MaxPoststepPenetration": Number(Summary.get("max_poststep_penetration")),
        "NativeIntegrationsPerFrame": Integer(Header.get("jolt_soft_integrations_per_frame")),
        "ConstraintSweeps": Integer(Header.get("jolt_constraint_sweeps_per_iteration")),
        "GroupBatch": Integer(Header.get("jolt_large_cloth_group_batch")),
    }
    return Row


def ReceiptRows(Folder: pathlib.Path) -> list[dict]:
    Receipts = json.loads((Folder / "Receipts.json").read_text())
    Require(isinstance(Receipts, list), f"{Folder}: invalid receipt list")
    CacheUnprofiled = ValidateCache(Folder)
    Rows = []
    for Receipt in Receipts:
        Stdout = Folder / Receipt["Stdout"]
        Stderr = Folder / Receipt["Stderr"] if Receipt.get("Stderr") else None
        Data = CheckReceipt(Folder, Stdout, Receipt, Stderr)
        Row = ParseOutput(Data, str(Stdout), CacheUnprofiled)
        for Key in ("Backend", "Scene", "Coupling"):
            Require(Row[Key] == Receipt[Key], f"{Stdout}: receipt {Key} mismatch")
        Require(Row["Workers"] == Receipt["Workers"], f"{Stdout}: receipt worker mismatch")
        Row.update({
            "Folder": Folder.name,
            "Variant": Folder.name[0] if Folder.name.startswith(("B-", "C-")) else "unknown",
            "Tail": Receipt.get("Tail", "cold_process"),
            "Repeat": Receipt.get("Repeat"),
            "PairKey": Receipt.get("Pair"),
            "Receipt": "Receipts.json",
            "Stdout": Stdout.name,
            "StdoutSha256": Receipt["StdoutSha256"],
            "ProcessElapsedSeconds": Number(Receipt.get("ElapsedSeconds")),
        })
        Cpu = Row["WholeProcessCpuSeconds"]
        Wall = Row["ProcessElapsedSeconds"]
        Row["ApproxAverageOccupiedCoresLowerBound"] = Cpu / Wall if Cpu is not None and Wall and Wall > 0 else None
        Rows.append(Row)
    SummaryPath = Folder / "Summary.json"
    if SummaryPath.exists():
        Summary = json.loads(SummaryPath.read_text())
        Require(Summary.get("Status") == "COMPLETE" and Summary.get("QualificationPass") is True,
                f"{Folder}: input suite incomplete or failed")
    return Rows


def GroupTimingRows(Folder: pathlib.Path) -> list[dict]:
    CacheUnprofiled = ValidateCache(Folder)
    SourcePath = Folder / "Source.json"
    Source = json.loads(SourcePath.read_text()) if SourcePath.exists() else {}
    Rows = []
    for Stdout in sorted(Folder.glob("Timing-*.jsonl")):
        GroupMatch = re.fullmatch(r"Timing-([BC])-W(\d+)-R(\d+)\.jsonl", Stdout.name)
        LazyMatch = re.fullmatch(r"Timing-(Control|Candidate)-([a-z0-9]+)-W(\d+)-R(\d+)\.jsonl", Stdout.name)
        Require(GroupMatch is not None or LazyMatch is not None,
                f"{Stdout}: unrecognized timing filename")
        if GroupMatch is not None:
            Variant, Workers, Repeat = GroupMatch.group(1), int(GroupMatch.group(2)), int(GroupMatch.group(3))
            ExpectedScene = None
        else:
            Variant, ExpectedScene, Workers, Repeat = (LazyMatch.group(1), LazyMatch.group(2),
                                                       int(LazyMatch.group(3)), int(LazyMatch.group(4)))
        ReceiptPath = Stdout.with_suffix(".receipt.json")
        Receipt = json.loads(ReceiptPath.read_text())
        Data = CheckReceipt(Folder, Stdout, Receipt, Stdout.with_suffix(".stderr"))
        Row = ParseOutput(Data, str(Stdout), CacheUnprofiled)
        Require(Row["Workers"] == Workers, f"{Stdout}: worker mismatch")
        if ExpectedScene is not None:
            Require(Row["Scene"] == ExpectedScene, f"{Stdout}: scene mismatch")
        if Folder.name.startswith("Lazy"):
            ExpectedBatch = 256 if Variant == "Control" else Source.get("LargeClothGroupBatch")
            Require(Row["GroupBatch"] == ExpectedBatch,
                    f"{Stdout}: Control/Candidate grouping batch mismatch")
        elif Variant == "C" and Source.get("LargeClothGroupBatch") is not None:
            Require(Row["GroupBatch"] == Source["LargeClothGroupBatch"],
                    f"{Stdout}: candidate grouping batch mismatch")
        Row.update({
            "Folder": Folder.name,
            "Variant": Variant,
            "Tail": "warm" if Row["WarmupFrames"] else "cold",
            "Repeat": Repeat,
            "PairKey": f"R{Repeat}",
            "Receipt": ReceiptPath.name,
            "Stdout": Stdout.name,
            "StdoutSha256": Receipt["StdoutSha256"],
            "ProcessElapsedSeconds": Number(Receipt.get("Seconds")),
        })
        Cpu = Row["WholeProcessCpuSeconds"]
        Wall = Row["ProcessElapsedSeconds"]
        Row["ApproxAverageOccupiedCoresLowerBound"] = Cpu / Wall if Cpu is not None and Wall and Wall > 0 else None
        Rows.append(Row)
    return Rows


def GroupStatsRows(Folder: pathlib.Path) -> list[dict]:
    Path = Folder / "GroupStats.jsonl"
    if not Path.exists():
        return []
    ReceiptPath = Folder / "GroupStats.receipt.json"
    Require(ReceiptPath.exists(), f"{Path}: missing group-stat receipt")
    CheckReceipt(Folder, Path, json.loads(ReceiptPath.read_text()), Folder / "GroupStats.stderr")
    Rows = []
    for Line in Path.read_text().splitlines():
        if not Line.startswith("[Physics:Admission] "):
            continue
        Record = json.loads(Line[len("[Physics:Admission] "):])
        for Sample in Record.get("Samples", []):
            Admission = Sample.get("Admission", {})
            Rows.append({
                "Folder": Folder.name,
                "Scene": Record.get("Scene"),
                "Workers": Record.get("Workers"),
                "Repeat": Sample.get("Repeat"),
                "Body": Sample.get("Body"),
                "Instrumented": Record.get("Instrumented"),
                "CreateMs": Sample.get("CreateMs"),
                "OptimizeMs": Admission.get("Optimize", {}).get("Milliseconds"),
                "UpdateGroups": Admission.get("UpdateGroups"),
                "ParallelEdges": Admission.get("ParallelEdges"),
                "NonParallelEdges": Admission.get("NonParallelEdges"),
                "ParallelVolumes": Admission.get("ParallelVolumes"),
                "NonParallelVolumes": Admission.get("NonParallelVolumes"),
            })
    return Rows


def MedianPresent(Members: list[dict], Field: str) -> float | None:
    Values = [Member[Field] for Member in Members if Member[Field] is not None]
    return statistics.median(Values) if Values else None


def Aggregates(Rows: list[dict]) -> list[dict]:
    Buckets = defaultdict(list)
    for Row in Rows:
        Key = tuple(Row[Field] for Field in
                    ("Folder", "Variant", "Benchmark", "Backend", "Scene", "Coupling", "Workers", "Tail"))
        Buckets[Key].append(Row)
    Output = []
    for Key, Members in sorted(Buckets.items()):
        Output.append(dict(zip(("Folder", "Variant", "Benchmark", "Backend", "Scene",
                                "Coupling", "Workers", "Tail"), Key)) | {
            "Processes": len(Members),
            "MedianProcessP50Ms": statistics.median(Member["P50Ms"] for Member in Members),
            "MinProcessP50Ms": min(Member["P50Ms"] for Member in Members),
            "MaxProcessP50Ms": max(Member["P50Ms"] for Member in Members),
            "MedianProcessP95Ms": statistics.median(Member["P95Ms"] for Member in Members),
            "MedianProcessP99Ms": statistics.median(Member["P99Ms"] for Member in Members),
            "MedianAdmissionMs": MedianPresent(Members, "AdmissionMs"),
            "MedianTeardownMs": MedianPresent(Members, "TeardownMs"),
            "MedianEngineSetupMs": MedianPresent(Members, "EngineSetupMs"),
            "MedianEngineSemanticDestroyMs": MedianPresent(Members, "EngineSemanticDestroyMs"),
            "MedianEngineOwnerReleaseMs": MedianPresent(Members, "EngineOwnerReleaseMs"),
            "MedianRunCpuSeconds": MedianPresent(Members, "RunCpuSeconds"),
            "MedianWholeProcessCpuSeconds": MedianPresent(Members, "WholeProcessCpuSeconds"),
            "MedianProcessElapsedSeconds": MedianPresent(Members, "ProcessElapsedSeconds"),
            "MedianApproxAverageOccupiedCoresLowerBound": MedianPresent(Members, "ApproxAverageOccupiedCoresLowerBound"),
            "MedianRunWorkingSetBeforeBytes": MedianPresent(Members, "RunWorkingSetBeforeBytes"),
            "MedianRunWorkingSetAfterBytes": MedianPresent(Members, "RunWorkingSetAfterBytes"),
            "MedianProcessLifetimePeakWorkingSetBytes": MedianPresent(Members, "ProcessLifetimePeakWorkingSetBytes"),
            "MedianPostRetireWorkingSetBytes": MedianPresent(Members, "PostRetireWorkingSetBytes"),
            "QualityWinnerClaim": None,
        })
    return Output


def Paired(Rows: list[dict]) -> list[dict]:
    Buckets = defaultdict(dict)
    for Row in Rows:
        if Row["Variant"] not in ("B", "C", "Control", "Candidate"):
            continue
        Key = tuple(Row[Field] for Field in
                    ("Benchmark", "Backend", "Scene", "Coupling", "Workers", "Tail", "PairKey"))
        Scope = Row["Folder"] if Row["Folder"].startswith(("Group", "Lazy", "Final-")) else "matrix"
        Slot = Buckets[(Scope, Key)]
        Require(Row["Variant"] not in Slot, f"duplicate {Row['Variant']} process for {Scope} {Key}")
        Slot[Row["Variant"]] = Row
    Output = []
    for (Scope, Key), Pair in sorted(Buckets.items()):
        if set(Pair) == {"B", "C"}:
            BaselineLabel, CandidateLabel = "B", "C"
        elif set(Pair) == {"Control", "Candidate"}:
            BaselineLabel, CandidateLabel = "Control", "Candidate"
        else:
            continue
        B, C = Pair[BaselineLabel], Pair[CandidateLabel]
        Output.append(dict(zip(("Benchmark", "Backend", "Scene", "Coupling", "Workers",
                                "Tail", "PairKey"), Key)) | {
            "Scope": Scope,
            "PairingDesign": "aligned same-cell/repeat across separate A/B and A/C cohorts; not direct B/C counterbalanced"
            if Scope == "matrix" else "direct same-folder B/C counterbalanced" if Scope.startswith(("Group", "Final-"))
            else "same-folder Control/Candidate repeat; launch order not asserted",
            "BaselineLabel": BaselineLabel, "CandidateLabel": CandidateLabel,
            "BaselineFolder": B["Folder"], "CandidateFolder": C["Folder"],
            "BFolder": B["Folder"], "CFolder": C["Folder"],
            "BaselineP50Ms": B["P50Ms"], "CandidateP50Ms": C["P50Ms"],
            "BP50Ms": B["P50Ms"], "CP50Ms": C["P50Ms"],
            "DeltaP50Ms": C["P50Ms"] - B["P50Ms"],
            "DeltaP50Percent": 100.0 * (C["P50Ms"] / B["P50Ms"] - 1.0),
            "BP95Ms": B["P95Ms"], "CP95Ms": C["P95Ms"],
            "DeltaP95Ms": C["P95Ms"] - B["P95Ms"],
            "BP99Ms": B["P99Ms"], "CP99Ms": C["P99Ms"],
            "DeltaP99Ms": C["P99Ms"] - B["P99Ms"],
            "BStateHash": B["StateHash"], "CStateHash": C["StateHash"],
            "ExactTerminalState": B["StateHash"] == C["StateHash"]
            if B["StateHash"] is not None and C["StateHash"] is not None else None,
            "QualityWinnerClaim": None,
        })
    return Output


def ComparisonSummary(PairedRows: list[dict]) -> list[dict]:
    Buckets = defaultdict(list)
    for Row in PairedRows:
        Key = tuple(Row[Field] for Field in
                    ("Scope", "BaselineLabel", "CandidateLabel", "Benchmark", "Backend",
                     "Scene", "Coupling", "Workers", "Tail"))
        Buckets[Key].append(Row)
    Output = []
    for Key, Members in sorted(Buckets.items()):
        Deltas = [Member["DeltaP50Percent"] for Member in Members]
        States = [Member["ExactTerminalState"] for Member in Members]
        ExactStates = all(States) if all(State is not None for State in States) else None
        Output.append(dict(zip(("Scope", "BaselineLabel", "CandidateLabel", "Benchmark",
                                "Backend", "Scene", "Coupling", "Workers", "Tail"), Key)) | {
            "PairingDesign": Members[0]["PairingDesign"],
            "Pairs": len(Members),
            "MedianBaselineP50Ms": statistics.median(Member["BP50Ms"] for Member in Members),
            "MedianCandidateP50Ms": statistics.median(Member["CP50Ms"] for Member in Members),
            "MedianBaselineP95Ms": statistics.median(Member["BP95Ms"] for Member in Members),
            "MedianCandidateP95Ms": statistics.median(Member["CP95Ms"] for Member in Members),
            "MedianBaselineP99Ms": statistics.median(Member["BP99Ms"] for Member in Members),
            "MedianCandidateP99Ms": statistics.median(Member["CP99Ms"] for Member in Members),
            "MedianMatchedDeltaP50Percent": statistics.median(Deltas),
            "MinMatchedDeltaP50Percent": min(Deltas),
            "MaxMatchedDeltaP50Percent": max(Deltas),
            "AllPairsFaster": all(Delta < 0 for Delta in Deltas),
            "AllPairsExactTerminalState": ExactStates,
            "QualityWinnerClaim": None,
        })
    return Output


def WorkerScaling(AggregateRows: list[dict]) -> list[dict]:
    Buckets = defaultdict(dict)
    for Row in AggregateRows:
        if Row["Variant"] not in ("B", "C") or not Row["Folder"].startswith(("B-", "C-")):
            continue
        Key = tuple(Row[Field] for Field in
                    ("Variant", "Benchmark", "Backend", "Scene", "Coupling", "Tail"))
        Require(Row["Workers"] not in Buckets[Key], f"duplicate scaling cap {Key} W{Row['Workers']}")
        Buckets[Key][Row["Workers"]] = Row
    Output = []
    for Key, Caps in sorted(Buckets.items()):
        if 1 not in Caps:
            continue
        One = Caps[1]["MedianProcessP50Ms"]
        for Workers, Row in sorted(Caps.items()):
            Output.append(dict(zip(("Variant", "Benchmark", "Backend", "Scene", "Coupling", "Tail"), Key)) | {
                "Workers": Workers,
                "Worker1P50Ms": One,
                "CappedP50Ms": Row["MedianProcessP50Ms"],
                "Worker1ToCapSpeedRatio": One / Row["MedianProcessP50Ms"],
                "MetricScope": "median process P50; not CPU utilization",
            })
    return Output


def NativeEngineTable(AggregateRows: list[dict]) -> list[dict]:
    Index = defaultdict(dict)
    for Row in AggregateRows:
        if Row["Variant"] not in ("B", "C") or not Row["Folder"].startswith(("B-", "C-")):
            continue
        Key = tuple(Row[Field] for Field in
                    ("Variant", "Backend", "Scene", "Coupling", "Workers"))
        Index[Key][(Row["Benchmark"], Row["Tail"])] = Row
    Output = []
    for Key, Records in sorted(Index.items()):
        Engine = Records.get(("engine", "cold_process"))
        if Engine is None:
            continue
        for Tail in ("cold", "warm"):
            Provider = Records.get(("provider", Tail))
            if Provider is None:
                continue
            Output.append(dict(zip(("Variant", "Backend", "Scene", "Coupling", "Workers"), Key)) | {
                "ProviderTail": Tail,
                "ProviderTotalP50Ms": Provider["MedianProcessP50Ms"],
                "ProviderTotalP95Ms": Provider["MedianProcessP95Ms"],
                "ProviderTotalP99Ms": Provider["MedianProcessP99Ms"],
                "EnginePhysicsCallP50Ms": Engine["MedianProcessP50Ms"],
                "EnginePhysicsCallP95Ms": Engine["MedianProcessP95Ms"],
                "EnginePhysicsCallP99Ms": Engine["MedianProcessP99Ms"],
                "BoundaryNote": "Different executables and measurement boundaries; do not subtract as Engine overhead",
            })
    return Output


def WriteCsv(Path: pathlib.Path, Rows: list[dict]) -> None:
    Fields = sorted({Key for Row in Rows for Key in Row})
    with Path.open("w", newline="", encoding="utf-8") as Stream:
        Writer = csv.DictWriter(Stream, fieldnames=Fields)
        Writer.writeheader()
        Writer.writerows(Rows)


def Main() -> None:
    Parser = argparse.ArgumentParser(description=__doc__)
    Parser.add_argument("--root", type=pathlib.Path, required=True)
    Parser.add_argument("--inputs", nargs="+", required=True)
    Parser.add_argument("--output", type=pathlib.Path)
    Args = Parser.parse_args()
    Root = Args.root.resolve()
    Output = (Args.output or pathlib.Path(__file__).parent).resolve()
    Output.mkdir(parents=True, exist_ok=True)
    Rows: list[dict] = []
    Stats: list[dict] = []
    Sources = []
    InputQuality = {}
    for Name in Args.inputs:
        Folder = (Root / Name).resolve()
        Require(Folder.is_dir() and Folder.parent == Root, f"{Name}: input must be a direct child of root")
        if (Folder / "Receipts.json").exists():
            Rows.extend(ReceiptRows(Folder))
        else:
            Rows.extend(GroupTimingRows(Folder))
        Stats.extend(GroupStatsRows(Folder))
        Sources.append(Name)
        SummaryPath = Folder / "Summary.json"
        if SummaryPath.exists():
            InputSummary = json.loads(SummaryPath.read_text())
            InputQuality[Name] = {
                "QualificationPass": InputSummary.get("QualificationPass"),
                "QualityEnvelopeProvided": InputSummary.get("QualityEnvelopeProvided"),
                "TimingWinner": InputSummary.get("TimingWinner"),
            }
        else:
            InputQuality[Name] = {"QualificationPass": None,
                                  "QualityEnvelopeProvided": None, "TimingWinner": None}
    Require(Rows, "no timing rows")
    AggregateRows = Aggregates(Rows)
    PairedRows = Paired(Rows)
    ComparisonRows = ComparisonSummary(PairedRows)
    ScalingRows = WorkerScaling(AggregateRows)
    NativeEngineRows = NativeEngineTable(AggregateRows)
    for Name, Records in (("Rows", Rows), ("Aggregates", AggregateRows),
                          ("Paired", PairedRows), ("ComparisonSummary", ComparisonRows),
                          ("WorkerScaling", ScalingRows), ("NativeEngine", NativeEngineRows),
                          ("GroupStats", Stats)):
        WriteCsv(Output / f"{Name}.csv", Records)
        (Output / f"{Name}.json").write_text(json.dumps(Records, indent=2) + "\n", encoding="utf-8")
    Summary = {
        "Schema": "FinalPerformanceReduction1",
        "Inputs": Sources,
        "ProcessRows": len(Rows),
        "AggregateRows": len(AggregateRows),
        "PairedRows": len(PairedRows),
        "ComparisonRows": len(ComparisonRows),
        "WorkerScalingRows": len(ScalingRows),
        "NativeEngineRows": len(NativeEngineRows),
        "GroupStatsRows": len(Stats),
        "InputQuality": InputQuality,
        "SourceMetadata": {Name: json.loads((Root / Name / "Source.json").read_text())
                           for Name in Sources if (Root / Name / "Source.json").exists()},
        "TimingValidation": "Provider marker/header/summary/lifecycle false; Engine CMake external and native profiling OFF",
        "NativeProfile": PROFILE,
        "QualityWinnerClaim": None,
        "QualityLimit": "No cross-backend or overall B/C winner without the predeclared physical envelope; paired deltas are timing diagnostics",
        "PairingBoundary": "Matrix B/C rows align scene/cap/tail/repeat across separate A/B and A/C cohorts; only Group and Final-Guards B/C rows are direct same-folder counterbalanced comparisons",
        "WorkingSetLimit": "Peak working set is process-lifetime, not a teardown leak or phase-local peak",
        "CpuLimit": "Run CPU spans admission-end through run-end, including warmup and quality/logging; do not divide by measured-frame total. Whole-process CPU divided by receipt wall is only a conservative approximate average occupied-core count, not worker utilization",
    }
    (Output / "Summary.json").write_text(json.dumps(Summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(Summary, indent=2))


if __name__ == "__main__":
    Main()

