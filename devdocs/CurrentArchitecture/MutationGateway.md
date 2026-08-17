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
is thread-safe, bounded to 4,096 pending commands, and never applies state inline. `Apply` is synchronous for
authoritative internal callers, but returns `WrongExecutionDomain` outside
`Main`. Arbitrary Luau is not scheduled on workers.

The typed command set is create, reflected-property update (including custom
class declarative properties), bounded attribute
update, class-extension property update, tag add/remove, reparent, and destroy.
Commands address existing objects with generation-checked `ObjectId` values.
Stale or destroyed targets fail before mutation. Creation currently requires an
owning parent so a newly created object has an authoritative owner; support for
an explicit root/object store can broaden that later.

Commands also carry a host-created `MutationAuthorityContext`. Local internal
commands retain their existing security context. Studio and future authenticated
peer contexts carry an assigned DataModel scope, and the gateway rejects targets,
parents, and creates outside it. Authority metadata is not part of the decoded
command payload and cannot be manufactured from names, hierarchy, or schema
metadata.

Property dispatch converges on `Instance::ApplyPropertyMutation`. It checks
property existence, read-only and permission metadata, execution affinity,
lifecycle, and the optional schema validator before invoking the reflection
writer. Luau property writes, reset-to-default, deserialization, and queued
property commands use this path. Generated setters also validate schema values,
which preserves synchronous native mutation without making the queue optional.
Failure results retain a narrow `MutationStatus`; one bounded formatter converts
that status into Luau and EditorHost diagnostics rather than collapsing it to a
generic property-rejected message.
Extension-property dispatch separately resolves the frozen extension
identity/version, target applicability, property identity, and exact scalar
type. It changes sparse Instance extension state and emits a dedicated journal
record; it never routes through Attributes.
Custom class property dispatch uses the ordinary reflected-property command,
but the frozen reflection entry retains its declaring class `SchemaId`, exact
definition version, scalar type, and default. The write resolves target
applicability and sparse state through that metadata before commit. It remains
distinct from both Attributes and extension state.

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

This pass does not implement command authentication, players, permissions,
transport, or a generalized transaction/rollback subsystem. A mutation result
is an in-process completion value, not a protocol response. Loopback batch
preflight and notification deferral are narrow state-applicator safe points;
ordinary synchronous native callbacks may still initiate a later mutation.

The deterministic snapshot baseline, scoped versioned journal records,
schema-driven property selection, in-process source/receiver session, and
protocol input hardening are now implemented; see `SnapshotBaseline.md`,
`LoopbackReplication.md`, and `ProtocolInputHardening.md`. Remaining work
includes durable world identity, real peer authentication, negotiated runtime
policy, production scheduler/coordinator execution, and a real transport. The
deterministic simulator and scheduler contract are implemented without gaining
a mutation or authority path.
