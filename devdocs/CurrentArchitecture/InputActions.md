---
status: current
owner: input-runtime
last_verified: 2026-11-03
---

# Input action foundation

## Decision and authority

`InputService -> InputContext -> InputAction -> InputBinding` is the canonical
authoring graph. These are normal native schema-backed Instances, with ordinary
parenting, names, generation-safe identity, destruction, reflection and project
persistence. Convenience constructors create the same objects as `Instance.new`.
They never create a parallel registry. Instance configuration is authoritative;
`CompiledInputGraph` is a disposable, runtime-local projection.

This decision preserves the Foundation runtime, mutation gateway, schema,
EditorHost and network authority contracts. There is no general ECS framework,
new platform input pipeline, process boundary or wire-format revision.

Definitions use saved and ordinary replicated property metadata. Studio creates
and edits them through existing EditorHost commands, transactions and Undo/Redo.
Project saving uses the existing archivable authoring graph; runtime-created
definitions follow normal runtime ownership. Constructors do not mark an
otherwise runtime-only subtree persistent. Saving/reopening allocates normal new
runtime ObjectIds; identity is stable during a lifetime, not a new durable UUID.
Project establishment creates the persistent InputService root before publishing
its initial revision, so a new Studio document can parent definitions without
attempting to insert a protected singleton service.

Detaching, disabling or misparenting a definition never destroys it. Only direct
service/context/action/binding chains compile. Incomplete or invalid bindings
remain inspectable and produce bounded diagnostics. Normal scalar setters reject
invalid enum values, nonfinite scales and unsupported modifier keys before commit.
Cross-property tuple validation occurs at construction and compilation so manual
authoring can pass through incomplete intermediate states.

## Public API

```luau
local Input = game:GetService("InputService")
local Gameplay = Input:AddContext("Gameplay", { Priority = 100 })
local Save = Gameplay:AddAction("Save", { Type = Enum.InputActionType.Bool })
Save:AddBinding(Enum.KeyCode.S, {
    Modifiers = { Enum.KeyCode.LeftControl, Enum.KeyCode.LeftShift },
})
Save.Pressed:Connect(function()
    print("[Game:Save] Save requested")
end)
```

Equivalent manual authoring:

```luau
local Context = Instance.new("InputContext")
Context.Name = "Gameplay"
Context.Priority = 100
Context.Parent = Input
local Action = Instance.new("InputAction")
Action.Name = "Save"
Action.Type = Enum.InputActionType.Bool
Action.Parent = Context
local Binding = Instance.new("InputBinding")
Binding.KeyCode = Enum.KeyCode.S
Binding.Modifier1 = Enum.KeyCode.LeftControl
Binding.Modifier2 = Enum.KeyCode.LeftShift
Binding.Parent = Action
```

| Instance | Configuration / methods |
| --- | --- |
| InputService | `AddContext(Name, Options?)`, `GetRuntimeSnapshot()` |
| InputContext | `Enabled=true`, `Priority=0`, `Consume=true`; `AddAction(Name, Options?)` |
| InputAction | `Enabled=true`, `Type=Bool`; `AddBinding(KeyCode, Options?)`, `IsDown()`, `GetValue()`, `GetVector()` |
| InputBinding | `Enabled=true`, `Type=Key`, `KeyCode=None`, `InputType=None`, `Modifier1..4=None`, `ExactModifiers=false`, `Scale=1`, `VectorScale=Vector2.one`, `Priority=0`, `Consume=false` |

`AddContext` accepts Enabled/Priority/Consume. `AddAction` accepts Enabled/Type.
`AddBinding` accepts Enabled/Modifiers/ExactModifiers/Scale/Priority/Consume.
Unknown options, wrong enum types, sparse arrays, duplicate modifiers, primary
key equal to a modifier and more than four modifiers reject before parenting.
The four individual reflected slots avoid adding arrays to the closed WireValue
format. Modifier slot order has no semantic significance.

`InputActionType` contains Bool, Scalar and Vector2. Bool ORs active sources.
Scalar sums finite binding contributions and clamps to [-1,1]. The first slice
uses Vector2 for existing frame-accumulated relative-pointer compatibility;
keyboard vector composites and gamepad axes are deferred. Digital scalar state
and transient vector state remain distinct for mixed legacy actions.

`Pressed` fires on false->true, `Changed(Vector2)` on a value transition,
`Released` on true->false, and `Canceled(Reason)` additionally on nonphysical
termination. All action values commit before any callbacks run. Bool/Scalar
Changed uses `(Value,0)`; pointer Changed uses the accumulated vector. Vector
frame reset is silent, preserving existing camera semantics.
Within one transition, publication order is Pressed (when beginning), Changed,
then Canceled (when applicable) and Released (when ending).
For Bool/Scalar actions, `GetVector()` provides `(GetValue(), 0)`; Vector2
actions return their frame pointer accumulator. `GetValue()` always reads the
separate scalar channel, including mixed legacy actions.

## Deterministic evaluation

1. UserInputService records physical input and retains its current public
   behavior. GuiRuntime routes the same HostEvent. InputService receives every
   event with the GUI-consumed flag, then unconsumed input reaches native Camera.
2. A chord admits only on a fresh, nonrepeat primary press while required
   modifiers are already held. Late modifiers never retroactively press an action.
   Extra modifiers are allowed unless ExactModifiers is true. Left/right Ctrl,
   Shift, Alt and Meta are distinct eight mask bits. CapsLock/NumLock are not chord
   modifiers. The primary modifier's own bit is excluded from exact comparison.
3. Key repeats do not create another Pressed. Releasing the primary or a required
   modifier ends admission. GUI-consumed presses never activate; consumed releases
   still retire active sources. Focus loss clears admitted keys and action state.
4. Contexts sort by descending Priority, then full ObjectId. Bindings sort within
   a context by descending Priority, then full ObjectId. Equal-priority outcomes
   are stable within that graph lifetime, independent of display name.
5. A matching consuming canonical binding claims its primary physical input for
   its context. Other actions in that context may cofire. Lower contexts become
   suppressed; modifier keys are prerequisites, not separately consumed resources.
   Different primary keys do not conflict just because they share a modifier.
   An enabled higher context can claim a held key while waiting for a fresh press:
   lower actions cancel, and the higher action does not invent a Pressed edge.
6. Disable/reparent/destroy/rebind cancels affected sources at the next input,
   frame, inspection or explicit native compilation boundary. Changed/re-enabled
   canonical definitions require a fresh primary press. Suppressed sources do not
   spring back while held. Unchanged definitions retain state across unrelated
   edits. Priority changes follow this same fresh-press policy.

Callbacks may author later changes. They mark definitions dirty; compilation
does not recurse during publication. Recursive semantic event submission during
publication is ignored. Destroyed ObjectIds never resolve to replacement objects.

## Derived layout and bounds

The evaluator owns compact context/action/binding rows, full generation-safe IDs,
side-specific modifier masks, primary-code candidate buckets, modifier dependency
buckets, action-to-binding indices, pressed/admitted bits and fixed transition
scratch storage. It owns no Instance pointers or callbacks.

Ordinary events use indexed candidates and affected actions; they do not walk
the Instance hierarchy, build match vectors or allocate container nodes. Modifier
changes visit dependent held primary buckets. Default signal/Luau dispatch is
outside this allocation claim. Dense conflicts remain bounded work proportional
to candidate count and the affected actions' binding lists.

Definition notifications coalesce until a safe point. The service takes a bounded
configuration snapshot only when dirty. The compiler compares full identities and
definitions, preserving unchanged admission. It repacks/sorts affected primary
buckets; removing/inserting dense rows repacks occupied buckets whose indices
move. Cold validation/snapshot extraction and temporary compiler maps remain
O(total definitions), deliberately outside the physical event path.

Limits: 64 contexts, 256 actions, 2048 bindings, four modifiers per binding,
4096 logical key slots, at most 512 transitions per operation, 64 validation
messages. Service authoring inspection also caps attempted children, including
invalid entries, at the sum of graph capacities. Aggregate overflow disables the
derived graph with a diagnostic; it never evaluates stale configuration.
Snapshot construction allocates bounded copies only on request.

## Compatibility and security

ActionMap becomes a name/positive-handle compatibility facade. Bind methods
create canonical Instances under an unarchivable adapter-owned context. Handle
tables contain weak Instance references, not another semantic configuration.
Existing 128-action/512-binding/64-byte-name API limits remain. The facade caches
named actions by definition epoch; ties prefer context priority then full IDs.
Direct Instance APIs remain preferable when names repeat across contexts.

Legacy Consume only prevents lower host/camera routing; legacy bindings cofire
and sum as before. A native generation-safe compatibility-context flag retains
that exception inside the single evaluator. Canonical consuming contexts can
suppress the legacy context. Shipped default modules fill only missing named
actions and remove only bindings they created; developer-authored definitions
are never overwritten. Movement, jump, orbit, Look and E/ButtonA interaction keep
the existing controller API and physical pipeline.

Pressed/admitted state, action values, diagnostics and transitions are local,
transient and absent from authoring revisions, persistence and replication.
Definition replication does not replicate device state. A client binding firing
never grants server gameplay authority; CharacterControlService and existing
network admission remain unchanged. Dedicated server hosts still initialize no
platform input backend.

## Inspection contract v1

`Input:GetRuntimeSnapshot()` returns an owned table copy. It has Version, Scope,
Revision, MemoryBytes, Modifiers, PressedKeys, Contexts, Actions, Bindings and
ValidationErrors. IDs are explicit Slot/Generation tables. Context rows expose
Enabled/Priority/Consume. Actions expose Enabled/Down/Value/Vector and last
transition sequence/reason. Bindings expose Matched/Active/Suppressed/Eligible
and WinningContext. Native inspection also exposes complete compiled definitions,
changed binding count, reindexed bucket count and evaluated candidate count.
Reading or editing the returned table cannot mutate the runtime or authoring graph.
PressedKeys contains numeric KeyCode values. Modifier bits 0..7 are left/right
Control, left/right Shift, left/right Alt and left/right Meta, in that order.
Snapshot Value is the scalar channel and Vector is the raw pointer accumulator;
the Bool/Scalar `GetVector()` convenience conversion is not applied to that field.

Cancellation reasons are None, Released, FocusLost, DefinitionChanged, Disabled,
Suppressed and GuiConsumed. Snapshots are latest-state observations, not unbounded
event history. Validation errors are bounded text in this slice; associating every
error with an offending ObjectId and adding an EditorHost runtime DTO are follow-on
debugger work. No private Engine pointer or native handle crosses EditorHost.

## Qualification and subsequent stages

`InputActionTests.cpp` covers hierarchy/persistence, schema and EditorHost
authoring/history, Luau constructor equivalence, chords, focus, context changes,
rebinding, lifecycle, stale IDs, diagnostics and reentrant callbacks. Existing
player/platform/interaction suites remain compatibility gates.

`InputActionBenchmark.cpp` retains a benchmark-only renamed copy of ActionMap at
`eb2ce26ab` and compares 16/128/512 bindings, 1/8/32 compiled context partitions,
spread and dense candidates. It records event P50/P95/P99, ordinary executable
new/new[] requests, capacity-based storage estimates and edit compilation latency.
Baseline has no contexts; comparisons use passthrough partitions. Compiler timing
excludes Instance snapshot extraction; compiled signal dispatch is excluded while
baseline empty native signals remain included. Results must state these limits.
There is no performance acceptance threshold or unsupported improvement claim.

Next stages: extend the read-only EditorHost inspection DTO and Studio visualizer;
add source-associated validation and editor chord authoring; qualify physical
device behavior; design explicit vector composites/gamepad axes/touch gestures;
then author remapping UI. Each stage retains the same Instance authority and
single evaluator. See InputActionsValidation.md for exact source/test evidence
and continuation commands.
