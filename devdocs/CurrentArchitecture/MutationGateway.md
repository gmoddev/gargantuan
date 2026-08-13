# Authoritative mutation gateway

## Implemented contract

The `Instance`/`DataModel` graph is authoritative state owned by the `Main`
execution domain. The runtime now has two supported mutation flows:

```text
Worker or future external producer
  -> typed MutationCommand
  -> MutationGateway::Submit
  -> Main calls Drain
  -> resolve ObjectId and validate
  -> apply committed mutation
  -> existing setter/hierarchy path records the change
  -> MutationCompletion receives a result

Main-domain engine code
  -> generated/custom setter or hierarchy method
  -> domain, lifecycle, and schema validation
  -> apply committed mutation
  -> record the change
```

`Engine::Step` drains queued commands before simulation and scripts. Submission
is thread-safe and never applies state inline. `Apply` is synchronous for
authoritative internal callers, but returns `WrongExecutionDomain` outside
`Main`. Arbitrary Luau is not scheduled on workers.

The initial typed command set is create, property update, reparent, and destroy.
Commands address existing objects with generation-checked `ObjectId` values.
Stale or destroyed targets fail before mutation. Creation currently requires an
owning parent so a newly created object has an authoritative owner; support for
an explicit root/object store can broaden that later.

Property dispatch converges on `Instance::ApplyPropertyMutation`. It checks
property existence, read-only and permission metadata, execution affinity,
lifecycle, and the optional schema validator before invoking the reflection
writer. Luau property writes, reset-to-default, deserialization, and queued
property commands use this path. Generated setters also validate schema values,
which preserves synchronous native mutation without making the queue optional.

## Mutation-path audit

| Path | Current rule |
| --- | --- |
| Generated property setters | Main/lifecycle assertion, schema validation, assignment, one committed property record |
| Custom `BasePart` position/rotation setters | Validate alias, then use canonical `CFrame` setter and record `CFrame` |
| Custom camera FOV setters | Validate alias, then use canonical `FieldOfView` setter and record `FieldOfView` |
| Camera simulation movement | Uses `SetCFrame`/`SetViewportSize`; no direct reflected-field writes |
| Script enabled setter | Main/lifecycle validation, state transition, one committed `Enabled` record |
| Luau `Instance` property writes | Checked reflection apply; native exceptions remain inside the Luau boundary |
| Deserialization and reset-to-default | Checked reflection apply with engine permission |
| Parent and destroy | Existing validated synchronous methods; records only after authoritative state changes |
| `InputObject::fromEvent` field initialization | Construction-only snapshot initialization before identity publication; not a mutation of a live authoritative object |

Non-`Instance` userdata uses a separate value-type reflection helper and is not
part of the authoritative hierarchy contract.

## Bounded change cursors

Property update records contain an owned `WireValue` snapshot captured after
assignment, so consumers do not need to reread the live object and `std::any`
cannot leak across the serialization boundary. `ChangeJournal` retains 4,096
records per scope by default. Capacity is configurable and
old records are evicted from the front. `CreateCursor` starts at the next commit.
`Read` returns a bounded batch and an advanced cursor. If a cursor predates the
oldest retained record, it returns `ResnapshotRequired` and no partial batch.

The older `ReadSince` API reads the unscoped diagnostic stream and remains for compatibility, but it
cannot report retention loss and must not be used by reliable replication.
Capacity changes and snapshot construction are Main-domain policy decisions even
though journal reads and commits are internally synchronized.

## Future work

This pass does not implement command authentication, transactions, rollback, or
transport. A mutation result is an
in-process completion value, not a protocol response. A callback can still
trigger a later synchronous mutation, so multi-object transaction boundaries
and deferred notification safe points remain open design work.

The deterministic snapshot baseline, scoped versioned journal records,
schema-driven property selection, and in-process source/receiver session are now
implemented; see `SnapshotBaseline.md` and `LoopbackReplication.md`. Remaining
work includes same-scope reference validation before commit, durable world
identity, command-origin authority, transaction boundaries, hostile-input
limits, and transport.
