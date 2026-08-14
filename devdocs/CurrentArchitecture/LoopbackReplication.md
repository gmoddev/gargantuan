# Versioned journal wire format and loopback replication

## Implemented now

`WireJournalRecord` version 3 is the incremental companion to snapshot version
3. Every record carries a nonzero per-scope sequence, a generation-checked
scope/world `WireObjectId`, an object `WireObjectId`, and exactly one operation
shape:

| Operation | Required payload |
| --- | --- |
| `Create` | class name |
| `PropertyUpdate` | property name and `WireValue` |
| `AttributeUpdate` | attribute name and `WireValue`; `Null` removes |
| `Reparent` | nullable parent `WireObjectId` |
| `Destroy` | no additional payload |

The JSON envelope and every record are independently versioned. Unknown
versions, unknown operations, invalid IDs, zero sequences, missing fields, and
operation-inappropriate fields fail closed. Snapshot and journal encoding call
the same `WireCodec` implementation for `WireObjectId` and `WireValue`; there
is no journal-specific value type or identity encoding.

Committed replicated-property journal entries capture a closed `WireValue` at
commit time. Native `std::any` remains inside reflection and mutation dispatch only; it
does not enter `ChangeRecord`, `WireJournalRecord`, snapshots, or serialized
documents. Objects entering a scope publish create, current replicated
properties, and reparent records for their subtree. Producers must order a new
object's scope entry before another scoped object publishes a reference to it.

The authoritative root DataModel's `ObjectId` is the initial replication scope
identity. `ChangeJournal` keeps a separately sequenced bounded stream per scope,
so unrelated DataModels cannot create cursor gaps or leak records into a
session. Unparented objects and non-replicated property changes remain in the
unscoped diagnostic stream. Moving a subtree between DataModels emits a destroy
in the old stream and a complete publication in the new stream.

## In-process session

`InProcessReplicationSession::Start` performs a real wire-shaped baseline:

```text
authoritative source hierarchy
  -> capture snapshot and cursor N
  -> serialize and parse snapshot version 3
  -> materialize separate receiver objects
  -> consume source records N, N+1, ...
  -> encode, serialize, parse, and apply wire journal records
```

Receiver objects have ordinary local runtime `ObjectId` values. Source IDs are
resolved only through the session's source-ID-to-receiver-object map, so the
receiver cannot alias or mutate source objects. Receiver materialization and
delta application run under a scoped journal-suppression policy; receiver-side
setter calls therefore cannot feed back into the authoritative source stream.
Suppression is thread-local and deliberately narrow, not a general way to skip
authoritative recording.

Scope matching and sequences are strict. A record for another scope is rejected.
A record below the expected cursor is rejected as a
duplicate, a record above it is rejected as out of order, and a source cursor
older than retained journal history returns `ResnapshotRequired`. The cursor
advances only after a record is validated and applied. There is no implicit
sorting or gap filling.

Creates are materialized as unparented candidate objects before later property
or reference records are applied. Reparenting beneath a tracked receiver object
promotes the candidate into the replicated hierarchy. This proves
create-before-reference behavior without weakening the public mutation gateway,
which still requires an owning parent for external create commands.

## Deliberate limits

This is an in-process architectural proof, not a network protocol. It does not
provide transport framing, authentication, peer authority, compression,
bandwidth budgets, schema negotiation, rollback, or multi-object transactions.
The DataModel-derived scope is runtime-lifetime identity, not a durable world
UUID. Cross-scope object references are not yet a supported wire contract and
must be rejected by a future pre-commit validator. Scope authorization and
client-specific visibility/filtering are also not implemented.

Wire parsers currently fail closed but do not yet impose hostile-input byte,
record-count, string-length, or nesting limits. These limits and fuzzing are
required before accepting documents from an untrusted transport.

The smallest recommended next task is a pre-commit reference/scope validator
plus bounded hostile-input limits and fuzz tests. That closes the remaining
local protocol holes before adding a loopback command/ack policy or sockets.
