# Selective coupling scheduling experiment

## Single-world zero-soft cadence

The experimental unified provider has a bounded dynamic fast path within its
single authoritative Jolt world. At each rigid `Step`, it uses U1/C4 if no
enabled soft body, sensor, or live rigid-soft attachment can require the
coupled U2/C8 cadence. An enabled soft admission or reenable switches to
U2/C8 before its first physical step; successful disable or retirement can
restore U1/C4 on the next step. Failed soft updates leave membership
unchanged. All rigid bodies, constraints, contacts, IDs, and moving colliders
stay in the same native world at every cadence. The sensor guard preserves
the half-frame neutral begin/end observation of transient sensor contacts.
The private `SchedulingQualification` checks membership transitions and the
sensor refusal; `SchedulingBenchmark` compares U1/C4 with a same-binary
forced-U2/C8 control for a gravity-loaded 128-rigid contact scene. This fast
path makes no claim that active soft bodies can be partitioned or advanced
at a lower cadence.


`SelectiveCouplingBenchmark.cpp` is a bounded implementation of split world
cadence using Jolt's existing `PhysicsSystem::Update` and `JobSystem` interfaces.
It is not a general dynamic island scheduler. The optimized class has an
authored collision partition: isolated rigid layer 3 cannot collide with
coupled rigid layer 1 or soft layer 2; isolated static layer 4 is subject to
the same exclusion. Both global and selective modes use
exactly the same pair filter. The scheduler refuses cross-partition joints,
soft influence, or any pair eligible for contact; near and moving-toward
spatial controls also refuse before the first update. All rigid bodies linked
by a joint to the coupled sphere stay in the coupled world. No body is frozen
or duplicated.

The global mode puts all bodies in one Jolt world and advances the entire
world twice per frame with eight collision steps per update. The selective
mode advances the coupled world at the same cadence and a separate isolated
rigid world once per frame with four collision steps. The same 12 soft
integrations per collision step yield 192 soft integrations per frame in both
modes. This preserves the qualified *schedule*, not by itself the complete
qualified contact fixture or energy proof. The test reports a rolling hash
of coupled rigid position, rotation, linear/angular velocity, soft world
transform, and every soft vertex local position/velocity at every frame
endpoint, isolated rigid penetration and speed across all frames, and step,
guard, and setup timings, plus per-frame update P50/P95. Qualification should require coupled trajectory
identity or diagnose any difference, plus physical quality gates for the
isolated scene. Its different rigid cadence legitimately changes isolated
trajectories. The existing internal-observation qualification remains the
authority for the corrected numerical solver's contact and energy gates.
`--mode refusal` checks the admission rules without a Jolt world;
`refusal-near`, `refusal-crossing`, and `refusal-joint` construct or request
unsafe scenes and must return `RefusedBeforeUpdate=true` before any step.
The benchmark accepts `--velocity-reconstruction difference|delta`. A
maintained Jolt build defaults to `delta`; a stock build defaults to and
accepts only `difference`. The existing constraint-family admission helper
is applied before creating the soft body.
`--layout sparse` retains the first 128-active-body screen. `--layout dense`
adds one isolated static floor and a gravity-driven rigid stack. Both worlds
use the same gravity vector in dense mode; the coupled soft and rigid bodies
set gravity factor zero, preserving their original free/coupled fixture.
The floor is created once, within its owning partition. The initial sphere
spacing is 0.91 m for 0.45 m radii and the lowest row begins 0.10 m above the
floor, so the authored scene does not begin with a large overlap. Per-frame
pair and floor penetration must each stay at or below 0.25 m, with an
initial-state gate before stepping. A rigid contact listener confirms
isolated pair/floor contact in at least a quarter of dense frames; its
callbacks are counted separately from the soft contact listener. Sparse and dense runs are separate
quality/cost comparisons; do not pool their timings.

Jolt source anchors at v5.6.0 `e77f175595e64cb44218cc9d9d56fc365ad0e36a`:

- `PhysicsSystem::Update` creates the broadphase, contact, island, rigid
  solve/integrate, and soft prepare/simulate/finalize jobs for every collision
  step (`Jolt/Physics/PhysicsSystem.cpp:176-640`). Its `inCollisionSteps`
  divides the update's timestep uniformly; there is no public per-island
  cadence argument.
- `JobApplyGravity` reads all rigid bodies active at step start
  (`PhysicsSystem.cpp:746-789`), and `JobFindCollisions` consumes the active
  rigid list (`:893-970`). Sleeping or temporarily deactivating unrelated
  bodies would change contacts, wakeups, gravity and feedback.
- `JobSoftBodyPrepare` enumerates active soft bodies, sorts them, allocates
  contexts and spawns collide/simulate/finalize jobs each collision step
  (`:2701-2780`). `JobSoftBodyFinalize` applies accumulated rigid velocity
  deltas (`:2842-2910`). The coupled world retains this entire path.
- The original numerical audit selected U2/C8/I12 after W1/C8/I24 failed a
  corrected speed-10 fixture despite equal 192 soft integrations; the audit
  explicitly did not prove 16 rigid advances are a minimum.

The two worlds share one bounded Jolt job pool and one 32 MiB temporary arena.
They step sequentially. This avoids a second worker pool and reserve, while
two `PhysicsSystem` instances still have separate broadphase, contact, body,
and constraint storage. No world shares a body pointer, constraint, or native
contact object with the other. The parent must keep this prototype behind
the experimental build boundary; no Engine semantic ID or production factory
is involved.

## Profiling and cost comparison

`ExternalProfile.cpp` implements the static-library `JPH_EXTERNAL_PROFILE`
measurement hooks. Build a separate profiled Jolt library with
`JPH_USE_EXTERNAL_PROFILE=ON` and `PROFILER_IN_DEBUG_AND_RELEASE=ON` in Release;
link both optimization sources into the benchmark. The ordinary timing binary
should use the same compiler profile with external profiling disabled. The
profiled binary emits per-stage inclusive nanoseconds and scope counts. Nested
and concurrent scopes overlap, so their sum is neither wall time nor the
critical path. Use it to identify broadphase, narrowphase, soft simulation,
job graph and barrier work before changing Jolt internals. Run the global
baseline first.

Use fresh processes and identical hardware, compiler flags, scene body count,
worker budget and frame count for global/selective comparisons. Compare
`TotalUpdateMs + GuardMs`, and report `SceneSetupMs` separately. Sample per
frame tails through `UpdateP50Ms`, `UpdateP95Ms`, and `GuardP95Ms`.
Working-set snapshots are taken before Jolt initialization and with live
worlds after the run; the peak is process-lifetime, not a teardown leak check.
The benchmark inspects every isolated rigid pose, velocity and pair separation
after every frame, and emits finite, speed, penetration and pass flags. The
coupled finite and maximum edge-stretch screen uses the existing 0.1 stretch
ceiling as a necessary frame-endpoint bound; it does not cover internal
integrations, pin drift, contact penetration or energy on its own. A
soft-contact callback must occur so the timing scene actually exercises
coupling. The callback count is event evidence, not an impulse or contact
projection count. The
`RunSelectiveQualification.py` runner starts fresh stock and patched processes
at worker budgets 1/2/8, with four default repeats and counterbalanced
global/selective and stock/patched launch order. It rejects any benchmark quality
failure, any global/selective coupled trajectory hash mismatch, and any
repeat or cross-worker coupled hash drift before reporting paired costs.
Pass `--output-dir` to retain every process's command, binary hash, elapsed
time, exit code, stdout and stderr, with `Receipts.json` updated after each
attempt. Stdout and stderr are retained as captured bytes with SHA-256 digests.
A stock physical failure remains recorded and unranked while the
maintained solver continues. `Summary.json` contains a speedup only for a
backend whose benchmark physical and hash gates pass for every worker/repeat.
Stock difference and maintained delta are separate physical configurations;
the runner makes no cross-backend equivalence or full internal energy claim.
At `N` isolated active rigids, the expected cost is roughly

`T_selective = T_coupled(U2/C8) + T_isolated(U1/C4) + T_guard`

against `T_global = T_coupled+isolated(U2/C8)`. The difference must exceed
the second world's setup and lifecycle overhead for the use case to be
economically useful. Profiled timings cannot be compared directly against
unprofiled timings because the atomics and clock reads add work.

This is a special semantic partition. It does not permit lower cadence for
spatially distant ordinary collidable bodies, a body that may contact a soft
body, or a joint-connected body. General selective island cadence would need
a source-level scheduler that accounts for newly forming contacts, wakeups,
contact persistence, CCD and state transfer; this experiment makes no such
claim. The prior rigid U1/C4 versus U2/C8 timing ratio is a cost signal,
not evidence of quality equivalence for isolated bodies.
