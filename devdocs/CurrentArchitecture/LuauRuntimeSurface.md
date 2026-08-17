# Current Luau runtime surface

## Scope

This is an inventory of the gameplay-facing surface currently registered in the
runtime VM. It is not a Roblox compatibility claim. A class or datatype existing
in native code does not make an unlisted API available to scripts.

## Globals and libraries

- `game` is the current runtime `DataModel`. `game.Workspace` is a stable direct
  service property and does not depend on the visible service `Name`. Direct access
  to a registered lazy service only resolves it after construction; use
  `game:GetService("Tags")` before relying on `game.Tags`. This construction-order
  inconsistency is tracked by KI-004.
- `Instance.new(className)` constructs an allowed detached runtime Instance.
- `print(...)` emits an `Information / Luau` diagnostic. `warn(...)` emits a
  `Warning / Luau` diagnostic. Arguments are tab-separated.
- `task.defer`, `task.delay`, `task.spawn`, and `task.wait` use the runtime task
  scheduler and retain the originating script security context.
- The sandboxed standard Luau libraries are opened by the matching Luau VM.
- Registered constructors/globals are `Axes`, `CFrame`, `Color3`, `Enum`,
  `Instance`, `Random`, `Signal`, `TweenInfo`, `UDim`, `Vector2`, and `Vector3`.
  `EnumItem`, `Rect`, and signal connections are supported values but do not have
  standalone constructor globals.

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
- `IsA`, `GetFullName`, `GetChildren`, `GetDescendants`, and `ClearAllChildren`;
- child, descendant, and ancestor `FindFirst...` name/class/`WhichIsA` families;
- `GetPropertyChangedSignal`, `GetAttributeChangedSignal`, `IsPropertyModified`,
  and `ResetPropertyToDefault`.

`IsPropertyModified` is currently a partial compatibility stub that always
returns true; callers should not use it to detect real default-state differences.

The ancestry, child, descendant, and destroying signals declared on `Instance`
are exposed. Nullable lookups and nullable object-reference properties return
Luau `nil`. Separate userdata wrappers for the same live Instance compare equal
through generation-safe `ObjectId` identity; clones compare unequal to sources.

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
`GetAttributeChangedSignal`. The `Tags` service returned by
`game:GetService("Tags")` provides `Add`, `Remove`, `Has`,
`GetTags`, `GetTagged`, and `GetTaggedAll`. Schema-defined extension properties
and data-only custom class properties are available through their existing
bounded Instance methods and retain exact schema identity.

## Scripts, modules, and networking

`Script` source is authored through Studio and scripts present in the Play
snapshot run through the runtime scheduler. Runtime-created or newly adopted
scripts are governed by the existing descendant scheduling behavior; this
milestone does not add a dynamic-script-start contract. `Script` and
`ModuleScript` clones preserve exact source but reset source-version conflict
tokens, bytecode, and execution state. Instance-backed `require` has partial
relative/`@game` module resolution; filesystem configuration, aliases, and a
broader module system are not complete.

Remote event/function classes and bounded bindings exist for explicitly
configured replication sessions. Local Studio Play has no peer topology, and
this inventory makes no general gameplay networking guarantee.

## Major intentional gaps

Players, character control, a broader RunService API, TweenService service
semantics, input actions, DataStore/HTTP services, assets, debugger/profiler/LSP,
full module execution/configuration, edit-mode orbit/pan/zoom/focus, and broad
gameplay service compatibility remain outside the current surface.
