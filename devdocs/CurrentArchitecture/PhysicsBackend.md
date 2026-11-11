---
status: current
owner: physics
last_verified: 2026-11-07
related_code:
  - include/gargantuan/physics/
  - src/physics/
  - include/gargantuan/classes/WorldRoot.hpp
  - src/classes/WorldRoot.cpp
  - tests/PhysicsBackendTests.cpp
  - tests/SoftBodyPhysicsTests.cpp
related_adrs: []
---

# Physics backend boundary

## Scope

Gargantuan uses Box3D for rigid-body simulation and a sibling engine-owned XPBD
backend for deformable simulation. Box3D remains an implementation behind an
engine-owned rigid contract; it is not an engine object model, identity system,
or public API. This document describes the rigid boundary and its coordination
with deformation. See [Soft-body Physics Foundation 2](SoftBodyPhysicsFoundation2.md)
for the current deformable job, collision, and rubber architecture, and
[Foundation 1](SoftBodyPhysicsFoundation.md) for the original semantic,
renderer, and persistence baseline.

The expansion contracts are [body controls and groups](PhysicsFoundationBodyControls.md),
[shape casts and overlaps](PhysicsQueryFoundation1.md),
[authored joints](PhysicsFoundationConstraints.md),
[bounded moving-body CCD](PhysicsFoundationContinuousCollision.md), and
[soft attachment control and diagnostics](PhysicsFoundationSoftControl.md).
Their tests and exact campaign qualification evidence determine platform coverage;
API presence alone is not a qualification receipt.

```text
BasePart / Constraint semantics
        -> WorldRoot coordination and safe points
        -> PhysicsWorld
        -> IPhysicsBackend
        -> Box3DPhysicsBackend
        -> Box3D

Cloth / RubberBody semantics
        -> WorldRoot coordination and safe points
        -> SoftBodyWorld
        -> ISoftBodyBackend
        -> XpbdSoftBodyBackend
        -> existing bounded JobSystem workers
```

`WorldRoot` owns one `PhysicsWorld` and one `SoftBodyWorld`. Each world owns one
backend instance. The rigid backend privately owns the Box3D world, bodies,
shapes, joints, transient event arrays, and backend-handle mappings. Neither
backend can expose its native storage through the other contract.

## Neutral semantic vocabulary

The engine-facing contract is defined by `PhysicsTypes.hpp` and
`PhysicsBackend.hpp`:

- `PhysicsBodyId` and `PhysicsConstraintId` are slot/generation value IDs;
- `PhysicsShapeDesc` describes the supported Part primitives, size, and optional
  immutable collision triangle resource;
- `PhysicsBodyDesc` describes transform, shape, motion mode, collision group/mask,
  damping, gravity scale, sleep, moving-body CCD, touch, and density;
- `PhysicsConstraintDesc` describes weld, ball socket, hinge, slider and
  distance joints between neutral body IDs, with local frames and bounded
  limit, motor and spring parameters;
- `PhysicsWorldConfig` and `PhysicsStepConfig` define gravity and explicit fixed-step input;
- `PhysicsBodyMotion` and `PhysicsContactEvent` are owned post-step results;
- `PhysicsKinematicMotionRequest` and `PhysicsKinematicMotionResult` describe a
  bounded capsule translation, collision resolution, velocity clipping, and
  floor/contact normals; and
- `PhysicsOperationResult` reports neutral success, invalid-ID,
  invalid-description, resource-limit, or backend-failure states.

The rigid contract intentionally has no controller policy,
projectile, network ownership, prediction, rollback, or deformable vocabulary.
Deformable vocabulary lives in the separate `SoftBodyTypes.hpp` contract.

## Identity and mapping

Physics identity is independent of `ObjectId` and independent of Box3D handles.
An Instance may currently have one body, but that association is a `WorldRoot`
coordination detail rather than an identity equivalence.

Each backend ID contains a nonzero slot and generation. Destroy invalidates the
slot and increments its generation before reuse. Invalid, stale, and duplicate
destroy operations fail closed with a structured result. Generation exhaustion
retires the slot rather than wrapping it.

The Box3D adapter privately maps a live neutral ID to a body record containing
the current `b3BodyId` and its primary/additional `b3ShapeId` values. Box3D body user data points only to a
backend-owned stable record. The adapter normalizes that record back to a
generation-checked `PhysicsBodyId` before returning an event. No Instance pointer
is stored in Box3D.

Shape replacement does not change `PhysicsBodyId`. This permits size, primitive,
collision, and touch changes without making backend reconstruction observable to
the engine. Destroying a body invalidates attached neutral constraint IDs before
the body slot can be reused.

## Shapes and bodies

`Part::GetPhysicsShape` converts public `PartType` and `Size` into neutral shape
semantics. Box, ball, cylinder, wedge, and corner-wedge Box3D geometry is
constructed only inside `Box3DPhysicsBackend.cpp`.

`MeshPart` defaults to inherited Box collision. `CollisionMode = StaticMesh`
resolves a separately validated canonical Collision asset from `AssetService`;
render triangles do not enter the physics contract. Its neutral immutable
resource contains positions, indices, asset reference, and revision. The adapter
reuses a native chunk set for bodies on the same revision; each consumer projects
one shape per chunk. Chunks contain at most 255 triangles and retain global edge
adjacency, so the backend's 256-triangle contact cache cannot omit a candidate.
MeshPart.Size scales canonical vertices per axis, matching
the renderer. Native mesh data retires after its last live shape.
Before native shape creation, the adapter rejects scaled triangles whose cross
product squared is below `1e-10`, non-finite scaled coordinates, and transformed
mesh vertices or body translation beyond 100,000 world units. This is the pinned
Box3D float-coordinate sanity bound; rejected updates keep the prior shape,
including transform-only updates.
`CFrame` can contain a directly constructed non-rigid matrix. Static mesh
collision requires unit orthogonal axes with positive handedness within
`1e-5`; shear, reflection, and non-unit bases fail before native mutation.
World-bound checks use the same normalized quaternion rotation as the Box3D
adapter, so a matrix accepted by this gate is checked in the projected pose.

Box3D creates mesh contacts only on static bodies. Unanchored MeshParts that
request `StaticMesh` use the inherited Box collision, including anchor
transitions, and receive one bounded `[Physics:Mesh]` diagnostic per WorldRoot.
Unavailable collision content fails the requested update, retaining any previous
coherent backend shape and neutral body identity. Initial body creation waits
for valid content. A bounded `[Physics:Mesh]` diagnostic reports failure. The
asset change journal schedules safe-point retry when content changes.
For Size components below 0.01, Box3D would silently clamp mesh scale; the
semantic projection instead uses the inherited Box with a bounded diagnostic.
Invalid direct neutral mesh descriptions fail closed before Box3D admission.

`BasePart` does not create Box3D shapes or hold a backend handle. `WorldRoot`
builds a `PhysicsBodyDesc` from committed Part state. The backend chooses whether
an update is an in-place transform/body-type change or a
private shape replacement.
`WorldRoot` keeps its successful Part admissions in traversal order and indexes
membership by the generation-bearing `ObjectId`. Repeated safe-point admission
does not append the same Part again; removal and shutdown retire the index with
the ordered entry. This avoids scanning all previously admitted Parts for each
new body while preserving the existing traversal order.
Post-step physics-authored CFrame publication suppresses feedback only for that
Part's own CFrame notification. A synchronous observer can author another Part
or another property on the same Part; those edits remain dirty for the next safe
point. If an observer changes the publishing Part's final CFrame, `WorldRoot`
marks it dirty after the callback returns so the authored pose reaches the
backend. The suppression scope is restored on callback failure.
Tracked Parts also have a private per-registration dirty binding. Every existing
property callback still runs in connection order, but repeated internal dirty
marks can reuse the binding flag after the first successful set insertion.
The flag resets before backend application so reentrant changes remain pending.
Removal and shutdown deactivate bindings before disconnecting callbacks; a held
old callback cannot dirty a replacement registration or access a retired world.
If first insertion fails, a conservative full tracked-Part scan remains pending
for the next safe point. Building that scan must succeed before its flag clears.
This derived cache changes neither ordinary mutation history nor the physics
backend's accepted state, control accumulation, motion order or solver.
CanTouch changes replace shapes transactionally: the pinned backend copies event
flags when creating a contact and does not refresh them for an in-place flag write.

An unsuccessful shape replacement leaves the previous backend shape and neutral
identity intact. The update reports failure, `WorldRoot` logs it with the
`[Physics:Backend]` or bounded `[Physics:Mesh]` prefix, and the dirty update
remains eligible for retry. The
engine does not claim silent physics synchronization success.

`PhysicsWorld::GetCollisionResidency` reports active shared resources, active
shape consumers, neutral position/index bytes, native Box3D mesh-data bytes,
failed mesh admissions, and cumulative admitted/retired resource and consumer
counts. A bounded last-failure message identifies the rejected ceiling. Each
PhysicsWorld admits at most 1,024 distinct live native mesh
resources, 65,536 consumers, 65,536 projected mesh shapes, 64 MiB of retained neutral position/index
buffers, and 64 MiB of `b3MeshData::byteCount`. `PhysicsWorldConfig` may lower
each ceiling for a world but cannot raise the hard maximum. Shared consumers
charge one resource and one canonical/native chunk set, plus one consumer and
the set's shape count each. Candidate and prior shapes are both charged until replacement succeeds.
When a ceiling denies admission, the prior shape and body identity remain live
and retryable; final retirement releases the budget. Build admission conservatively
reserves the complete global adjacency mesh plus staged chunk blobs within the
remaining native-byte budget, using geometry-derived blob upper bounds before
construction. The reported native bytes sum retained chunks. `PeakBuildNativeBytes`
records peak charged blobs for completed builds; builder scratch, C++ arrays and
unsuccessful-build peaks are excluded. A denied candidate is destroyed before publication. Box3D does not expose
per-shape storage size, so native shape bytes remain explicitly unknown. Weak
cache entries do not pin a resource or prevent retirement.
The neutral collision resource key is capped at 128 bytes before caching.
Read-only residency additionally reports `NativeChunks`, `ChunkTriangleLimit`,
`MeshShapeLimit`, active/created/retired mesh shapes and completed build peak bytes.
`CollisionMeshShapeLimit` lowers the mesh projection ceiling without changing
primitive admission or any residency-demand policy.

## Main-domain safe points

Generated setters commit authoritative Instance state and then fire their normal
property signal. For a tracked `BasePart`, `WorldRoot` observes `CFrame`, `Size`,
`Anchored`, `CanCollide`, `CanTouch`, and `Shape` and records only a dirty neutral
body intent. It does not call Box3D from the setter callback.

For MeshPart, it also observes `Mesh` and `CollisionMode`.

At the beginning of `StepPhysics`, on Main and before any Box3D step,
`WorldRoot` applies dirty body updates in sorted `ObjectId` order. Gravity and
constraint changes use the same pre-step boundary. Pending impulses are applied
immediately before a fixed simulation step. Removal or destruction erases dirty
work before destroying the body, so a dead Part cannot be recreated by an older
intent.

Mesh asset revision changes are observed through the bounded AssetService
journal at the same safe point. Matching MeshParts are marked dirty; a journal
gap causes a rescan of tracked Parts. Failed shape replacement preserves the
previous body, shape, and constraint identity for retry.
Detached stack WorldRoots have no asset observer and remain usable for native
kinematic queries; asset observation does not call shared ownership APIs on them.

Creating or removing a descendant is also a Main-domain non-stepping safe point.
Those operations may create or destroy neutral bodies immediately because the
current engine has one serialized Main simulation domain and cannot execute them
concurrently with `IPhysicsBackend::Step`.

## Constraints

`Constraint`, `WeldConstraint` and `JointConstraint` expose neutral constraint
descriptions only. `JointConstraint` resolves direct-child attachment frames and
supports ball sockets, limited/motorized hinges, limited/motorized sliders and
rigid or spring distance joints. See
[Physics constraint expansion](PhysicsFoundationConstraints.md) for exact
authoring fields, units and failure behavior.
`WorldRoot` resolves endpoint Parts to neutral body IDs, validates that the
constraint and both endpoints belong to the same world, and asks `PhysicsWorld`
to create the joint.

Changes to enabled state, endpoints, attachment frames and authored joint
parameters are queued for the pre-step safe point. Native admission is capped
at 4,096 live joints per world by default; `PhysicsWorldConfig::ConstraintLimit`
may lower that cap. `TryCreateConstraint` returns a structured `ResourceLimit`
result when full, while the older ID-returning `CreateConstraint` remains a
compatibility wrapper. Replacement creates a candidate before destroying its
predecessor, so a full world retains the old joint until capacity becomes
available. WorldRoot logs one bounded diagnostic for a failed authored update.
Body shape replacement preserves the Box3D body and attached joint. Body removal
causes the backend to invalidate attached constraint IDs; `WorldRoot` then drops
the stale association and can reconcile a still-live constraint if its endpoints
later become valid again.

## Step and publication

`WorldRoot` retains the existing 60 Hz fixed step, four substeps, four-step
per-frame cap, and accumulator reset after the cap. The backend owns direct world
stepping and copies transient Box3D move/contact/sensor arrays into neutral
vectors before returning. Each motion/contact collection is capped at 65,536
entries per step; truncation is explicit in the result and logged by `WorldRoot`.

Dynamic body motion is resolved from `PhysicsBodyId` to a live tracked Part. The
result is published through the ordinary authoritative `SetCFrame` path. A
narrow publication guard prevents that physics-authored write from being queued
back into the backend. Fuzzy equality suppresses no-op publication.

The experimental selective readback path retains the ordered Part list and
authoritative backend state. `WorldRoot` counts tracked bodies from accepted
create/update state and removal, without a new backend API. An unavailable
state makes that count unknown and selects an ordered backend read to recover
it. At a fixed-step safe point, a world with at least 8,192 Parts and no more
than one-eighth accepted dynamic bodies may build a derived dynamic-only
readback view. Once active, it retires that view below 4,096 Parts or above
one-quarter dynamic bodies; smaller or denser worlds use ordered readback and
retain no derived index. The gap limits threshold flapping. Identity, body
generation, weak lifetime, and accepted motion mode are checked before using
each entry. A mismatch or allocation failure falls back to an ordered read in
the same step and retries rebuilding later. These thresholds are provisional
benchmark policy, not an engine guarantee or solver behavior change.

This is the current authoritative transform model. It does not yet distinguish
server physics, replicated transforms, prediction, or developer ownership.

## Contact and sensor events

The Box3D adapter enables contact/sensor event collection according to neutral
`CanCollide` and `CanTouch` state. It copies both contact and sensor begin/end
events and tracks native shape-pair membership against generation-checked body
pairs. It applies the complete native event batch before emitting a neutral
zero-to-positive Begin or positive-to-zero End transition. Crossing a native chunk
seam does not end a still-touching body pair. Destroying a body purges its contact
membership; shape replacement retains old pair identity until queued End events
are applied. Native pairs and changed body pairs are each capped at 65536. An
observation overflow latches `EventsTruncated` for the remaining world lifetime,
marking subsequent event history incomplete until teardown/reset. Physical
collision still evaluates all admitted chunks.

`WorldRoot` resolves both bodies to live Parts before firing `Touched` or
`TouchEnded`. Each Part receives the signal only when its current `CanTouch` is
true. No Box3D callback/event structure or borrowed event memory reaches an
Instance.

## Queries

The two active gameplay queries are `Workspace:Raycast(Origin, Direction,
Params?)` and `Workspace:MoveKinematicCapsule(Position, Radius, Height,
Translation, Velocity)`. `WorldRoot` flushes pending body changes before either
query and passes only neutral requests to `PhysicsWorld`. Returned Luau values
own copied semantic results and contain no body handle or borrowed backend
memory. See [Physics Query Foundation 1](PhysicsQueryFoundation1.md) for the
raycast API, filter, identity, ordering, timing, limits, LOS integration, and
benchmark evidence.

The Box3D adapter uses `b3World_CollideMover`, `b3SolvePlanes`,
`b3World_CastMover`, and `b3ClipVector`. It resolves contact planes toward the
requested target before each cast, which preserves tangential horizontal motion
while gravity is clipped by a floor. Work is bounded at five mover iterations
and 32 collision planes per collection. Reaching the plane bound is explicit in
`PlanesTruncated`. Current `CanCollide` state filters both plane collection and
casts. Non-finite positions/translations/velocities, non-positive radii, and
heights below the capsule diameter fail closed.
Exact duplicate mover planes are coalesced across chunks. Ray candidates are
deduplicated to the nearest hit per neutral body before the candidate bound;
ties within a body use a deterministic normal ordering. Query scratch epochs in
native body records are disposable derived state, accessed on the existing Main
physics/query boundary.

This primitive supplies collision authority rather than movement policy. The
engine-shipped Luau controller decides gravity integration, walk speed, floor
slope acceptance, jump/air-jump rules, and bounded step attempts. A replacement
controller can call the same method directly. Shape casts, overlap APIs,
moving-platform metadata, and soft-body query participation are not implemented.

## Failure and lifetime behavior

- invalid and stale body/constraint IDs fail closed;
- duplicate destruction is safe and returns an invalid-ID status;
- non-finite or non-positive body descriptions are rejected;
- failed shape replacement preserves the prior coherent backend state;
- body destruction first destroys and invalidates attached constraints;
- post-step results own all data needed outside the backend;
- `WorldRoot::Destroying` and the native destructor both run one idempotent shutdown path;
- shutdown disconnects property/descendant observers and clears pending work before backend destruction; and
- the backend destroys joints before bodies and destroys the Box3D world last.

## Box3D confinement and replaceability

### Settled all-static world no-work path (3L investigation)

The adapter maintains one live dynamic-body count and a static-world dirty
flag. After a complete step processes mutations, an entirely static world
returns fresh empty motion/contact events until a participating mutation.
It never replays native event buffers. Even sleeping dynamic bodies always
take the full backend step. There is no new queue, partial activation, public
budget, worker-thread mutation or alternative transform authority.

Body create/destroy, anchor transitions, constraint create/destroy, gravity,
impulse and collision/touch-participating body updates invalidate the no-work
path. Only updates whose old and new descriptions are both anchored,
non-colliding and non-touching can leave it clean. Native transforms, shape
replacement and broadphase query proxies still update immediately; queries do
not wait for a later step. Box3D's moved-proxy bitset coalesces repeat moves of
one live proxy and destruction removes its entry, so these moves do not add
an unbounded deferred update list. Generation validation occurs before state
accounting. Dynamic count follows successful native body creation, failed
shape cleanup, anchor changes and valid destruction.

This is not a work cap for dynamic worlds, frequently mutated static worlds,
dense sensor overlaps, or atomic content activation. Those retain their
existing costs/limits. `TestStaticWorldStepReference` compares contact events
and body states against a world forced to run every native step by a distant,
non-interacting dynamic body. It covers seeded shape/touch/transform changes,
idle no-replay, destroy/reload, stale identities, dynamic transitions,
constraints and immediate queries. The current targeted MSVC and Clang 19
ASan/UBSan/LSan tests pass; end-to-end envelope validation is recorded separately in
the [3L.3 ledger](ContentAvailabilityFoundation3L_3Validation.md).

Runtime Box3D includes and `b3*` symbols are confined to
`src/physics/Box3DPhysicsBackend.cpp`, its private conversion header and
`src/physics/Box3DMeshChunks.hpp`. CMake
still names the Box3D dependency and include directory; that is unavoidable
build/dependency integration rather than a runtime semantic leak.

The pinned Box3D submodule `8441b4a06d6d09dcfb0b0f704df4d847d1437b92`
has an SSE2 `b3LoadV` load that casts a 4-byte-aligned three-float vector to
`double*`. Alignment sanitizers reject this before a physics raycast can return.
`cmake/Box3DCompatibility.cmake` verifies the exact pinned `src/simd.h` SHA256
(LF `688fef3ccbbda3f0923ab5a227bf143408ae02aefa63f79ffbb1798ec71c37b4`,
Windows CRLF `ceab82f8c72fe58dc74f2ba0c8db1f721a793a859fc9da3cf058a18028bbfd62`), copies the
required Box3D build sources and public headers into the build directory, and
patches that private `simd.h`. The correction copies the first eight bytes
into an aligned local `double` before `_mm_load_sd`, preserving the three float
lanes and zero fourth lane. The vendor checkout and public Box3D API remain
unchanged; unknown source hashes fail configuration. A direct 4-byte-aligned
12-byte-lane test and an unknown-source guard test cover the compatibility
boundary, while the physics and asset sanitizer suites exercise actual queries.

The same checked private-copy recipe corrects the pinned block allocator's
unaligned free-list pointer load/store using `memcpy`, without changing element
stride, layout or byte accounting. Original `block_allocator.c` hashes are LF
`0e7f3cf86b6ae33d4650853681f409d2a9f9259801dbeb56914f3da0a376fd60`
and CRLF `7d663bd7b0690fad773f332c7dee71c8965f6dcb06fab10695ad9aa185b38076`;
the patched canonical LF hash is
`0863d789b5cc260a6645e4e2430e501e974235a9f1c369e3b4e272acc8316794`.
The C17 regression uses 12/20-byte strides, growth/preallocation, LIFO reuse and
final byte balance. Smoke/incremental guards cover both patched input files.

### Dense mesh contact correction (KI-2B-001)

The pinned Box3D rigid-contact path has a 256-candidate local triangle cache.
The old single-shape projection could fill it with noncontacting AABB candidates
and miss a real support surface. A 300-decoy/one-support regression proves that
miss in the old Windows/Linux profiles while a filtered ray still hits support.
The adapter now builds globally identified adjacency, partitions spatial BVH order
into at most 255 triangles per native shape, and preserves edge flags through each
chunk's triangle reorder. No native contact query can overflow on an admitted
chunk. The vendor source and canonical artifact formats are unchanged.

Shared-resource accounting, staged rollback and body-level event aggregation
preserve lifetime and identity across chunks. Standalone tests check edge flags,
geometry coverage, hashes and budgets; physics tests check the missed contact,
seam transitions, shared shapes, denial/recovery and stale identities. Exact
qualification and remaining independent physics limits belong in the
[KI-2B-001 report](../Validation/DenseMeshContactKI2B001.md). This corrects candidate
omission; it is not a claim of mathematically exact contacts for arbitrary geometry.

`IPhysicsBackend` can be implemented and tested without including Box3D. The
focused test suite supplies a recording backend to prove neutral creation,
update, and step delegation. A different rigid backend can implement the
contract without changing BasePart, Constraint, networking, rendering,
persistence, or Luau APIs. Selecting the default adapter remains an internal
physics implementation choice.

## Deliberately deferred

This rigid boundary does not implement another rigid engine, constraint
breakage, dynamic concave collision, a native character motor, projectiles, terrain
physics, physics networking, ownership, interpolation, prediction, or rollback.
Deformation is implemented only through the sibling boundary documented above.
Rendering has no dependency on either physics backend's native handles.
