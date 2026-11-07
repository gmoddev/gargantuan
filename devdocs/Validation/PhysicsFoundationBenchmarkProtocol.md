# Physics foundation expansion benchmark protocol

`gargantuan_physics_foundation_benchmark` exercises the Engine `WorldRoot`
safe-point path. It emits one JSON header followed by one JSON row per case.
Run `--quick` for a smoke check, then use the default frame counts for evidence.
`--case=dense-contacts`, `--case=weld-chain`, and `--case=body-churn` select one
case. The header records the fixed 1/60-second input step.

| Case | Workload | Required observed behavior |
| --- | --- | --- |
| `dense-contacts` | 128 dynamic boxes in two close layers over a floor | Finite published positions, no floor penetration, gravity response, body retirement |
| `weld-chain` | 32 boxes connected by 31 welds with one anchored endpoint | Finite published positions, endpoint separation, joint retention, body/joint retirement |
| `body-churn` | 30 cycles of 32 dynamic boxes, one step, and destruction | All 32 bodies admitted and retired on every cycle |

`WorldStep` measures the complete semantic step; `RigidStep` and `BackendStep`
are the existing runtime work-phase samples inside it. Their timings overlap and
must not be added. The resident-memory fields are process working set on
Windows or RSS on Linux. They include allocator retention, shared libraries,
and other process state, so they are only an attribution clue. `AllocationCount`
is `null`: there is no isolated allocator hook for this workload. The soft-body
benchmark separately reports worker utilization, queued bytes, solver work, and
estimated owned storage.

Compare identical source fixtures, compiler settings, build mode, thread
budget, machine, warmup, and measured frame counts. Preserve raw JSON and exact
source/build identifiers. The existing activation, query, mesh-contact, and
soft-body benchmarks supply complementary workload baselines; do not sum their
times or compare them as equivalent scenes. New collision-group update, CCD,
shape-cast, overlap, and advanced-joint capabilities are **unavailable** in a
pre-feature baseline. Record them as `null`/`unsupported` until an actual
operation and its behavioral gate exist; zero milliseconds is not a baseline.

For a future Jolt comparison, keep semantic scene definitions and observed
quality gates equal. Report Engine-owned cross-solver coordination, lifecycle,
diagnostics, and patchset/rebase maintenance separately from frame time. A
working rigid body influenced by a deformable body is required before claiming
bidirectional coupling or consolidation value.
