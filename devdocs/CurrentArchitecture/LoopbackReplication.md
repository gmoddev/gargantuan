# Versioned journal wire format and loopback replication

## Implemented now

`WireJournalRecord` version 1 is the incremental companion to snapshot version
1. Every record carries a nonzero sequence, a generation-checked
`WireObjectId`, and exactly one operation shape:

| Operation | Required payload |
| --- | --- |
| `Create` | class name |
| `PropertyUpdate` | property name and `WireValue` |
| `Reparent` | nullable parent `WireObjectId` |
| `Destroy` | no additional payload |

The JSON envelope and every record are independently versioned. Unknown
versions, unknown operations, invalid IDs, zero sequences, missing fields, and
operation-inappropriate fields fail closed. Snapshot and journal encoding call
the same `WireCodec` implementation for `WireObjectId` and `WireValue`; there
is no journal-specific value type or identity encoding.

Committed property journal entries now capture a closed `WireValue` at commit
time. Native `std::any` remains inside reflection and mutation dispatch only; it
does not enter `ChangeRecord`, `WireJournalRecord`, snapshots, or serialized
documents. Object-reference capture publishes the referenced object's ID before
committing the referring property record, establishing create-before-reference
ordering.

## In-process session

`InProcessReplicationSession::Start` performs a real wire-shaped baseline:

```text
authoritative source hierarchy
  -> capture snapshot and cursor N
  -> serialize and parse snapshot version 1
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

Sequences are strict. A record below the expected cursor is rejected as a
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
The process-global source journal also does not yet tag records with a world or
DataModel identity. The session can distinguish objects created after its
snapshot, but moving an already-existing external object into the replicated
root requires resnapshot because its original `Create` record predates the
cursor.

Wire parsers currently fail closed but do not yet impose hostile-input byte,
record-count, string-length, or nesting limits. These limits and fuzzing are
required before accepting documents from an untrusted transport.

The smallest recommended next task is explicit replication scope/world identity
on committed records plus schema-driven property replication policy. That
removes dependence on the process-global journal before adding a loopback
command/ack protocol or sockets.
