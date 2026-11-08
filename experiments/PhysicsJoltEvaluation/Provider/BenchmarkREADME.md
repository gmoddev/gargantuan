# Bounded soft-body provider benchmark

`PhysicsJoltProviderBenchmark` takes five positional arguments:

```
PhysicsJoltProviderBenchmark jolt|xpbd cloth4k|cloth16k|cloth4x4k|rubber4k|rubber16k none|static-sphere frames workers
```

Frames must be 1–30 and requested workers 1–8. The scenes contain 64×64 or
128×128 cloth, four 64×64 cloth bodies, 16³ rubber, or 25³ rubber. They use
the same `SoftBodyDefinition`, particle mass, compliance, gravity, fixed top-row
cloth attachments, and central uniform impulse in both backends. The impulse
is `movable vertex count × particle mass × 2`, corresponding to a nominal
2 m/s lateral velocity before damping and constraint response. Each measured
frame is 1/60 second. Setup and body admission are outside timed frames.
The XPBD constructor treats the worker argument as a maximum and may use fewer
threads when the host has fewer cores; `xpbd_workers_actual` records the
backend's step profile count. Jolt's argument is its configured total budget
(main thread plus native workers). Compare actual budgets before drawing any
scaling conclusion.

`none` isolates the soft workload. Jolt still spends time in its shared native
world and reports that as `rigid_ms`; its `soft_ms` is extraction after the
combined solve. `static-sphere` adds one anchored rigid sphere per body. XPBD
receives the same sphere geometry in its neutral collider snapshot; Jolt owns
it in the native world. Compare the coupled and isolated cost separately for
each backend. This scene does not compare reciprocal dynamic rigid feedback.

The executable prints per-frame `rigid_ms`, `soft_ms`, and `total_ms`, followed
by mean, median, and p95 total time. Its final state metrics are mean and
center vertex height, maximum attachment error, mean and maximum axial-neighbor
strain, and rubber axis-aligned bounding-box volume ratio. They expose obvious
material and stability differences; bounding-box volume is not tetrahedral
volume conservation. Use final positions and these metrics before making any
performance ranking. The benchmark deliberately prints no winner.
`CompareBenchmark.py jolt.log xpbd.log` emits absolute differences for those
physical metrics and leaves `timing_winner` null. Supplying a reviewed JSON
envelope with `--envelope limits.json` permits a timing winner only when every
relevant absolute error is within its named threshold and both requested
worker counts equal the XPBD profile's actual worker count. The envelope requires
`mean_y`, `center_y`, `max_pin_error`, `mean_edge_strain`, and
`max_edge_strain`; rubber also requires `mean_volume_ratio`. These thresholds
are in world units for heights and pin error, and dimensionless for strain
and the volume ratio. The comparison rejects mismatched scenes, coupling
choices, or frame counts.

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
shape uses Jolt cell
diagonals as an approximation of XPBD shape matching. A timing advantage is
not evidence of equivalent material quality when sag, strain, attachment, or
volume measurements disagree. The observed error envelope and any accepted
quality threshold must be recorded alongside a benchmark result.
