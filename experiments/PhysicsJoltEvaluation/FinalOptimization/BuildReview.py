"""Build a readable evidence table from ReducePerformance.py's checked JSON."""

from __future__ import annotations

import argparse
import json
import pathlib


Parser = argparse.ArgumentParser(description=__doc__)
Parser.add_argument("--input", type=pathlib.Path, default=pathlib.Path(__file__).parent,
                    help="directory containing checked reduction JSON; Review.md is written there")
Args = Parser.parse_args()
HERE = Args.input.resolve()
SUMMARY = json.loads((HERE / "Summary.json").read_text())
COMPARE = json.loads((HERE / "ComparisonSummary.json").read_text())
SCALING = json.loads((HERE / "WorkerScaling.json").read_text())
NATIVE_ENGINE = json.loads((HERE / "NativeEngine.json").read_text())
AGGREGATES = json.loads((HERE / "Aggregates.json").read_text())
GROUPS = json.loads((HERE / "GroupStats.json").read_text())
PAIRED = json.loads((HERE / "Paired.json").read_text())


def F(Value, Digits=2):
    if Value is None:
        return "—"
    return f"{Value:,.{Digits}f}"


def Percent(Value):
    return "—" if Value is None else f"{Value:+.2f}%"


def State(Value):
    return "unreported" if Value is None else "same" if Value else "changed"


def Table(Headings, Rows):
    return "\n".join([
        "| " + " | ".join(Headings) + " |",
        "| " + " | ".join("---" for _ in Headings) + " |",
        *("| " + " | ".join(str(Cell) for Cell in Row) + " |" for Row in Rows),
    ])


def Match(**Fields):
    return [Row for Row in COMPARE if all(Row.get(Key) == Value for Key, Value in Fields.items())]


def ComparisonRow(Row):
    return [Row["Scene"], "W" + str(Row["Workers"]), Row["Tail"], Row["Pairs"],
            F(Row["MedianBaselineP50Ms"]), F(Row["MedianCandidateP50Ms"]),
            Percent(Row["MedianMatchedDeltaP50Percent"]),
            F(Row["MedianBaselineP95Ms"]), F(Row["MedianCandidateP95Ms"]),
            F(Row["MedianBaselineP99Ms"]), F(Row["MedianCandidateP99Ms"]),
            State(Row["AllPairsExactTerminalState"])]


HEAD = ["Scene", "Cap", "Tail", "Pairs", "B P50 ms", "C P50 ms", "Δ P50",
        "B P95", "C P95", "B P99", "C P99", "State"]

LINES = [
    "# Final physics timing reduction",
    "",
    f"Validated {SUMMARY['ProcessRows']} uninstrumented process receipts, "
    f"{SUMMARY['PairedRows']} comparison rows, and {SUMMARY['GroupStatsRows']} separately "
    "instrumented group-stat rows. Native Jolt runs were checked for U2/C8/I5/N80/Q2 "
    "and the full-update sleep clock. Provider markers and captured Engine CMake "
    "flags show timing profiling off. P50/P95/P99 below come from measured frames "
    "only; process comparisons use the median across separate processes. "
    "Matrix B/C comparisons align the same scene, cap, tail, and repeat across "
    "separate A/B and A/C cohorts; they are not directly counterbalanced B/C pairs. "
    "Group and Final-Guards trials are direct same-folder B/C pairs. "
    "Negative Δ means C took less time.",
    "",
    "## 16K cloth, native Provider timing",
    "",
    Table(HEAD, [ComparisonRow(Row) for Row in sorted(
        Match(Scope="matrix", Benchmark="provider", Backend="jolt", Scene="cloth16k"),
        key=lambda Row: (Row["Workers"], Row["Tail"]))]),
    "",
    "The target eight-worker cloth improves in both cold and warm aligned comparisons. "
    "One-worker cloth slows in both tails. Its changed state hash is expected from "
    "the opt-in 1024-vertex grouping and must be judged by the physical envelope.",
    "",
    "## 16K cloth, full Engine physics-call timing",
    "",
    Table(HEAD, [ComparisonRow(Row) for Row in sorted(
        Match(Scope="matrix", Benchmark="engine", Backend="jolt", Scene="cloth16k"),
        key=lambda Row: Row["Workers"]) ]),
    "",
    "Engine receipts do not report a trajectory hash; `unreported` is not a mismatch. "
    "Provider total and Engine physics-call timing have different executable and "
    "measurement boundaries. Their difference is not an Engine overhead estimate.",
    "",
    "## Native Jolt across the other scenes",
    "",
    Table(HEAD, [ComparisonRow(Row) for Row in sorted(
        Match(Scope="matrix", Benchmark="provider", Backend="jolt"),
        key=lambda Row: (Row["Scene"], Row["Workers"], Row["Tail"]))
        if Row["Scene"] != "cloth16k"]),
    "",
    "## XPBD (A) timing control",
    "",
    Table(HEAD, [ComparisonRow(Row) for Row in sorted(
        Match(Scope="matrix", Benchmark="provider", Backend="xpbd"),
        key=lambda Row: (Row["Scene"], Row["Workers"], Row["Tail"]))]),
    "",
    "XPBD is shown as a timing control. The Provider suites explicitly mark "
    "`QualityEnvelopeProvided=false`; this table does not rank XPBD against Jolt.",
    "",
    "## XPBD (A), full Engine physics-call timing",
    "",
    Table(HEAD, [ComparisonRow(Row) for Row in sorted(
        Match(Scope="matrix", Benchmark="engine", Backend="xpbd"),
        key=lambda Row: (Row["Scene"], Row["Workers"]))]),
    "",
    "## Native Provider versus full Engine boundaries",
    "",
    Table(["Source", "Scene", "Cap", "Provider tail", "Provider total P50 ms",
           "Engine physics-call P50 ms"], [
        [Row["Variant"], Row["Scene"], "W" + str(Row["Workers"]), Row["ProviderTail"],
         F(Row["ProviderTotalP50Ms"]), F(Row["EnginePhysicsCallP50Ms"])]
        for Row in sorted(NATIVE_ENGINE, key=lambda Row: (Row["Scene"], Row["Workers"],
                                                      Row["ProviderTail"], Row["Variant"]))
        if Row["Backend"] == "jolt" and Row["ProviderTail"] == "cold"]),
    "",
    "## Worker-cap scaling, native Jolt warm runs",
    "",
    Table(["Source", "Scene", "W1 P50 ms", "W8 P50 ms", "W1/W8 speed ratio"], [
        [Row["Variant"], Row["Scene"], F(Row["Worker1P50Ms"]),
         F(Row["CappedP50Ms"]), F(Row["Worker1ToCapSpeedRatio"])]
        for Row in sorted(SCALING, key=lambda Row: (Row["Scene"], Row["Variant"]))
        if Row["Benchmark"] == "provider" and Row["Backend"] == "jolt"
        and Row["Tail"] == "warm" and Row["Workers"] == 8]),
    "",
    "These are process P50 speed ratios between worker caps, not CPU utilization.",
    "",
    "## Admission, retirement, CPU, and working set (full matrix cohorts)",
    "",
    Table(["Source", "Scene", "Cap", "Tail", "Admission ms", "Teardown ms",
           "Run CPU s", "Whole CPU s", "Receipt wall s", "Approx cores lower bound",
           "Peak WS MiB", "Post-retire WS MiB"], [
        [Row["Variant"], Row["Scene"], "W" + str(Row["Workers"]), Row["Tail"],
         F(Row["MedianAdmissionMs"]), F(Row["MedianTeardownMs"]),
         F(Row["MedianRunCpuSeconds"], 3), F(Row["MedianWholeProcessCpuSeconds"], 3),
         F(Row["MedianProcessElapsedSeconds"], 3), F(Row["MedianApproxAverageOccupiedCoresLowerBound"], 2),
         F(Row["MedianProcessLifetimePeakWorkingSetBytes"] / (1024 * 1024))
         if Row["MedianProcessLifetimePeakWorkingSetBytes"] is not None else "—",
         F(Row["MedianPostRetireWorkingSetBytes"] / (1024 * 1024))
         if Row["MedianPostRetireWorkingSetBytes"] is not None else "—"]
        for Row in sorted(AGGREGATES, key=lambda Row: (Row["Scene"], Row["Workers"], Row["Variant"]))
        if Row["Folder"].startswith(("B-Provider", "C-Provider"))
        and Row["Benchmark"] == "provider" and Row["Backend"] == "jolt"
        and Row["Scene"] in ("cloth16k", "rubber4k") and Row["Workers"] in (1, 8)
        and Row["Tail"] == "warm"]),
    "",
    "`Run CPU` spans admission-end through run-end, including warmup and quality/logging. "
    "It is not measured-frame CPU utilization. `Approx cores lower bound` uses "
    "captured whole-process CPU divided by receipt wall time, which includes more "
    "startup/shutdown than the CPU scope. Peak working set is process-lifetime "
    "and does not prove a phase-local peak or leak. Exact receipt wall seconds are "
    "in `Rows.csv/json`.",
    "",
    "## Isolated experiments",
    "",
    Table(["Scope", "Scene", "Cap", "Pairs", "Control P50 ms", "Candidate P50 ms",
           "Δ P50", "Range Δ", "All faster", "State"], [
        [Row["Scope"].split("-2026")[0], Row["Scene"], "W" + str(Row["Workers"]),
         Row["Pairs"], F(Row["MedianBaselineP50Ms"]), F(Row["MedianCandidateP50Ms"]),
         Percent(Row["MedianMatchedDeltaP50Percent"]),
         Percent(Row["MinMatchedDeltaP50Percent"]) + " to " + Percent(Row["MaxMatchedDeltaP50Percent"]),
         "yes" if Row["AllPairsFaster"] else "no", State(Row["AllPairsExactTerminalState"])]
        for Row in sorted(COMPARE, key=lambda Row: (Row["Scope"], Row["Scene"], Row["Workers"]))
        if Row["Scope"].startswith(("Group", "Lazy", "Final-"))]),
    "",
    "### Final guarded candidate, direct B/C pairs",
    "",
    Table(["Repeat", "B P50 ms", "C P50 ms", "Δ P50", "B P95 ms", "C P95 ms",
           "B P99 ms", "C P99 ms", "State"], [
        [Row["PairKey"], F(Row["BP50Ms"]), F(Row["CP50Ms"]),
         Percent(Row["DeltaP50Percent"]), F(Row["BP95Ms"]), F(Row["CP95Ms"]),
         F(Row["BP99Ms"]), F(Row["CP99Ms"]), State(Row["ExactTerminalState"])]
        for Row in sorted(PAIRED, key=lambda Row: Row["PairKey"])
        if Row["Scope"].startswith("Final-")]),
    "",
    Table(["Group trial", "Update groups", "Parallel edges", "Serial edges"], [
        [Name, Record["UpdateGroups"], Record["ParallelEdges"], Record["NonParallelEdges"]]
        for Name, Record in sorted({Row["Folder"].split("-2026")[0]: Row for Row in GROUPS}.items())]),
    "",
    "`Lazy-Trial` isolates lambda clearing at group size 256. The compiled "
    "1024-group setting in `Lazy-Rubber-Confirmation` applies only to cloth with "
    "at least 16K vertices; rubber4k uses the original 256-group path in both "
    "binaries. Its five pairs therefore test the lazy-clear source on rubber "
    "without a group-order change; the signs remain mixed. Group512/1024 cloth "
    "trials change trajectory hashes. The `C-Long` physical evidence is separate "
    "and must decide whether that changed trajectory is acceptable.",
    "",
    "## Observed regressions and decision boundary",
    "",
]

Regressions = [Row for Row in COMPARE if Row["Scope"] == "matrix"
               and Row["Backend"] == "jolt" and Row["MinMatchedDeltaP50Percent"] > 0]
LINES.append(Table(["Benchmark", "Scene", "Cap", "Tail", "Aligned Δ P50 range"], [
    [Row["Benchmark"], Row["Scene"], "W" + str(Row["Workers"]), Row["Tail"],
     Percent(Row["MinMatchedDeltaP50Percent"]) + " to " + Percent(Row["MaxMatchedDeltaP50Percent"])]
    for Row in sorted(Regressions, key=lambda Row: (Row["Benchmark"], Row["Scene"],
                                                   Row["Workers"], Row["Tail"]))]))
LINES.extend([
    "",
    "A positive range means every aligned same-cell/repeat comparison in that small timing sample was slower; "
    "it is not a statistical confidence interval. The 1024-group candidate's "
    "16K-cloth W8 result is substantial, while single-worker cloth and some "
    "small-scene configurations regress. The full physical quality envelope, "
    "including preserved negative controls, is required before any adoption or "
    "cross-backend winner. All per-process P50/P95/P99, CPU/working-set, admission, "
    "retirement, hashes, and receipt identifiers remain in the CSV and JSON files. "
    "Separately instrumented native scopes and exact 40-frame prefix checks "
    "are in `ProfileComparison.md/json`; those scopes do not rank frame timing.",
    "",
])

(HERE / "Review.md").write_text("\n".join(LINES), encoding="utf-8")
print(HERE / "Review.md")

