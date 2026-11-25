---
status: current
owner: runtime
last_verified: 2026-08-21
related_code:
  - assets/classes/
  - assets/runtime/
  - assets/services/
  - src/scripting/
  - tests/PlayerRuntimeTests.cpp
related_adrs: []
---

# Current Luau runtime surface

## Scope

This is an inventory of the gameplay-facing surface currently registered in the
runtime VM. It is not a Roblox compatibility claim. A class or datatype existing
in native code does not make an unlisted API available to scripts.

## Globals and libraries

- `game` is the current runtime `DataModel`. Direct member access to any registered
  canonical service and `game:GetService(name)` use the same DataModel-scoped lazy
  singleton path, regardless of which occurs first. Canonical lookup does not
  depend on the service's visible `Name`. `FindService` remains non-constructing;
  unregistered names retain ordinary child/member lookup behavior.
- `Instance.new(className)` constructs an allowed detached runtime Instance.
- `print(...)` emits an `Information / Luau` diagnostic. `warn(...)` emits a
  `Warning / Luau` diagnostic. Arguments are tab-separated.
- `task.defer`, `task.delay`, `task.spawn`, and `task.wait` use the runtime task
  scheduler and retain the originating script security context. Luau Signal
  Connect, Once, and Wait callbacks retain that context as well.
- The sandboxed standard Luau libraries are opened by the matching Luau VM.
- Registered constructors/globals are `Axes`, `CFrame`, `Color3`, `Enum`,
  `Instance`, `Random`, `RaycastParams`, `Signal`, `TweenInfo`, `UDim`, `UDim2`,
  `Vector2`, and `Vector3`. `AnimationTrack`, `EnumItem`, `Rect`, and signal
  connections are supported values but do not have standalone constructor
  globals.

`print`/`warn` format nil, booleans, numbers, strings, native `Vector3`, and
bounded type labels for Instances, EnumItems, other userdata, tables, functions,
and threads. Formatting never calls `__tostring` or other user code. A call
accepts at most 64 arguments, converts at most 512 UTF-8 bytes per argument, and
emits at most 2,048 UTF-8 bytes with an explicit truncation marker. Malformed
UTF-8 becomes `<invalid utf-8>`. One call is one diagnostic record even when its
string contains newlines.

During Play the path is `Luau -> ScriptEngine diagnostic callback -> bounded
PlaySession queue -> Studio Output`. The engine queue retains 256 ordered records
and evicts its oldest record on pressure; Studio retains 500 Output entries.
Neither queue blocks simulation waiting for the UI. Session identity gates polls,
so stopped-session diagnostics cannot be appended as a later active session.
Diagnostics are runtime observations only: they do not enter persistence,
snapshots, revisions, or transaction history.

## Instance lifecycle and hierarchy

Available properties are `Archivable`, `ClassName`, `Destroyed`, `Name`, and
nullable `Parent`. Available lifecycle/query methods are:

- `Instance:Clone()` and `Instance:Destroy()`;
- `IsA`, `GetFullName`, `GetChildren`, and `GetDescendants`;
- `GetAncestors` returns a nearest-parent-first snapshot. `IsAncestorOf`
  and `IsDescendantOf` test strict ancestry against the current parent chain.
  None of these queries includes the receiver. `ClearAllChildren` destroys
  the snapshotted direct children and their subtrees.
- child, descendant, and ancestor `FindFirst...` name/class/`WhichIsA` families;
- `GetPropertyChangedSignal`, `GetAttributeChangedSignal`, `IsPropertyModified`,
  and `ResetPropertyToDefault`.

`IsPropertyModified` is currently a partial compatibility stub that always
returns true; callers should not use it to detect real default-state differences.

`Destroy` and `ClearAllChildren` require `MutateDataModel` through generated
native-method dispatch, including inherited calls. Both ordinary Server and
Client runtime profiles have it; network-only inbound contexts do not.

The ancestry, child, descendant, and destroying signals declared on `Instance`
are exposed. Nullable lookups and nullable object-reference properties return
Luau `nil`. References to the same live Instance in one VM reuse its
userdata, including for dictionary lookups and `rawequal`. Clones receive
distinct keys. A retained wrapper for a destroyed Instance keeps its identity
as an existing table key after the native slot is reused.

`Clone()` requires every node in the source subtree to be live, `Archivable`,
constructible, and non-protected. It returns a complete detached subtree:

- the cloned root has `Parent == nil`, while descendant parent edges are kept;
- every node is a distinct native object with a fresh generation-safe identity;
- saved and supported semantic properties, exact custom class/schema identity,
  Attributes, Tags, extension/custom property state, script source, descendants,
  and internal object-reference topology are copied;
- internal references are remapped to cloned descendants; a supported external
  reference remains pointed at the same live object and normal first-adoption
  scope checks decide whether adoption is legal;
- signals/subscribers, Luau threads/coroutines, bytecode, physics bodies, renderer
  resources, and network/runtime connection state are not copied.

The detached result has no physics/render publication. Assigning its root to a
live runtime parent uses bounded atomic first adoption and creates normal runtime
subsystem state. `Parent = nil` after adoption keeps DataModel ownership. An
adopted object cannot move to another DataModel. Clone uses engine semantics and
never calls EditorHost structural Duplicate.

## Parts, Attributes, Tags, and extensions

`BasePart` currently exposes `Anchored`, `CanCollide`, `CanTouch`, `CastShadow`,
`CFrame`, `Color`, `Position`, `Rotation`, `Size`, and `Transparency`, plus
`ApplyImpulse`, `Touched`, and `TouchEnded`. `Part` additionally exposes `Shape`.

Attributes support removal with nil and bounded boolean, number, string,
`Vector2`, `Vector3`, `Color3`, `UDim`, `UDim2`, and `CFrame` values through
`SetAttribute`, `GetAttribute`, `GetAttributes`, and
`GetAttributeChangedSignal`. The `Tags` service returned by either `game.Tags` or
`game:GetService("Tags")` provides `Add`, `Remove`, `Has`,
`GetTags`, `GetTagged`, and `GetTaggedAll`. Schema-defined extension properties
and data-only custom class properties are available through their existing
bounded Instance methods and retain exact schema identity.

## Player input, Players, and kinematic characters

`UserInputService` remains the physical device service. `ActionMap` is the
canonical semantic gameplay layer with bounded key, mouse-button, and pointer-
delta binding/unbinding; `IsDown`, scalar, and transient vector reads; and
begin/change/end signals. Focus loss and frame boundaries clear active/transient
state as described in [Player runtime](PlayerRuntime.md).

The canonical `Players` service exposes nullable `LocalPlayer`, deterministic
`GetPlayers`, opt-out properties for default controller/camera/autoload, and
Player-added/removing signals. A runtime Player exposes immutable `PlayerId`,
nullable `Character`, load/reset/remove methods, and character lifecycle signals.
`KinematicCharacter` exposes the root, position, velocity, ground/floor state,
and capsule dimensions used by replaceable controllers.

`Workspace:MoveKinematicCapsule` returns bounded owned collision-resolution
data. `Workspace` also exposes `Raycast`, sphere/capsule/box casts, and
`GetPartsInSphere`, `GetPartsInCapsule`, and `GetPartsInBox`. These queries
accept optional `RaycastParams` filtering, flush pending body changes, and
return bounded owned hit or overlap values. See `PhysicsQueryFoundation1.md`
and the current declarations in `assets/classes/WorldRoot.luau` for signatures
and limits.

`AssetService` exposes `ResolveAsset`, `IsAssetAvailable`, and
`GetAssetMetadata` for bounded canonical catalog lookup. A skinned `MeshPart`
may directly contain an `Animator`; `Animator:LoadAnimation` creates a
runtime-only `AnimationTrack` from a compatible available Animation asset.
The track's playback controls and properties are native userdata bindings in
`src/animation/AnimationTrack.cpp`. Neither lookup nor track playback grants
raw asset bytes, filesystem access, or a persistent animation handle.

The integrated renderer authoring surface also includes `PointLight` and
`SpotLight` (concrete `LocalLight` descendants), `ReflectionProbe`, and
`Lighting.ShadowCascadeCount`, `ShadowDistance`, `EnvironmentRotation`,
`IblIntensity`, and `ToneMapping`. Their current declaration, setter, and
publication limits are documented in the public
[world and rendering API](../../docs/src/content/docs/api/world-and-rendering.mdx).
Radiance HDR import and material texture mip preparation remain native
authoring/renderer paths, not new Luau import or GPU-control methods. The
older BasePart list above is a historical subset; use the declarations and
generated catalog for all members.

The executable carries engine-shipped `DefaultActionMap`,
`DefaultCharacterRuntime`, `DefaultLocomotion`, and `DefaultCamera` Luau modules. They start in a
runtime-only unarchivable subtree and clean up on Stop. Games can serialize the
Players opt-out flags and continue using the low-level input, RunService, Camera,
and kinematic query APIs.

## Scripts, modules, and networking

`Script` source is authored through Studio and scripts present in the Play
snapshot run through the runtime scheduler. Runtime-created or newly adopted
Scripts enter the ScriptEngine activation queue. The queue is snapshotted at
the start of each Step, so a newly queued Script starts no earlier than the
next Step; a removed Script is skipped unless requeued before its turn, and
destruction prevents requeueing. Each Script keeps its own `script` global
through yields. Its initial coroutine root is released after a completed
`lua_resume`; separately rooted tasks, signal callbacks, and Remote work can
continue. Shutdown stops new callback admission and cleans tracked Scripts.
These are runtime lifecycle rules, not a Source authoring grant. `Script` and
`ModuleScript` clones preserve exact source but reset source-version conflict
tokens, bytecode, and execution state. Instance-backed `require` restores the
requiring Instance from its live full path and supports the `@game` alias used
by shipped modules. Filesystem configuration, additional aliases,
duplicate-name disambiguation, and a broader module system are not complete.

Configured network sessions expose structured events, bounded invocations,
Player targets, and explicit peer addresses. The server resolves the sending
Player from its live session. Local Studio Play has no peer topology.
[Remote contracts and compatibility](LuauRemotes.md)

[Property tweening](TweenRuntime.md) schedules bounded reflected property writes
on Main and uses the caller's gameplay property capabilities.

## Major intentional gaps

Final multiplayer Players behavior, a broader RunService API, gamepad action sources,
DataStore/HTTP services, debugger/profiler/LSP, full module
configuration, edit-mode orbit/pan/zoom/focus, and broad gameplay service
compatibility remain outside the current surface. Humanoid compatibility is
permanently excluded: canonical `Character` plus narrow native primitives and
replaceable Luau composition is the supported actor model.
