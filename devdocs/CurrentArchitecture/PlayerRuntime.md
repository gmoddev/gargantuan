---
status: current
owner: runtime
last_verified: 2026-09-01
related_code:
  - assets/classes/KinematicCharacter.luau
  - assets/classes/Player.luau
  - assets/runtime/
  - assets/services/ActionMap.luau
  - assets/services/Players.luau
  - include/gargantuan/physics/
  - src/classes/Player.cpp
  - src/physics/Box3DPhysicsBackend.cpp
  - src/services/ActionMap.cpp
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
- `ActionMap` maps bounded physical bindings to semantic action state;
- `Players` owns the runtime-local gameplay player lifetime;
- `Player` owns the relationship to one replaceable runtime character;
- `KinematicCharacter` stores public controller state instead of hiding it in
  one copied controller script;
- `Workspace:MoveKinematicCapsule` supplies neutral bounded collision data; and
- engine-shipped Luau owns default bindings, assembly, movement, step/jump
  policy, and camera policy.

```text
native host input -> UserInputService -> ActionMap
                                     -> shipped/replacement Luau policy
Workspace geometry -> PhysicsWorld -> MoveKinematicCapsule result
Players -> Player -> KinematicCharacter -> visible RootPart
```

The low-level path remains supported: a game can disable both defaults and use
`UserInputService`, `RunService`, `Camera`, and
`Workspace:MoveKinematicCapsule` directly.

## ActionMap

`ActionMap` is a canonical DataModel service above `UserInputService`. It does
not replace physical input signals or device state. Its public methods are:

| API | Current contract |
| --- | --- |
| `BindKey(ActionName, KeyCode, Scale, Priority, Consume)` | Add one keyboard or normalized gamepad-button binding and return a positive runtime binding ID. |
| `BindMouseButton(ActionName, InputType, Scale, Priority, Consume)` | Add one of the three supported mouse-button bindings. |
| `BindPointerDelta(ActionName, Scale, Priority, Consume)` | Add scaled frame-accumulated relative pointer delta. |
| `Unbind(BindingId)` / `UnbindAction(ActionName)` | Remove one binding or every binding for an action. |
| `IsDown`, `GetValue`, `GetVector` | Read digital, scalar, or transient vector action state. |
| `GetBindingCount()` | Observe current bounded binding usage. |

`ActionBegan`, `ActionChanged`, and `ActionEnded` publish semantic transitions.
Multiple digital bindings sum and clamp to `[-1, 1]`; multiple pointer bindings
for the same action accumulate before one change notification. Matching
bindings publish in descending priority and stable binding-ID order. `Consume`
marks the host event handled after `UserInputService` has recorded physical
state, preventing the legacy native camera route from also acting on it. It
does not hide physical state from `UserInputService`.

Focus loss ends active actions and clears transient Look state. Pointer vectors
also reset at the engine frame boundary. The native service accepts at most 128
action names, 512 bindings, and 64 UTF-8 bytes per action name. Mutation requires
`MutateDataModel`.

The shipped `DefaultActionMap.luau` is the player-policy location that names W,
A, S, D, Space, and RMB. It declares `MoveForward`, `MoveBackward`,
`MoveLeft`, `MoveRight`, `Jump`, `CameraOrbit`, and `Look`. The controller itself
depends only on those semantic names. While the shipped camera is enabled,
these bindings consume the legacy native-camera route without hiding physical
state from `UserInputService`. Binding shape, priority, and multiplicity
leave room for later remapping. Interaction Foundation separately ships
`DefaultInteractionRuntime.luau`, which maps keyboard E and normalized gamepad
South / `ButtonA` to the `Interact` semantic action. It does not add a second
input map.

## Players and character lifecycle

`Players` is a canonical lazy DataModel service. An authoring DataModel does not
invent a local player. Constructing a runtime `Engine` creates exactly one
`LocalPlayer` for the current local Play model, assigns nonzero `PlayerId = 1`,
parents it under `Players`, and fires `PlayerAdded`. `GetPlayers()` returns live
players in deterministic identity order.

`PlayerId` is gameplay identity. It is not an EditorHost connection ID,
transport peer ID, socket, authentication identity, or object address. No
current code connects `Player` to gargantuan-node, PlayerAuth, matchmaking, or
replication ownership. A separate native-only, immutable `PlayerIdentity`
(`Provider`, `Subject`) seam now supplies EntitlementService checks. Local Play
initializes `(local, player-1)`; a future authenticated server bootstrap may
initialize the same engine-owned type. Luau cannot submit or mutate it.

`Player.Character` is nullable and accepts any live canonical `Character` in
the same DataModel. The default assembly chooses `KinematicCharacter`, but an
NPC can use Character semantics without a Player and future movement
specializations do not require changing the Player schema. The lifecycle is:

1. `LoadCharacter` or `ResetCharacter` removes the old character.
2. `CharacterSpawnRequested` fires synchronously for replaceable Luau assembly.
3. Assembly assigns the new character, committing the relationship before
   `CharacterAdded` fires.
4. `RemoveCharacter`, replacement, external character destruction, Player
   teardown, or Engine Stop fires `CharacterRemoving` and clears the relation.
5. Player-owned removal/replacement destroys the old character exactly once.

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
