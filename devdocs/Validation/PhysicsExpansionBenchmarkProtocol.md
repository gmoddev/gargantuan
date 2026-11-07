# Physics expansion after-only benchmark protocol

`gargantuan_physics_expansion_benchmark` exercises the integrated Engine
`WorldRoot` API on a Release build. It emits one JSON header and one row per
case. Use `--quick` as a smoke check, then run the default sample counts.
`--case=advanced-queries`, `--case=moving-body-ccd`, and
`--case=hinge-slider-motors` select bounded case groups. Every row requires
observed behavior and resource retirement; a failing gate exits nonzero.

| Case | Fixed workload | Behavioral gate |
| --- | --- | --- |
| `advanced-queries` | Two coincident anchored boxes, 128 casts and capped overlaps per sphere, capsule, and box | Stable nearest semantic hit, distance and unit normal, deterministic one-result truncation, rejection above the 256-result cap, no hits after destruction |
| `moving-body-ccd-off/on` | 32 one-step trials each, dynamic 0.5-unit ball at −3 moving +360 units/s against a dynamic 0.1-unit wall moving −2 units/s | Effective authored CCD flag, finite published pose, exact body retirement; all CCD-on balls stop before x=1 |
| `hinge-slider-motors` | One authored motorized door hinge and one slider, 120 fixed 1/60-second steps | Both move within their limits, then all four bodies and two joints retire |

`Step`, `Cast`, and `OverlapLimit1` give measured P50/P95/P99/max wall time.
The CCD cases have identical geometry, speed, timestep, and body admission;
the off case reports its observed stopped count, touch events, and mean final
position without assuming it provides the same collision quality. Cases run
off then on, so their small timing difference remains sensitive to cache and
scheduler effects. Repeat the whole pair on the same machine and compare
distribution and behavior together.

Resident bytes are process working set on Windows or RSS on Linux. They include
allocator retention and unrelated process state; they do not isolate Box3D or
WorldRoot allocations. `AllocationCount` is `null` because no scoped allocator
hook is available. Body/joint retirement is verified through the WorldRoot
maps and reported as exact counts, independent of process RSS.

These APIs did not exist in the pre-feature source. A before column for casts,
overlaps, moving-body CCD, or advanced authored joints is `unsupported`, with
timing and observed outcome set to `null`; zero milliseconds would be false
evidence. Keep the earlier activation, ray, mesh, soft-body, and baseline
compatible foundation workloads separate. Record source hash, compiler and
build settings, machine, raw JSON, and fixture version with every comparison.
