# Ordinary Workspace native counter scope

The diagnostic binaries retain the Release build's scene, solver, rendering and shipped Luau modules. Independently relinked translation units collect additional evidence around the EditorHost's normal call to `Engine::Step`. The unchanged binaries supply the promotion measurements. [Ordinary scene and fixed acquisition](SpatialOrdinaryPrecision.md) · [Released individual-step phase comparisons](SpatialOrdinaryAttribution.md)

## Historical and stock runtime

The historical and stock native comparisons each used 16 fresh processes. All 12,288 enabled-window ticks reconciled with the released counters, including all 282 ticks in grouped polls. Six additional ticks elapsed during counter resets. Each component uses the steps selected by the enclosing whole-step rank in that process. Values are stock minus historical, in microseconds unless indicated, with two-sided paired-block 95% intervals. [Individual native observations](SpatialOrdinaryAttribution/NativeStock/NativeTicks.zip) · [Collector polls](SpatialOrdinaryAttribution/NativeStock/Polls.csv.gz) · [Paired comparisons](SpatialOrdinaryAttribution/NativeStock/Comparisons.csv)

| Measure | All individually timed steps | 94th–96th whole-step ranks | Slowest 5% of whole steps |
| --- | ---: | ---: | ---: |
| Whole step | +14.02 (−7.50 to +35.54) | +26.45 (+6.16 to +46.73) | +3.32 (−39.27 to +45.91) |
| Character admission outside the world collision query | +5.12 (+4.25 to +5.98) | +9.75 (+7.43 to +12.08) | +10.27 (+9.23 to +11.32) |
| World collision query | +2.11 (−0.41 to +4.63) | −0.21 (−5.58 to +5.16) | +5.62 (+1.08 to +10.16) |
| Script-facing Move outside admission | +0.86 (−0.41 to +2.13) | +0.65 (−1.27 to +2.56) | +2.35 (−0.17 to +4.87) |
| Physics phase | +1.84 (+0.22 to +3.46) | +5.19 (−0.77 to +11.16) | +1.43 (−4.61 to +7.46) |
| Render publication | +2.95 (+0.88 to +5.02) | +11.00 (+4.69 to +17.30) | +8.84 (+0.15 to +17.53) |
| Scheduled cycles, thousands | +66.4 (−30.1 to +162.9) | +118.5 (+27.7 to +209.3) | +149.5 (+51.2 to +247.8) |

Every measured tick executed one script-facing Move, one nested admission and one nested WorldRoot::ResolveKinematicMotion call. The world query includes terrain/domain checks, applying pending physics changes and the native capsule solver. The two “outside” entries subtract the nested call on that exact tick. Current effective-world resolution ran about 1.26 times per tick and accounted for 3.32 µs on average, including any calls made outside character admission. Timing this additional function on stock also adds observer work within its callers. Stock requested 1.33 more C++ allocations (95% +0.34 to +2.31) and 426 more bytes (+309 to +544) per step. Both versions averaged about 18 VM allocation callbacks and 7,024 requested bytes. Main was switched out on 38 historical and 23 stock fully recorded ticks. The worker's existing kernel logger stayed active during this qualification. The Windows HAL exposed generic cache event names on the worker and neither tested process had configured thread hardware counters. [Complete native counts](SpatialOrdinaryAttribution/NativeStock/Comparisons.csv) · [Per-process signed motion counts](SpatialOrdinaryAttribution/NativeStock/Regimes.csv)

Every tick reported one grounded collision at the same returned Y position. Three historical and one stock process returned negative Y velocity on all their timed ticks. Every such return preserved the input velocity bits and reported applied Y of exactly 1.1920928955078125 × 10⁻⁷. The other processes returned zero for both quantities. The shared vendor solver clips velocity against planes that exert push. The shipped script writes the preserved negative velocity and then clamps it to zero, creating two observable setter calls. All processes contribute to the aggregate estimates. [Native return and process values](SpatialOrdinaryAttribution/NativeStock/Regimes.csv) · [Shipped locomotion callback](../../assets/runtime/DefaultLocomotion.luau)

An observer calibration used the identical private historical binary on both arms and changed the observer environment setting. Four balanced blocks of 128 enabled ticks per process produced an average difference of +3.26 µs (−3.07 to +9.59), and a p95 difference of −6.70 µs (−43.84 to +30.44). The interval allows observer effects in either direction at p95. The custom allocation operator and relinked code layout are common to both calibration arms. [Calibration process receipts](SpatialOrdinaryAttribution/Observer/Processes.csv) · [Paired calibration](SpatialOrdinaryAttribution/Observer/Comparisons.csv)

## Stock and optimized runtime

Six balanced blocks compared stock with optimized commit `4af419f00`. There were 24 fresh processes and 6,144 fully reconciled native ticks. Of these, 6,110 were individually observed in the released poll stream. Thirty-four appeared in grouped polls with matching sums. Mean physics fell from 13.06 to 11.17 µs. The paired change was −1.89 µs (95% −3.16 to −0.61) across all individual steps and −7.00 µs (−12.49 to −1.51) in the 94th–96th whole-step rank band. Ordered dynamic readback saved 0.28 µs (0.23 to 0.33) on an average step. Each version dispatched three signals and four script callbacks per tick. [Treatment process records](SpatialOrdinaryAttribution/NativeTreatment/Processes.csv) · [Raw native ticks](SpatialOrdinaryAttribution/NativeTreatment/NativeTicks.zip)

Main requested 4.59 fewer C++ allocations per step (95% 3.66 to 5.51) and 540 fewer bytes (399 to 680). The p95 rank band had 7.01 fewer allocations (1.92 to 12.10). The short cohort's whole-step p95 difference was −2.38 µs (−38.89 to +34.14). Character admission outside the world collision query changed by +0.02 µs (−0.85 to +0.89) on average. All 12 stock processes returned zero grounded velocity, while one of 12 optimized processes consistently preserved its negative input. [Paired timing and allocation results](SpatialOrdinaryAttribution/NativeTreatment/Comparisons.csv) · [Contact by process](SpatialOrdinaryAttribution/NativeTreatment/Regimes.csv)

The separate count-only binaries observed exactly 640 enabled-window steps in four fresh processes per arm. Each row counts actual expression evaluations or loop visits. Separate `weak_ptr::lock` expressions and copying the schema's owning `shared_ptr` have distinct counters. [Raw step counts](SpatialOrdinaryAttribution/ExactOperations/NativeTicks.zip) · [Per-process ranges and totals](SpatialOrdinaryAttribution/ExactOperations/OperationSummary.csv)

| Actual operation over 640 steps per arm | Stock | Optimized |
| --- | ---: | ---: |
| Character admission's explicit assignment walk | 640 | 0 |
| Nodes visited / parent weak locks in explicit membership walks | 1,920 / 1,280 | 0 / 0 |
| Motion comparisons of two world owners | 640 | 0 |
| Prompt domain lookups / indexed cell finds | 162 / 118,098 | 0 / 0 |
| Synchronous registry reads / shared registry owner copies | 38,840 / 38,840 | 30,840 / 0 |
| Eager class-name resolution on a script write | 2,720 | 0 |
| DataModel owner weak-lock attempts in GetDataModel | 12,698 | 7,860 |
| Ordered readback Part visits / recount Part visits | 8,950 / 0 | 0 / 0 |
| Impulse Part visits / transfer-protection lookups | 8,950 / 358 | 8,925 / 0 |
| Collision-asset observer entries / service lookups / change-journal reads | 1,280 / 1,280 / 1,280 | 1,280 / 0 / 0 |
| Terrain edit-work polls | 640 | 0 |
| Incremental render publications / empty grid-map constructions | 620 / 620 | 620 / 0 |
| C++ new calls / bytes around incremental grid-map setup | 2,480 / 634,880 | 0 / 0 |
| C++ new calls during grid-map assignment | 0 | 0 |

Stock grid setup requested **four allocations and 1,024 bytes on each of 620 incremental publications**. The impulse loop still visits 25 Parts per fixed substep on both versions. The two arms accumulated 358 and 357 substeps respectively. The count-only arm observed 2,720 attempted script writes on stock and 2,560 on the optimized executable. Natural contact modes can produce an extra grounded velocity write. These totals describe the sampled scenes. The direct counters and the allocation timing cohort use separate processes. [Complete operation totals](SpatialOrdinaryAttribution/ExactOperations/OperationTotals.csv)

The optimized MSVC object for the stock synchronous schema getter performs a locked increment when acquiring the active owner and a locked exchange-add when releasing it. The final getter jumps to the borrowed-reference getter. Both final functions use ordinary instructions on this read, while the by-value getter still emits the locked increment. This verifies the machine-level operation behind the observed reduction of 38,840 owner copies. The receipts contain the COFF hashes and each disassembled function. [Stock assembly](SpatialOrdinaryAttribution/ExactOperations/SchemaAssembly-Stock.json) · [Final assembly](SpatialOrdinaryAttribution/ExactOperations/SchemaAssembly-Final.json)

Source comparison with `284443879` attributes the explicit-membership walk, prompt domain resolution, transfer guard and incremental BlockGrid map to the spatial feature work. The historical version already used the shared schema owner and eager class names, scanned empty prompt cells and anchored Parts, read collision-asset changes and polled idle terrain. The optimized candidate reduces both the newly introduced work and these older costs. Each measured historical-versus-stock p95 also includes the remaining runtime and packaging differences.

The eight tested products use byte-identical adjacent runtime trees, including all 12 Luau modules and the font. The digest hashes each sorted relative UTF-8 path, a NUL byte, then the file's binary SHA-256 digest. The canonical candidate's full 35-file distribution manifest also verified. Its SDL DLL differs from stock in six file bytes, all in the PE header and debug-directory timestamps. Their code sections are identical. Historical SDL differs from both in 127 of 1,917,440 code-section bytes. [Executable, source, runtime and dependency hashes](SpatialOrdinaryAttribution/Products.csv)

## Independent worker comparison

A Windows Ryzen 9 5900X worker compared the same private historical and final duration binaries directly. Six balanced blocks, 192 warmup ticks and 256 enabled-window ticks per process yielded 6,144 independently verified native steps. The Main thread occupied logical CPU 4 with Engine affinity 0–11 in last-level cache group 0. The sampler occupied CPU 18 in cache group 12. Both arms had two consistently negative grounded returns among 12 fresh processes. No build ran during the capture. [Processor topology](SpatialOrdinaryAttribution/WorkerHistoricalFinal/WorkerCpuSets.json) · [Exact inputs](SpatialOrdinaryAttribution/WorkerHistoricalFinal/Inputs.json) · [Native steps](SpatialOrdinaryAttribution/WorkerHistoricalFinal/NativeTicks.zip)

On that host the instrumented whole-step p95 was 170.20 µs historical and 172.47 µs final, a change of +2.27 µs (95% −23.29 to +27.83). Mean physics fell 0.80 µs (−1.34 to −0.26), dynamic readback fell 0.29 µs (−0.33 to −0.24) and Main requested 3.01 fewer C++ allocations per step (0.33 to 5.69 fewer). Character admission outside the world collision query still took 1.40 µs more per average step (+1.18 to +1.61) and 2.08 µs more around the whole-step p95 rank (+1.70 to +2.47). The final executable resolved the effective world once per tick in this scene. These are paired estimates within the worker. The fixed promotion gate uses the independently acquired normal controller executables. [Paired worker estimates](SpatialOrdinaryAttribution/WorkerHistoricalFinal/Comparisons.csv) · [Contact and scheduling records](SpatialOrdinaryAttribution/WorkerHistoricalFinal/Regimes.csv)

## Instrumentation

| Measurement | Scope and interpretation |
| --- | --- |
| Inclusive and exclusive time | A native work phase uses the runtime's existing steady clock. Exclusive time subtracts timed nested phases. The selected C++ function scopes may overlap these phases and each other. Summing their durations would count some work twice. |
| Scheduled cycles and switches | Two Windows dispatcher snapshots on Engine Main bracket the sampled step. The cycle count excludes interrupted time. The wait-reason bitmap records reasons since the previous read. The brackets also include entering and leaving the native work capture. No frequency conversion is applied. |
| C++ allocations | The linked benchmark allocator counts successful C++ new requests and requested bytes on Engine Main. Allocation totals inside a function or phase include its callees. The separate operation probe brackets grid-map setup and the later assignment individually. |
| Luau allocation | The existing VM callback receives new and resize requests. The observer records event counts, requested bytes, growth and shrink and preserves any prior callback. Some free paths in the pinned VM do not invoke this callback. `lua_totalbytes` before and after each step independently supplies the VM heap change. |
| Character contact | The native script-facing return supplies input velocity, output velocity, requested displacement, applied displacement, resulting position, collision and floor flags. The raw archive preserves the IEEE-754 Y values. Per-process results retain both naturally occurring signs of output velocity. |
| Signals | The engine's opt-in dispatch observer counts script/native callbacks and separates snapshots, invoking callbacks and cleanup. Dispatch profiling adds work, including its own timestamps. |
| Direct operation count | A separate matched set of translation units increments counters at evaluated native expressions and loop bodies. This distinguishes a guard entering, a guard actually fetching its provider, and a scan actually visiting a Part. The count-only binary is a separate experiment. |

Selecting every hardware-counter index on this Windows 11 Ryzen 9 7950X3D controller returned zero configured indices. Across four calibration pairs every 5 ms sleep produced one context switch and 0.17–0.23 million scheduled cycles. Each 1.5 ms busy period produced zero switches and 6.30–6.36 million cycles. This checks that the exposed dispatcher fields distinguish sleeping and running under this short calibration. An unmapped counter supplies no cache-miss measurement. [Raw Windows calibration](SpatialOrdinaryAttribution/ThreadCounterCalibration.csv) · [Windows thread profiling](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-enablethreadprofiling) · [Windows counter fields](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-performance_data)

The profiler buffers samples in memory and writes the native CSV on process shutdown. Each archive contains its original per-process CSV, the external released-counter poll deltas and the binary hash for every process. The verifier checks each native duration against all constituent ticks in every external poll, including multi-step polls. Ranked phase comparisons use the same individually identified steps as the released-counter procedure. Camera, script and physics behavior run normally while the native observers are active.

The precise C++ injection sites are generated from the pinned source and fail if the expected insertion does not match. Compiling a private overlay uses its original translation-unit flags and the existing benchmark allocation probe. The diagnostic evidence reader also checks the native IEEE-754 contact classifications. [Duration overlay](../../tests/PrepareSpatialOrdinaryProfile.py) · [Operation overlay](../../tests/PrepareSpatialOrdinaryOperations.py) · [Evidence verifier](../../tests/SpatialOrdinaryDiagnosticEvidence.py)
