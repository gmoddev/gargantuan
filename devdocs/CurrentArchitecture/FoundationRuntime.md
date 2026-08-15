# Runtime foundation: ownership, identity, jobs, and changes

## Status and scope

This document describes the foundation implemented in the first post-audit pass.
It is a runtime contract, not a claim that networking, persistence, parallel
simulation, or Studio are implemented.

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

Hierarchy mutation is committed before ancestry notifications run. Callbacks
therefore observe the new parent/child collections rather than a half-applied
operation. Callbacks remain synchronous and can initiate a later mutation; a
broader transaction/notification queue is future work.

Destruction has a monotonic lifecycle:

1. the object enters its private destroying state;
2. `Destroyed` becomes true and its registry entry is invalidated;
3. the committed property notification and `Destroying` signal run;
4. children are destroyed and the object is detached;
5. a destroy change is committed.

Repeated or reentrant `Destroy` calls are no-ops. `Destroyed` is reflected as a
read-only, non-editable property; scripts and ordinary native callers cannot
move the lifecycle backwards.

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
- Generation wrap skips zero. Avoiding practical 32-bit generation exhaustion
  remains a future protocol concern.

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
Moving a subtree between scopes removes
it from the old stream and republishes its current baseline into the new stream.

Records remain an in-process prototype. Retention is bounded per scope and cursor reads
detect eviction with `ResnapshotRequired`; the default capacity is 4,096.
Transaction IDs, compaction, rollback, and network transport are not yet
implemented. Consumers must not treat this as an untrusted network protocol. See
`MutationGateway.md` for the reader and authoritative apply contracts.

## Luau and checked type boundaries

Module resolution uses checked dynamic type handling. Standalone Instance roots
use an explicit helper: a `DataModel` root is preserved, while another root is
parented under a new DataModel's Workspace. Retained deserialization paths own
their strings.

Native userdata dispatch catches C++ exceptions at the Luau callback boundary
and converts them into ordinary Luau errors. Generic dispatch rejects missing or
wrong-tag receivers before invoking native methods. `Vector2` remains a
`glm::vec2` value stored in tagged userdata; its binary arithmetic validates both
operands explicitly, permits scalar-left multiplication only, and does not
define ordering. Its library constants are readonly value userdata. Direct
library callbacks that do not pass through userdata dispatch still need a
complete boundary inventory in a later hardening pass.

## Remaining blockers before network replication

Snapshot baseline, serialized references, scoped journal records, and isolated
in-process receiver apply are implemented. Before a network prototype, the
smallest remaining blockers are pre-commit same-scope reference validation,
hostile-input byte/count/depth limits and fuzzing, authenticated command-origin
policy, and explicit transaction/notification safe points. Durable world IDs,
schema migration, and transport negotiation remain later work.
