---
status: planned
authority: non-normative
owner: spatial-runtime
related_current_architecture:
  - devdocs/CurrentArchitecture/FoundationRuntime.md
  - devdocs/CurrentArchitecture/RenderExtraction.md
  - devdocs/CurrentArchitecture/RendererBackendBoundary.md
  - devdocs/CurrentArchitecture/PhysicsBackend.md
  - devdocs/CurrentArchitecture/PhysicsQueryFoundation1.md
  - devdocs/CurrentArchitecture/AudioFoundation1.md
  - devdocs/CurrentArchitecture/AnimationFoundation2GpuSkinning.md
  - devdocs/CurrentArchitecture/AnimationFoundation2SemanticAnchors.md
  - devdocs/CurrentArchitecture/LoopbackReplication.md
---

# Spatial regions and portal topology

## Status and intent

This is a detailed future-design proposal, not current engine behavior or an
accepted public API. It describes how Gargantuan could support spaces that are
locally Euclidean but connected through non-Euclidean topology: a small exterior
door may lead to a much larger interior, rooms may have impossible neighbors,
and rays, sound, and visibility may traverse the same links.

The central direction is:

> Space is a bounded graph of locally Euclidean regions. The DataModel owns the
> authoritative graph; rendering, physics, audio, queries, Studio, and networking
> consume explicit projections of it.

This should not begin as a special rendering effect or a `PortalPart` that each
subsystem interprets independently. The topology and its transform semantics must
be defined first. Exact class names, property names, schema IDs, and wire formats
require an ADR and prototype evidence before becoming contracts.

## Current baseline and required change

The current engine already has useful boundaries:

- one DataModel owns authoritative Instances, identity, mutation, and committed
  change history;
- renderer code consumes immutable `RenderPublication` values and never traverses
  the live DataModel;
- `WorldRoot` owns neutral rigid and deformable physics worlds whose backend
  handles never escape their adapters;
- `Workspace:Raycast` resolves backend-neutral hits back to generation-safe
  semantic objects on Main;
- `SemanticSpatialResolver` resolves static and animated Attachment chains into
  renderer-independent semantic transforms consumed by Sound and Interaction;
- GPU skinning publishes one immutable palette per rig and keeps gameplay anchor
  authority in the CPU animation/semantic path rather than GPU readback;
- `AudioRuntime` owns semantic voices and resolves positional sources through
  the shared semantic spatial resolver; and
- snapshots, journals, and receiver projections distinguish source identity from
  local materialized identity.

The current model still assumes one coordinate space for Workspace rendering,
physics, queries, and positional audio. `WorldRoot` owns one rigid world and one
deformable world, `RenderFrameState` has one camera, and public raycasts accept an
ordinary world-space origin and direction. Portal topology therefore requires a
coordinated architecture change; it cannot be added correctly in only the
renderer.

## Relationship to semantic spatial resolution

The current `SemanticSpatialResolver` and the proposed region topology solve
different layers and should not become competing spatial authorities:

```text
animation pose + Part/Attachment chain
    -> SemanticSpatialResolver
    -> exact semantic transform inside one current world
    -> region membership + SpatialTopologyStore
    -> SpatialAddress { Region, LocalTransform }
    -> Sound / Interaction / queries / rendering
```

The resolver remains responsible for joint-bound and static Attachment
composition, cache invalidation, transient semantic revision, and exact matrix
output. It must not search portal graphs, choose replication interest, own
physics worlds, or inspect renderer palettes. The topology layer supplies the
owning region and maps an already resolved semantic transform only when a
consumer deliberately traverses an edge.

`Attachment.WorldCFrame` currently means a transform in the single Workspace
coordinate system. Region support must not silently reinterpret that public
property. Before implementation, an ADR must choose an explicit region-aware
address surface while preserving existing default-region reads. The preferred
direction is to compose a `SpatialAddress` for region-aware native consumers and
leave the current CFrame-only property as the default-region compatibility
surface; exposing a separate script-visible region/address value can follow a
schema and compatibility decision.

Animated scale and shear remain inside `SemanticSpatialTransform::Matrix` for
trusted consumers. Portal Foundation 1 still maps regions with rigid CFrames.
Composition is therefore `region-local semantic matrix -> rigid portal mapping`;
it does not collapse animated nonuniform scale into CFrame or turn it into portal
scale.

## Goals

The first complete feature should support:

- multiple regions whose local coordinates may overlap without interaction;
- directed or two-way rigid portal links between regions;
- larger-on-the-inside interiors without duplicating authoritative geometry;
- camera views clipped and composited through visible portal apertures;
- atomic transfer of characters and ordinary rigid assemblies after crossing;
- ray, line-of-sight, and interaction traversal through portals;
- bounded topology-aware positional audio;
- region-aware simulation, streaming, and replication interest; and
- Studio authoring, validation, preview, save, reopen, and Play behavior.

## Initial non-goals

The initial architecture must explicitly exclude:

- arbitrary scale, reflection, shear, or nonlinear portal transforms;
- one rigid body colliding in two physics regions simultaneously;
- constraints whose endpoints occupy different regions;
- deformable bodies straddling a portal;
- unbounded recursive views, queries, acoustic paths, or replication expansion;
- renderer-owned teleport decisions;
- implicit region selection from names, paths, camera state, or script trust; and
- treating journal sequence as a portal-view or network-delivery sequence.

Rigid rotation and translation are enough for larger-on-the-inside spaces. Scale
can be researched later only after mass, force, gravity, velocity, animation,
audio, and network semantics are specified together.

## Conceptual authoring model

The likely public concepts are `SpatialRegion` and `SpatialPortal`. The following
shape is illustrative rather than accepted schema:

```text
SpatialRegion : Instance
    Enabled
    SimulationPolicy

SpatialPortal : Instance
    SourceRegion       : SpatialRegion
    DestinationRegion  : SpatialRegion
    SourceFrame        : CFrame
    DestinationFrame   : CFrame
    ApertureSize       : Vector2
    TwoWay             : boolean
    Enabled            : boolean
```

A region defines an independent local coordinate space, not an offset within a
hidden global coordinate system. Region A at `(0, 0, 0)` and Region B at
`(0, 0, 0)` do not overlap because a spatial address is a pair:

```cpp
struct SpatialAddress
{
    ObjectId Region;
    CFrame LocalTransform;
};
```

`ObjectId` remains DataModel-scoped object identity. A second public identity
system is unnecessary. Persistence and replication use ordinary schema-aware
object references, subject to their existing durable-identity and visibility
rules.

Spatial participants need explicit membership. A future schema may expose a
`SpatialRegion` reference on spatial roots, with `nil` meaning Workspace's
non-destroyable default region. Descendants may inherit membership for authoring
convenience, but a rigid assembly, character assembly, constraint graph, or
deformable body must resolve to exactly one region. A hierarchy path is never
itself identity or authority.

`SpatialPortal` is semantic topology, not required wall geometry. Creators may
place ordinary Parts around an aperture, while the portal describes the bounded
crossing and view surface. A two-way portal is one validated pair of inverse
directed edges so separately authored halves cannot drift out of agreement.

## Authoritative ownership

The DataModel should own an internal `SpatialTopologyStore` keyed by stable
ObjectIds. It contains only validated region membership, portal links, aperture
descriptions, and immutable topology revisions. It does not own GPU resources,
backend physics handles, audio voices, or network connections.

`SpatialTopologyStore` does not replace `SemanticSpatialResolver`. An Engine may
coordinate both, but their caches, revisions, limits, and failure diagnostics
remain separate. A topology revision does not invalidate animation pose state;
it invalidates only the composed region/address and graph-consumer caches that
depend on membership or portal edges.

Committed schema writes flow through the ordinary mutation path:

```text
script / Studio / server command
    -> type, reference, authority, and topology validation
    -> atomic DataModel mutation
    -> committed journal records
    -> topology revision
    -> physics/render/audio/network projections
```

No subsystem may discover portals by traversing mutable Instances during its own
work. Main constructs bounded immutable or owned semantic inputs at safe points.

Topology validation must reject non-finite frames, invalid or cross-DataModel
references, zero/negative apertures, unsupported transform types, membership
cycles, mixed-region assemblies, and changes that would leave a live constraint
cross-region. Graph cycles and self-links may be legal topology, but every graph
consumer remains depth-, work-, and result-bounded.

Destroying a referenced region needs preflight before the monotonic Instance
destruction lifecycle begins. The initial rule should reject destruction while
participants or enabled portals reference the region. A later explicit operation
may transactionally migrate participants and remove links, but silent fallback to
the default region could create collisions or expose hidden content.

## Transform contract

For rigid transforms, crossing from a source frame `S` to a destination frame
`D` maps an object transform `X` as:

```text
X' = D * inverse(S) * X
```

The same relative transform maps a camera. Direction vectors, linear velocity,
angular velocity, contact normals, ray directions, and listener orientation use
only its rotation. Positions use rotation and translation. Distance, mass, time,
and speed magnitude are unchanged in the initial rigid-only design.

The portal contract must define its front face, crossing direction, aperture
plane, edge inclusion, epsilon, and destination offset. The destination receives
a small deterministic normal offset so the transferred origin does not
immediately cross the inverse edge. Non-finite output or an invalid destination
fails before authoritative state changes.

## Crossing and transfer

Foundation 1 should transfer an entire spatial root when its defined crossing
anchor moves through the aperture. For a character this is the character root;
for a rigid assembly it is the assembly root or center of mass. Merely touching
the plane or crossing outside the aperture does nothing.

During a fixed step:

1. detect swept anchor crossings against enabled source apertures;
2. sort candidates by earliest time of impact, then portal ObjectId;
3. validate the destination region, membership, bounds, and transfer policy;
4. finish the source-world step and copy owned motion results;
5. at a Main non-stepping safe point, atomically update region membership,
   transform, linear velocity, and angular velocity;
6. remove/recreate or transfer neutral physics state behind `WorldRoot` without
   exposing backend handles; and
7. publish one committed logical transfer before render, audio, and replication
   extraction.

If admission to the destination physics world fails, no partial region or
transform change commits. The source body remains coherent and the failure emits
a bounded diagnostic. The first version may defer the unused remainder of the
substep rather than attempting multiple crossings in one physics step.

Character-aware presentation may clip the visible character at the aperture
before its root crosses, while authority and collision remain entirely in the
source region. Once the root crosses, ownership transfers as a unit. True
cross-region rigid-body interaction would require proxy bodies and mapped
constraints in both worlds and is a separate advanced milestone.

## Physics worlds and simulation policy

`WorldRoot` should evolve from owning one rigid/deformable pair to coordinating a
bounded map of region-local worlds:

```text
WorldRoot
    -> SpatialTopologyStore
    -> Region A: PhysicsWorld + SoftBodyWorld
    -> Region B: PhysicsWorld + SoftBodyWorld
    -> Region C: PhysicsWorld + SoftBodyWorld
```

Each neutral backend ID remains independent from ObjectId and meaningful only to
its owning physics world. Region transfer may replace the neutral ID while the
authoritative Instance ObjectId remains stable. Backend-native IDs never enter
topology, journals, rendering, Studio, or networking.

All regions initially inherit Workspace gravity and fixed-step policy. Per-region
gravity, time scale, or solver selection should not be introduced incidentally.
Simulation policies may later allow `Active`, `Reduced`, and `Dormant` behavior,
but transitions must be deterministic and server-authoritative. A region needed
by an occupied player, an imminent transfer, or authoritative gameplay cannot be
put to sleep merely because no portal is visible.

## Rendering

The renderer continues to consume immutable values. A future publication needs
region membership on render objects plus bounded portal-view inputs; it must not
contain `SpatialRegion*`, `SpatialPortal*`, or any other Instance pointer.

```text
authoritative committed topology and scene
    -> RenderPublisher
    -> immutable region/object/portal publication
    -> renderer-owned projection
    -> main view
       -> visible aperture selection
       -> transformed child view
       -> region-local culling
       -> stencil/scissor/clip
       -> composite
```

For each accepted child view, the renderer transforms the parent camera by the
portal mapping, culls only destination-region objects, clips against the exit
plane, and restricts work to the projected aperture. Stencil rendering with an
oblique near plane is the likely first implementation; an offscreen target is an
implementation option, not a semantic requirement.

Animated rigs retain the current publication contract. One object pose revision
and one renderer-owned skin palette are shared by every main or child view that
draws that rig. Portal recursion may multiply culling and draw work, but it must
not resample animation, resolve semantic anchors, publish another palette, or
upload one palette per view. Shadow and opaque passes within every accepted view
must bind the same validated pose revision.

Recursive views carry the traversed portal-edge path. Repeating an edge may be
stopped as a loop, and every frame must enforce:

- maximum recursion depth;
- maximum generated child views;
- minimum projected aperture area;
- aggregate secondary-view pixel and draw budgets;
- per-view culling and light limits; and
- stable selection order, such as projected area followed by portal ObjectId.

Candidate starting values for measurement are depth 4, eight child views, a
256-pixel minimum aperture, and an aggregate secondary pixel budget no larger
than the main view. These are prototype limits, not promises. When a limit or
destination-publication gap is reached, the aperture renders a deterministic
fallback surface; the renderer never requests authority or exposes stale region
contents.

Picking through a portal returns a bounded path plus the destination object's
stable identity from the displayed child view. EditorHost still receives pixels,
view metadata, and stable identities rather than renderer or GPU handles.

## Spatial queries and interaction

The existing `Workspace:Raycast` should retain its current single-region
contract for compatibility. Portal traversal needs an explicit-origin API so
results never depend on a caller's camera or hierarchy by accident. An
illustrative native shape is:

```cpp
SpatialRaycastResult RaycastSpatial(
    SpatialAddress Origin,
    glm::vec3 Direction,
    SpatialQueryPolicy Policy);
```

The query casts in the current region up to the nearest solid hit or portal
aperture. If the portal wins, it subtracts traveled distance, transforms the ray,
records the edge, and continues in the destination region. A hit returns the
semantic object, local hit position/normal, hit region, total traveled distance,
and optionally the bounded portal path. Existing filter roots remain semantic
ObjectIds and must not become backend handles.

Queries require maximum distance, portal depth, visited-edge, candidate, and
total-work limits. A tie between a solid hit and aperture needs one canonical
epsilon and ObjectId ordering. Limit exhaustion is an explicit failure, not a
partial closest-hit claim. InteractionService, authoritative hitscan, AI vision,
and later shape casts should reuse this traversal rather than implement their own
portal recursion.

For an Attachment-anchored ProximityPrompt, `SemanticSpatialResolver` first
produces the current static or animated local endpoint. Region membership then
forms its `SpatialAddress`, and the spatial LOS traversal connects the
character's address to that endpoint. Animation does not mutate topology or
produce journal records, while a region transfer or portal edit remains an
authoritative committed change.

## Audio and acoustics

Foundation 1 positional audio remains ordinary same-region attenuation. The
shared semantic spatial resolver continues to supply a Sound's current static or
animated Attachment transform without knowing about portals. A later
topology-aware mixer combines that transform with region membership, then searches
a bounded portal graph from source to listener. For an accepted path it
accumulates local segment distances and per-portal transmission, then transforms
the apparent source through the final portal into listener-region coordinates for
pan and direction.

The path search must cap visited regions, edges, alternate paths, and work per
voice. It should use a topology revision cache and deterministic tie-breaking.
No-path and budget-exhausted behavior is silence or a documented same-region
fallback, never an unbounded search. Occlusion, diffraction, reverb, and HRTF are
later layers; the first useful result is sound that appears to come through the
doorway with bounded distance and transmission loss.

## Replication, interest, and streaming

The authoritative server owns region membership, portal state, and crossings.
Clients receive receiver-specific projections. A topology edge and destination
region identity must arrive before any object update that references them, but
authoritative journal order must not be reused as packet or acknowledgement
order.

Portal visibility naturally informs interest without becoming its only source:

- the player's occupied region is always relevant;
- nearby enabled destinations may be prefetched before crossing;
- visible portal paths add bounded render interest;
- gameplay, audio, or server policy may add non-visible interest; and
- leaving interest unpublishes receiver state rather than destroying the
  authoritative object.

The client must not render or query destination content until the required
topology revision and region baseline are coherent. Missing data displays the
fallback aperture. Region visibility is also an authorization boundary: a
client must not receive a secret destination merely because it can guess a
region or portal ObjectId.

Region streaming needs explicit loaded, published, active, dormant, and failed
states. Disk/asset loading, network publication, simulation activation, and
render readiness are distinct transitions with bounded queues, timeouts, and
cancellation.

## Persistence and compatibility

Regions, portals, membership, and transforms are saved schema state. Project
serialization must validate all references and topology before replacing a live
document. Snapshot and replication formats need explicit version changes when
region membership begins crossing those boundaries; an unknown region or stale
generation fails closed.

Existing projects load into the default region with unchanged CFrames. Existing
`Workspace:Raycast`, physics, audio, and rendering behavior remains the default
when no explicit regions or portals exist. The zero-portal path must have
negligible overhead and preserve deterministic output.

Durable authoring identity remains a broader project-format concern. This
proposal does not redefine runtime ObjectId as a durable UUID.

## Studio authoring

Studio remains a client of EditorHost. Required future surfaces include:

- create, rename, inspect, and delete-safe region operations;
- explicit portal endpoint linking with source/destination previews;
- aperture and facing gizmos in each region's local coordinates;
- a region selector/layer view without rewriting the ordinary hierarchy;
- validation for dangling links, mixed-region assemblies, unsupported scale,
  and portal loops that exceed preview budgets;
- camera-through-portal preview and picking using bounded EditorHost view paths;
- save/reopen and undo/redo of topology transactions; and
- isolated Play tests proving authoring state is not mutated by traversal.

Studio never receives a physics world, render projection, GPU resource, or
mutable topology store. Its document model holds versioned DTOs and submits
expected-revision commands through EditorHost.

## Failure, security, and observability

Every public or cross-process boundary needs explicit limits for regions,
portals, incident edges, aperture size, crossings per tick, recursive views,
query hops, audio graph work, streamed bytes, and queued region transitions.
Malformed topology is rejected atomically. Runtime disappearance disables the
affected edge and emits bounded diagnostics rather than dereferencing stale
identity.

Suggested diagnostic categories are `[Spatial:Topology]`,
`[Spatial:Traversal]`, `[Render:PortalView]`, `[Physics:Region]`, and
`[Audio:PortalPath]`. Diagnostics may include scoped IDs, revisions, counts, and
limit codes, but not hidden region content or authorization-sensitive names.

Server-side traversal validates authority at the destination as well as the
source. Clients cannot claim a crossing transform, destination region, physics
result, or interest expansion. Studio/plugin scripts gain no privilege from
owning, naming, or parenting a region or portal.

## Delivery sequence

### 0. Decision and prototype evidence

- Accept an ADR for region membership, portal facing, transfer timing,
  persistence, `WorldCFrame` compatibility, and failure semantics.
- Prototype immutable topology snapshots and rigid transform round trips.
- Measure zero-portal overhead, semantic-address composition, and bounded graph
  traversal without invalidating unchanged animation-anchor caches.

### 1. Region-local ownership

- Add the default region and explicit membership without visible portals.
- Split `WorldRoot` coordination into bounded region-local physics worlds.
- Prove save/reopen, clone, destruction, constraints, Play isolation, and
  replication baseline behavior.

### 2. Authoritative rigid traversal

- Add rigid portal topology and origin/assembly crossing at a Main safe point.
- Transform CFrame and motion atomically with deterministic candidate ordering.
- Add destination admission failure, loop, lifecycle, and high-speed sweep tests.

### 3. Portal rendering

- Extend immutable publication/projection values with regions and portal views.
- Implement clipped child views, recursion/work budgets, fallback surfaces, and
  portal-aware picking.
- Prove a rig uses one pose revision and palette upload even when drawn through
  multiple portal views.
- Benchmark tiny, fullscreen, nested, cyclic, and many-portal scenes.

### 4. Shared traversal consumers

- Add explicit spatial raycasts and move line-of-sight/interaction onto them,
  composing their current semantic animated endpoints with region membership.
- Add bounded topology-aware positional audio without a second Attachment or
  animation resolver.
- Keep traversal algorithms shared at the semantic layer while preserving each
  subsystem's output and ownership boundary.

### 5. Multiplayer and streaming

- Version topology and membership replication.
- Add server-authoritative crossings, prefetch, per-peer region interest, and
  coherent fallback behavior under loss and latency.
- Test unauthorized regions, stale topology, reconnect, and publication churn.

### 6. Advanced research

- Character/body visual splitting before transfer.
- Cross-region rigid proxies and constraints.
- Deformable traversal.
- Scale-aware portals and non-rigid topology only with a separate accepted
  physical-semantics design.

## Exit criteria for the first complete slice

The feature is not complete until automated tests demonstrate:

- two overlapping local-coordinate regions remain isolated;
- a character crosses both directions with transform and velocity preserved;
- a larger interior renders only through its aperture before entry;
- nested/cyclic portals stop deterministically at configured budgets;
- raycast and interaction results traverse the same topology as rendering;
- animated and static prompt endpoints traverse from the shared semantic
  resolver without transient pose updates mutating topology;
- audio, including joint-bound Sound, appears through the correct bounded portal
  path;
- a skinned rig visible through multiple portal views reuses one semantic pose
  and palette upload;
- save/reopen, Play/Stop, undo/redo, and replication preserve topology identity;
- destination admission and malformed/stale topology fail without partial state;
- unauthorized destination content is neither replicated nor picked; and
- a project with no portals retains current behavior and measured performance.

Only after these contracts are implemented and verified should the resulting
documents move from future architecture into current architecture.
