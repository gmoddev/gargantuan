# Bounded soft-body provider benchmark

See [FeatureMatrix.md](FeatureMatrix.md) for qualified Engine and provider
behavior; benchmark observations here do not expand that support scope.

`PhysicsJoltProviderBenchmark` takes five required positional arguments,
followed by optional receipt identifier, warmup count, XPBD reference
substep count, and quality trace path:

```
PhysicsJoltProviderBenchmark jolt|xpbd cloth4k|cloth16k|cloth4x4k|rubber4k|rubber16k none|static-sphere|driven-sphere frames workers [shared-run-pair [warmup-frames [xpbd-substeps [quality-trace-path]]]]
```

Measured frames must be 1–30, warmup frames 0–30, and requested workers 1–8.
Omitting the optional arguments preserves the old cold, unpaired invocation;
the seven-argument form with `shared-run-pair` also remains valid and cold.
The scenes contain 64×64 or
128×128 cloth, four 64×64 cloth bodies, 16³ rubber, or 25³ rubber. They use
the same `SoftBodyDefinition`, particle mass, compliance, gravity, fixed top-row
cloth attachments, and central uniform impulse in both backends. The impulse
is `movable vertex count × particle mass × 2`, corresponding to a nominal
2 m/s lateral velocity before damping and constraint response. Each measured
frame is 1/60 second. Setup and body admission are outside timed frames and
reported as `startup_admission_ms`. Warmup advances the same scene, applies
the same complete-body, revision, and no-frozen-work checks, and excludes its
step times from measured mean, median, and p95. With zero warmup, the first
measured frame is cold after admission. Per-frame output is printed
outside its own timer but can perturb following frames. Treat a single run as
a bounded cold provider-call sample; alternate and repeat runs on one host
before making a steady-state claim. Pass the same nondefault `shared-run-pair`
identifier to both backend runs to make a comparison eligible for ranking.
Host, OS, source translation-unit build stamp, and a checksum of the running
benchmark executable are printed in the receipt. The checksum is a comparison
fingerprint, not an authentication hash; preserve the actual binary hash in
the external runner's evidence as well.
Jolt rows also print the compiled world-update, collision-step, soft temporal
iteration, constraint-sweep, and soft-integration counts in both header and
summary; XPBD rows report zero for those Jolt-only fields. The runner checks
their agreement and `integrations = updates × collision steps × temporal
iterations`, so an optimized solver build cannot silently inherit the legacy
U2/C8/I12 label.
The XPBD constructor treats the worker argument as a maximum and may use fewer
threads when the host has fewer cores; `xpbd_workers_actual` records the
backend's step profile count. Jolt's argument is its configured total budget
(main thread plus native workers). Compare actual budgets before drawing any
scaling conclusion.

`none` isolates the soft workload. Jolt still spends time in its shared native
world and reports that as `jolt_world_ms`; its `jolt_extract_ms` is publication
after the combined solve. XPBD reports its complete soft step as
`xpbd_step_ms`. `static-sphere` adds one anchored rigid sphere per body. XPBD
receives the same sphere geometry in its neutral collider snapshot; Jolt owns
it in the native world. The sphere starts with a positive 0.2 m surface gap
ahead of the common +X impulse; the benchmark records minimum surface
separation and maximum poststep penetration across frames. Compare coupled
and isolated cost separately for each backend. This scene does not compare
reciprocal dynamic rigid feedback.

`driven-sphere` is a separate experiment with the same positive initial gap.
Its authored sphere center moves 1.2 m left over 0.66 seconds, sampled at
each Jolt frame boundary or XPBD reference substep boundary. Jolt updates an
anchored native body and enables a private soft contact listener only for this
scene; the receipt requires at least one actual contacting vertex. That
listener makes the Jolt driven row timing-instrumented. XPBD receives matching
moving collider snapshots but has no native contact callback in this harness.
The runner therefore requires a matching `none` control for each backend,
scene, worker budget, cold/warm tail, and repeat, then finds the first frame
where their full soft-position trajectory hashes differ. The control plus
positive contact band supports a bounded response observation, not reciprocal
dynamic-rigid feedback. `static-sphere` remains its distinct, unmodified
scene and its prior non-contact observations remain labeled as such. No driven
row receives a timing winner.

The ninth positional argument selects an **experimental XPBD reference** with
4, 8, 16, or 32 complete `Soft->Step` calls per neutral frame. One means the
unchanged production `Quality=Automatic` step. Reference mode sets
`Quality=High` and maps the default 0.04 frame damping to
`1 - (1 - 0.04)^(1 / substeps)` on each substep; the initial impulse is applied
only once. Every substep is checked for complete, finite, nonfrozen work and
advancing body revisions. The measured frame timer contains all substep calls
and those validation checks. A separate unpinned, zero-gravity, free-cloth
control runs after benchmark teardown and requires the ratio of its second to
first frame displacements to match 0.96 within 0.002. The receipt prints the
control result, total measured XPBD substeps, dispatched jobs, quality mode,
damping mode, and actual substep count; Jolt records zero actual XPBD substeps.
The control tests free-velocity retention, not gravity, collision, or material
equivalence. Reference timing is never a production XPBD timing rank or a
claim of equal native solver budget.

The optional tenth argument writes a quality-only binary position trace after
each frame timer. It records every warmup and measured frame in body and vertex
order as little-endian float32 XYZ, after a 32-byte header: magic `PJTRACE1`,
version 1, body count, vertices per body, total frame count, warmup frame
count, and float32 frame interval. Both header and summary label
`quality_trace=1` and `timing_instrumented=1`; traced time is not rankable.
The trace compares the same indexed topology without asserting equal materials.

The executable prints warmup frames as `warmup_frame=N` and measured frames as
`frame=N`, with per-frame `jolt_world_ms`, `jolt_extract_ms`,
`xpbd_step_ms`, and `total_ms`, followed
by mean, median, and p95 total time. Its final state metrics are mean and
center vertex height, maximum attachment error, mean and maximum axial-neighbor
strain, rubber axis-aligned bounding-box and tetrahedral volume ratios, maximum
tetra volume error, and mean cell-diagonal strain. It also prints actual
constraint counts, XPBD jobs dispatched, and worker utilization. Use these
physical measurements before making any performance ranking. The benchmark
deliberately prints no winner. This is a provider-call microbenchmark: it
excludes `WorldRoot`'s collider snapshot, Part event/publication work, render
dirty bookkeeping, and outer Engine frame work.

After the final quality inspection, the benchmark explicitly destroys every
soft body and every authored Jolt static collider, then releases the soft
provider before the shared rigid provider. A `lifecycle` JSON line reports
provider setup/admission time, teardown time, and resource deltas from before
admission to live admission, after the run, and after provider retirement.
Working set snapshots show live process residency; process-lifetime peaks are
not phase peaks or leak proof. Allocation counters exist only in the separate
instrumented allocation target. Its `timing_instrumented=1` marks all frame
times invalid for performance comparison.
Admission checks exact vertex and attachment counts and requires a nonempty
constraint graph. Every warmup and measured frame must report all bodies and
vertices simulated, no frozen, stale, dropped, or truncated work, finite
published positions, and advancing vertex revisions with stable identities
and topology. Reference substep validation is included in its frame timer.
`CompareBenchmark.py jolt.log xpbd.log` emits absolute differences for those
physical metrics and leaves `timing_winner` null. Supplying a reviewed JSON
envelope with `--envelope limits.json` permits a timing winner only when every
relevant pair difference is within its named threshold, each backend's value
is within a reviewed `absolute_ranges` interval for every relevant metric,
and both requested
worker counts equal the XPBD profile's actual worker count. The envelope requires
`mean_y`, `center_y`, `max_pin_error`, `mean_edge_strain`, and
`max_edge_strain`; rubber also requires `mean_volume_ratio`. These thresholds
are in world units for heights and pin error, and dimensionless for strain
and volume ratios. Rubber also requires `mean_tetra_volume_ratio`,
`max_tetra_volume_error`, and `mean_diagonal_strain`, yet the helper still
withholds a rubber timing winner because Jolt's added cell diagonals replace
XPBD's global shape recovery. A coupled envelope additionally requires
`max_poststep_penetration`, `final_min_surface_distance`, and an absolute
`maximum_allowed_penetration`; both runs must reach the contact band after
starting with a positive gap. The band and penetration metrics are geometric
proxies; they do not prove a solver contact was generated. The helper withholds
a coupled timing winner until contact callbacks or a separate trajectory
control establish actual response. The comparison rejects mismatched scenes,
coupling choices, or frame counts.
For example, an envelope must include `"absolute_ranges": {"max_pin_error":
[0, 0.05], ...}` with a finite two-sided range for every reported quality
field. The ranges are scene-specific physical acceptance limits chosen before
examining the paired timing result. Pair differences alone cannot establish
that either material stayed within a usable shape envelope.
It also withholds a timing winner unless host, OS, build stamp, executable
fingerprint, explicit run-pair identifier, and warmup frame count match, and
both logs report uninstrumented timing. Pool capacity and dispatched jobs are
both reported:
XPBD submits one job per body, so one-body scenes cannot occupy its whole pool.
That limited parallelism is an actual backend scaling property under the same
CPU budget, not a reason to erase a measured timing difference.

`RunPerformanceQualification.py` executes fresh-process Jolt/XPBD pairs for
both cold and ten-frame-warm tails. Defaults are five scenes, both coupling
choices, worker budgets 1/2/8, 30 measured frames, and three repeats. It
alternates backend order across pairs and retains byte-exact stdout/stderr,
SHA-256 hashes, command lines, elapsed time, exit status, source and binary
identities, the CMake cache, and incremental receipts before parsing or
comparison. A failed or timed-out process remains in the output directory and
ends the matrix with an error. Use `--scenes`, `--couplings`, `--workers`,
`--tails`, and `--repetitions` for a smaller bounded matrix. For example:

```
python RunPerformanceQualification.py --binary PhysicsJoltProviderBenchmark --cmake-cache CMakeCache.txt --engine-source ENGINE_COMMIT --jolt-source JOLT_COMMIT --output ProviderEvidence --scenes cloth4k --couplings none --workers 1
```

The runner invokes `CompareBenchmark.py` for each matched pair and preserves
its raw comparison output. Without `--envelope` every pair remains unranked.
Both performance runners impose a total 900-second wall-clock deadline by
default, independently of the per-process timeout. Set
`--total-timeout-seconds` within 60–7200 for a bounded shard. Before each
native or comparator launch, the runner passes the smaller remaining deadline
and per-process timeout to `subprocess.run`; an expired deadline stops launches,
records `QualificationPass=false`, and retains an incremental failed summary.
With an envelope, the comparator can rank only when its material, topology,
coupling, worker, and physical requirements pass. Cold and warm tails are
reported separately; no ratio is pooled across scenes or treated as an Engine
frame result. `PhysicsJoltEngineBenchmark` uses the same scene and coupling
arguments but has a separate timing scope and does not take a run-pair argument.
`--xpbd-substeps` selects the reference mode in the external runner and is
recorded on both sides of every pair. The comparator withholds a timing winner
for all reference rows, even if an envelope is supplied.
`--quality-trace` writes and hashes a separate raw trace for each process,
including partial files from failed attempts. The runner checks exact byte
coverage and header identity, then reports RMS and maximum point displacement
for each frame plus peak and final values in the pair receipt. Use a small
trace matrix, such as one 16K cloth scene, one worker, 30 measured frames,
10 warmup frames, and one repeat. Trace runs remain timing-instrumented and
unranked; run fresh pairs without the flag for timing after reviewing curves.
Request `--couplings none driven-sphere` to run the driven response control;
the runner refuses driven-only matrices.

The native damping coefficient is calibrated for the qualified two half-frame
updates, eight collision steps per half, and 12 Jolt soft integrations per
collision step. XPBD multiplies velocity by `1 - Damping` once per neutral
frame. Jolt applies `max(0, 1 - rate × dt/192)` 192 times; the provider sets
`rate = -expm1(log1p(-Damping)/192) × 192/dt` for the actual frame duration.
This matches free-velocity retention at the frame boundary. XPBD applies
gravity and authored force before one damping multiplication; Jolt interleaves
acceleration and damping across its native integrations, so forced trajectories
can still differ. Both adapters distribute point impulses to four nearest
movable nodes by inverse distance, though XPBD queues the operation until its
step and Jolt applies it to current native velocities. Only the common
uniform-impulse operator is used in this benchmark.

Cloth stretch and bend edges share neutral topology and compliance values,
but the solvers and correction order differ. XPBD `Quality=Automatic` selects
eight iterations for 4K bodies and four for 16K bodies; Jolt uses the fixed
qualified 12 integrations per collision step (192 per neutral frame). Rubber
uses Jolt cell diagonals as an approximation of XPBD shape matching. A timing
advantage is
not evidence of equivalent material quality when sag, strain, attachment, or
volume measurements disagree. The observed error envelope and any accepted
quality threshold must be recorded alongside a benchmark result.

`PhysicsJoltEngineBenchmark` uses the same scene names and argument order,
but constructs a real `DataModel` and authored `Cloth` or `RubberBody` Instances,
top-row `SoftBodyAttachment` pins for cloth, and optional anchored sphere
Parts. It uses production Box3D/XPBD through the default `WorldRoot` for
`xpbd`, and the experimental provider through the existing test friend seam
for `jolt`. Its timed region is one `WorldRoot::StepPhysics(1/60)` call, so it
includes pending-input application, rigid step, motion/event publication,
`BuildSoftBodyColliders()` on both backends, soft step, and deformable state
publication. It excludes game loop scheduling, rendering, networking, and
setup/admission. `physics_step_wall_ms` is therefore a physics-call cost, not
whole Engine frame time. The worker argument caps both backends through the
existing test friend seam before admission; XPBD is still created by the
production factory and reports its actual worker/job counts. Use the actual
counts when comparing costs. The Engine summary separately reports `setup_ms`,
`semantic_destroy_ms`, and `owner_release_ms`; teardown calls
`Game::Destroy()`, checks that the `WorldRoot` identity maps are empty, then
releases authored owners and shared `World`/`DataModel` references. After each
timed physics call and before the next frame, the harness checks finite
vertices and axial edges,
stable body identity/revisions, and no frozen or dropped work. It reports
`mean_y`, `center_y`, `max_pin_error`, `peak_pin_error`,
`mean_edge_strain`, `max_edge_strain`, `peak_edge_strain`, and rubber
`mean_volume_ratio` (axis-aligned bounding-box volume). Those checks are
outside the step timer but can affect the next frame's cache state; compare
fresh, counterbalanced processes under the same harness.

`RunEnginePerformanceQualification.py` runs the Engine benchmark in fresh
six-argument processes with cold, 30-frame Jolt/XPBD pairs. Defaults cover the
five scenes, both coupling choices, worker budgets 1/2/8, and three repeats,
with backend order balanced across the matrix. It keeps byte-exact raw logs,
hashes, source and CMake identities, and incremental failure receipts. The
pair file reports each backend's `WorldRoot::StepPhysics` mean, their cost
ratio, and absolute quality-field differences. It always leaves
`TimingWinner` null because these scenes have no reviewed Engine material and
contact equivalence envelope. Its times are Engine physics-call costs and are
never pooled with the provider-call results.
