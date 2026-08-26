---
status: current
owner: physics
last_verified: 2026-08-26
related_code:
  - assets/classes/WorldRoot.luau
  - include/gargantuan/datatypes/RaycastParams.hpp
  - include/gargantuan/physics/PhysicsTypes.hpp
  - include/gargantuan/physics/PhysicsBackend.hpp
  - include/gargantuan/classes/WorldRoot.hpp
  - src/datatypes/RaycastParams.cpp
  - src/classes/WorldRoot.cpp
  - src/physics/Box3DPhysicsBackend.cpp
  - tests/PhysicsBackendTests.cpp
  - tests/PhysicsQueryBenchmark.cpp
related_adrs: []
---

# Physics query foundation 1

## Public API and ownership

`Workspace` is the public query owner because it already exposes the live
`WorldRoot` spatial and kinematic query surface. A separate `PhysicsService`
would duplicate world selection without improving the backend boundary. Normal
gameplay Luau can call:

```luau
local Params = RaycastParams.new()
Params.FilterType = Enum.RaycastFilterType.Exclude
Params.FilterDescendantsInstances = { LocalPlayer.Character }

local Result = Workspace:Raycast(Origin, Direction, Params)
```

The exact signature is:

```luau
Workspace:Raycast(
    Origin: Vector3,
    Direction: Vector3,
    Params: RaycastParams?
) -> {
    Instance: BasePart,
    Position: Vector3,
    Normal: Vector3,
    Distance: number,
}?
```

`Origin` is a world-space point. `Direction` is a world-space vector whose
magnitude is the maximum distance and whose normalized value is the cast
direction. A valid miss returns `nil`. Invalid arguments use the ordinary
native-to-Luau error path. The returned table is read-only and owns a snapshot
of the hit position, normalized world normal, and distance. It contains the
authoritative live `BasePart`, not a renderer or backend identifier. Material
is omitted because asset material and physical material do not yet share a
clean gameplay contract.

## RaycastParams and filtering

`RaycastParams` is mutable native userdata local to a Luau VM; it is not an
`Instance`, journal record, replicated object, or persistence format. It has:

- `FilterType: Enum.RaycastFilterType`, default `Exclude`;
- `FilterDescendantsInstances: { Instance }`, default empty.

The assignment copies and deduplicates a contiguous array of at most 128 live
Instance roots. It stores weak references, so a params value does not keep a
removed DataModel hierarchy alive. A destroyed root makes query preparation
fail closed. Nil/non-Instance entries, holes, nonnumeric keys, invalid enum
types, and more than 128 entries are rejected at assignment. A root from a
different `WorldRoot` is rejected at query time.

Filtering is semantic. `Exclude` removes bodies whose owning `BasePart` is a
root or descendant of a root. `Include` admits only such bodies. `WorldRoot`
walks each root hierarchy once while preparing the query, resolves Parts through
its existing body registry, deduplicates neutral `PhysicsBodyId` values, and
sorts the final body set. Box3D receives only that bounded neutral set and a
filter enum; it never receives Luau values or Instance pointers.

## Neutral backend boundary and identity

The query path is:

```text
gameplay Luau
    -> Workspace / WorldRoot semantic validation and filter preparation
    -> PhysicsWorld::Raycast
    -> IPhysicsBackend::Raycast(PhysicsRaycastRequest)
    -> backend-neutral PhysicsRaycastHit candidates
    -> WorldRoot generation-safe owner resolution and canonical result
```

`PhysicsRaycastRequest` contains only finite neutral vectors and a sorted
`PhysicsQueryFilter`. Each candidate contains a generation-safe
`PhysicsBodyId`, world position, world normal, and distance. `PhysicsWorld` is
only delegation and has no Box3D knowledge.

`Box3DPhysicsBackend` implements the contract with the pinned public
`b3World_CastRay` callback API and the same shapes used by simulation. Its
private shape-owner map resolves a `b3ShapeId` to the backend's current
slot/generation `PhysicsBodyId`. The callback copies hits before returning;
neither borrowed callbacks nor `b3*` values escape. Invalid normals or positions
fail the query rather than publishing NaN, and `WorldRoot` emits at most one
`[Physics:Query]` backend-failure diagnostic during its lifetime. A different
rigid backend can implement the same request/result types without changing
Luau, `WorldRoot`, or InteractionService.

`WorldRoot` already owns the bidirectional mapping between authoritative Part
`ObjectId` values and neutral body IDs. There is no second collider registry.
A backend candidate is accepted only if both the body generation and the weak
semantic Part mapping are still live and in this world. Destroy, body rebuild,
slot reuse, reparent, Play/Stop, and query-result lifetime therefore cannot make
a stale hit resolve to a replacement Instance.

## Ordering and snapshot timing

Box3D may report candidates in implementation-dependent callback order, so the
public closest hit is selected at the semantic boundary. Materially smaller
distance wins. Distances inside
`max(1e-5, 8 * float_epsilon * max(1, abs(distance)))` are treated as an
effective tie, and the smallest generation-safe semantic `ObjectId` wins.
Backend body ID is used only to make the copied candidate vector stable before
semantic resolution; it is not gameplay ordering authority.

Queries are synchronous on the serialized Main simulation domain. Before a
cast, `WorldRoot` flushes sorted pending CFrame, Size, Shape, Anchored,
CanCollide, and CanTouch changes through the same safe point used before a
physics step. The query then observes that coherent committed state and the
last completed simulation step. It never runs concurrently with a Box3D step.
Gameplay calls during Heartbeat/PostSimulation see the completed current-frame
physics state; InteractionService evaluates after PostSimulation and before
PreRender. No renderer, GPU, Studio process, or async query worker participates.

## Bounds and participation

| Boundary | Limit / behavior |
| --- | --- |
| Direction magnitude | `[0.0001, 100000]` world units; zero, NaN, Inf, and larger casts fail |
| Filter roots | 128 assigned roots |
| Descendant traversal | 16,384 semantic objects while preparing a filter |
| Resolved filter bodies | 4,096 neutral bodies |
| Backend hit candidates | 4,096 copied candidates |
| Overflow | explicit query failure; no partial closest-hit claim |

Foundation 1 queries rigid `BasePart` colliders only. Box, ball, cylinder,
wedge, and corner-wedge use their actual current Box3D simulation geometry,
including current approximations inherent in those shapes. A body participates
only while `CanCollide` is true; the existing schema has no separate `CanQuery`
property. Soft-body cloth/rubber participation is deferred and is not faked by
inserting XPBD meshes into Box3D.

The public call has no additional global per-frame quota. Its input, traversal,
filter-body, candidate, and distance work are already hard bounded. Higher-level
systems remain responsible for narrowing how often they query; InteractionService
does so explicitly.

## Interaction line of sight

`ProximityPrompt.RequiresLineOfSight` is a saved, future-replicated Boolean with
default `false`, preserving existing game behavior. When true,
InteractionService casts from the authoritative character interaction origin to
the exact resolved prompt anchor, including Attachment offsets. It excludes the
interacting player's character with ordinary `RaycastParams`. A miss is visible;
a hit is also visible when the hit Part is in the prompt's parent/anchor
hierarchy. Any other closest rigid hit blocks the prompt.

Candidate distance and ObjectId ordering occurs first. At most eight candidates
are LOS-tested per player query. Begin activation, every hold update/completion,
and final zero-duration press validation repeat the same general Workspace
raycast, so moving an occluder into a hold cancels it and inserting one between
presentation and press prevents `Triggered`. The query is completed before the
gameplay callback runs, so a callback may destroy physics objects safely.

## Validation and benchmark evidence

Release tests cover hit/miss, exact endpoint, invalid vectors and distance,
read-only result snapshots, include/exclude/descendant/deduplicated filters,
bounded Lua arrays, destroyed/cross-world roots, body destruction and recreate,
rotated and transformed colliders, all five current primitive types, stable
equal-distance ties, repeated Luau casts, headless operation, and an editor
atomic CFrame+Size transaction followed by a live collider raycast. Interaction
coverage includes own-character exclusion, target acceptance, Attachment
anchors, obstruction/opt-out, hold cancellation, target movement, final press
revalidation, persistence, FirstCompleteGame, and Play/Stop isolation.

The local Windows Release benchmark on 2026-08-26 used 512 samples per case and
1, 100, 1K, and 10K collider layouts. Values are regression evidence, not an
SLA. The process could not read the host CPU model, so no unverified hardware
name or secondary-host result is attached.

| 10K-collider case | Total P50 / P95 / P99 | Dominant P95 component |
| --- | ---: | ---: |
| Sparse nearest hit | 4.6 / 5.8 / 6.9 us | backend 5.4 us |
| Dense nearest hit | 39.9 / 52.6 / 57.4 us | backend 49.6 us |
| Corridor nearest hit | 10.1 / 13.4 / 14.7 us | backend 12.8 us |
| Sparse miss | 0.5 / 0.5 / 0.7 us | backend 0.5 us |
| Dense short ray | 10.5 / 13.9 / 16.3 us | backend 12.5 us |
| Dense 4,096-body subtree exclusion | 631.6 / 938.7 / 1247.0 us | filter preparation 866.5 us |

Semantic owner resolution remained at or below 0.3 us P95 in the reported 10K
cases. The large-subtree case intentionally pays for the one bounded hierarchy
walk; filters with one root/body remain sub-microsecond to prepare.

## Extension path

Foundation 2 can add `RaycastAll`, sphere/block casts, and sphere/box overlaps
by adding small neutral request/result types beside `PhysicsRaycastRequest`.
They should reuse `PhysicsQueryFilter`, the `WorldRoot` filter builder, neutral
slot/generation identities, semantic owner resolution, finite coordinate and
candidate bounds, and Main-domain safe point. Public APIs should be added only
when implemented; there are no no-op placeholders or generic callback DSL.

Other priorities are a deliberate `CanQuery`/sensor participation decision,
soft-body query semantics without rigid duplication, richer per-collider
identity if compound colliders land, and authoritative server-side use of the
same Workspace boundary for hitscan and visibility validation.
