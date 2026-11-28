# Ordinary Workspace slow-step attribution

The unchanged historical (`284443879`) and current (`d28a2ad74`) Windows Editors expose all five cumulative native timing phases in the same response. A recording wrapper around the fixed comparison recovered the components of each individual whole step. The 24-Part scene also runs the Engine's default character, locomotion and camera scripts. The capture included 32 fresh processes in eight balanced four-process blocks, 192 warmup ticks and 384 enabled-window ticks per process. There were 12,227 individually timed ticks, 61 ticks in multi-step polls and two ticks during counter resets. The existing process affinity, normal priority, scene and RPC sequence were preserved. [Fixed protocol](SpatialOrdinaryPrecision.md) · [Shipped process receipts](SpatialOrdinaryAttribution/Released/Processes.csv) · [Raw aligned timings](SpatialOrdinaryAttribution/Released/Polls.csv.gz)

For each process we sorted intact observations by whole-step duration and simulation tick. Each table entry compares the component times on those **same** ranked steps. The last column covers whole steps at and above the nearest-rank 95th percentile. Figures are current minus historical, in microseconds, with two-sided 95% paired block intervals. The phase contributions add to the whole-step value per observation. `Other` comprises the remaining work timed by the inclusive `Engine::Step` scope. [Complete paired calculations](SpatialOrdinaryAttribution/Released/Comparisons.csv)

| Native time | All individual steps | 94th–96th whole-step ranks | Slowest 5% of whole steps |
| --- | ---: | ---: | ---: |
| Whole Engine step | +8.39 (−1.00 to +17.77) | +30.31 (+8.09 to +52.54) | +31.38 (+14.37 to +48.40) |
| PreSimulation | +5.07 (−0.60 to +10.74) | +19.82 (−2.05 to +41.68) | +24.01 (+5.87 to +42.14) |
| Physics | +1.47 (+0.91 to +2.03) | +7.64 (+4.41 to +10.86) | +3.85 (+0.84 to +6.86) |
| Animation | −0.08 (−0.21 to +0.06) | −0.37 (−0.85 to +0.10) | −0.21 (−0.72 to +0.29) |
| Render publication | +2.12 (+0.95 to +3.29) | +0.24 (−4.75 to +5.22) | +4.88 (−0.82 to +10.58) |
| Other | −0.19 (−2.82 to +2.44) | +2.99 (−5.59 to +11.57) | −1.14 (−9.11 to +6.82) |

The direct p95 whole-step difference was +31.04 µs (+7.66 to +54.42). Native phase values come from single-tick count/total deltas. The separate `MainCyclesBetweenReplies` field includes EditorHost RPC processing and sampler cadence. The cycle delta cannot be charged exclusively to the Engine step.

## SDL package control

Both SDL3 DLLs report version 3.4.12 and have equal file size. Their PE `.text` sections differ in 127 of 1,917,440 bytes. Two private current-runtime closures had 35 byte-identical files including the executable and differed only in `SDL3.dll`. Six balanced blocks, 256 timed-window ticks per process, gave a whole-step p95 difference of +24.73 µs (−26.06 to +75.53) with current SDL. The corresponding 94th–96th band difference was +21.97 µs (−27.40 to +71.35). The interval includes zero and changes larger than the stock matched difference. This control leaves the package-level SDL contribution unresolved. [SDL process receipts](SpatialOrdinaryAttribution/SDL/Processes.csv) · [Raw SDL control](SpatialOrdinaryAttribution/SDL/Polls.csv.gz) · [Paired control analysis](SpatialOrdinaryAttribution/SDL/Comparisons.csv)

## Reproduction

The capture and analysis use the existing historical and current Windows Release packages and Python's standard library. Acquisition requires the same two-cache-group layout as the fixed benchmark. Choose a fresh destination. For the SDL control, use the current product on both sides, varying its single matching-major-version SDL library. The DLL SHA-256 values are recorded in the fixed protocol.

```powershell
python -B tests/SpatialConstructionStepProfile.py --baseline $Historical --current $Current --output $Raw --blocks 8 --frames 384 --warmup 192
python -B tests/SpatialConstructionStepAttribution.py --input $Raw --output $ComparisonCsv
```
