# Ordinary Workspace whole-step qualification after runtime changes

The optimized Windows Engine's whole-step p95 increased **1.14%** in the primary 24-Part scene. Its one-sided 95% upper limit was **+2.65%** and rose to **+3.28%** under the prespecified missing-tick bound. The primary result leaves the proposed 2% ceiling unresolved. The scene with six additional moving rigid bodies met the ceiling under both calculations. Each result applies to its own workload on this controller. [Matched block results](SpatialOrdinaryOptimized/Blocks.csv) · [Full estimates](SpatialOrdinaryOptimized/Summary.json)

| Ordinary Workspace scene | Matched blocks | Paired p95 change | Two-sided 95% interval | One-sided 95% upper | Upper allowing all missing ticks |
| --- | ---: | ---: | ---: | ---: | ---: |
| 24 anchored Parts | 64 | +1.14% | −0.63% to +2.95% | +2.65% | +3.28% |
| 24 anchored Parts and 6 moving bodies | 16 | −0.25% | −2.34% to +1.90% | +1.51% | +1.68% |

The paired absolute p95 change was +4.13 µs (95% interval −2.47 to +10.73 µs) in the primary scene and −0.83 µs (−12.45 to +10.78 µs) in the moving scene. Across the primary processes the median p95 was 389.85 µs for historical and 392.15 µs for optimized. The equivalent moving-scene medians were 556.05 and 559.45 µs. These four values summarize each arm separately. The paired estimates use the order-matched blocks. [Per-process observations](SpatialOrdinaryOptimized/Processes.csv)

The unmodified acquisition sampled 261,892 of 262,148 primary steps individually and 49,132 of 49,152 moving-scene steps individually. In the primary runs, 118 historical and 134 optimized steps shared a poll with another step. Respectively one and three more steps occurred while resetting the counter. In the moving runs, 14 historical and six optimized steps shared a poll, and neither arm lost a step during reset. Constraining grouped durations to their measured sums gives supplementary one-sided upper bounds of +2.86% and +1.48%. A separate NumPy analysis reconstructed the per-process ranks, both intervals, missing-tick bounds and native duration sums from the retained observations. [Individual and grouped steps](SpatialOrdinaryOptimized/Ticks.csv.gz) · [Independent audit output](SpatialOrdinaryOptimized/IndependentAudit.json)

An earlier comparison against the historical executable measured +4.41% in the primary scene for the preceding stock build. The earlier and optimized studies use separate process blocks. The native experiments traced costs directly in the implementation and measured lower physics time and fewer C++ allocations after optimization. [Earlier qualification](SpatialOrdinaryPrecision.md) · [Matched native instrumentation and implementation comparisons](SpatialOrdinaryNativeCounters.md)

## Fixed acquisition and recovery

The Windows comparison uses the original historical EditorHost at `284443879` and the normal optimized CMake Release target at `4af419f00`. Their executable SHA-256 values are `0f43758828929e453d054689e81524a662e75c9e26d8ec4d4a4089396795d4aa` and `a62153f7657927d20e9a9d8bd2ffc6c8b0b7575b4afd736dd6a38b0324e7afce`. All 13 adjacent runtime files are identical. The candidate's 35-file distribution manifest verified. [Product and dependency hashes](SpatialOrdinaryAttribution/Products.csv)

Each process runs the normal headless EditorHost Play clock with its ordinary character, locomotion and camera scripts. The primary scene contains 24 authored anchored Parts. The sensitivity scene adds six moving rigid bodies with periodic velocity edits. The cumulative `EditorPlayInclusiveWallTimeV1` timer measures work inside the simulation's inclusive `Engine::Step` on Main. Window polling, between-step sleeping and GPU presentation fall outside its boundaries. The same normal-priority Ryzen 9 7950X3D controller ran both arms. Engine affinity was logical CPUs 16–31 in one last-level cache group, Main used CPU 24 and the sampler used CPU 8 in another group. [Original scene, estimator and timing contract](SpatialOrdinaryPrecision.md)

The fixed sample comprises 64 four-process blocks of 1,024 enabled-window steps for the primary scene and 16 blocks of 768 steps for the moving scene, with 192 warmup ticks per process. Every block uses the original seeded ABBA/BAAB order. Each process contributes a nearest-rank p95. The analysis averages the two observations per arm within each block and uses matched block log-ratios as the uncertainty units for percentage changes and Student intervals. The proposed 2% criterion uses the one-sided 95% upper limit and the originally specified all-short/all-long missing-tick sensitivity. Separately reported group-sum bounds use the exact recorded nanoseconds of each multi-step poll. The original acquisition, observation and scene-generator source files match the earlier study. [Acquisition](../../tests/SpatialConstructionPrecision.py) · [Fixed analysis](../../tests/SpatialConstructionPrecisionAnalysis.py) · [Independent NumPy audit](../../tests/SpatialOrdinaryIndependentAudit.py)

The primary raw acquisition opened at 03:37:18.687452 UTC on November 28. After block 37, historical block 38 run 1 failed during atomic project creation, before Play, warmup or any timing sample. The raw prefix closed at 04:06:08.535729 UTC. The volume had 369,852,993,536 bytes free on inspection. The native error identifies a filesystem exception during atomic project setup. Its specific operating-system cause was unavailable. A fresh temporary project root on the same volume entered the original next position at 04:08:01.678054 UTC. The fixed acquisition measured all 108 remaining primary processes by 04:29:01.939659 UTC and all 64 moving processes by 04:39:07.365327 UTC. Every original summary field, group sum and ordered individual duration from the preceding 148 processes matched the completed primary record. The recovery packet retains the original files plus separate hashes for both the prefix and suffix. [Recovery receipt and UTC events](SpatialOrdinaryOptimized/Recovery/Receipt.json) · [Original and resumed observations](SpatialOrdinaryOptimized/Recovery/RawBlocks.zip) · [Recovery procedure](../../tests/SpatialOrdinaryResume.py)

## Reproduction

Use each executable's complete adjacent runtime on the same topology. The acquisition destinations must be fresh. Run the two scenes sequentially. The archive verifier can be run independently from the retained raw data.

```powershell
python -B tests/SpatialConstructionPrecision.py --baseline $Historical --current $Candidate --output $Primary --scene ordinary --blocks 64 --frames 1024 --warmup 192
python -B tests/SpatialConstructionPrecision.py --baseline $Historical --current $Candidate --output $Moving --scene rigid --blocks 16 --frames 768 --warmup 192
python -B tests/SpatialConstructionPrecisionAnalysis.py --input $Primary --input $Moving --output $Archive --candidate-sha256 $CandidateSha256
python -B tests/SpatialConstructionPrecisionAnalysis.py --verify $Archive --candidate-sha256 $CandidateSha256
python -B tests/SpatialOrdinaryIndependentAudit.py --input $Archive --candidate-sha256 $CandidateSha256
```
