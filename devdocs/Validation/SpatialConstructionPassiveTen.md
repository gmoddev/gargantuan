# Ten-trial passive frame comparison

The Windows x64 EditorHost comparison did not establish the proposed 2% passive p95 limit. The recent ordinary scene had a paired whole-Engine-step p95 change of **−3.51%** with a 95% confidence interval from **−15.70% to +10.45%**. Its p99 change was −3.42% (−20.28% to +17.02%). Both intervals include zero. Creating an empty domain and grid consistently cost time in the physics/camera portion of the tick.

Each trial created a project and launched a fresh headless EditorHost Play process on the same Ryzen 9 7950X3D Windows 10.0.26300 host. A script created 24 anchored Parts. The third configuration added one empty SpatialRegion with an empty BlockGrid. The historical Engine was `284443879`, SHA-256 `0f43758828929e453d054689e81524a662e75c9e26d8ec4d4a4089396795d4aa`. The recent Engine was `b7e0685bf`, SHA-256 `eddc5fb3a33ed4761e9ddcf899683f0b7e240ec3f2c5488b981ff595141b06d4`. Order rotated across ten blocks. One Engine process ran at a time. The three measurements below ran sequentially.

| Measurement, µs | Historical ordinary | Recent ordinary | Recent empty domain and grid |
| --- | ---: | ---: | ---: |
| Simulation portion p50 / p95 / p99 | 7.40 / 16.95 / 26.60 | 7.60 / 17.45 / 26.40 | 11.20 / 23.45 / 33.45 |
| Inclusive Engine step p50 / p95 / p99 | 101.25 / 177.30 / 239.60 | 95.55 / 174.90 / 219.95 | 104.40 / 179.55 / 226.20 |

The table reports medians of the ten per-process percentiles. Each paired difference below uses two runs from the same trial. Intervals use the mean difference ± 2.262 times its standard error across ten pairs. Percentage intervals apply the same calculation to log ratios before converting back to percentages. Ticks within a process do not count as independent trials.

| Paired comparison | Mean difference in p95, µs (95% CI) | Mean difference in p99, µs (95% CI) |
| --- | ---: | ---: |
| Recent ordinary minus historical, simulation portion | +0.62 (−0.56 to +1.80) | −0.54 (−2.85 to +1.77) |
| Recent empty domain/grid minus recent ordinary, simulation portion | +5.71 (+4.55 to +6.87) | +8.40 (+5.51 to +11.29) |
| Recent ordinary minus historical, inclusive Engine step | −7.34 (−31.50 to +16.82) | −10.99 (−55.45 to +33.47) |
| Recent empty domain/grid minus recent ordinary, inclusive Engine step | +4.80 (−12.00 to +21.60) | +5.93 (−24.95 to +36.81) |

The existing script observer timed PreSimulation to PostSimulation with diagnostics disabled. This includes world physics, camera stepping and callback dispatch. It excludes the rest of Main. Each process contributed 1,024 ticks after 64 warmup ticks, for 30,720 observations. The local performance counter had 0.1 µs resolution. [Simulation trials](SpatialConstructionPassiveTen.csv)

The second run used the native `EngineStep` counter from `EditorPlayInclusiveWallTimeV1` for the same script and scene. It includes work inside headless `Engine::Step`, including script, physics and render publication. IPC, the EditorHost event loop, desktop presentation and GPU completion are outside the interval. Every recorded observation was a positive change in total nanoseconds accompanied by **exactly one** new sample and checked against the rolling maximum. After at least 64 warmup steps, the driver collected 192 individual steps per process. Four reset windows per process kept the 60-slot counter from wrapping. All 5,760 samples qualified, with zero skipped multi-step intervals. The CSV retains each raw nanosecond value. [Whole-step trials](SpatialConstructionWholeStepTen.csv)

The observer stored 60 × 5 native 64-bit time values per active Play session and timed work scopes inside each step. The driver made 14,784 local snapshot requests during the measured windows. Across processes, median request p50/p95 was 194/324 µs outside the timed Engine call. A separate ten-pair calibration used 192 script-timed ordinary ticks per process and alternated native diagnostics on/off without polling the native counters. Enabling diagnostics changed subphase p95 by −0.11 µs (95% CI −1.66 to +1.44). This bounds the observed effect within that subphase of single-machine headless Play under the normal scheduler. The inclusive timer requires diagnostics to be enabled. [Observer calibration](SpatialConstructionDiagnosticCost.csv)

The proposed compatibility gate also calls for zero additional per-Part state without opt-in. Each current BasePart contains a nullable SpatialDomainMember binding pointer. Only an explicit assignment allocates the binding object. Timing alone cannot establish the other structural, mutation, serialization and network parts of the gate. The empty domain/grid increased simulation p95 in every paired trial. The Engine currently steps each attached region's native physics world. A candidate fast path would skip empty rigid and soft backend steps after servicing pending terrain, grid, reservation and retirement work, then wake on the first pending simulation change. This experiment did not qualify such a change. [SpatialDomainMember](../../include/gargantuan/runtime/SpatialDomainMember.hpp) · [world stepping](../../src/classes/WorldRoot.cpp)

## Reproduction

Use each executable with its retained adjacent runtime files. Run the drivers sequentially from the repository and choose fresh CSV paths.

```powershell
python -B tests/SpatialConstructionOrdinaryFrames.py --engine $Recent --baseline $Historical --trials 10 --output $PhaseCsv
python -B tests/SpatialConstructionWholeStep.py --engine $Recent --baseline $Historical --trials 10 --frames 192 --output $StepCsv
python -B tests/SpatialConstructionWholeStep.py --engine $Recent --baseline $Historical --trials 10 --frames 192 --observer-cost --output $CostCsv
```
