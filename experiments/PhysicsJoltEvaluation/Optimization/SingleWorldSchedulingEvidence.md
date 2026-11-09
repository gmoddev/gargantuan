---
status: experimental-measured
authority: non-normative
date: 2026-11-09
---

# Single-world rigid cadence evidence

## Frozen C candidate

The final unprofiled 1024-group cohort used Engine
`f4ffd911baab53ebee82a3d3d62e040568d17d7e` and Gantria Jolt
`7e59df5fd7f3e23504f2e696e2b095bf706b26b4`. The same 128-sphere,
gravity-loaded executable ran fast U1/C4 and forced full U2/C8 for 240 frames,
three fresh processes per mode at each worker cap. Its bodies all had
`CanTouch=false`; it contained no enabled soft body or joint. The cohort
predates the live-joint fast-cadence guard, but has zero live joints, so that
guard does not change its selected schedule. Postguard qualification is a
separate source gate and was pending when this reduction was written.

`ReduceSchedulingBenchmark.py` validated all 18 raw stdout/stderr digests,
exit codes, empty stderr, source receipt, CMake feature and profiling flags,
command identity, one executable path, finite state, native contact, and the
unchanged 0.25 m floor/pair penetration gates. The source and CMakeCache
SHA-256 digests were `ebdee4d08babd288983a92b0ab9ebf4f3d54a431e19a632382ada784fe89a345`
and `4e5fb2ffa46141ea89c9c03e2237a564af39bf2622a0520790e1b726ebc4cdd4`.
All checks passed. Times below measure provider rigid `Step`; each latency
percentile and maximum is the median of three per-process values. Speedup is
the median of the three matched full/fast process ratios.

| Worker cap | Fast / full ms per 240 frames | Median paired speedup | Fast P50 / P95 / P99 / max ms | Full P50 / P95 / P99 / max ms |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 231.55 / 790.40 | 3.38× | 0.938 / 1.215 / 1.327 / 1.346 | 3.254 / 3.694 / 3.994 / 4.304 |
| 2 | 250.39 / 838.18 | 3.35× | 0.932 / 1.737 / 2.034 / 2.083 | 3.218 / 6.556 / 7.545 / 7.639 |
| 8 | 505.66 / 1920.54 | 3.81× | 2.108 / 2.406 / 2.499 / 2.688 | 8.035 / 9.020 / 9.187 / 9.505 |

Every run reported native contact in 240/240 frames. Worst fast floor/pair
penetration was 0.01402/0.01673 m; forced full was 0.000292/0.000410 m.
Both met the 0.25 m gate. Different cadence changes trajectory and contact
penetration, so these receipts make no contact-event or trajectory-equivalence
claim. The raw cohort and validated `SchedulingReduction.json` are at
`C:/Users/aiden/.codex/artifacts/jolt-final-performance-20261109/C-Schedule-20261109T070737`.

## Earlier C256 smoke

The local C256 smoke cohort used Engine
`935bccf1bc82055abcc3bc00780b576029bf560d`, Gantria Jolt
`8ba1408477826f3a86f54e59b4c17518a0ef147f`, the unprofiled MSVC
Release build, and the 256-vertex large-cloth grouping setting. This is a
pre-final source checkpoint. The 128-sphere gravity-loaded fixture has no
enabled soft body, touch-enabled rigid body, or live joint; physical native contact remains
enabled. `fast` advances the same Jolt world at U1/C4, while `full` forces
U2/C8 in the same benchmark executable. Each process ran 240 frames.

`ReduceSchedulingBenchmark.py` checked all 18 raw stdout/stderr digests
against receipts, exit codes, empty stderr, parsed source and CMake flags,
command identity, one executable path, finite state, actual native contact,
and floor/pair penetration at most 0.25 m. All checks passed. The process
median times below measure provider rigid `Step` and exclude the subsequent
quality inspection. Speedup is the median of three matched full/fast process
ratios at each worker count. The frame P95 values are medians of the three
per-process P95 values.

| Worker cap | Fast / full ms per 240 frames | Median paired speedup | Fast / full frame P95 ms |
| ---: | ---: | ---: | ---: |
| 1 | 229.09 / 800.93 | 3.50× | 1.186 / 3.875 |
| 2 | 241.59 / 892.53 | 3.82× | 1.933 / 6.699 |
| 8 | 510.02 / 1909.79 | 3.68× | 2.451 / 8.997 |

All 18 runs reported native contact pairs in 240/240 frames. The worst
observed fast floor/pair penetration was 0.01402/0.01673 m; the full control
was 0.000292/0.000410 m. Both schedules met the unchanged 0.25 m physical
gate. Their rigid trajectories and penetration differ because cadence differs;
the result does not establish contact-event or trajectory identity. The
eligibility guard keeps touch-enabled bodies, all enabled soft bodies, and any
live joint at the qualified U2/C8 cadence, so this speedup applies only to
explicitly non-touch rigid content with no enabled soft load or live joint.

These smoke receipts contain aggregate update time and per-process frame P95,
but no raw frame times. P50, P99 and maximum frame latency cannot be recovered
from them. The benchmark now emits those fields for subsequent fresh-process
qualification. The full raw cohort and `SchedulingReduction.json` are at
`C:/Users/aiden/.codex/artifacts/jolt-final-performance-20261109/C256-Smoke-20261109T064632`.
