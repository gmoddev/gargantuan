---
status: current
owner: physics
last_verified: 2026-08-23
related_code:
  - assets/classes/DeformableBody.luau
  - assets/classes/Cloth.luau
  - assets/classes/RubberBody.luau
  - assets/classes/SoftBodyMaterial.luau
  - assets/classes/SoftBodyAttachment.luau
  - include/gargantuan/physics/SoftBodyTypes.hpp
  - include/gargantuan/physics/SoftBodyBackend.hpp
  - src/physics/SoftBodyBackend.cpp
  - src/physics/XpbdSoftBodyBackend.cpp
  - include/gargantuan/classes/WorldRoot.hpp
  - src/classes/WorldRoot.cpp
  - src/render/RenderPublisher.cpp
  - tests/SoftBodyPhysicsTests.cpp
  - tests/SoftBodyPhysicsBenchmark.cpp
related_adrs: []
---

# Soft-body Physics Foundation 1

Foundation 1 is the accepted semantic, persistence, and renderer-integration
baseline. [Foundation 2](SoftBodyPhysicsFoundation2.md) is the current job,
broadphase, primitive-contact, and rubber architecture.

## Outcome and decision gate

Foundation 1 selects **Option B: a Gargantuan-owned, CPU XPBD deformable
backend beside the existing Box3D rigid backend**.

Box3D remains the rigid-body boundary. This change does not fork Box3D, put
deformable concepts into its adapter, or replace it. `WorldRoot` owns one
`PhysicsWorld` for rigid simulation and one `SoftBodyWorld` for deformation.
Both expose engine-owned value contracts and generation-safe IDs.

The slice is accepted for continued development with these gates:

- cloth and rubber both have real, deterministic vertical slices;
- semantic state survives persistence, reload, Play, restart, and destruction;
- steady deformation reaches the persistent renderer mesh path as vertex-only
  updates;
- the default `Automatic` tier meets the 60 Hz simulation budget on the measured
  Ryzen 9 7950X3D through the 64k stress case, although 64k collision has narrow
  whole-pipeline headroom and is not a recommended default;
- the secondary Ryzen 9 5900X Linux-container run independently passes the
  focused suite and confirms the same tier decision: 16k has substantial
  headroom, while the 64k collision case is too narrow for a general default;
  and
- no mobile performance claim is made because no real mobile device was measured.

This is a foundation, not a final production solver. Self-collision, soft/soft
collision, tearing, two-way rigid coupling, rotational rubber shape matching,
networked deformation, and job-parallel solving remain deliberately deferred.

## Solver evaluation

The decision used current official project material:

| Direction | Evidence | Decision |
| --- | --- | --- |
| Extend Box3D | Box3D's official [3D simulation](https://box2d.org/documentation3d/md_simulation.html) and [body](https://box2d.org/documentation3d/group__body.html) contracts are rigid-body contracts. | Rejected. A private deformable fork would couple unrelated lifecycles and make upstream updates harder. |
| Jolt soft bodies | Jolt has XPBD soft bodies and an engine/job architecture ([repository](https://github.com/jrouwe/JoltPhysics), [architecture](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md)), but its released API documentation still describes soft bodies as [in development](https://jrouwe.github.io/JoltPhysicsDocs/5.4.0/class_soft_body_creation_settings.html). | Strongest future third-party adapter candidate, not selected for this narrow first slice. It would import a second broad physics runtime before Gargantuan has evidence that its extra surface is needed. |
| NVIDIA PhysX deformables | Current PhysX deformable and PBD paths are documented as GPU/CUDA facilities ([deformables](https://nvidia-omniverse.github.io/PhysX/ovphysx/latest/simulation_setup/deformables.html), [GPU compatibility](https://nvidia-omniverse.github.io/PhysX/physx/5.7.0/docs/GPURigidBodies.html)). | Rejected for the foundation. It conflicts with headless server simulation and the AMD CPU baseline. |
| Bullet soft bodies | Bullet provides broad portable soft-body support under a permissive license in its [official repository](https://github.com/bulletphysics/bullet3). | Not selected. Bringing in a second general physics stack would broaden build, lifetime, and adapter scope beyond the two required vertical slices. |
| Gargantuan-owned XPBD sibling | XPBD gives time-step-independent compliance and a compact constraint model; see the [XPBD paper](https://mmacklin.com/xpbd.pdf). | Selected. It preserves Box3D, is CPU/headless portable, matches the semantic and renderer boundaries exactly, and leaves room for a future Jolt-backed `ISoftBodyBackend`. |

The choice is not a claim that a custom solver will eventually outperform or
out-feature every library. It is the smallest reversible boundary that delivers
the required behavior without replacing the rigid stack. A later backend spike
can compare Jolt behind the same `ISoftBodyBackend` contract.

## Ownership and neutral contract

```text
Cloth / RubberBody / SoftBodyMaterial / SoftBodyAttachment semantics
    -> WorldRoot safe-point reconciliation
    -> SoftBodyWorld
    -> ISoftBodyBackend
    -> XpbdSoftBodyBackend
    -> owned SoftBodyState values
    -> WorldRoot deformation state
    -> RenderDirtyAccumulator
    -> RenderPublisher
    -> persistent renderer mesh
```

`SoftBodyTypes.hpp` is the backend-neutral vocabulary:

- `SoftBodyId` is a nonzero slot/generation value, independent of `ObjectId`,
  `PhysicsBodyId`, renderer identity, and solver storage;
- `SoftBodyDefinition` owns kind, rest transform/dimensions, fixed resolution,
  material, quality, collision mode, enabled state, and attachments;
- `SoftBodyMaterialDesc` owns particle mass, damping, stretch/bend/shape
  compliance, friction, and collision thickness;
- `SoftBodyCollider` is a current rigid primitive snapshot, not a Box3D handle;
- `SoftBodyState` owns stable topology and copied positions plus topology/vertex
  revisions; and
- `SoftBodyStepConfig`, `SoftBodyStepResult`, and `SoftBodyStepProfile` own all
  step inputs, outputs, truncation flags, and stage timings.

No Box3D, Jolt, Bullet, PhysX, SDL, GPU, Instance, or borrowed solver type appears
in that contract. `SoftBodyWorld` owns its backend through `ISoftBodyBackend`.

Slots are reused only after incrementing their generation. Stale and duplicate
destroy operations fail closed. Generation exhaustion retires a slot instead of
wrapping. Slot order also defines deterministic body scheduling and active-budget
fallback.

## Public object model

Each cloth or rubber body is one semantic Instance. Simulation vertices are
internal solver data; they are not child Instances.

| Class | Meaningful surface |
| --- | --- |
| `DeformableBody` | Abstract base with `Enabled`, `Position`, `Size`, appearance, `Quality`, `CollisionMode`, optional `Material`, `ApplyForce`, and `ApplyImpulse`. |
| `Cloth` | Rectangular grid with `ResolutionX` and `ResolutionY`, each 2–256. |
| `RubberBody` | Elastic lattice with `ResolutionX/Y/Z`, each 2–40 and still subject to the 65,536-vertex backend cap. |
| `SoftBodyMaterial` | Shared serializable mass, damping, compliance, friction, and thickness controls. |
| `SoftBodyAttachment` | When enabled beneath a deformable body, pins one stable zero-based `VertexIndex` to a world-space `Position`. |

All properties above are schema-backed, serializable semantic state and are
marked for replication. Runtime positions, velocity, XPBD multipliers, backend
IDs, mesh revisions, and GPU state are not public or serialized. End-to-end
networked deformation is outside this milestone; the schema markers preserve
future semantic replication without pretending that solver snapshots are game
state.

If multiple enabled attachments name one vertex, `WorldRoot` deterministically
uses the attachment with the lowest `ObjectId`. Invalid attachment indices cause
the candidate body update to fail without replacing the prior coherent body.

Example cloth:

```luau
local Material = Instance.new("SoftBodyMaterial")
Material.ParticleMass = 0.08
Material.Damping = 0.05
Material.StretchCompliance = 0.000001
Material.BendCompliance = 0.0002
Material.Friction = 0.35

local Cloth = Instance.new("Cloth")
Cloth.Name = "Banner"
Cloth.Position = Vector3.new(0, 12, 0)
Cloth.Size = Vector3.new(12, 8, 0.5)
Cloth.ResolutionX = 64
Cloth.ResolutionY = 64
Cloth.Quality = Enum.DeformableQuality.Automatic
Cloth.Material = Material

local LeftPin = Instance.new("SoftBodyAttachment")
LeftPin.VertexIndex = 0
LeftPin.Position = Vector3.new(-6, 16, 0)
LeftPin.Parent = Cloth

local RightPin = Instance.new("SoftBodyAttachment")
RightPin.VertexIndex = 63
RightPin.Position = Vector3.new(6, 16, 0)
RightPin.Parent = Cloth

Material.Parent = Cloth
Cloth.Parent = workspace
Cloth:ApplyForce(Vector3.new(40, 0, -15))
```

Example rubber body:

```luau
local Rubber = Instance.new("RubberBody")
Rubber.Position = Vector3.new(0, 8, 0)
Rubber.Size = Vector3.new(4, 4, 4)
Rubber.ResolutionX = 8
Rubber.ResolutionY = 8
Rubber.ResolutionZ = 8
Rubber.Parent = workspace
Rubber:ApplyImpulse(Vector3.new(20, 5, 0))
```

## Implemented solver behavior

### Cloth

Cloth creates an XY rest grid centered on `Position` with stable row-major
vertices and two triangles per cell. It builds:

- horizontal and vertical stretch constraints;
- both diagonal shear constraints per cell; and
- two-vertex horizontal and vertical bend constraints.

Distance constraints use XPBD compliance and reset transient multipliers each
fixed step. Pinned vertices have zero inverse mass and are restored exactly after
each solver iteration. Unpinned vertices integrate gravity, uniform body force,
uniform body impulse, and per-step damping.

### Rubber

Rubber creates an XYZ lattice, edge-distance constraints, and triangles for the
six outer surfaces. A translation-preserving rest-shape recovery constraint pulls
movable vertices toward their lattice offsets. Pinned vertices anchor the
translation; without pins the current centroid anchors it.

This is a useful elastic vertical slice, not a full volumetric/FEM model. Shape
recovery is translation-only and does not extract a best-fit rotation. There are
no volume constraints or plasticity.

### Collision and forces

`WorldRoot` snapshots live collidable Parts after the rigid step, in sorted
`ObjectId` order. Ball Parts become sphere tests. Box Parts use their oriented
box. Cylinder, wedge, and corner-wedge snapshots currently use a conservative
oriented-box approximation inside the deformable backend. Collision thickness
expands the primitive, and friction removes the configured fraction of
tangential motion after final contact resolution.

Collider kind, positive finite size, finite position, and approximately
orthonormal rotation are validated. Invalid or excess snapshots are skipped and
reported through `CollidersTruncated`; they cannot inject NaN/Inf into solver
state.

Rigid collision is currently one-way: rigid poses affect deformables, but
deformables do not apply reaction impulses to Box3D. There is no self-collision,
soft/soft collision, continuous collision detection, contact cache, or broadphase
beyond the bounded primitive list.

`ApplyForce` and `ApplyImpulse` accept finite world-space vectors. They accumulate
until the next fixed step and are distributed over the body's total movable mass.
Input that is non-finite or would overflow the accumulator is rejected before
mutation.

## Quality tiers and hard fallback

| Quality | Constraint iterations | Active-vertex budget | Intended use |
| --- | ---: | ---: | --- |
| `Low` | 2 | 4,096 | Conservative device/server budget. |
| `Medium` | 4 | 16,384 | Normal desktop and provisional upper mobile tier pending real-device evidence. |
| `High` | 8 | Up to the world cap | Explicit quality opt-in; large High bodies are not covered by the Automatic benchmark. |
| `Automatic` | 8 through 4,096; 4 through 16,384; 2 above 16,384 | Up to the world cap | Current default; reduces iteration count as topology grows. |

The backend does not silently decimate topology. If an enabled body cannot fit
the remaining deterministic active-vertex budget, it retains its last stable
positions, returns `Simulated = false`, and sets `VerticesTruncated`. This keeps
render identity and attachment indices stable. `Enabled = false` also freezes a
body but is not reported as budget truncation.

No mobile class is assigned from CPU model guesses. `Low` is the conservative
starting point for a future real-device lab and `Medium` is only a candidate;
actual mobile defaults require measured sustained thermal and renderer evidence.

## Hard limits

| Limit | Value | Failure behavior |
| --- | ---: | --- |
| Live soft bodies | 64 | New body rejected. |
| Vertices per body | 65,536 | Definition rejected atomically. |
| Live vertices per world | 131,072 | Create/update rejected atomically. |
| Live constraints per world | 1,048,576 | Create/update rejected atomically. |
| Attachments in one definition | 4,096 | Definition rejected atomically. |
| Rigid collider snapshots per step | 4,096 | Excess/invalid entries skipped, truncation reported. |
| Fixed deformable steps per frame | 4 | Existing accumulator cap resets after the cap. |
| Fixed interval | 1/60 second | Explicit single-step input. |

Definitions also reject non-finite/non-positive sizes, invalid resolution,
invalid material ranges, non-finite attachment positions, and out-of-range
attachment indices. Failed updates preserve the previous body and are retried by
`WorldRoot` after a logged `[Physics:SoftBody]` diagnostic.

## Phase order and future jobs

All current physics and publication work runs on Main. Within each fixed step the
order is:

1. reconcile dirty rigid and deformable definitions at the pre-step safe point;
2. apply queued rigid impulses and deformable forces/impulses;
3. step Box3D rigid physics;
4. publish rigid transforms and contact/touch events to authoritative Instances;
5. snapshot current collidable Part primitives;
6. step the deformable backend;
7. copy owned `SoftBodyState` values into `WorldRoot` and mark deformable render
   dirtiness; then
8. after simulation and `PreRender`, publish and synchronously apply the immutable
   render update.

Property setters never enter a stepping backend. They commit Instance state and
queue sorted neutral reconciliation work. Removal is a non-stepping Main safe
point and erases pending work before destroying backend identity.

The future job boundary is explicit but not yet active: Main can freeze body
definitions, collider snapshots, forces, and fixed-step input; an owned physics
job can solve only that immutable batch; Main can then generation-check and merge
owned results before render publication. Solver records and writable arrays must
remain job-owned, and hierarchy/property callbacks must remain on Main. Parallel
constraint coloring or per-body jobs require deterministic merge tests before
activation.

## Render and headless behavior

`RenderPublisher` resolves a live `DeformableBody` to its engine-owned
`SoftBodyState`, validates finite positions and indices, rebuilds normals, UVs,
and bounds, and emits one renderer-neutral mesh. Stable topology derives a stable
`RenderMeshIdentity` from `ObjectId`:

- first publication or full resync emits mesh topology plus object creation;
- steady simulation emits a vertex-only range update;
- topology revision change requests a full resync and rebuilds residency; and
- body destruction emits object and mesh removal.

SDL's existing persistent dynamic-mesh cache consumes the same publication.
Physics does not write GPU memory or expose solver arrays. The CPU
`RenderProjection` uses the identical publication contract in headless servers
and tests, so simulation has no renderer dependency. The benchmark measures this
headless projection/application path; it is not an SDL GPU upload benchmark.

## Persistence and lifecycle

Persistence saves only reconstructible semantics: class, hierarchy, material
properties/reference, body size/position/quality/collision mode, fixed resolution,
and attachment vertex/position/enabled state. It deliberately excludes positions,
velocity, multipliers, pending forces, `SoftBodyId`, revisions, render identity,
and GPU state.

Deserialization rebuilds a new backend identity from those semantics. Local Play
deserializes an isolated runtime DataModel, creates and simulates a fresh body,
and releases that world on Stop. A subsequent Play creates another fresh runtime.
Renderer restart requests a full publication and reconstructs mesh residency.
Destroy removes solver mappings, state, dirty work, render object, and mesh.
`WorldRoot` shutdown disconnects observers and clears both rigid and deformable
state through its idempotent teardown.

## Determinism and validation evidence

`gargantuan_soft_body_physics_tests` covers:

- generation reuse, stale IDs, duplicate destruction, hard bounds, invalid
  material, non-finite forces, and invalid collider isolation;
- exact position equality for two same-build worlds given identical fixed-step
  inputs and deterministic body/collider/attachment ordering;
- cloth topology, exact pins, gravity sag, stable topology, and rigid collision;
- material friction reducing tangential contact motion;
- rubber deformation and recovery;
- Low-tier budget freeze without topology mutation;
- `WorldRoot` state ownership;
- full, incremental vertex-only, restart/full-resync, and deletion render paths;
- semantic-only JSON persistence and reload; and
- isolated Play, Stop teardown, and Play restart.

Exact equality is guaranteed by the current deterministic order on the same
build/platform for identical inputs. Cross-compiler and cross-architecture
bitwise determinism is not claimed; that requires a dedicated floating-point
portability contract.

## Release benchmark method

`gargantuan_soft_body_physics_benchmark` emits JSONL. Each case uses one body,
`Automatic` quality, a 60 Hz fixed step, 30 warmup frames, and either 240 measured
frames (hanging/collision) or 600 measured frames (sustained/rubber). Cloth uses
32², 64², 128², and 256² grids. Rubber uses 4³, 8³, and 16³ lattices. Collision
cases include an anchored 100×2×100 floor. Sustained cases apply force every
frame.

Stage distributions are recorded separately for `WorldRoot::StepPhysics`, solver
integration, constraints, collision, state extraction, render publication, and
headless projection application. Allocation time is body/world construction
through initial full publication. Estimated solver bytes count owned solver
capacity. Process working set is also emitted, but it includes the runtime and
allocator and is not treated as solver memory.

Primary hardware: **AMD Ryzen 9 7950X3D**, Windows x64, MSVC Release,
single Main simulation thread. Times are milliseconds.

### Cloth results

| Vertices / scenario | Step mean | Step p99 | Step max | Solver p99 | Collision p99 | Extraction p99 | Publication p99 | Apply p99 | Allocate | Solver MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1,024 hanging/pinned | 0.452 | 0.466 | 0.467 | 0.459 | 0.007 | 0.001 | 0.024 | 0.005 | 1.010 | 0.228 |
| 1,024 collision | 0.601 | 0.627 | 0.633 | 0.621 | 0.161 | 0.002 | 0.023 | 0.005 | 2.068 | 0.228 |
| 1,024 sustained | 0.448 | 0.462 | 0.468 | 0.456 | 0.007 | 0.002 | 0.023 | 0.004 | 0.856 | 0.228 |
| 4,096 hanging/pinned | 1.859 | 1.904 | 1.912 | 1.885 | 0.027 | 0.011 | 0.092 | 0.016 | 3.339 | 0.815 |
| 4,096 collision | 2.451 | 2.567 | 3.764 | 2.542 | 0.674 | 0.018 | 0.116 | 0.018 | 3.219 | 0.815 |
| 4,096 sustained | 1.850 | 1.907 | 2.848 | 1.885 | 0.027 | 0.013 | 0.087 | 0.016 | 3.013 | 0.815 |
| 16,384 hanging/pinned | 3.885 | 4.023 | 5.126 | 3.881 | 0.064 | 0.121 | 0.601 | 0.064 | 9.086 | 3.819 |
| 16,384 collision | 5.136 | 5.329 | 6.362 | 5.206 | 1.393 | 0.083 | 0.527 | 0.064 | 9.047 | 3.819 |
| 16,384 sustained | 3.922 | 4.075 | 5.246 | 3.925 | 0.101 | 0.090 | 0.599 | 0.067 | 9.614 | 3.819 |
| 65,536 hanging/pinned | 8.826 | 9.582 | 10.387 | 8.939 | 0.327 | 0.274 | 2.397 | 0.264 | 26.724 | 13.639 |
| 65,536 collision | 11.539 | 13.077 | 13.606 | 12.452 | 4.015 | 0.237 | 2.507 | 0.206 | 20.973 | 13.639 |
| 65,536 sustained | 8.564 | 9.686 | 10.283 | 8.949 | 0.266 | 0.341 | 2.361 | 0.265 | 26.129 | 13.639 |

The 64k Automatic tier uses two solver iterations. Its collision p99 step plus
separately observed publication/application p99 values is about 15.79 ms if
conservatively added, but percentiles from separate distributions are not a
measured end-to-end percentile. This is too narrow to classify 64k as a general
60 Hz default once gameplay, scripts, actual GPU work, and OS variance are added.
16k is the current practical desktop ceiling with useful headroom.

### Rubber results

| Case / vertices | Step mean | Step p99 | Step max | Solver p99 | Collision p99 | Publication p99 | Apply p99 | Allocate | Solver MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Small / 64 | 0.022 | 0.024 | 0.027 | 0.023 | 0.011 | 0.003 | 0.001 | 0.306 | 0.009 |
| Medium / 512 | 0.188 | 0.201 | 0.748 | 0.197 | 0.082 | 0.012 | 0.003 | 0.384 | 0.065 |
| Stress / 4,096 | 1.543 | 1.603 | 1.962 | 1.587 | 0.648 | 0.070 | 0.015 | 2.403 | 0.480 |

The process working set ranged from roughly 11.6 to 28.3 MiB across primary
cases. The per-case solver estimate is the comparable owned-memory figure.

### Secondary 5900X results

The approved secondary run used an isolated, disposable Linux container on the
existing Docker worker without stopping or modifying its six pre-existing
containers. Host and toolchain identity:

- AMD Ryzen 9 5900X 12-Core Processor, constrained to 12 logical CPUs and 12 GiB;
- Docker Desktop 29.2.1 with cached `postgres:17-alpine` as the disposable build
  environment;
- Alpine Linux/musl, GCC 15.2.0, CMake 4.2.3, and Shaderc/glslc 2026.1;
- Release build, single Main simulation thread, headless `dummy`/`offscreen` SDL
  backends; and
- NVIDIA GeForce GTX 1650 SUPER present on the host but unused by this CPU/headless
  benchmark.

The selective archive was 229,295,895 bytes with SHA-256
`44EA298BD935714E0929C6562DA25F9B2C94FFCA5C74F5D257AF79AC111CE722`.
It excluded `.git`, build products, caches, and `.codexlock`. The focused
soft-body executable built and passed before the benchmark ran. Windows-generated
reflection includes in the isolated staging copy were mechanically normalized
from backslashes to portable forward slashes, and the repository's existing GNU
`-Wno-changes-meaning` compatibility flag was applied to all targets through the
remote CMake configuration. Neither accommodation changed the authoritative
local source or solver behavior.

Secondary cloth results, using the same cases and columns as the primary run:

| Vertices / scenario | Step mean | Step p99 | Step max | Solver p99 | Collision p99 | Extraction p99 | Publication p99 | Apply p99 | Allocate | Solver MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1,024 hanging/pinned | 0.549 | 0.597 | 0.618 | 0.570 | 0.019 | 0.023 | 0.064 | 0.005 | 9.628 | 0.230 |
| 1,024 collision | 0.645 | 0.686 | 0.690 | 0.669 | 0.121 | 0.020 | 0.069 | 0.013 | 2.184 | 0.230 |
| 1,024 sustained | 0.554 | 0.621 | 0.690 | 0.616 | 0.020 | 0.018 | 0.068 | 0.008 | 1.301 | 0.230 |
| 4,096 hanging/pinned | 2.257 | 2.312 | 2.325 | 2.257 | 0.043 | 0.043 | 0.179 | 0.022 | 4.956 | 0.920 |
| 4,096 collision | 2.653 | 2.935 | 3.267 | 2.855 | 0.537 | 0.047 | 0.182 | 0.049 | 3.781 | 0.920 |
| 4,096 sustained | 2.307 | 2.825 | 2.935 | 2.759 | 0.057 | 0.055 | 0.202 | 0.032 | 4.872 | 0.920 |
| 16,384 hanging/pinned | 4.760 | 5.545 | 5.727 | 5.378 | 0.141 | 0.145 | 0.736 | 0.081 | 14.786 | 3.682 |
| 16,384 collision | 5.581 | 6.630 | 6.863 | 6.458 | 1.101 | 0.128 | 0.730 | 0.071 | 9.318 | 3.682 |
| 16,384 sustained | 4.668 | 4.990 | 5.329 | 4.778 | 0.118 | 0.121 | 0.644 | 0.071 | 12.093 | 3.682 |
| 65,536 hanging/pinned | 9.923 | 10.687 | 10.759 | 9.873 | 0.210 | 0.454 | 2.970 | 0.347 | 39.322 | 14.739 |
| 65,536 collision | 12.020 | 13.759 | 13.818 | 12.600 | 2.766 | 0.458 | 2.938 | 0.247 | 27.085 | 14.739 |
| 65,536 sustained | 9.897 | 10.664 | 12.251 | 9.786 | 0.202 | 0.439 | 2.657 | 0.236 | 38.882 | 14.739 |

Secondary rubber results:

| Case / vertices | Step mean | Step p99 | Step max | Solver p99 | Collision p99 | Publication p99 | Apply p99 | Allocate | Solver MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Small / 64 | 0.020 | 0.029 | 0.046 | 0.027 | 0.007 | 0.009 | 0.001 | 0.341 | 0.010 |
| Medium / 512 | 0.178 | 0.212 | 0.221 | 0.190 | 0.069 | 0.025 | 0.004 | 0.833 | 0.077 |
| Stress / 4,096 | 1.420 | 1.487 | 1.554 | 1.424 | 0.435 | 0.144 | 0.023 | 3.317 | 0.563 |

For the 64k collision case, separately adding step, publication, and application
p99 values gives 16.94 ms. That is not an end-to-end percentile because the
three distributions are sampled separately, but it consumes the 60 Hz frame
budget before gameplay, scripts, OS variance, or real GPU work. The equivalent
16k conservative sum is 7.43 ms. The secondary evidence therefore reinforces
16k as the practical desktop ceiling and 64k as stress/explicit opt-in territory.

The raw 15-record JSONL is 18,122 bytes with SHA-256
`38853E9DC2D00838C8459CFB75127BED3A62CD6679BC6BBFFE32FA96364EB787`
and remains at
`C:\Sandbox\Codex\Artifacts\gargantuan-softbody-f1-01a02c16\secondary-5900x.jsonl`
on the worker. The extracted source workspace, persistent build cache, and
selective archive remain in their task-specific `C:\Sandbox\Codex` directories;
the disposable benchmark container was removed automatically. All six unrelated
containers remained running after the run.

No Android or iOS device was available. Desktop numbers are not relabeled as
mobile estimates.

## Commands

Windows Release build and focused checks:

```powershell
cmake --build build-release-native --config Release --parallel 4
ctest --test-dir build-release-native -C Release --output-on-failure
.\build-release-native\gargantuan_soft_body_physics_tests.exe
.\build-release-native\gargantuan_soft_body_physics_benchmark.exe --quick
.\build-release-native\gargantuan_soft_body_physics_benchmark.exe
```

Benchmark selection is available through `--filter=<substring>`,
`--warmup=<frames>`, and `--frames=<frames>`.

## Follow-up plan

1. Run real Android and iOS device tiers under sustained thermal load before
   assigning a mobile default; begin with `Low` and evaluate `Medium` only from
   measured evidence.
2. Add a backend-comparison spike for Jolt behind `ISoftBodyBackend`; do not
   change public Instances or replace Box3D during the spike.
3. Move immutable body/collider batches to bounded physics jobs, starting with
   independent bodies and deterministic Main-domain merge.
4. Add a collider broadphase and exact cylinder/wedge primitives before raising
   collider/body budgets.
5. Evaluate self-collision and soft/soft collision as separately bounded
   features; do not silently enable quadratic work.
6. Upgrade rubber from translation-only recovery to rotation-aware shape
   matching and consider volume constraints if gameplay evidence needs them.
7. Define server authority, semantic replication, client interpolation, and
   topology compatibility before any networked deformation state.
8. Treat tearing, FEM, final assets, and fluid/particle systems as later projects,
   not extensions hidden inside this foundation.
