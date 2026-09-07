---
status: current
owner: runtime
last_verified: 2026-09-03
related_code:
  - include/gargantuan/runtime/SpatialTypes.hpp
  - include/gargantuan/runtime/SpatialRuntimeProjection.hpp
  - src/runtime/SpatialRuntimeProjection.cpp
  - include/gargantuan/runtime/SpatialRegionIndex.hpp
  - src/runtime/SpatialRegionIndex.cpp
  - include/gargantuan/network/ReplicationRelevance.hpp
  - src/network/ReplicationRelevance.cpp
  - tests/SpatialRegionIndexTests.cpp
  - tests/SpatialRegionIndexBenchmark.cpp
  - tests/ReplicationRelevanceTests.cpp
related_adrs:
  - devdocs/CurrentArchitecture/CharacterReplicationFoundation3H.md
  - devdocs/CurrentArchitecture/PhysicsBackend.md
  - devdocs/CurrentArchitecture/RenderExtraction.md
---

# Spatial runtime projection foundation 3K

Foundation 3K establishes one narrow rule for orthogonal runtime participation:

> Inheritance expresses semantic API taxonomy. Runtime participation in an
> orthogonal engine subsystem is represented by subsystem-owned projection
> state, not capability inheritance.

The first concrete projection is spatial. It separates authoritative Instance
state, derived runtime placement, and rebuildable acceleration cells without
creating a generic ECS, facet framework, streaming system, or public space API.

## Authority and ownership

The direction of authority is one way:

```text
authoritative Instance/ObjectId generation
    -> committed semantic CFrame or bounds mutation
    -> SpatialRuntimeProjectionStore synchronization
    -> SpatialRegionIndex cell memberships
    -> 3H candidate ObjectIds
    -> 3E semantic peer relevance
```

`Instance` remains the authority for identity, hierarchy, lifecycle, reflected
properties, serialization, scripting, and replication. `BasePart.CFrame`,
`BasePart.Size`, and `Character.CFrame` remain semantic state. A projection
cannot mutate an Instance, grant control, select LocalPlayer, define relevance,
materialize an object, or advance Character publication history.

`SpatialRuntimeProjectionStore` owns spatial projection records. The current
server `ReplicationRelevance` context owns the store lifetime, but no longer
owns duplicate projection position or dirty state. The store contains no
`Instance` pointer. The relevance layer retains only its weak semantic object
reference, root membership set, and signal connections, then resolves current
semantic state at the established Main-safe relevance point.

The acceleration index owns only cell membership metadata. It no longer stores
a second copy of bounds. This gives three explicit categories:

| category | representation | owner | authority |
| --- | --- | --- | --- |
| semantic object state | `Instance`, `ObjectId`, CFrame/Size/hierarchy | DataModel | authoritative |
| runtime spatial projection | `SpatialRuntimeProjection` | spatial store | derived from semantic state |
| acceleration state | `SpatialCellAddress`, region buckets | region index | rebuildable optimization |

## Pose, space, and cell types

The former `SpatialAddress` was an index bucket key despite its location-like
name. 3K replaces it rather than preserving an ambiguous alias:

- `SpatialPose { SpatialSpaceId Space; CFrame LocalTransform; }` represents
  semantic placement supplied to the projection boundary.
- `SpatialCellAddress { SpatialSpaceId Space; SpatialCellCoordinate Cell; }`
  represents derived acceleration metadata.
- `SpatialCellAddressForPosition` is the explicit cell conversion operation.

A cell address is not serialized, journaled, replicated, or used as identity.
It may be destroyed and rebuilt without changing observable world semantics.
The stable FNV hash includes both space slot and generation plus all three
signed cell coordinates. It is diagnostic distribution data, not security or
identity.

`SpatialSpaceId { uint32 Slot, uint32 Generation }` is a strong internal
identity. Zero in either component is invalid. It is not a pointer, hierarchy
path, name, allocation address, client value, or public Luau value.

`DefaultSpatialSpace {1,1}` is created with every store and cannot be destroyed.
Production 3H/3E creates a one-space store and routes all existing content and
trusted focus queries explicitly through it. Existing packages therefore keep
their exact single-space behavior without a new property or migration.

The generic store supports at most 256 configured spaces and defaults to 16 for
internal use. Destroyed empty slots increment generation before reuse. A slot
at maximum generation retires instead of wrapping. An occupied space cannot be
destroyed, and a stale generation fails registration, transfer, and query.

## Projection representation and lifecycle

One projection contains:

```text
full ObjectId association
SpatialPose
space-local SpatialBounds
wide saturating revision
one dirty bit
```

The full ObjectId is the store key and therefore includes authoritative object
generation. Storage scales with actual spatial participants, cell memberships,
and configured space slots. Non-spatial Instances receive no projection.

Creation validates ObjectId, live space generation, finite transform, bounds,
and every index limit. The index membership is prepared and committed first;
only then is the projection published. Projection allocation failure removes
the just-created index entry. There is never a committed index entry without a
projection after the operation returns.

Removal first removes every index membership, decrements the owning space's
live projection count, and erases the projection. Stale dirty ObjectIds may
remain only in the fixed-capacity dirty vector until the next safe drain; full
ObjectId lookup makes them inert. They never resolve to a reused slot.

No projection owns or prolongs an Instance. The production verifier requires:

```text
one live spatial root relation
    == one live full-generation projection
    == one generation-matched ObjectRegistry object
    == one index entry in the same space
```

It also reconstructs authoritative CFrame/bounds and compares them with the
projection, making safe-point drift observable rather than silent.

## Mutation and dirty coalescing

Committed Character/BasePart CFrame and BasePart Size signals call `MarkDirty`.
The store keeps one dirty bit plus one pre-reserved ObjectId vector bounded by
the configured projection ceiling. Repeated changes before synchronization
coalesce; there is no task, callback object, timer, or historical transform
queue per movement.

At the normal relevance safe point, `ReplicationRelevance` resolves the newest
semantic CFrame/bounds once and calls `Update`. Index update derives and checks
the complete candidate membership, installs new memberships while the old
entry is still valid, removes retired memberships, and finally commits the
projection pose/bounds/revision. A validation/resource failure leaves the old
index and projection intact, marks relevance unhealthy, and prevents further
selection. Semantic state is never silently paired with an old healthy index.

Same-cell movement updates the projection and cell entry revision without
bucket churn. Crossing or teleporting considers only old and final membership;
it never walks the intervening path.

## Transactional isolated-space transfer

The internal `Transfer` operation preserves the same ObjectId generation:

```text
validate destination space, pose, and bounds
    -> derive and capacity-check destination cells
    -> insert bounded destination memberships
    -> remove source memberships
    -> commit destination projection and space counts
```

Invalid/stale destination state or index failure leaves source pose, bounds,
revision, space count, and cells unchanged. A successful transfer increments
the projection revision exactly once. There is no intermediate query-visible
state in which the object belongs to both spaces after the operation returns.

This is an internal projection/index capability, not a production gameplay
teleport API. Production physics still owns one `WorldRoot` scene, renderer
extraction reads ordinary semantic CFrames, audio/interaction use
`SemanticSpatialResolver`, and Character authority/control remain single-space.
3K therefore does not pretend that moving a physical Character between isolated
simulation spaces is supported. A future owner must coordinate physics,
renderer, audio, animation, and network transitions before exposing that path.

## Two-space isolation proof

The test-capable store creates two isolated spaces and registers:

```text
DefaultSpace: Part A (0,0,0), Character A (10,0,0)
Space B:      Part B (0,0,0), Character B (10,0,0)
```

Queries explicitly name one space. The DefaultSpace origin query returns only
A identities; the Space B query returns only B identities. The two origin
objects have equal local cell coordinates but unequal `SpatialCellAddress`
values. Per-space large-object buckets likewise prevent the conservative
fallback from leaking candidates across spaces.

The raw index may service several trusted internal query volumes and spaces in
one call, but there is no public cross-space query. The projection store first
validates every space generation. Local queries never scan unrelated-space
large objects, and ordered cell lookup begins at the complete space/cell key.

The Release benchmark adds 10K, 100K, and 999,999 unrelated-space roots while
holding one DefaultSpace root and its local query fixed. This stays within the
existing one-million-object hard ceiling and demonstrates that query work is
tied to local cells/candidates rather than all objects in other spaces.

## Existing subsystem boundaries

- **3H:** now consumes `SpatialCellAddress`; all membership keys carry complete
  space generation. Its sparse bounds, large-object, query, and failure limits
  are otherwise unchanged.
- **3E:** remains sole semantic relevance owner. Space proximity is only a
  candidate source; owner/global/required policy and hysteresis still decide.
- **3J:** desired/known structural state and reliable accepted commit remain
  unchanged. Projection existence never means peer materialization.
- **3F/3G/GCHR:** runtime Character publication begins only after existing
  structural commit and uses unchanged materialization/control epochs. No cell
  or space field was added to GCHR.
- **Physics:** retains its independent generation-safe backend projection in
  `WorldRoot`; simulation output commits authoritative CFrame, which marks the
  spatial projection dirty. 3K does not combine broadphases or physics scenes.
- **Renderer:** still extracts immutable render publications from semantic
  BasePart state. It stores no projection/Instance pointer and performs no
  cross-space extraction in this milestone.
- **Animation/root motion:** still produces authoritative Character motion at
  the normal simulation safe point. It does not own spatial state.
- **Audio/interaction:** continue resolving attachment semantics through
  `SemanticSpatialResolver`; this is not replaced or silently made multi-space.

The authoritative server step therefore remains:

```text
simulation/physics/root motion finalizes semantic transforms
    -> CFrame/Size dirty signals coalesce
    -> 3H projection/index synchronization at relevance safe point
    -> 3H candidate query in DefaultSpace
    -> 3E relevance
    -> 3J structural work
    -> 3F/3G Character publication
```

No renderer, camera, GPU, or client scheduler is required. With no server
session/peer relevance owner, no spatial projection store is created by 3K.

## Bounds, failure, and security

The store adds no unbounded container:

| resource | hard/default behavior |
| --- | --- |
| runtime spaces | hard 256; store default 16; production 1 |
| projections | existing index configured maximum; production 65,536 |
| dirty entries | at most configured projections; pre-reserved |
| cells/memberships | existing 3H limits |
| temporary transactional overlap | at most one object's bounded memberships |
| query scratch | existing pre-reserved 3H limits |

Invalid ObjectId, stale space generation, non-finite pose, invalid bounds,
duplicate/missing projection, occupied space destruction, index capacity, and
allocation failure return explicit native status. No operation silently drops
an object, truncates query output, advances authority, or returns a partial
candidate result. Aggregate saturating metrics expose active spaces/
projections, dirty high-water, create/update/transfer/remove, dirty coalescing,
and failed transactions without per-object labels.

Clients cannot construct a space/projection, choose a cell, transfer an object,
request candidates, affect last projection revision, or use spatial state to
gain Character control. All inputs to this seam are native authoritative state.
Stale ObjectId and space generations cannot bind to replacements. Identical
local coordinates in different spaces do not alias candidates or authority.

## Persistence and compatibility

`SpatialSpaceId`, projection revision/dirty state, bounds cache, cell addresses,
and index memberships are transient runtime data. Project/package snapshots
continue persisting reflected semantic CFrame/Size/hierarchy only. There is:

- no package or journal schema change;
- no GRPL, GCHR, handshake, or Remote protocol change;
- no ordinary Luau property, method, event, or callback;
- no Studio, Node, MCP, or Telemetry repository change;
- no authoring journal record for cell crossing or projection update.

Static DefaultSpace games therefore preserve pre-3K observable semantics and
encoded network/package bytes. The rename is native-source-only and prevents a
rebuildable cell key from being mistaken for semantic location.

## Verification

The spatial suite covers strong space identity, DefaultSpace, two spaces with
identical Part/Character local transforms, per-space queries, distinct cell
keys, transaction success/failure, occupied-space rejection, space generation
reuse, stale ObjectId/dirty rejection, dirty coalescing, lifecycle teardown,
and a deterministic 10,000-operation randomized create/update/transfer/remove
state machine with consistency verification every 37 operations.

It retains all 3H boundary, bounds, large-object, false-negative, query-limit,
100,000-cell churn, and ObjectId generation tests. `ReplicationRelevanceTests`
proves the production DefaultSpace path, semantic movement synchronization,
boundary crossing, Character runtime movement, reparent/destroy cleanup, and
semantic/projection drift verification.

The local MSVC Release verification on 2026-09-03 measured:

| fixture | result |
| --- | --- |
| projection payload layout | 120 bytes (`SpatialPose` 56, `SpatialSpaceId` 8, `SpatialCellAddress` 32) before ordered-map node overhead |
| 100 A + 100 B at identical local origin | each query returned exactly its own 100 identities and zero from the other space |
| 100,000 dirty notifications | one dirty token, 99,999 coalesces |
| transfer/lifetime churn | 100 transfers, 100 object generations, and 100 space generations completed consistent and empty |
| DefaultSpace fixed query, 10K / 100K / 1M objects | 0.057479 / 0.064274 / 0.068682 ms mean; 1,728 candidates; zero query allocations |
| DefaultSpace query with 0 / 10K / 100K / 999,999 unrelated-space objects | 0.000178 / 0.000258 / 0.000348 / 0.000388 ms mean; p99 0.0007 / 0.0010 / 0.0059 / 0.0085 ms; one candidate; zero query allocations |
| 10K same-cell / crossing / teleport updates | 1.0790 / 3.9938 / 3.5545 ms total; same-cell path zero allocations |
| 3J admission, 32 peers | 12.5498 ms full convergence, four ticks, 2,496 selected/committed, zero budget deferrals |
| 3J admission, 500 peers | 6,465.9 ms full convergence, 91 ticks, exact 8,192 selected-state cap, zero journal-lag failures |
| 3F/3G 500-peer mixed Character workload | 305,860 state B/s, 309,460 total B/s, 350 messages/s, 4,000 states/s |

The benchmark's percentile samples include timer/outlier noise at sub-microsecond
scale; the important isolation result is that adding almost one million objects
to another space did not add a scan or candidate to the fixed local query.

The next measured foundation should be package-backed region content
availability: immutable package region manifests can build on explicit space
identity and projection ownership without turning cell addresses into semantic
or persisted locations. Cross-space physics/renderer/audio transfer, portals,
streaming policy, public region APIs, generic facets/ECS, and distributed world
travel remain separate future work.
