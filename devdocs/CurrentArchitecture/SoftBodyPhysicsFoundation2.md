---
status: current
owner: physics
last_verified: 2026-08-23
related_code:
  - include/gargantuan/physics/SoftBodyTypes.hpp
  - include/gargantuan/physics/SoftBodyBackend.hpp
  - src/physics/SoftBodyBackend.cpp
  - src/physics/XpbdSoftBodyBackend.cpp
  - include/gargantuan/classes/DeformableBody.hpp
  - src/classes/DeformableBody.cpp
  - src/classes/WorldRoot.cpp
  - tests/SoftBodyPhysicsTests.cpp
  - tests/SoftBodyPhysicsBenchmark.cpp
related_adrs: []
---

# Soft-body Physics Foundation 2

## Decision

Foundation 2 keeps the selected Gargantuan-owned CPU XPBD backend alongside
unchanged Box3D rigid physics. The backend is still viable for the current
fixed-topology cloth and bounded elastic-body scope. Independent-body jobs
materially reduce wall time when multiple bodies are active, the deterministic
broadphase removes the 4,096-collider narrow-phase cliff, and rotation-aware
shape matching plus tetrahedral volume constraints make `RubberBody` respond as
an elastic body instead of a translation-only rest-shape cloud.

The architecture remains:

```text
WorldRoot
  |- PhysicsWorld  -> IPhysicsBackend  -> Box3DPhysicsBackend (rigid only)
  `- SoftBodyWorld -> ISoftBodyBackend -> XpbdSoftBodyBackend
                         |
                         `- existing JobSystem / Worker execution domain
```

No Box3D handle, `Instance*`, `WorldRoot*`, hierarchy collection, renderer
object, SDL resource, or mutable callback enters a soft-body job. Box3D remains
rigid-only and renderer publication remains Main-owned.

Foundation 1's semantic model is preserved. Two small semantic additions were
necessary to express Foundation 2 behavior without exposing solver controls:

- `SoftBodyMaterial.VolumeCompliance` describes resistance to semantic volume
  change; it is persisted and replicated like the other material semantics.
- `DeformableBody:ApplyImpulseAtPosition(impulse, position)` expresses an
  off-center impulse. A central impulse cannot describe angular excitation, so
  this is a concrete missing operation rather than a solver-native knob.

`ShapeCompliance` remains the semantic elastic shape-recovery control. No XPBD
multiplier, iteration count, tetrahedron, covariance, or polar-decomposition
setting is public.

## Job architecture

### Main snapshot

`WorldRoot` still reconciles semantic bodies, materials, attachments, forces,
and rigid primitive snapshots on Main. `SoftBodyWorld::Step` admits at most one
batch. It validates and copies neutral collider descriptions, constructs one
immutable broadphase, and moves each eligible backend-owned body record into a
batch work item. The slot retains only identity, definition revision, and the
last authoritative published state while its item is in flight.

Each work item contains only:

- `SoftBodyId` plus definition revision;
- value-type semantic definition and attachments;
- owned nodes and neutral XPBD constraints;
- the fixed `DeltaTime`, gravity, forces, central impulse, and bounded point
  impulses; and
- a shared read-only neutral collider snapshot and broadphase.

Main decides the exact 1/60-second step. There is no completion-time-derived
simulation clock and no queued elapsed-time replay.

### Worker solve

Foundation 2 uses the repository's existing `JobSystem` and its `Worker`
execution-domain scope. It does not introduce a second executor implementation.
One bounded `JobSystem` instance is owned by each XPBD backend, with
`min(hardware_concurrency - 1, 8)` workers and a minimum of one. That ownership
keeps backend shutdown explicit today and leaves a narrow replacement seam for
a future shared scheduler.

One job solves one whole body. Constraints inside a body remain serial and
deterministically ordered. This matches the current interaction graph: there is
no soft/soft collision, so bodies share only immutable rigid collider data.

Workers integrate, solve distance/shape/volume constraints, query rigid
collision candidates, resolve primitive contacts, and extract an owned state.
They never access semantic Instances, mutate hierarchy state, call a renderer,
or publish callbacks.

### Deterministic merge and stale results

After the job group completes, Main merges states in `SoftBodyId` order rather
than completion order. A result is accepted only when all of these remain true:

1. the slot is live;
2. slot and result generations match;
3. the definition revision matches;
4. the slot still expects the in-flight record; and
5. shutdown has not started.

Destroy increments the generation. Disable, resolution changes, material
changes, and other reconfiguration increment the definition revision and install
a replacement record. Consequently, an older result cannot resurrect a body or
overwrite a newer topology/material. A failed job or dispatch restores the
current record and its pending inputs only if the same generation/revision is
still current. Shutdown marks the backend first, waits for the admitted batch,
discards its results, drains workers, and then releases storage.

The accepted state is authoritative before `WorldRoot` records deformable
dirtiness. The existing renderer path is unchanged:

```text
worker result
  -> generation/revision check on Main
  -> authoritative SoftBodyState
  -> RenderDirtyAccumulator
  -> RenderPublication
  -> persistent deformable mesh
```

## Bounds and backpressure

The default hard bounds are:

| Resource | Bound |
| --- | ---: |
| In-flight batches | 1 |
| Workers | 8 |
| Bodies per batch | 64 |
| Aggregate vertices per batch/world | 131,072 |
| Aggregate constraints per batch | 1,048,576 |
| Result position bytes per batch | 8 MiB |
| Rigid collider snapshots | 4,096 |
| Point impulses per body/step | 64 |

`Step` is a join-before-return fixed-step operation, so normal Main execution
cannot accumulate frames behind the game clock. A concurrent second admission
is rejected immediately and increments `BacklogDrops`; no extra frame or result
queue is created. Bodies beyond quality, vertex, constraint, or result-byte
budgets retain their last state, publish `Simulated=false`, and increment
`FrozenBodies`. Inputs are consumed only by an admitted body. This is an
explicit freeze/drop policy rather than unbounded latency.

Low, Medium, High, and Automatic keep their Foundation 1 meanings. Foundation 2
does not raise Automatic. Internal quality policy can still lower iterations,
lower topology, freeze bodies, or disable rigid collision. Simulation remains
headless and render fidelity remains independently reducible. No mobile
performance claim is made without physical mobile hardware.

## Collision broadphase

Foundation 2 uses a compact deterministic sweep structure over conservative
world-space collider AABBs. Construction validates at most 4,096 neutral
snapshots, computes bounds, and sorts entries by minimum X with original
snapshot ordinal as the tie-breaker. A body query scans until entry minimum X is
beyond the body maximum X, rejects non-overlap on all axes, then sorts candidate
ordinals before narrow phase.

This choice was preferred over a dynamic tree because collider snapshots are
rebuilt once per fixed step, read by many independent jobs, and already hard
bounded. The structure is immutable, engine-owned, deterministic, linear to
build after sorting, and has no Box3D dependency. The benchmark includes an
explicit `BruteForceReference` mode and tests require identical deformation
output. False positives are safe because every candidate still runs narrow
phase; tests compare broadphase and brute-force outputs and cover rotated bounds,
sparse/dense layouts, invalid colliders, and the 4,096 hard limit.

Candidate counts are accumulated per solver collision query. With eight solver
iterations, one candidate/body appears as eight total candidates in a profile.

## Primitive narrow phase

All contact work remains behind `SoftBodyCollider` and uses semantic primitive
dimensions and transforms:

- ball uses analytic point/sphere projection with cloth thickness;
- box uses an exact point/sphere-versus-OBB closest-point test, including inside,
  edge, corner, rotated, and grazing cases;
- cylinder uses analytic local-space capped-cylinder side/rim/cap projection;
- wedge and corner-wedge use bounded convex half-space sets matching their
  semantic hulls, with closest-plane projection; and
- normals are transformed back through the collider rotation before velocity
  clipping and friction.

Wedge collision therefore uses a general bounded convex representation rather
than render meshes or class-specific triangle soup. Collision remains discrete;
CCD is not claimed. High-speed tests assert stable finite behavior inside the
current non-CCD contract, not tunnelling prevention.

## Rubber Foundation 2

Each rubber solve computes movable-node and rest centroids and the current/rest
covariance. A stable iterative polar rotation extracts the best-fit rigid
orientation. Shape recovery targets the rotated rest offsets around the current
centroid, so deformation is restored without pulling a rotated body back to its
original world orientation.

An off-center point impulse is distributed to the four closest movable nodes by
normalized inverse distance. This gives the body both linear and angular motion
without exposing backend particles publicly.

Volume preservation is included. Each regular rubber lattice cell is split into
five consistently oriented tetrahedra. XPBD signed-volume constraints use the
semantic `VolumeCompliance`, reset their multipliers each fixed step, and run in
the same bounded quality iteration loop. This is deliberately not FEM,
plasticity, fracture, or an unbounded volumetric mesh. Tests measure retained
tetrahedral volume after rotational deformation, along with rotation, local span
recovery, attachments, and rigid collision.

## Validation coverage

`gargantuan_soft_body_physics_tests` adds Foundation 2 checks for:

- exact jobified versus synchronous-reference results for the same body;
- deterministic identity-ordered merge for multiple bodies;
- concurrent second-step backpressure;
- destroy, disable, resolution/material reconfiguration, slot reuse, Stop, and
  shutdown while work is in flight;
- stale generation/revision rejection without state resurrection;
- broadphase equivalence to brute force and candidate reduction;
- rotated ball, box, cylinder, wedge, and corner-wedge contact for thin cloth;
- rotation-aware rubber response to an off-center impulse;
- local shape recovery and bounded tetrahedral volume loss; and
- the Foundation 1 persistence/reload, Play/Stop, renderer restart, and world
  restart paths.

The tests deliberately force a large High-quality job before racing lifecycle
operations. Same-build/platform deterministic equality is retained. Cross-ISA
or cross-compiler bitwise equality is still not promised.

## Release benchmark method

The benchmark now records P50/P95/P99/max for Main snapshot, dispatch, worker
wait, integration, constraints, broadphase build/query, narrow collision, merge,
state extraction, render publication, and headless projection application. It
also reports worker count/utilization, jobs/frame, candidate colliders/query,
queued result bytes, estimated owned solver bytes, allocation/construction time,
process working set, and backlog/freeze/stale counters.

The tables below use 3 warmup and 20 measured frames unless stated otherwise.
They are CPU/headless measurements at a fixed 1/60-second step. Allocation time
is body/world construction through initial publication; allocator call counts
are not claimed because the repository has no isolated allocator hook. The
owned-capacity estimate is the comparable memory measure.

Primary: Ryzen 9 7950X3D, Windows x64, MSVC Release. Secondary: Ryzen 9 5900X,
Alpine/musl GCC 15.2 Release in the verified Docker Desktop workflow, limited to
12 logical CPUs and 12 GiB. GPU work is excluded.

### Jobification before/after

The synchronous reference runs the identical Foundation 2 solver/collision path
without job dispatch, isolating jobification rather than comparing different
rubber or collision algorithms. This case uses four independent 4K cloth bodies,
32 colliders, 5 warmup frames, and 60 measured frames.

| Machine / mode | Mean ms | P50 | P95 | P99 | Speedup | Jobs | Utilization |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 7950X3D synchronous | 10.824 | 10.828 | 11.039 | 11.135 | 1.00x | 0 | 0% |
| 7950X3D jobified | 3.026 | 3.041 | 3.346 | 3.474 | 3.58x | 4 | 49.5% |
| 5900X synchronous | 10.526 | 10.499 | 10.651 | 11.003 | 1.00x | 0 | 0% |
| 5900X jobified | 3.332 | 3.377 | 3.657 | 3.769 | 3.16x | 4 | 44.8% |

Main snapshot, dispatch, and merge total about 0.015 ms/frame on the primary
jobified case and 0.125 ms/frame on the secondary container case. Worker wait is
the dominant wall stage. One body still maps to one job, so a single large body
does not use eight workers; this is intentional for Foundation 2.

A final primary validation repeat after the test-only lifecycle additions measured
11.315 ms synchronous and 3.316 ms jobified (3.41x). Its jobified P99 was 5.440
ms, versus 3.474 ms in the tabled run, which records the observed scheduler-tail
variance without changing the multi-body scaling conclusion.

### Required cloth and rubber sizes

Single-body values are hanging/pinned cloth and sustained rubber. Times are
step mean / P50 / P95 / P99 in milliseconds.

| Case | 7950X3D | 5900X |
| --- | ---: | ---: |
| Cloth 1K | 0.468 / 0.466 / 0.481 / 0.483 | 0.611 / 0.606 / 0.651 / 0.654 |
| Cloth 4K | 1.939 / 1.841 / 2.496 / 2.516 | 2.306 / 2.266 / 2.583 / 2.609 |
| Cloth 16K | 3.891 / 3.885 / 3.927 / 3.983 | 4.616 / 4.614 / 4.657 / 4.659 |
| Cloth 64K stress | 8.788 / 8.362 / 11.105 / 12.135 | 9.901 / 9.899 / 10.084 / 10.108 |
| Rubber 64 | 0.065 / 0.060 / 0.085 / 0.092 | 0.099 / 0.097 / 0.108 / 0.117 |
| Rubber 512 | 0.504 / 0.489 / 0.597 / 0.619 | 0.631 / 0.605 / 0.732 / 0.757 |
| Rubber 4K | 4.274 / 4.168 / 4.653 / 5.352 | 4.858 / 4.833 / 4.875 / 5.255 |
| Rubber 32K stress | 10.656 / 9.646 / 13.610 / 13.827 | 10.885 / 10.803 / 11.181 / 11.186 |

Rotation and volume make rubber materially more expensive than Foundation 1's
translation-only recovery. Quality policy retains authority to reduce topology
and iterations. The 32K rubber case is a stress case, not a new default.

### Multi-body scaling

| Case | 7950X3D mean / P99 / utilization | 5900X mean / P99 / utilization |
| --- | ---: | ---: |
| 4 x 4K cloth | 3.086 / 3.405 / 45.8% | 3.308 / 3.542 / 41.7% |
| 8 x 4K cloth | 3.133 / 3.832 / 71.8% | 3.142 / 3.763 / 84.3% |
| 4 x 16K cloth | 7.113 / 9.955 / 45.2% | 6.598 / 7.409 / 44.5% |
| 32 x 64 cloth | 0.199 / 0.366 / 83.2% | 0.966 / 1.892 / 77.9% |
| 4 x 4K cloth + 4 x 512 rubber | 2.969 / 3.316 / 54.1% | 3.132 / 3.569 / 53.3% |

The container has higher dispatch cost for many tiny jobs, but remains bounded.
For medium and large independent bodies, worker utilization and wall-time
scaling are useful on both machines.

### Collider scaling

The sparse benchmark places one plausible floor candidate among 32, 256, 1,024,
or 4,096 colliders. `Candidates` is per body collision query.

| Colliders / mode | 7950X3D mean / P99 / candidates | 5900X mean / P99 / candidates |
| --- | ---: | ---: |
| 32 broadphase | 0.697 / 0.758 / 1 | 0.791 / 0.990 / 1 |
| 32 brute force | 8.093 / 8.809 / 32 | 4.349 / 4.500 / 32 |
| 256 broadphase | 0.705 / 1.134 / 1 | 0.932 / 1.706 / 1 |
| 256 brute force | 61.874 / 64.246 / 256 | 30.552 / 31.438 / 256 |
| 1,024 broadphase | 0.772 / 0.935 / 1 | 1.015 / 1.270 / 1 |
| 1,024 brute force | 260.965 / 282.788 / 1,024 | 122.435 / 134.994 / 1,024 |
| 4,096 broadphase | 1.172 / 1.740 / 1 | 1.351 / 1.729 / 1 |
| 4,096 brute force | 1,079.567 / 1,130.920 / 4,096 | 482.870 / 489.894 / 4,096 |

At 4,096 colliders the primary broadphase spends about 0.258 ms building,
0.026 ms querying, and 0.206 ms in narrow phase; brute force spends about
1,077.6 ms in narrow phase. On the secondary these stages are 0.254, 0.010, and
0.110 ms versus 480.8 ms brute-force narrow phase. Dense layouts legitimately
return more candidates and do not promise the sparse-world reduction; the hard
bounds and equivalence guarantee still hold.

## Sanitizer and race status

The final Windows x64 MSVC Release build succeeds and full CTest passes 23/23.
The focused executable also passes after adding separate in-flight disable,
reconfigure, destroy/slot-reuse, shutdown, backlog, dense/sparse/rotated
broadphase, 4,096+1 collider-limit, primitive edge/grazing, rubber-volume, and
persistence checks. The final Ryzen 9 5900X Release focused run passes as well.

On the isolated 5900X worker, GCC 15.2 RelWithDebInfo with
`-fsanitize=address,undefined` passes the same focused suite with
`detect_leaks=1`, `halt_on_error=1`, and UBSan stack traces enabled. This is
memory and undefined-behavior evidence only.

ThreadSanitizer was configured and linked separately with GCC 15.2
`-fsanitize=thread`. It cannot execute on the current Alpine/musl Docker stack.
The default Docker profile first aborts at `tsan_platform_linux.cpp:290` because
the `personality()` call is denied. A one-shot isolated retry with seccomp
unconfined gets past that boundary, then GCC's TSAN runtime aborts before `main`
at `tsan_platform_linux.cpp:571` because its expected thread/TLS bounds do not
match musl (`thr_beg < tls_addr`). This is an exact toolchain/runtime blocker,
not a test failure and not race evidence. Foundation 2 therefore makes no race-
cleanliness claim; a glibc or otherwise TSAN-supported CI worker is still
required. The deterministic lifecycle/fault suite is retained as the practical
concurrency regression gate on the current stack.

## Foundation 2 assessment

- **Owned XPBD viability:** yes for bounded, fixed-topology cloth and gameplay
  rubber. The neutral backend and renderer boundary remain sound.
- **Meaningful job scaling:** yes for multiple independent bodies: 3.58x on the
  primary and 3.16x on the secondary 4x4K reference, with useful eight-body
  utilization. Single-body scaling is intentionally unchanged.
- **Jolt comparison:** now warranted as a Foundation 3 spike behind
  `ISoftBodyBackend`, especially for robust volumetric elements, contact, and
  single-body parallelism. It is not evidence to replace Box3D or the current
  semantic APIs.
- **Practical default:** keep Automatic unchanged; 16K is the practical desktop
  cloth ceiling/default high-end tier, while 64K cloth and 32K rubber remain
  explicit stress/opt-in. Physical mobile measurements are still required.
- **Production cloth blockers:** self-collision, soft/soft collision, robust CCD
  or swept contact for tunnelling, tearing/topology change, production asset/LOD
  policy, two-way rigid coupling, and physical mobile thermal evidence.
- **Production rubber blockers:** more robust volumetric discretization and
  inversion handling, richer contact/two-way coupling, validated material
  calibration, possible single-body parallelism, comparison with Jolt, and
  physical mobile evidence. The current regular lattice is not arbitrary-mesh
  FEM.

## Exact Foundation 3 priorities

1. Build a measured Jolt deformable comparison behind `ISoftBodyBackend`; keep
   Box3D rigid-only and do not alter public Instances during the spike.
2. Add a read-only interaction-graph seam for future soft/soft work, then design
   separately bounded cloth self-collision and soft/soft collision. Do not add
   quadratic pair scans to the current independent-body scheduler.
3. Evaluate constraint coloring or another deterministic intra-body strategy
   only for single 16K/64K bodies where profiles prove body-level jobs are
   insufficient.
4. Add swept/discrete-contact policy for high-speed cloth and stronger
   persistent contact manifolds without importing renderer meshes.
5. Evaluate robust tetrahedralization/inversion handling and two-way rigid
   coupling for production rubber; keep FEM, plasticity, and fracture separate.
6. Define topology-change semantics, render residency updates, and persistence
   implications before tearing.
7. Run sustained Android and iOS hardware tiers under thermal load and set
   topology/iteration/collision/freeze policy from evidence.
8. Define server authority, snapshot/interpolation, and topology compatibility
   before networking deformable runtime state.

Soft/soft collision, self-collision, networking, plasticity, FEM, and renderer
redesign remain outside Foundation 2.
