---
status: experimental
authority: non-normative
date: 2026-11-07
---

# Expanded physics comparison

This isolated experiment follows the explicitly authorized integration of
[PR18](https://github.com/gmoddev/gargantuan/pull/18). Actual merged main is
`5b6782e35c4e24fd80bc8d5526104c615c72d84d`, tree
`41068e0812c33c01cc3ce5a8dc03834186a48af1`, with parents original main
`04c6385f26de9b7d96b48de06c911ecea692796c` and qualified source
`e19ee0d692595826cbc40249b11395cba7599059`. The resulting tree equals the
qualified feature tree. Fresh post-merge qualification is a separate gate;
implementation preparation does not promote pending jobs to passing evidence.

Production defaults remain Box3D plus Gargantuan XPBD. This branch does not
authorize a backend migration, public API change, persistence revision, release,
deployment, or merge of experimental code. The older `ae1a7ba68` experiment and
its failures remain unchanged.

## Baseline capabilities

The expanded mainline includes velocity/force/torque/impulse controls, mass and
inertia inspection, sleep and kinematic controls, mutual 16-group filtering,
ray/sphere/capsule/box casts and bounded overlaps, bounded moving-body CCD,
ball/hinge/slider/distance joints with motors/limits/springs, admitted imported
static mesh chunks, transactional soft attachment updates, and bounded soft
diagnostics. WorldRoot owns authoritative safe points, neutral generation IDs,
copied publication and retirement. Project v5 saves bounded document-local
references while retaining versions0–4 reads and v4 package content compatibility.

Current contact is one-way discrete rigid-to-soft. Soft/soft contact, native
soft group membership, general swept deformable contact, arbitrary dynamic
concave collision and constraint breakage remain unsupported. Legacy Weld offset
capture is an inherited limitation; authored attachment frames supply explicit
mechanism semantics.

## Build boundary

Configure the normal Engine with `GARGANTUAN_BUILD_PHYSICS_JOLT_EVALUATION=ON`
and `PHYSICS_EVALUATION_JOLT_ROOT` pointing at a verified Jolt checkout. The option
defaults OFF and does not alter production factories. The pinned current release
is [v5.6.0](https://github.com/jrouwe/JoltPhysics/releases/tag/v5.6.0), commit
`e77f175595e64cb44218cc9d9d56fc365ad0e36a`. CMake rejects a different checkout.
Targets link the real production core; the previous CFrame fixture shim is not
used. GPU compute, Jolt samples, debug renderer, profiler and LTO are disabled.
The controlled common x64 SIMD profile is SSE2; supplementary Jolt default-ISA
results, if collected, must be a distinct configuration. MSVC runtime linkage
matches the parent build. Exact compiler flags and hashes belong in run receipts.

There are three runners: rigid neutral adapter, soft quality, and reciprocal
coupling. Each has a separate `Allocations` executable using the same scene
source. Timing builds retain ordinary allocators. Allocation builds count C++
new plus explicit Box3D and Jolt hooks, including harness allocations inside
the captured phase. This is requested storage, not complete process heap usage;
other malloc, allocator metadata, shared-library storage and OS allocations are
excluded. Never rank instrumented times as production timing measurements.

## Prospective measurement protocol

Quality checks are declared before measurement. Preserve every invalid or failed
run; a faster failing solver gets no performance advantage. Reproduce the old
cloth scene separately from new calibrated workloads. Nominally equal iteration
or compliance numbers do not establish equal stiffness or accuracy.

- Use identical geometry, units, masses, timestep and external inputs per pair.
  Default dt is1/60; native rigid comparison retains four collision substeps.
- Report actual constraints, solver iterations and damping mapping. Calibrated
  cloth resolution scaling holds total movable mass/area constant; the old
  reproduction retains particle mass0.1 and gravity196.2. Do not mix tracks.
- Cloth acceptance initially requires finite coordinates, maximum tensile edge
  strain<=10%, attachment drift<=1e-4 and primitive penetration<=0.1 world units.
  Rubber additionally requires relative volume error<=15% and no inversion.
  Every fixture must report the actual implemented metric and threshold; omitted
  observations cannot silently pass. Contact and mechanism gates additionally
  observe geometric support, bounded limits, motor response and retirement.
- Allow only a bounded diagnostic/calibration sweep (at most three configurations
  per failing workload class before retaining the failure). Record configuration
  changes and their reason before collecting comparative timing runs.
- Collect at least three fresh process repeats for accepted paired workloads;
  use30 warmup and120 measured fixed steps unless a named lifecycle or coupling
  scenario needs an explicit different horizon. Retain individual samples,
  process-level P50/P95/P99 and ranges; do not pool independent process runs.
- Thread budgets1/2/8 mean total participating threads, including the caller.
  Record actual XPBD workers, Jolt background workers and caller participation.
  A setting that does not affect a backend is labeled, not presented as scaling.
- Separate creation/admission, mutation, simulation, query, copied extraction,
  collider synchronization and teardown. Retain process CPU, RSS/peak RSS and
  separately instrumented allocation/storage observations. Process peaks are
  lifetime high-water values, not phase-local peaks or proof of retirement.
- Compare paired builds only on the same machine/compiler/configuration. Windows
  and Linux each produce their own paired ratios. No cross-machine speedup.

## Reciprocal coupling gate

The highest-priority prototype uses one actual Jolt PhysicsSystem containing
both rigid and soft bodies. A paired collision-disabled control must demonstrate
that rigid motion changes because of deformable contact. Run sphere-on-cloth,
rubber push, moving rigid into hanging cloth, repeated contacts and simultaneous
interactions. Record impulse/momentum, penetration, energy, finite deformation,
repeat state hashes, synchronization and full frame cost. Account for gravity,
damping and anchored/prescribed-motion external work before discussing
conservation. A raw energy difference alone is not a conservation test.

Any experimental XPBD-to-Box3D feedback remains private to this branch, uses
generation-safe mapping and an explicit safe point, and is not production
qualification. If that bounded path cannot pass, say so; current production
one-way contact is not equivalent to reciprocal Jolt contact. The hybrid keeps
Box3D as sole rigid authority and must charge collider mirroring, duplicate
contact worlds and feedback transfer rather than simulating two authoritative
rigid copies without accounting.

## Decision and maintenance

Compare A (Box3D+XPBD), B (Box3D+Jolt soft) and C (unified Jolt) separately from
raw speed. Identify which coupling, scheduling, contact, diagnostics, lifecycle
and tests could disappear, and which identity, authority, asset, public API,
networking, persistence and platform work remains Engine-owned. Quantify bounded
adapter and patch scope, integration unknowns, regression/rollback burden and
upstream rebase exposure. Fewer lines alone do not establish lower cost.

The recommendation must select retain, selective XPBD improvement, bounded
hybrid adoption, or unified migration recommendation. It does not authorize
implementing a production migration. Final measurements, review and the exact
experimental continuation checkpoint will be linked after execution; this file
is the pre-measurement protocol, not a completed result.
