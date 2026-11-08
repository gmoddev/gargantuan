# Bounded soft-body provider benchmark

`PhysicsJoltProviderBenchmark` takes five required positional arguments and
one optional receipt identifier:

```
PhysicsJoltProviderBenchmark jolt|xpbd cloth4k|cloth16k|cloth4x4k|rubber4k|rubber16k none|static-sphere frames workers [shared-run-pair]
```

Frames must be 1–30 and requested workers 1–8. The scenes contain 64×64 or
128×128 cloth, four 64×64 cloth bodies, 16³ rubber, or 25³ rubber. They use
the same `SoftBodyDefinition`, particle mass, compliance, gravity, fixed top-row
cloth attachments, and central uniform impulse in both backends. The impulse
is `movable vertex count × particle mass × 2`, corresponding to a nominal
2 m/s lateral velocity before damping and constraint response. Each measured
frame is 1/60 second. Setup and body admission are outside timed frames.
The first measured frame is cold after admission. Per-frame output is printed
outside its own timer but can perturb following frames. Treat a single run as
a bounded cold provider-call sample; alternate and repeat runs on one host
before making a steady-state claim. Pass the same nondefault `shared-run-pair`
identifier to both backend runs to make a comparison eligible for ranking.
Host, OS, source translation-unit build stamp, and a checksum of the running
benchmark executable are printed in the receipt. The checksum is a comparison
fingerprint, not an authentication hash; preserve the actual binary hash in
the external runner's evidence as well.
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

The executable prints per-frame `jolt_world_ms`, `jolt_extract_ms`,
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
Admission checks exact vertex and attachment counts and requires a nonempty
constraint graph. Every measured frame must report all bodies and vertices
simulated, no frozen, stale, dropped, or truncated work, and advancing vertex
revisions with stable identities and topology.
`CompareBenchmark.py jolt.log xpbd.log` emits absolute differences for those
physical metrics and leaves `timing_winner` null. Supplying a reviewed JSON
envelope with `--envelope limits.json` permits a timing winner only when every
relevant absolute error is within its named threshold and both requested
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
It also withholds a timing winner unless host, OS, build stamp, executable
fingerprint, and explicit run-pair identifier match. Pool capacity and dispatched jobs are both reported:
XPBD submits one job per body, so one-body scenes cannot occupy its whole pool.
That limited parallelism is an actual backend scaling property under the same
CPU budget, not a reason to erase a measured timing difference.

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
whole Engine frame time. The worker argument configures Jolt only; XPBD uses
the production default pool and reports its actual worker/job counts. Use
those fields when comparing costs.
