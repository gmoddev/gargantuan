# Native spatial collision demand and movement admission

A WorldRoot owns its local Box3D provider, Terrain field and admitted collision for spatial participants. A native host can request a collision observation, validate an exact destination and move an ordinary Part at a Main safe point. The GameSession that controls a network subject supplies the authenticated audience, content bindings and reservation authority.

## Collision observations

`WorldRoot::RequestSpatialCollision` creates a Main-owned holder for one native audience and epoch, bounds in domain-local coordinates and an optional set of required native owners. Supply finite bounds with strictly increasing corners. Ordinary negative coordinates are valid. Missing required owners have no authority to certify an empty volume.

Matching bounds and required owners in one activation share a single immutable native capture. The holder keeps its own deadline, priority and area. Moving or releasing one holder preserves other holders’ requests. Each scheduled WorldRoot step captures up to two distinct requests. An explicit pump admits at most four captures. Oldest-served requests run first and priority breaks equal-age ties. With zero holders, normal stepping skips the queue.

A domain accepts up to 64 live holders, eight per audience, 64 required owners per request and 128 tracked audience epochs. Each holder may request up to 60 seconds. Only its audience’s holder limit reports Budget. Retiring an audience epoch invalidates its holders and rejects that epoch on reuse. Domain detachment invalidates all its holders. Receipt snapshots are transient, process-local observations. Call `ValidateSpatialDestination` at the actual movement boundary.

## Creator collision observations

A running server Script in Offline or NetworkServer mode can call `Workspace:RequestCollisionArea(Minimum, Maximum, Seconds?, Priority?)` or call the same method on a live SpatialRegion in its DataModel. Use strictly increasing finite world-local corners. The default duration is five seconds. The accepted priorities are Background, Normal and Interactive. Network clients receive `nil, "Unavailable"`.

The native userdata exposes `GetStatus`, `SetArea`, `SetPriority`, `Renew` and `Release`. A status snapshot is read-only and contains an opaque string Revision plus Life and Collision. A handler checks the active engine, Main, server execution, ReadDataModel capability and the authenticated issuing Script object and run generation on every use. Deferred tasks inherit their issuing execution. A new run of the same Script cannot use its previous handles. Passing a handle through a shared ModuleScript gives another Script no rights to it.

```luau
local Room = workspace:FindFirstChild("Room")
local Near = Vector3.new(-4, 0, -4)
local Far = Vector3.new(4, 8, 4)
local View = assert(Room:RequestCollisionArea(Near, Far, 5, "Interactive"))
local Tool = assert(Room:RequestCollisionArea(Near, Far, 5, "Normal"))

task.wait()
local Snapshot = Tool:GetStatus()
if Snapshot.Life == "Active" and Snapshot.Collision == "Ready" then
    Tool:SetArea(Vector3.new(20, 0, 0), Vector3.new(28, 8, 8))
end
View:Release()
Tool:Release()
```

A creator can observe a collision facet with this API. The movement gateway makes its own current native clearance and authority checks.

## Checked movement

`PrepareSpatialDestination` binds a receipt to its live DataModel, domain activation, exact bounds, required owners and currently applied collision. Ordinary bodies contribute their native pose, shape, filter and body generation. Terrain contributes field lifetime, version, admitted query coverage and native chunks. BlockGrid contributes installed physical pieces with owner pose, catalog geometry, local coordinates and content revision. Query-only pieces do not occupy a movement destination.

`CheckSpatialLanding` validates source identity, source pose and activation, destination coverage and clearance. `CommitSpatialTransfer` repeats that check on Main. The native profile accepts an unconstrained ordinary Part or a single-root KinematicCharacter with an ordinary Part or box-colliding MeshPart. Character admission checks both the rigid root and the full upright controller capsule using the controller’s collision groups. A second physical rig participant declines the move. During a supported cross-domain move the destination admits physics before the source releases its body. The Instance, authored parent and velocities survive. Controller and root commit the same destination pose. Previous floor contact and presentation offset retire. A collidable, nonqueryable shape still blocks clearance.

## Bilateral network authority

The version 2 GST2 payload binds the session, world, player, controlling focus, source, destination, subject, optional Character identity, requested poses, source velocities, exact bounds, canonical provider set and collision digest. A separate digest identifies the subject’s installed shape, dimensions and collision masks, including capsule geometry and controller velocity when present. The receiver resolves identifiers into its own DataModel and checks its local landing before reporting Ready. Portable content and coverage revisions, owner pose and stable piece and chunk addresses contribute to the collision digest. Native object handles, field lifetimes and body handles stay local. The codec bounds each message to 1,024 bytes and accepts up to 64 providers and a proof footprint of 1,024 native pieces.

1. The server issues Offer after capturing destination evidence and reserving its local resources.
2. The controlling replica emits Ready only after an independent native capture matches the offer. The server sends Reserve after revalidation.
3. The replica emits Held only after its reservation succeeds. The server checks its scope, exact proof and lease again, applies the move on Main and sends Commit.
4. The replica applies the move against its still-held local evidence, then sends Applied. Repeated packets do not apply movement twice.

A DataModel host installs `AcquireSpatialTransferProtection` with its authoritative controlled-subject resolver. The adapter registers leases in that protection’s reservation table. Only the bilateral state machine can arm a consumed-once native movement grant at the commit boundary. Direct membership and pose, ancestor reparenting, destruction, prepared property edits, physical resource replacement and controlled body forces consult that protection. Source and destination aliases in another DataModel cannot satisfy the grant. A receiver failure after server commit produces `CommittedUnconfirmed`. The controlling peer must resynchronize before authority resumes.

The protected network profile accepts an anchored or stationary kinematic source. Destination rigid colliders other than the relocating subject must be anchored, and the destination must contain no deformables. These checks ensure an independent backend simulation cannot enter a reserved volume between machines. The runtime may reject changes to a whole mesh, grid or terrain owner during the handoff. Ordinary anchored Parts outside the footprint can still move elsewhere outside it.

The host owns wire dispatch, authenticated epoch selection, reconnect and revocation, installed content provenance, subject control and reconciliation. Transport bindings must use the current controlling session when calling the state machine. A selected viewport or a client-supplied scope never grants movement authority.

### Destination content in GameSession

The native spatial handshake uses GSES 9, and a managed spatial package pair uses GSES 10. A trusted server transition can temporarily add one destination volume to an established peer. GSDA version 1 binds the connection epoch, monotone grant serial, destination source ObjectId, and a sorted list of up to 256 public structural identities. Each axis of the volume spans at most 512 units. The server derives the list from native collision providers, their ancestors and the controlled subject. Final replication policy must admit each identity and its hard references before the server queues the manifest. The grant expires after 15 seconds and is private to its connection.

Each destination has a separate BlockGrid subscription set. GBGS version 3 carries the grant serial on every message. Installation requires the current structural owner, native field, placement, and motion projection. A released stage retires its replica content. Promotion adopts the staged subscriptions and retires the previous chart. A transfer still requires independent native collision preparation on each endpoint. An empty-terrain grant binds the server's observation to the precise volume and serial. Both peers recheck actual terrain emptiness before using the binding.
