# Runtime foundation: ownership, identity, jobs, and changes

## Status and scope

This document describes the runtime authority and lifecycle contract, including
the measured Runtime Foundation 6 core changes. Subsystem completeness and
qualification belong to their current architecture documents and acceptance
records; this document does not certify parallel simulation or product capacity.

The governing rule is:

> Parallelize computation; serialize authoritative state mutation.

The `Instance`/`DataModel` hierarchy remains authoritative state owned by the
`Main` execution domain. Worker jobs may compute independent results, but they
must hand results back to the authoritative domain before mutating Instances.
The hierarchy is deliberately not made generally thread-safe with per-object
locks.

## Instance ownership and destruction

Parents own children with `shared_ptr`. A child now retains its parent with a
`weak_ptr`, so retaining a child cannot keep its ancestry alive and an expired
parent cannot become a dangling pointer. `SetParent` rejects the object itself
and every descendant as a new parent.

Never-adopted Instances may form a detached subtree. Their first parent in a
DataModel atomically adopts that bounded subtree after lifecycle, schema, depth,
count, ownership, and object-reference preflight. `Parent = nil` after adoption
retains its DataModel association. An adopted Instance cannot migrate to another
DataModel. See `PostPlayValidation.md` for the complete state matrix.

`Instance:Clone()` creates a bounded, independent detached subtree through
engine-owned semantic serialization. The root is unparented, internal hierarchy
and object-reference topology are retained, and first adoption creates ordinary
runtime subsystem state. Clone never copies signals, execution state, or backend
resources and never routes through EditorHost authoring authority. See
`LuauRuntimeSurface.md` for the copied-state contract.

Hierarchy mutation is committed before ancestry notifications run. Callbacks
therefore observe the new parent/child collections rather than a half-applied
operation. Callbacks remain synchronous and can initiate a later mutation; a
broader transaction/notification queue is future work.
Per-property signals are allocated when requested through
`GetPropertyChangedSignal`. Ordinary reflected property, Parent and Destroyed
notifications do not allocate per-property signals unless requested. Existing
subscriptions still fire synchronously after the committed change, including
during nested mutation. The separate custom-schema property path still creates
its notification signal on commit.

Destruction has a monotonic lifecycle:

1. the object enters its private destroying state;
2. `Destroyed` becomes true and its registry entry is invalidated;
3. the committed property notification and `Destroying` signal run;
4. children are destroyed and the object is detached;
5. a destroy change is committed.

Repeated or reentrant `Destroy` calls are no-ops. `Destroyed` is reflected as a
read-only, non-editable property; scripts and ordinary native callers cannot
move the lifecycle backwards.

## Engine composition lifetime

Engine constructor-body failure uses a local RAII construction lifetime to run
normal Engine teardown before its initialized members are destroyed. This
disconnects native world/descendant callbacks, stops content/provider workers,
detaches runtime services, and clears script-owned handlers while their VM is
alive. A world retained by the caller cannot retain callbacks into an Engine
whose construction failed. Ordinary destruction remains idempotent. Hosts and
fixtures must also keep the borrowed renderer alive until Engine destruction,
and keep Engine alive until all borrowing GameSessions are destroyed.

## ObjectId semantics

`ObjectId` is a pair of 32-bit values: a registry slot and its generation. It is
not derived from a memory address. Identity is published lazily once the object
has shared ownership, because a safe registry entry stores a `weak_ptr` rather
than a borrowed pointer.

- A zero slot or generation is invalid.
- Lookup succeeds only when the slot and generation match and the weak object is
  still alive.
- `Destroy` invalidates lookup before destruction callbacks run.
- A slot may be reused after invalidation, but its generation is incremented.
- A stale ID never resolves to the replacement object in that slot.
- A slot is permanently retired at maximum generation rather than wrapping to
  an identity that could collide with historical input.

IDs are native-only from Luau's perspective. `WireObjectId` serializes the same
slot/generation pair for snapshots, references, journal targets, and DataModel
scope identity. Receiver materialization resolves source IDs through a
session-owned map rather than forcing them into the local registry.

## JobSystem and execution domains

`JobSystem` owns a fixed worker pool. Its initial API provides thread-safe
submission, `JobGroup` completion, draining, and explicit shutdown. Shutdown
drains by default; non-draining shutdown drops queued jobs but allows active jobs
to finish. New submissions are rejected after shutdown.

Exceptions are caught at the worker boundary. The first exception associated
with a group is retained for inspection and does not terminate a worker.
Cancellation tokens and work stealing are intentionally absent.

The defined affinity categories are `Main`, `Worker`, `Render`, `Simulation`, and
`IO`. Only `Main` and `Worker` currently select real execution behavior; the
others reserve vocabulary for later boundaries. Generated property setters,
parenting, and destruction reject mutation outside `Main`. Arbitrary Luau is not
submitted to workers.

## Reflection schema metadata

`InstanceProperty` remains the single property schema. It now has explicit
metadata for:

- persistence (`Transient` or `Saved`, selected by `SetSerializable`);
- future replication (`None` or `FutureReplicated`);
- write authority (`Main` or `Any`);
- editability;
- semantic editor datatype and closed wire type;
- category/group, optional numeric range, and optional editor hint;
- compound datatype identity;
- stable native-enum type/item identity;
- nullable object-reference class constraints;
- an optional native validation predicate;
- existing read/write permission levels.

The class generator accepts the corresponding declarative fields. Snapshot and
incremental replication now consume `FutureReplicated` explicitly; persistence
consumes that same canonical property policy. Main-domain authority,
read-only state, and validation are checked on wired mutation paths.

Project custom classes reuse this canonical class model. Their actual runtime
schema identity is stored separately from the native C++ implementation host,
so `ClassName`, `IsA`, reflection, persistence, and replication see the custom
class while native ownership and destruction continue through an engine-owned
host. The initial `DataOnly` host policy permits only `Engine.Folder`; project
code cannot register constructors, callbacks, methods, or lifecycle hooks.

## Ordered committed changes

`ChangeJournal` assigns monotonic sequence numbers per DataModel scope while
holding its journal lock. Its payload model represents object creation,
reflected/extension/attribute updates, tag changes, reparenting, and destruction.
First adoption publishes a detached subtree into one scope. Moving an already
adopted subtree between DataModel scopes is rejected.
Ordinary batch commits append under that lock and roll back any appended tail
if allocation fails. They evict the oldest retained records only after the
whole batch has appended, so failed batches do not consume sequence numbers
or partially evict history.
The prepared-property qualification executable also sweeps ordinary batch
allocation failures while a stream is full. Its test allocator records whether
a failure happened after a record was appended, then verifies exact retained
handle identity, sequence and eviction counters through rollback and a
successful ordered retry. Renderer dirtiness may conservatively request a full
resync after failure; this journal guarantee does not imply dirty state rollback.

Records remain an in-process prototype. Retention is bounded per scope and cursor reads
detect eviction with `ResnapshotRequired`; the current default capacity is 16,384.
Committed records are immutable and internally held by shared handles. `ReadPinned`
captures a bounded sequence and its cursor under the journal lock, then lets the
caller examine that stable snapshot without keeping the lock or copying payloads.
Eviction, clear, and scope retirement drop journal ownership but cannot invalidate
an active pinned read. The existing value-returning `Read` still returns independent
record copies, with the same cursor and retention behavior.
The process-global journal implements logical per-DataModel ownership. A
DataModel's final shared-owner release retires only its own journal stream and
render-dirty scope, including headless scopes without a renderer consumer.
`Destroy()` alone preserves final removal records while live consumers still
own the world. Cleanup does not reset another world's cursors or sequence and
does not allocate a new ObjectId. A replacement world has a different
generation-safe scope. The native retirement helpers are private to DataModel;
no script, package, or network cleanup API is exposed.
Authoring transaction identity and bounded semantic history are implemented
separately from this sequence; see `AuthoritativeTransactions.md`. Compaction,
rollback, and network transport are not implemented. Consumers must not treat this as an untrusted network protocol. See
`MutationGateway.md` for the reader and authoritative apply contracts.

## Luau and checked type boundaries

Module resolution uses checked dynamic type handling. Standalone Instance roots
use an explicit helper: a `DataModel` root is preserved, while another root is
parented under a new DataModel's Workspace. Retained deserialization paths own
their strings.

Native userdata dispatch catches C++ exceptions at the Luau callback boundary
and converts them into ordinary Luau errors. Generic dispatch rejects missing or
wrong-tag receivers before invoking native methods. A null shared native object
always pushes Luau `nil` and cannot become truthy tagged userdata. `Vector2` remains a
`glm::vec2` value stored in tagged userdata; its binary arithmetic validates both
operands explicitly, permits scalar-left multiplication only, and does not
define ordering. Its library constants are readonly value userdata. Direct
library callbacks that do not pass through userdata dispatch still need a
complete boundary inventory in a later hardening pass.

`Engine` owns every DataModel descendant subscription it installs. Teardown
unbinds the descendant-added callback and disconnects the descendant-removed
connection before releasing its `ScriptEngine` owner. The Engine-owned VM has
shared native lifetime; each active Lua entry pins it until that entry unwinds.
Logical runtime shutdown happens immediately when Engine destruction begins:
it stops activation, disconnects tracked Lua signal/remote handlers and retires
queued task references before descendant/service destruction. The active Lua
call may finish returning or reporting an error; physical `lua_close` waits for
the final active entry to release its pin. This does not add another DataModel
authority or change callback security contexts. VM entry, shutdown and mutable
runtime work remain Main-owned; shared lifetime does not permit concurrent Lua
execution. `ScriptEngine` tracks every Script it
steps and every Luau-backed signal connection it creates. It retires coroutine
and callback registry references while the VM is still alive, before
`lua_close`. A live Engine also observes DataModel destruction and performs this
runtime teardown before the DataModel destroys its services and descendants.
Engine-first, DataModel-first and callback-triggered destruction have regression
coverage; qualification status of the latest lifecycle changes is recorded in
`../Validation/RuntimeFoundation6Qualification.md`.

Active Engine event/frame calls also retain `GuiRuntime` while routed callbacks
unwind. Destruction still clears transient GUI state and drops the Engine's
owner immediately. Transient-state clearing guards nested calls and snapshots
focus/editing entries before callbacks, preventing callback-triggered teardown
from invalidating iteration. Neither resource pin changes scene authority.

Script activation roots the newly created coroutine from the top of the main
Lua stack before loading/resuming it and restores the caller's stack height.
The registry reference remains independent of library tables and temporary
stack values. Cleanup requested by self-disable or self-destruction waits for
the active resume to return; it then releases the coroutine and preserves the
requested disabled/restart state. Activation queue iteration uses an owning
entry snapshot and checks current queue membership before each turn, so
callbacks cannot invalidate an iterator. New entries wait until the next Step.

Terminal Script coroutines release their own registry roots after resume returns.
Native task, signal-wait, Remote and entitlement resumptions identify the original
Script through a private weak thread association. They capture the coroutine's
state before resuming: an invalid nested resume is not completion, and a successful
return can leave results on the coroutine stack. The existing eight-entry idle
prune pass reconciles completion through Luau's coroutine library. Independently
rooted tasks and callbacks retain their own lifetime and Script sandbox identity.

Logical shutdown also closes the Engine-owned ActionMap facade to new bindings
before native teardown callbacks run. It retires only the facade's bounded legacy
handles through their existing per-ID destruction path. Authored InputService
definitions remain outside that ownership table. Runtime cleanup cannot depend
on a Lua Destroying handler after Lua handlers have been disconnected.

## Networking and downstream ownership

Snapshot baselines, serialized references, scoped journal records, receiver apply
and protocol-input hardening are implemented. Runtime changes retain the current
networking owners: `ReplicationFoundation3I.md` owns immutable revisioned object
templates; `ReplicationFoundation3J.md` owns scheduler-accepted per-peer history;
`ContentAvailabilityFoundation3L_3.md` owns bounded dependency planning. D03's
`POOLED_CORRECTNESS_V1` remains the accepted service qualification contract.
Render dirty coalescing never rewrites those reliable histories. See
`RuntimeFoundation6Qualification.md` in `devdocs/Validation/` for exact source,
measured improvements and remaining qualification limits. Durable persistence
identity and schema migration remain separate future work.

## Runtime Foundation 6 storage and change-distribution disposition

The integrated continuation retains ordinary Instance fields as authoritative
storage. Main still validates and commits each ordinary mutation, records its
ordered history and invokes existing observers synchronously. The measured
changes optimize implementation overhead: journal batch append/rollback,
unobserved ordinary notifications, same-object render-domain accumulation,
typed incremental extraction, unchanged-model inverse reuse and indexed physics
admission. No script API, object identity, wire format or scheduling authority
changes. Qualification scope is recorded in
`../Validation/RuntimeFoundation6Qualification.md`.

The current architectural choice is optimized Instance authority plus
consumer-owned immutable or compact derivatives. A bounded direct/SoA/snapshot
experiment includes setter, dirty capture and synchronization costs. Its one
consumer does not repay the extra SoA synchronization, and per-object immutable
snapshot allocation costs more again. Consequently this continuation does not
move authoritative transforms, velocity, visibility or hierarchy into an ECS,
add native storage handles to Luau, or create a general component registry.
These measurements do not rule out a future many-consumer or narrowly scoped
system experiment; that requires its own complete authority/synchronization
comparison and lifecycle gates.

Physics derives backend descriptions and consumes authored changes at existing
Main safe points. Post-step simulation publishes through ordinary Instance
setters, with suppression limited to its own redundant CFrame feedback. The
combined compact control/readback representation was rejected on complete-path
cost. The continuation instead includes a selective dynamic readback index for
large, predominantly static worlds, following paired Windows and Linux sparse,
dense, lifecycle and boundary comparisons. It activates at 8192 Parts with at
most one eighth accepted dynamic bodies and retires below 4096 Parts or above
one quarter dynamic bodies. These measured private thresholds are not asserted
optimal. Ordered controls and publication remain unchanged; unknown accounting,
invalid identities and failed index allocation use ordered full readback in the
same step. Final combined-source acceptance remains a separate gate. Body handles stay
private derived resources, tied to complete ObjectId generations. This storage
work neither replaces Box3D/XPBD nor changes qualified solver behavior.

Rendering consumes immutable RenderPublication and owns its disposable
RenderProjection. The publisher validates live generation and WorldRoot ancestry
before incremental extraction, and still emits complete final-state values.
For recognized render-affecting changes, a domain hint cannot mask other final
fields. New render dependencies must be classified or request full resync.
Exact model equality may reuse the cached inverse. A lost publication base
rejects the delta; the caller must request a full resync, after which the
publisher rebuilds complete state.
Renderer instancing remains independently opt-in. Fewer CPU publication
milliseconds do not qualify native GPU execution or change its defaults.

Replication retains the 3I revisioned immutable template catalog and 3J/3L
per-peer planning/accepted history. The existing scoped journal is already a
typed committed input. A second global event bus would duplicate classification,
bounded cursors and recovery without measured benefit here. Final renderer state
and redundant internal physics dirty work may coalesce. Required callbacks,
ordinary journal records, transaction boundaries, create/destroy/reparent and
reference dependencies, and accepted reliable transport ownership retain their
existing ordering. Explicit prepared mutation keeps its distinct atomic observer
contract and is not substituted for ordinary setters based on throughput.

Creation and retirement retain DataModel admission, monotonic destruction and
slot-plus-generation identity. Indexed physics membership reduces repeated
admission searches without changing traversal order or inventing deferred
script-visible lifecycle. Existing 3L content activation, materialization and
retirement remain the large-world owners. New caches must remain disposable,
bounded by their subsystem and reconstructible after full resync; a stale slot
must never alias a replacement Instance.

This continuation does not parallelize mutable graph traversal or mutation.
Existing bounded JobSystem computation returns results to Main; immutable
renderer/network projections retain their accepted thread boundaries. The
single-consumer snapshot/SoA results do not justify new cross-thread queues or
moving live Instance work to a worker. Dense setters, full rebuilds, replica
application, replication preflight and occasional full-engine tails remain
measured ceilings. Streaming, terrain and multiplayer should extend the current
subsystem-owned projections and admission budgets, with targeted data-oriented
storage introduced only where complete-path evidence repays its synchronization
and lifetime costs.

The combined evaluation adds an opt-in Main-owned signal-dispatch profile and
private WorldRoot listener counters. Disabled profiles take no timing samples.
These counters are diagnostic and inclusive under reentrancy; callback and
signal totals must not be added to ordinary setter timings. They expose no Luau
capability or mutable storage handle. The per-registration dirty-binding, epoch
and hot/cold variants remain isolated experiments: reduced repeated-callback
work did not repay their full physics safe-point cost in paired measurements.
