# Post-Play Instance and input correctness

## Binding invariant

A null shared native object always pushes Luau `nil`; only a non-null pointer may
be boxed as tagged userdata. This covers nullable properties and all current
child, descendant, and ancestor lookup families. A valid wrapper retains shared
ownership. A wrapper held after `Destroy()` remains memory-safe but reads, writes,
and methods other than idempotent `Destroy()` fail as a destroyed Instance.

`game.Workspace` resolves through service identity rather than visible `Name`, so
renaming it does not break access. New services receive their canonical name
before first parenting; new projects persist `Workspace`, not `Instance`.

## Detached lifecycle and first adoption

`Instance.new()` creates a shared, unscoped Instance. It may receive a lazy,
generation-safe `ObjectId`, but has no DataModel scope, journal publication,
renderer extraction, or physics presence. Detached Instances may form a subtree.

Assigning a detached root to a live parent performs first adoption:

1. collect and bound only the candidate subtree;
2. reject cycles, stale nodes, excessive count/depth, missing schema, foreign
   ownership, and persisted/replicated object references that would escape the
   target DataModel;
3. reserve the parent edge and DataModel ownership count;
4. associate every node with the target DataModel;
5. install the edge and publish through existing scoped ObjectId and hierarchy
   paths.

Preflight is O(subtree), and no descendant is associated or published before it
succeeds. Parts enter existing physics and render paths through hierarchy
notifications. Persisted/replicated constraint references may target the adopted subtree or a live
node in the target DataModel.

`Parent = nil` removes hierarchy, physics, and render presence but retains the
original DataModel association and identity. It is ordinary unparenting, not a
return to never-adopted state. Same-DataModel reparenting remains valid;
DataModel A to DataModel B is always rejected, including after unparenting.

Destroying a never-adopted Instance invalidates lazy identity without publishing.
Destroying an adopted Instance removes subsystem state, publishes destruction in
its scope, invalidates registry generation, and releases ownership. Unreachable
detached or unparented Instances use existing shared-userdata lifetime cleanup.

Play owns a separately deserialized runtime DataModel, so runtime adoption never
mutates the authoring graph. Stop discards it without changing authoring revision,
dirty state, hierarchy, or history.

## Mutation diagnostics

`MutationStatus` has one bounded formatter shared by Luau and EditorHost results.
Luau property errors identify class/property and distinguish wrong type, stale
target/reference, validation, authority, and resource rejection. `SetParent`
preserves hierarchy-cycle, destroyed-parent, cross-DataModel, and adoption errors.
Diagnostics expose no pointers, registry internals, or authority tokens.

## Play pointer input

Studio forwards one semantic pointer-button event for a press or release from the
focused Play viewport. EditorHost validates the closed button/state vocabulary and
constructs `HostEvent`. `PlaySession` returns `HostEventResult`, so the existing
free-camera relative-pointer command reaches Studio.

RMB down captures only when requested; RMB up releases. Focus loss releases Studio
capture and clears runtime keys/buttons and relative mode. Stop, session disposal,
and EditorHost disconnect also release capture and reset the motion baseline.

## Validation matrices

| API | Found | Missing |
| --- | --- | --- |
| `FindFirstChild` | live `Instance` | `nil` |
| `FindFirstChildOfClass` | live `Instance` | `nil` |
| `FindFirstChildWhichIsA` | live `Instance` | `nil` |
| descendant lookup variants | live `Instance` | `nil` |
| ancestor lookup variants | live `Instance` | `nil` |
| nullable `Parent` | live `Instance` | `nil` |

| State transition | Result |
| --- | --- |
| constructed or cloned -> detached subtree | valid, unscoped |
| detached subtree -> live parent | atomic first adoption |
| adopted -> same DataModel parent | valid reparent |
| adopted -> `Parent = nil` | unparented, still DataModel-owned |
| DataModel A -> DataModel B | rejected |
| live -> `Destroy()` | monotonic destroyed/stale state |

The follow-up core Luau QoL pass added bounded `print`, normalized `warn`, and
engine-owned detached `Clone`; see `LuauRuntimeSurface.md`. Richer logging,
edit-mode viewport navigation, character control, service expansion, assets, and
networking expansion remain outside this correctness pass.
