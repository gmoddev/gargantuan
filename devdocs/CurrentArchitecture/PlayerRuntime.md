---
status: current
owner: runtime
last_verified: 2026-11-03
related_code:
  - assets/classes/KinematicCharacter.luau
  - assets/classes/Player.luau
  - assets/runtime/
  - assets/services/ActionMap.luau
  - assets/services/InputService.luau
  - assets/services/Players.luau
  - include/gargantuan/physics/
  - src/classes/Player.cpp
  - src/physics/Box3DPhysicsBackend.cpp
  - src/services/ActionMap.cpp
  - src/services/InputService.cpp
  - src/services/Players.cpp
  - tests/PlayerRuntimeTests.cpp
related_adrs: []
---

# Player runtime contracts

## Scope and extracted evidence

The player runtime productizes the existing Studio Play controller proof. It
does not introduce a second character demo or replace Gargantuan's Luau-first
gameplay architecture. The proof established runtime Luau, physical input,
relative pointer motion, camera-relative movement, orbit camera, gravity/jump,
simulation/render hooks, runtime assembly, rendering, physics, and isolated
Play/Stop. The extracted contracts are:

- `UserInputService` continues to own physical device state;
- `InputService` compiles canonical input Instances into local semantic state;
- `ActionMap` preserves the existing name-based controller API over that evaluator;
- `Players` owns the runtime-local gameplay player lifetime;
- `Player` owns the relationship to one replaceable runtime character;
- `KinematicCharacter` stores public controller state instead of hiding it in
  one copied controller script;
- `Workspace:MoveKinematicCapsule` supplies neutral bounded collision data; and
- engine-shipped Luau owns default bindings, assembly, movement, step/jump
  policy, and camera policy.

```text
native host input -> UserInputService -> GuiRuntime -> InputService
                                                       -> ActionMap facade
                                                       -> shipped/replacement Luau policy
Workspace geometry -> PhysicsWorld -> MoveKinematicCapsule result
Players -> Player -> KinematicCharacter -> visible RootPart
```

The low-level path remains supported: a game can disable both defaults and use
`UserInputService`, `RunService`, `Camera`, and
`Workspace:MoveKinematicCapsule` directly.

## ActionMap

`ActionMap` is a canonical DataModel compatibility service above the physical
`UserInputService` authority. Its Bind methods create real `InputContext`,
`InputAction`, and `InputBinding` Instances under `InputService`; the same derived
indexed evaluator serves direct Instance authoring and legacy controllers.
The facade does not replace physical signals or retain a second semantic
evaluator. See [Input action foundation](InputActions.md) for the canonical API,
schema, persistence, context conflicts, diagnostics, and bounded compilation.
Its compatibility methods remain:

| API | Current contract |
| --- | --- |
| `BindKey(ActionName, KeyCode, Scale, Priority, Consume)` | Add one keyboard or normalized gamepad-button binding and return a positive runtime binding ID. |
| `BindMouseButton(ActionName, InputType, Scale, Priority, Consume)` | Add one of the three supported mouse-button bindings. |
| `BindPointerDelta(ActionName, Scale, Priority, Consume)` | Add scaled frame-accumulated relative pointer delta. |
| `Unbind(BindingId)` / `UnbindAction(ActionName)` | Remove adapter-owned bindings by positive runtime ID or action name; authored bindings remain intact. |
| `IsDown`, `GetValue`, `GetVector` | Read digital, scalar, or transient vector action state. |
| `GetBindingCount()` | Observe current bounded binding usage. |

`ActionBegan`, `ActionChanged`, and `ActionEnded` publish semantic transitions.
Multiple digital bindings sum and clamp to `[-1, 1]`; multiple pointer bindings
for the same action accumulate before one change notification. Matching
bindings publish in descending priority and stable generation-safe identity
order. Legacy bindings retain their cofire behavior: `Consume` marks the host
event handled after `UserInputService` records physical state, preventing the
legacy native camera route from also acting on it without suppressing sibling
legacy actions. Canonical consuming contexts may suppress lower contexts,
including the legacy context. Consumption never hides physical state from
`UserInputService`.

Focus loss ends active actions and clears transient Look state. Pointer vectors
also reset at the engine frame boundary. The native service accepts at most 128
action names, 512 bindings, and 64 UTF-8 bytes per action name. Mutation requires
`MutateDataModel`. These compatibility limits are distinct from the canonical
graph's 64 contexts, 256 actions, and 2048 bindings. Definition Instances persist
when authored in the project; device state and action values remain local and
transient. The adapter context is runtime-owned and unarchivable.

The shipped `DefaultActionMap.luau` is the player-policy location that names W,
A, S, D, Space, and RMB. It declares `MoveForward`, `MoveBackward`,
`MoveLeft`, `MoveRight`, `Jump`, `CameraOrbit`, and `Look`. The controller itself
depends only on those semantic names. While the shipped camera is enabled,
these bindings consume the legacy native-camera route without hiding physical
state from `UserInputService`. Binding shape, priority, and multiplicity
leave room for later remapping. Interaction Foundation separately ships
`DefaultInteractionRuntime.luau`, which maps keyboard E and normalized gamepad
South / `ButtonA` to the `Interact` semantic action. Both modules fill only
missing named canonical actions and clean up only bindings they created;
developer-authored actions are preserved. They do not add a second input map.

## Players and character lifecycle

`Players` is a canonical lazy DataModel service. An authoring DataModel does not
invent a local player. Constructing a runtime `Engine` creates exactly one
`LocalPlayer` for the current local Play model, assigns nonzero `PlayerId = 1`,
parents it under `Players`, and fires `PlayerAdded`. `GetPlayers()` returns live
players in deterministic identity order.

Local Play and session players receive a runtime-owned `PlayerGui` before
`PlayerAdded`. GUI provisioning and respawn policy are documented in
[StarterGui and PlayerGui](PlayerGui.md).

`PlayerId` is gameplay identity. It is not an EditorHost connection ID,
transport peer ID, socket, authentication identity, or object address. No
current code connects `Player` to gargantuan-node, PlayerAuth, matchmaking, or
replication ownership. A separate native-only, immutable `PlayerIdentity`
(`Provider`, `Subject`) seam now supplies EntitlementService checks. Local Play
initializes `(local, player-1)`; a future authenticated server bootstrap may
initialize the same engine-owned type. Luau cannot submit or mutate it.
Use `PlayerId` to key per-player Luau state within one runtime. A property,
method, or signal can return a fresh userdata wrapper for the same Player.
Those wrappers compare equal, while Luau table keys use raw userdata identity.

`Player.Character` is nullable and accepts any live canonical `Character` in
the same DataModel that is not already assigned as another Player's Character
or explicit possession. A failed assignment leaves both Player relationships
intact. The default assembly chooses `KinematicCharacter`, but an
NPC can use Character semantics without a Player and future movement
specializations do not require changing the Player schema. The lifecycle is:

1. `LoadCharacter` or `ResetCharacter` removes the old character.
2. `CharacterSpawnRequested` fires synchronously for replaceable Luau assembly.
3. Assembly assigns the new character, committing the relationship before
   `CharacterAdded` fires.
4. `RemoveCharacter`, replacement, external character destruction, Player
   teardown, or Engine Stop fires `CharacterRemoving` and clears the relation.
5. Player-owned removal/replacement destroys the old character exactly once.

On the synchronous mutation path, the pre-removal callback exposes the original
Character and reserves the pending replacement for its new owner. Nested
removal or replacement on that Player is rejected, as are assignments of the
pending target to another Player. The reservation ends when the callback
returns or fails. A throwing native observer at this stage aborts the pending
assignment. Once the new relationship commits, its
destruction connection is already active for `GetPropertyChangedSignal("Character")`
observers. Those observers can destroy the current Character or assign a
different one. The previous Character stays reserved for retirement and cannot
be reassigned as a Character or possessed. The Player invokes that retirement
before propagating a native property observer error. If a reentrant observer
supersedes the replacement or destroys it, only the current live binding can
emit `CharacterAdded`. `CharacterAdded` handlers can still install a
project-specific template. Once external Character or Player destruction has
started, the Player logs native Character observer errors and continues its
teardown. Replica application still uses its existing signal deferral.

`Player:SetPossession(EntityOrNil)` is an independent, non-owning relationship
for server gameplay policy. Entity eligibility is a live descendant of the
same DataModel's Workspace, regardless of its Instance class. The Player must
be a live runtime identity under Players. The server admits at most one Player
per target across explicit possession and stock Character control. Reassignment or
release of explicit possession preserves the target. Destruction, losing
Workspace ancestry, or Player removal revokes possession, and PlayerRemoving
observers see the relationship cleared. The server records a fresh local
possession revision on each change for downstream input routing.

`Player.PossessedEntity` is a soft replicated reference. Clients can inspect
it when the target is materialized in their view. Its setter is restricted to
the native projection boundary. The structural receiver can receive a new
Player's hard Character relationship before the previous Player's soft
possession release. Both replica preflight and live application allow this
temporary overlap with a soft reference and reject duplicate hard Character
relationships. Server Luau calls `SetPossession`; rejected
assignments preserve the existing relationship. Observers can use
`GetPropertyChangedSignal("PossessedEntity")`. Synchronous reassignment from
this notification is rejected. `Player.Character` retains its existing
assembly and teardown contract.

### Network possession input and pose

`CharacterControlService:SubmitPossessionIntent(Movement, Facing, Jump)`
accepts local client intent for the trusted LocalPlayer. The client compares
the replicated possession with the live replica resolved from the server's
lease. Admission requires a ready connection, a structurally materialized
target, a fresh control epoch, a monotonic input sequence, and bounded intent.
Packet processing retains only the newest analog sample and a latched jump
edge. It runs on Main without executing a project callback at packet ingress.
Rebinding retires old leases before admitting new owners.

Server project code consumes one input sample per Player per authoritative
frame through `GetPossessionInput(Player)` in `RunService.PreSimulation`.
A neutral sample has `Fresh = false`. A missing lease or second read returns
`nil`. The Luau table carries the authoritative tick, acknowledged input,
input age, yaw, movement vector and jump edge. Stock Character control is
suspended while a distinct entity is possessed. Restoring stock control
uses the existing Character assembly and input contracts.

`MovePossessedPart(Player, Transform)` admits one finite rigid transform for
the ready Player's independent anchored BasePart per `PreSimulation` frame.
Translation is capped at 16 metres per update. The controller chooses
collision policy through the existing world queries. The accepted transform
updates the authoritative part, spatial projection and physics synchronization
through existing property observers. Runtime movement avoids per-frame
structural writes. Leaving this mode commits the final pose for later
structural readers.

After simulation, the session publishes the current pose to structurally
materialized recipients every three simulation ticks. Reliable publication
control is queued immediately. The initial snapshot aligns to the recipient's
connection-slot phase, with at most two ticks of scheduling delay while the
scheduler can accept work. Once admitted, the existing three-tick interval is
measured from the last successful admission. A failed admission can be retried
on the next eligible tick. A peer receives
at most 16 active pose streams, with its own target selected first. The client
applies the latest valid authoritative sample to the local anchored body.
The client can reuse the weak replica binding while its accepted structural
sequence stays current. It rechecks anchoring, hierarchy and destruction before
applying any sample. A structural advance or materialization change invalidates
the cached resolution.
Render extraction uses a bounded, interpolated presentation offset without
extrapolation. A part nested under a Character leaves this pose mode so it
cannot compete with the Character transform owner.

The generic relationship accepts any eligible Instance. The realtime
pose primitive currently supports anchored BaseParts outside Character
hierarchies. Dynamic bodies use the existing physics APIs and structural
readback policy. GCHR still owns Character prediction, correction and
network animation actions.

`Character` is a Folder-based semantic root with authoritative `CFrame`, its
`Position` alias, an optional descendant `RootPart`, and the bounded
`Character:Move` admission method. `KinematicCharacter : Character` adds only
`Velocity`, `Grounded`, `FloorNormal`, `CapsuleRadius`, and `CapsuleHeight`.
This is explicitly not Humanoid compatibility. Default assembly creates one
non-colliding anchored Part for rendering and leaves collision authority with
the capsule query. Animation root motion submits to the same Character
authority rather than setting MeshPart CFrame.

Engine shutdown destroys shipped modules first so their cleanup functions can
disconnect signals, unbind actions, release pointer capture, restore camera
state, and remove the character. It then fires `PlayerRemoving`, destroys
`LocalPlayer`, clears the service relation, and tears down the VM/world.

## Kinematic physics primitive

`Workspace:MoveKinematicCapsule(Position, Radius, Height, Translation,
Velocity)` returns an owned Luau table containing:

- `Position` and `AppliedTranslation`;
- collision-clipped `Velocity`;
- `ContactNormal` and the most upward `FloorNormal`;
- `Collided` and `HasFloor`; and
- `PlanesTruncated` when the native collision-plane bound was reached.

The neutral request/result types live in `PhysicsTypes.hpp` and the virtual
operation lives on `IPhysicsBackend`; no Box3D type or handle crosses that
boundary. The Box3D adapter uses its mover APIs with at most five contact/solve/
cast iterations and 32 collision planes per iteration. It filters current
`CanCollide` body state, solves tangential motion toward the requested target,
applies bounded depenetration, and clips velocity against final planes. Invalid,
non-finite, non-positive, or too-short capsule descriptions fail closed.

Floor acceptance, gravity integration, walk speed, jump eligibility, air-jump
policy, slope threshold, and step attempts are not native controller policy.
The shipped controller applies them in Luau. Its first step policy uses at most
three additional capsule queries only after grounded horizontal motion loses
progress, and accepts the result only when it finds a higher walkable floor.

## Engine-shipped Luau defaults

CMake copies five bounded source resources to the runtime payload next to the
executable:

- `DefaultActionMap.luau` — default physical-to-semantic bindings;
- `DefaultCharacterRuntime.luau` — Player spawn assembly and cleanup;
- `DefaultLocomotion.luau` — movement, gravity, jump, landing, collision, and
  step policy through `Character:Move`;
- `DefaultCamera.luau` — relative RMB orbit and character follow; and
- `DefaultPlayerRuntime.luau` — startup and reverse-order cleanup.

At runtime `Players` creates an unarchivable `PlayerRuntimeModules` Folder with
four ModuleScripts and one client-context bootstrap Script. Engine executes the
bootstrap once before the ordinary project Script queue, after true
schema-only PreRun and DataModel/LocalPlayer construction. Game projects do
not copy these sources, and the runtime-only subtree is not part of the
authoring snapshot. `DefaultControllerEnabled`, `DefaultCameraEnabled`, and
`CharacterAutoLoads` are serializable opt-out switches that can be set before
runtime startup. When both default controller and camera are disabled, the
runtime module subtree is not created at all.

Shipped code runs with the same `ClientRuntime` gameplay capability profile as
ordinary client-context Luau; its location grants no Core, Studio, filesystem,
process, or schema authority. Task continuations and signal callbacks retain
the security context in which they were registered. `ProcessService` enforces
`ProcessControl`, which the player runtime does not receive.

## Default camera

The default camera sets the current Camera to `Scriptable`, follows
`LocalPlayer.Character.Position`, and derives CFrame from character-relative
focus, distance, yaw, and pitch. `Camera.Yaw` remains unwrapped. `Camera.Pitch`
is clamped to `[-85, 85]` degrees. The controller derives planar movement from
the resulting Camera forward/right vectors.

`CameraOrbit` begin sets `UserInputService.MouseBehavior = LockCenter`.
`UserInputService` converts that state change to the synchronous
`SetRelativePointerMode` host command; semantic `Look` then contains true host
relative deltas. RMB end, focus loss, module shutdown, or Stop returns mouse
behavior to `Default`. The module restores the previous Camera type and CFrame
when disabled.

The older native free-camera remains active when the default camera is disabled,
preserving the proven custom-controller path and host capture behavior.

## Play isolation and network session ownership

The entire Players tree, character, action state, runtime modules, connections,
and tasks live in the PlaySession runtime DataModel/VM. Repeated Stop destroys
them and does not mutate, dirty, save, revise, or add history to authoring state.

Offline Play retains the local `PlayerId = 1` lifecycle above. In a networked
runtime, `GameSession` now owns the split: an accepted server connection creates
one authoritative Player, structural replication publishes it, and GSES names
that exact Player ObjectId and session-scoped PlayerId to establish
`Players.LocalPlayer`. The client never chooses or uploads its Player object.
`ConnectionId` remains native-only and is not Player identity. Disconnect,
replacement, or generation change removes the association and cannot inherit
the previous control epoch.

Network server defaults run `DefaultCharacterAssembly`,
`DefaultNetworkLocomotion`, and `DefaultNetworkServerRuntime`; network client
defaults run `DefaultActionMap`, the same locomotion policy,
`DefaultNetworkCharacterRuntime`, `DefaultCamera`, and
`DefaultNetworkClientRuntime`. Only the server responds to automatic spawn
requests. The client materializes `Player.Character` and submits semantic intent
through `CharacterControlService`. Packaged client Script and ModuleScript source
is hydrated from the already-validated local package into matching replicated
objects because Script source is deliberately not sent by structural replication.

## Deliberately deferred

This slice does not add matchmaking, gargantuan-node integration, PlayerAuth,
DataStore, persistent account identity, motion warping, UI, gamepad
defaults, moving platforms, one-way collision, arbitrary query filtering, or a
full-featured slope/step motor. Humanoid compatibility is permanently excluded;
`Character` remains the canonical actor type. The shipped runtime
resources currently remain external payload files beside the executable; build
and distribution flows must preserve that runtime directory.
