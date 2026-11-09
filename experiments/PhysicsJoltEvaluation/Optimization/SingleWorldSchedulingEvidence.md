---
status: experimental-measured
authority: non-normative
date: 2026-11-09
---

# Single-world rigid cadence smoke reduction

The local C256 smoke cohort used Engine
`935bccf1bc82055abcc3bc00780b576029bf560d`, Gantria Jolt
`8ba1408477826f3a86f54e59b4c17518a0ef147f`, the unprofiled MSVC
Release build, and the 256-vertex large-cloth grouping setting. This is a
pre-final source checkpoint. The 128-sphere gravity-loaded fixture has no
enabled soft body or touch-enabled rigid body; physical native contact remains
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
eligibility guard keeps touch-enabled bodies and all enabled soft bodies at
the qualified U2/C8 cadence, so this speedup applies only to explicitly
non-touch rigid content with no enabled soft load.

These smoke receipts contain aggregate update time and per-process frame P95,
but no raw frame times. P50, P99 and maximum frame latency cannot be recovered
from them. The benchmark now emits those fields for subsequent fresh-process
qualification. The full raw cohort and `SchedulingReduction.json` are at
`C:/Users/aiden/.codex/artifacts/jolt-final-performance-20261109/C256-Smoke-20261109T064632`.
