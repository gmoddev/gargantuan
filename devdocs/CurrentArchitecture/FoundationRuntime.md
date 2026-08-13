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
  and defining a wire representation are future protocol concerns.

IDs are native-only for now. Module cache identity uses `ObjectId` instead of a
raw address. Serialization does not yet write IDs or graph references.

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

- persistence (`Transient` or `Saved`, with existing `Serializable` mapped to
  `Saved`);
- future replication (`None` or `FutureReplicated`);
- write authority (`Main` or `Any`);
- editability;
- an optional native validation predicate;
- existing read/write permission levels.

The class generator accepts the corresponding declarative fields. Metadata is
descriptive except where current property dispatch can enforce it: main-domain
authority, read-only state, and validation are checked on the wired mutation
paths. No replication or persistence transport consumes the new flags yet.

## Ordered committed changes

`ChangeJournal` assigns monotonic sequence numbers while holding its journal
lock. Its payload model represents object creation, property update, reparent,
and destroy. The current proof integration covers identity publication,
generated property setters, reparenting, and destruction.

Records are an in-process prototype. Retention is now bounded and cursor reads
detect eviction with `ResnapshotRequired`; the default capacity is 4,096.
Snapshots, transaction IDs, compaction, rollback, and serialization are not yet
implemented. Consumers must not treat this as a replication protocol. See
`MutationGateway.md` for the reader and authoritative apply contracts.

## Luau and checked type boundaries

Module resolution uses checked dynamic type handling. Standalone Instance roots
use an explicit helper: a `DataModel` root is preserved, while another root is
parented under a new DataModel's Workspace. Retained deserialization paths own
their strings.

Native userdata dispatch catches C++ exceptions at the Luau callback boundary
and converts them into ordinary Luau errors. Direct library callbacks that do
not pass through userdata dispatch still need a complete boundary inventory in
a later hardening pass.

## Blockers before loopback replication

The authoritative command queue and bounded cursor are now implemented. Before a
first loopback server/client prototype, the smallest remaining blockers are:

1. define a snapshot baseline paired with the bounded journal cursor;
2. persist ObjectIds (or a separate serialized identity) and resolve graph
   references during load;
3. define schema versioning and deterministic wire encodings;
4. assign authenticated command origins to reflection permission levels;
5. finish the audit of direct Luau C callbacks and signal reentrancy safe points;
6. add bounded document/tree limits and malformed-input fuzzing.

Serialized identity/reference resolution and a cursor-paired snapshot baseline
are now implemented. The recommended next task is a versioned wire encoding for
journal records plus an in-process source/receiver session. That proves the
complete replication flow without selecting a network transport.
