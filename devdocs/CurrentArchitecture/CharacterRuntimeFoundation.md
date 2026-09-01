---
status: current
owner: runtime
last_verified: 2026-09-01
related_code:
  - assets/classes/Character.luau
  - assets/classes/KinematicCharacter.luau
  - assets/classes/Player.luau
  - assets/runtime/DefaultCharacterRuntime.luau
  - assets/runtime/DefaultLocomotion.luau
  - include/gargantuan/classes/Character.hpp
  - src/classes/Character.cpp
  - src/services/Players.cpp
  - tests/PlayerRuntimeTests.cpp
related_adrs: []
---

# Character runtime foundation

## Decision and scope

`Character` is Gargantuan's canonical native semantic for a movable gameplay
entity. It is a schema-backed `Folder`, not a body-layout aggregate, controller
state machine, or player identity. `KinematicCharacter : Character` installs
the current capsule movement specialization. `Player.Character` points to the
base `Character`; an NPC or AI-owned Character does not need a `Player`.

There is deliberately no native `Humanoid`. Health, locomotion modes, body-part
assumptions, animation graphs, combat, climbing, ragdoll, and game-specific
states remain composition and Luau policy. Adding them to one native object
would couple unrelated games to an irreversible aggregate before those
contracts exist.

## Public/schema model

| Member | Contract | Why native |
| --- | --- | --- |
| `CFrame` | Saved, future-replicated authoritative transform. Authored writes use the normal mutation and journal path. | Physics, render, semantic, persistence, and future replication need one canonical state. |
| `Position` | Non-persisted alias of `CFrame.Position`. | Preserves a small ergonomic read/write seam without duplicating state. |
| `RootPart` | Optional saved, future-replicated descendant `BasePart`; synchronized to accepted Character transforms. | A generation-safe reference is required to publish accepted spatial state. |
| `Move(Translation, Velocity?)` | Luau request for bounded collision-authoritative movement; returns accepted position, translation, velocity, contact/floor data, and truncation flags. | Luau cannot safely mutate the physics backend or serialize a main-thread controller query. |

`Move` is the only public motion admission method. It requires
`MutateDataModel`, accepts only finite vectors, and fails if the Character has
no supported native movement specialization. The C++-only
`CharacterMotionRequest` adds source, local-space translation, and bounded yaw
for Animation Foundation 3A. The C++-only result distinguishes requested and
applied motion and records controller/physics time. Those types are an internal
authority seam, not a second transform API.

Native `Character` is responsible only for:

- generation/lifecycle validation;
- finite and per-step translation/yaw limits;
- dispatch to the installed `KinematicCharacter` capsule primitive;
- committing the accepted transform through one simulation path;
- synchronizing the optional RootPart and its physics/render listeners;
- keeping authored property edits distinct from transient Play simulation; and
- returning requested-versus-accepted motion for diagnostics and future
  intentional motion warping.

Accepted simulation movement does not construct an authoring ChangeJournal
record. It updates the runtime clone's authoritative transform, fires already
registered property listeners, marks RootPart transform dirtiness, and lets the
world synchronize its collider at the next safe point. Authored `CFrame` writes
retain normal persistence, history, and future-replication journaling. This
separation prevents Play movement from dirtying or revising the authoring
world and removes a per-Character per-frame heap payload.

## Engine-shipped Luau policy

The shipped Luau layer owns all replaceable default behavior:

- `DefaultCharacterRuntime.luau` assembles one `KinematicCharacter`, creates a
  non-colliding visual RootPart, handles the Player spawn signal, and owns
  teardown;
- `DefaultLocomotion.luau` maps semantic actions to camera-relative movement,
  integrates gravity, decides jump/ground/slope state, previews bounded step
  queries, and commits every accepted segment through `Character:Move`;
- `DefaultActionMap.luau` names the default physical bindings; and
- `DefaultCamera.luau` follows `Player.Character` and owns orbit policy.

Walk speed, gravity integration, jump speed, walkable-floor threshold, step
height, air-jump policy, camera behavior, and future clip selection are not
native Character state. A game can set `Players.DefaultControllerEnabled =
false` before Engine startup and install an ordinary project Script. The player
runtime test does exactly that, assembles a Character, calls `Character:Move`,
and proves no native default movement continues underneath it.

## Bootstrap order and replacement

Gargantuan's literal `.gargantuan/prerun.luau` phase is schema-only. It runs
before DataModel construction with `DefineSchema` authority and therefore
cannot create Players, Characters, runtime modules, or signal connections.
Character's native schema is registered during native schema bootstrap.

After the runtime DataModel exists, Engine initializes `LocalPlayer`, mounts
the bounded engine-shipped modules, and executes `DefaultPlayerRuntime` as the
engine bootstrap before the ordinary project Script queue. This is the
runtime-module equivalent of character pre-run, while preserving the security
boundary of the true schema PreRun. It initializes once; the unarchivable
`PlayerRuntimeModules` subtree and all connections are destroyed before VM and
world teardown. Ten Play/Stop cycles and a pre-existing project Script verify
ordering, no duplicated bindings, and cleanup.

Replacement mechanisms are intentionally simple:

- `DefaultControllerEnabled = false` disables assembly and locomotion;
- `DefaultCameraEnabled = false` disables the shipped camera independently;
- `CharacterAutoLoads = false` retains the spawn contract but suppresses the
  automatic first load; and
- project Luau may use `CharacterSpawnRequested`, set `Player.Character`, or
  manage NPC Characters without native engine changes.

## Authority, collision, gravity, and lifecycle

Each Character has exactly one transform owner: Character admission. Animator
submits a root-motion request; default or custom Luau submits a `Move` request;
neither writes the MeshPart world transform as an alternative authority. The
current specialization calls the existing bounded multi-plane kinematic
capsule solver. Collision may fully accept, clip, block, or slide a request.
Rejected displacement is discarded for that interval and is never accumulated
as hidden debt.

Root-motion translation includes its authored vertical component. Gravity is
not implicitly added by Animator; the default locomotion module continues to
integrate gravity and supplies the current velocity. Foundation 3A supports
world-up yaw only. The capsule is rotationally symmetric, so yaw cannot rotate
an oriented collider through geometry. Arbitrary root pitch/roll remains in
the visual residual pose.

`Player` replacement and destruction operate on `Character`, not
`KinematicCharacter`. Character destruction clears `Player.Character` if one
exists; NPC destruction needs no Player coordination. RootPart must be a live
Character descendant. ObjectId plus generation/liveness checks reject stale
animation work, and teardown invalidates tracks and connections before their
VM or DataModel disappears.

## Persistence, packaging, headless, and future networking

Characters are ordinary schema Instances. Their authored hierarchy, CFrame,
RootPart reference, Mesh assets, Animator, and project scripts pass through the
existing save, clone, and package systems; there is no Character asset type.
Engine-shipped modules are staged in `runtime/` and included in the package
closure. They are resolved relative to the executable, so relocated packages
do not depend on the source tree.

Headless/server execution owns the same Character admission path and requires
no renderer. A future replication layer can publish accepted Character state
plus animation AssetId/content revision, action identity/start time, and
control revision. Client prediction and correction must reconcile to accepted
Character state, never accept a client-supplied root position as authority.
The same boundary can later hand accepted motion to region/portal transfer
without teaching Animator about global topology.

## Bounds and explicit deferrals

One request is limited to 16 metres of translation and 90 degrees of yaw.
Motion, velocity, transforms, capsule dimensions, sampled keys, and results
must be finite. Physics contact iteration and planes retain their existing
bounds. Root-motion request storage is capped by the 4,096-Animator runtime
bound and reused after warmup.

Foundation 3A does not add Humanoid, health, a native locomotion graph, IK,
retargeting, motion warping, ragdoll, climbing/vaulting/combat systems, root
motion prediction, a replication protocol, portal transfer, a Character
editor, or an animation graph/timeline UI.
