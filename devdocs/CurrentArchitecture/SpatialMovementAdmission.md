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

Motion resolves the current physical root through the live Instance hierarchy. Ordinary ancestors inherit from their parent. BaseParts and BlockGrids with an explicit binding resolve it against the owning DataModel. A retired explicit target or a ViewportFrame terminates the route without a physics world. The first enclosing WorldRoot supplies the structural domain. The same native resolver serves rendering and queries. Reparenting, assigning a different region, replacing a character root, or retiring a target affects the next lookup, including lookups inside synchronous lifecycle observers. Collision readiness and transfer reservations are checked separately for each requested move.

When a Character has a RootPart, motion requires that Part to be live and still descend from the same Character. The resolver checks that ownership during the domain walk. If an explicit assignment supplies a world below the Character, it follows the remaining parents to establish ownership. An out-of-world character with a live DataModel can use the scripting API's canonical Workspace fallback. An unmanaged native prediction actor can use its caller's world. A destroyed, reparented or unavailable root closes admission until the owner supplies a valid replacement. Ordinary attached script motion uses the resolved world after checking the actor's DataModel. It avoids a second lookup and construction check for the Workspace service.

`PrepareSpatialDestination` binds a receipt to its live DataModel, domain activation, exact bounds, required owners and currently applied collision. Ordinary bodies contribute their native pose, shape, filter and body generation. Terrain contributes field lifetime, version, admitted query coverage and native chunks. BlockGrid contributes installed physical pieces with owner pose, catalog geometry, local coordinates and content revision. Query-only pieces do not occupy a movement destination.

`CheckSpatialLanding` validates source identity, source pose and activation, destination coverage and clearance. `CommitSpatialTransfer` repeats that check on Main. The native profile accepts an unconstrained ordinary Part or a single-root KinematicCharacter with an ordinary Part or box-colliding MeshPart. Character admission checks both the rigid root and the full upright controller capsule using the controller’s collision groups. A second physical rig participant declines the move. During a supported cross-domain move the destination admits physics before the source releases its body. The Instance, authored parent and velocities survive. Controller and root commit the same destination pose. Previous floor contact and presentation offset retire. A collidable, nonqueryable shape still blocks clearance.

## Bilateral network authority

GST2 version 3 binds the session, world, player, controlling focus, source, destination, subject and optional Character identity to the requested poses, source body velocities, exact bounds, canonical provider set and collision digest. The subject geometry digest covers its installed shape, dimensions, collision masks and capsule geometry. For a controlled Character, the offer also carries the source controller velocity, floor normal and grounded state. The receiver resolves identifiers in its own DataModel, checks the source and verifies its local landing before reporting Ready. Portable content and coverage revisions, owner pose and stable piece and chunk addresses contribute to the collision digest. Native object handles, field lifetimes and body handles stay local. The codec bounds each message to 1,024 bytes and accepts up to 64 providers and a proof footprint of 1,024 native pieces.

1. The server issues Offer after capturing destination evidence and reserving its local resources.
2. The controlling replica emits Ready only after an independent native capture matches the offer. The server sends Reserve after revalidation.
3. The replica emits Held only after its reservation succeeds. The server checks its scope, exact proof and lease again, applies the move on Main and sends Commit.
4. The replica applies the move against its still-held local evidence, then sends Applied. Repeated packets do not apply movement twice.

A DataModel host installs `AcquireSpatialTransferProtection` with its authoritative controlled-subject resolver. The adapter registers leases in that protection’s reservation table. Only the bilateral state machine can arm a consumed-once native movement grant at the commit boundary. Direct membership and pose, ancestor reparenting, destruction, prepared property edits, physical resource replacement and controlled body forces consult that protection. Source and destination aliases in another DataModel cannot satisfy the grant. A receiver failure after server commit produces `CommittedUnconfirmed`. The controlling peer must resynchronize before authority resumes.

The protected network profile accepts an anchored or stationary kinematic source. Destination rigid colliders other than the relocating subject must be anchored, and the destination must contain no deformables. These checks ensure an independent backend simulation cannot enter a reserved volume between machines. The runtime may reject changes to a whole mesh, grid or terrain owner during the handoff. Ordinary anchored Parts outside the footprint can still move elsewhere outside it.

The host owns wire dispatch, authenticated epoch selection, reconnect and revocation, installed content provenance, subject control and reconciliation. Transport bindings must use the current controlling session when calling the state machine. A selected viewport or a client-supplied scope never grants movement authority.

The server records the native domain activation when it issues an ordinary movement window. Detaching and reattaching a named space requires a new receipt even if the domain identity, bounds, owner list and cell contents coincide. The activation is local runtime state and adds no packet bytes.

### Requesting a player transfer

A running server Script with MutateDataModel can call `Player:RequestSpatialTransfer(Destination, DestinationPose, Minimum, Maximum)`. The player and destination must belong to that Script's DataModel. Supply a Workspace or SpatialRegion, a rigid destination CFrame and strictly increasing corners in destination coordinates. Each axis supports a maximum extent of 512 units. The bounds must contain the complete landing box and controller capsule. The method returns a request handle and nil, or nil and a reason such as `Invalid`, `Unavailable`, `Denied` or `Busy`.

`Request:GetStatus()` returns `Pending`, `Applied`, `Canceled`, `Expired` or `CommittedUnconfirmed` for an admitted request. `Request:Cancel()` returns its current disposition. A terminal handle keeps its own result through later requests. Its issuing Script execution owns both methods. Disabling, restarting or removing that Script invalidates its live authority. The negotiated server session resolves the current controlling Player, source root, source chart and session epoch. An Offline server Script uses the same native landing and commit checks for its local Player, and a successful move has an immediate Applied disposition.

Online, a destination can stage for up to 15 seconds. The three-second bilateral exchange begins after the server obtains its native receipt and owner content bindings. Character command and possession epochs are revoked while staging. The controller's regular structural stream waits between the native server commit and the replica's Applied acknowledgment. The server promotes the destination audience only after that acknowledgment. An unconfirmed native commit requires the controlling connection to resynchronize.

### Destination content in GameSession

The native spatial handshake uses GSES 17, and a managed spatial package pair uses GSES 18. A trusted server transition can temporarily add one destination volume to an established peer. GSDA version 1 binds the connection epoch, monotone grant serial, destination source ObjectId, and a sorted list of up to 256 public structural identities. Each axis of the volume spans at most 512 units. The server derives the list from native collision providers, their ancestors and the controlled subject. Final replication policy must admit each identity and its hard references before the server queues the manifest. The grant expires after 15 seconds and is private to its connection. These profiles require the SR2 v3 source checkpoint and GBGS5/BGRW2 rectangular cell coverage. Earlier spatial peers reject the version before any gameplay content is transferred.

Each destination has a separate BlockGrid subscription set. GBGS version 5 carries the grant generation on every message. Offers and installation acknowledgments also name the grid’s source domain-membership generation. Installation requires the current structural owner, native field, placement and motion projection. A released stage retires its replica content. Promotion adopts the staged subscriptions and retires the previous chart. A transfer still requires independent native collision preparation on each endpoint. An empty-terrain grant binds the server's observation to the precise volume and serial. Both peers recheck actual terrain emptiness before using the binding.

### Domain-local terrain

GSDT version 1 wraps the terrain window offer, fragment and installed receipt with the named domain's structural identity, grant generation and sample spacing. A peer receives one active window for its authorized named domain and one for a staged destination. The server selects a 27-chunk neighborhood around the controlled actor or destination volume. Client input cannot select that neighborhood. The server checks the chart against its final structural audience before sending. Each window uses the existing 1 MiB bound, content-addressed leaves, unchanged-leaf reuse and native contour preparation. Multiple connections share the four server encoding slots. The client has two decoding slots. Query admission requires the requested swept bounds to fit inside the installed neighborhood with its contour halo.

Transfer readiness binds the portable terrain manifest to the installed local field, matching window revision and active destination serial. A chart change retires the old field and collision resources. Promotion retains the already installed destination window. Managed Development servers can persist multiple independently addressed terrain fields. A distributed Player receives terrain sample bytes through its authorized connection. Its package contains only the Workspace bootstrap pin when the project uses it.

### Ordinary controlled motion

For a spatial session the authoritative host issues a GSDM area around the controlled subject. The peer must independently project the same installed primitive, terrain and grid collision set, including an explicit empty terrain field, before acknowledging the digest. The server checks the exact swept bounds and recalculates the native digest immediately before applying movement. Adding a collider before the next replication tick invalidates the previous authorization. The actor's own root does not count as an obstacle in its coverage digest. A subject, chart or possession change requires fresh evidence. During a bilateral transfer the ordinary input path closes.

Spatial sessions send input to the authoritative character simulator and apply its reconciled motion on the client. This allows terrain, grids and Parts to change normally while the controller waits for newly installed collision. Non-spatial sessions retain their existing prediction setting. Server-driven NPC movement uses the local simulation.

A spatial package may bind Workspace terrain even when the first authorized chart is an interior. The original Workspace bootstrap runs solely for peers that start in Workspace. Leaving Workspace cancels its bootstrap sender and removes its local samples and collision. Returning uses the same grant-bound destination window machinery as entering an interior. The server transmits the bounded public manifest whenever a spatial Player has only a package digest. This keeps the rest of the terrain directory private.
