# Ordinary Workspace whole-step measurement

## Results

The 24-Part ordinary Workspace measured **4.41% higher whole-step p95** in the current shipped Engine. The two-sided 95% interval was **+1.93% to +6.94%**. The one-sided 95% upper bound was **+6.52%**, so this measurement did not establish the proposed 2% limit. The paired absolute change was +21.22 µs (95% +7.95 to +34.49 µs). The two-sided interval includes a 2% increase.

| Ordinary workload | Historical p50 / p95 / p99, µs | Current p50 / p95 / p99, µs | Paired p95 change, 95% interval | One-sided 95% upper |
| --- | ---: | ---: | ---: | ---: |
| 24 anchored Parts, 64 blocks | 193.70 / 479.15 / 625.90 | 205.00 / 501.60 / 654.40 | +4.41% (+1.93% to +6.94%) | +6.52% |
| Same scene plus six moving Parts, 16 blocks | 265.35 / 631.05 / 878.90 | 278.90 / 672.75 / 870.70 | +2.34% (−5.76% to +11.14%) | +9.52% |

The absolute columns give medians of per-process percentiles. In the primary scene the paired p50 change was +3.67% (+1.26% to +6.15%) and p99 was +6.51% (+2.94% to +10.21%). Historical-first and current-first blocks gave primary p95 changes of +4.72% and +4.09%. Grouping four successive blocks yielded +1.96% to +6.90%. The moving-body scene's paired absolute p95 change was +6.91 µs (−66.68 to +80.50 µs). Its order groups gave +7.75% and −2.80%, and its four-block grouped interval was −9.95% to +16.31%. [Paired blocks](SpatialOrdinaryPrecision/Blocks.csv)

The primary two-sided interval spans 5.02 percentage points, compared with 26.15 in the earlier ten-trial study. The same historical binary had median per-process p95 of 177.30 µs in that unpinned study and 479.15 µs here. The matched comparison applies to these binaries, this pinned shared desktop and this workload. [Earlier ten trials](SpatialConstructionPassiveTen.md)

### Sampling coverage

| Workload and binary | Contiguous native ticks | Individually timed | Multi-step polls / omitted ticks | Reset gaps / omitted ticks |
| --- | ---: | ---: | ---: | ---: |
| Anchored, historical | 131,082 | 130,330 | 368 / 742 | 10 / 10 |
| Anchored, current | 131,078 | 130,314 | 374 / 758 | 6 / 6 |
| Moving, historical | 24,579 | 24,390 | 89 / 186 | 2 / 3 |
| Moving, current | 24,578 | 24,434 | 70 / 142 | 2 / 2 |

All 320 scheduled processes completed on their first attempt under the final protocol, each with 192 warmup steps. One historical process in each scene had more than 5% omitted ticks. Placing all missing durations at either extreme therefore left the prespecified one-sided upper sensitivity unbounded in both scenes. Using the recorded native sum of each poll group in the supplementary calculation bounds the primary block-mean change to **+3.75% to +5.55%** with a conservative one-sided 95% upper limit of **+8.02%**. The moving-body figures are +1.42% to +3.40% and +11.20%. Allowing zero-nanosecond durations gives the same figures at the displayed precision. The bounds on the observed block means and the uncertainty limits answer separate questions. [Process receipts](SpatialOrdinaryPrecision/Processes.csv) · [Raw ordered ticks and gaps](SpatialOrdinaryPrecision/Ticks.csv.gz)

The two scenes ran sequentially from 01:15 to 02:16 UTC on November 28, 2026 under CPython 3.12.6, with 594,999 timed-window diagnostic polls and 6,656 counter windows. Median benchmark Engine CPU consumption was 6.92% of one logical processor in the primary scene and 8.34% in the moving-body scene. The phase totals show exploratory primary increases of 5.92 µs per instrumented tick for PreSimulation, 1.36 µs for physics and 1.97 µs for render publication. Determining which code accounts for the p95 change would require durations for those phases on individual slow ticks. The earlier diagnostics-on/off calibration timed the script-driven simulation interval. [Instrument calibration](SpatialConstructionPassiveTen.md)

## Fixed protocol

The historical Windows x64 EditorHost is `284443879`, executable SHA-256 `0f43758828929e453d054689e81524a662e75c9e26d8ec4d4a4089396795d4aa`. The current Windows x64 EditorHost is `d28a2ad74`, executable SHA-256 `ec39cd5036d6569985af9b07f5e8fb48dfc43bab051d8a513b787ec9b34d48b4`. The later `d23849fa4` tree changes qualification documents. Each executable loads its own shipped adjacent runtime. Both binaries must pass the same scene and native counter checks.

The primary scene creates the same 24 anchored Parts and ordinary PreSimulation/PostSimulation observers used in the earlier ten-trial comparison. The original observer's final sort is postponed beyond the capture. There is no spatial opt-in, named region or grid. A secondary scene adds six ordinary dynamic Parts above the anchored tiles. Every 30 ticks a server script sets each body's upward velocity to 36. The startup check requires that each body has physically moved. Both scenes run in normal headless EditorHost Play with persistence disabled.

The primary comparison has **64 symmetric blocks**. Each block launches four fresh processes with historical/current order ABBA or BAAB. Exactly 32 blocks use each orientation, shuffled with seed 20261127. Each process warms the native counter for at least 192 ticks and then targets 1,024 ticks in enabled native counter windows. The separately labeled dynamic sensitivity has 16 balanced blocks of four processes, 192 warmup ticks and 768 enabled-window ticks. Both sample sizes and the analysis below were fixed before running either comparison. The acquisition retains every complete process and block. A native counter inconsistency or saturated ring stops its attempted capture.

The benchmark launches one Engine process at a time, sleeping between ticks of the normal EditorHost Play clock. A Windows 11 Ryzen 9 7950X3D has two OS-reported last-level cache groups. The Engine process can use logical CPUs 16–31 in cache group 16. Its initial thread, which runs EditorHost Main and Play, stays on logical CPU 24. The sampler uses logical CPU 8 in the other cache group. The driver checks the process/thread affinity at both ends of each run and records CPU time and process priority. This is a shared desktop. Other applications use normal scheduling and Windows retains its existing High performance power plan. The controller observed a separate preview Engine during preparation.

The enabled `EditorPlayInclusiveWallTimeV1` native timer surrounds the production Engine step. Each process queries the EngineStep total, maximum and sample count with a 4 ms sleep between queries. The 60-slot window resets before overflow, after 48 observed ticks. An individual duration qualifies when the count increases by precisely one, the total increases and the new maximum agrees. A multi-step observation records its count and total separately. A capture ends at the first observation reaching the attempted count. A final multi-step observation can include a small overshoot. Summed known and grouped durations must equal the native EngineStep total. These timings include physics, script callbacks and Engine render publication. They exclude EditorHost RPC work, the remaining host loop, rendering/presentation in a real Player, and simulation time between calls.

For each process compute the nearest-rank p95. For each block subtract the mean historical log p95 from the mean current log p95. Report the geometric ratio of those block effects, its two-sided 95% paired Student interval, and the one-sided 95% upper bound against 1.02. The independent unit for uncertainty is the four-process block. Also report absolute paired microseconds, p50/p99, the two order groups and a longer-timescale sensitivity grouping four adjacent blocks. The primary result concerns this ordinary scene and this system. The dynamic scene has a separate inferential result.

Missing-step bounds insert all omitted ticks below or above every observed duration within each process. To bound the one-sided interval, form every block's minimum and maximum compatible log ratio, take the mean of its upper endpoints and bound the sample standard deviation using the furthest endpoint of every block from the overall center of the intervals. The 2% criterion requires the upper bound to stay below 1.02 after this sensitivity.

An excluded same-binary method pilot used two four-process blocks of 384 measured ticks to verify the affinity, window boundaries and polling cadence. A second excluded four-process smoke verified six bodies actually moved. Neither pilot compared the historical and current binaries.

### Cumulative tick amendment

At 01:13 UTC on November 28, before calculating any historical/current effect, the first acquisition was stopped after 11 complete ordinary blocks (44 processes, 45,056 attempted native ticks). It preserved each observed group's count and duration but discarded the `SimulationTick` field accompanying each response. That made it impossible to count unobserved ticks during the short intervals between counter windows. All inferential results use the restarted acquisition. The fixed workloads, sample sizes, order, warmup, Engine code, RPC sequence and statistical criterion were preserved. [Superseded receipts](SpatialOrdinaryPrecision/Superseded/)

The final acquisition retains each response's cumulative simulation ordinal. Native ring growth must match the ordinal increase within each window. The difference between the last ordinal in a window and the starting ordinal in the next window counts any reset-gap ticks. `Attempted` spans every tick from the first enabled measurement window's starting ordinal through the last observed ordinal. Its value equals exact ticks plus multi-step-poll ticks plus reset-gap ticks. Each missing-step rank bound includes both sources of missing duration.

### Supplementary use of recorded group sums

After the full primary capture, an additional sensitivity used the recorded nanosecond sum `S` of each group of `k` omitted poll ticks. For a positive integer threshold `x`, positive integer tick durations allow at most `min(k, floor((S−k)/x))` group members above `x`. At least one member exceeds `x` when `S > kx`, otherwise the minimum is zero. Binary search over integer nanoseconds finds the smallest and largest feasible nearest-rank p95 for each process. Reset-gap durations remain unconstrained. The replay also permits zero-nanosecond ticks, replacing the maximum count by `min(k, floor(S/(x+1)))`. The same conservative block uncertainty calculation then applies to these finite per-process bounds. This analysis was added after observing the primary missingness and is reported alongside the prespecified unconstrained result.

## Reproduction

Use the respective retained Windows Release executable and adjacent runtime for each revision. On this Windows 11 Pro build 26300 host the three adjacent OpenSSL and Protobuf DLLs are identical across packages. SDL3 differs, with SHA-256 `26f3c5ae065de7f04016d4849fbeae280d8730945f2778099f84373b02348f1c` historically and `93a5c07a190c8e01352a34894ecbc70d39bc9f1ed5bb7cb81e61a15dcb2c5beb` currently. The comparison measures the complete shipped configurations. Acquisition requires two distinct cache groups and fresh output directories. The affinity arguments default to the controller's topology. Run the two scenes sequentially.

```powershell
python -B tests/SpatialConstructionPrecision.py --current $Current --baseline $Historical --output $Ordinary --scene ordinary --blocks 64 --frames 1024 --warmup 192
python -B tests/SpatialConstructionPrecision.py --current $Current --baseline $Historical --output $Rigid --scene rigid --blocks 16 --frames 768 --warmup 192
python -B tests/SpatialConstructionPrecisionAnalysis.py --input $Ordinary --input $Rigid --output $Receipt
python -B tests/SpatialConstructionPrecisionAnalysis.py --verify devdocs/Validation/SpatialOrdinaryPrecision
```

The analysis validates source hashes, scheduled order, native nanosecond sums, omitted group positions and rank calculations. `Processes.csv` holds the per-process native counters and percentiles. `Blocks.csv` holds the paired effects, with natural-log ratios and arithmetic differences in microseconds. Each `Ticks.csv.gz` row contains exact durations in chronological order, zero-based positions/counts/durations for multi-step polls, and positions/counts for reset-gap ticks. Setup and warmup precede the measurement. The replay requires only Python's standard library and the repository's existing measurement helpers.
