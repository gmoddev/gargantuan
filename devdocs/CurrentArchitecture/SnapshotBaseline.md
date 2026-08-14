# Snapshot baseline and serialized identity

## Implemented contract

The snapshot layer is the first deterministic, transport-shaped representation
of authoritative `Instance` state. It is separate from the existing asset model
serializer, whose compatibility format and public API remain unchanged.

A version 2 snapshot contains:

- an ordered list of objects in hierarchy traversal order;
- each object's source `ObjectId`, class, name, and optional parent ID;
- reflected properties explicitly marked `FutureReplicated`, sorted by name;
- explicit object-reference values;
- the scoped `ChangeCursor` immediately after the captured baseline.

The cursor carries both the source DataModel `ObjectId` scope and the next
sequence in that scope's journal stream. Capture is restricted to the
authoritative `Main` domain. Identity publication
finishes before the cursor is sampled, so any lazy object-creation records are
inside the baseline and incremental consumption begins exactly at
`Cursor.NextSequence`.

## Serialized ObjectId semantics

`WireObjectId` is an explicit pair of unsigned 32-bit `Slot` and `Generation`
fields. It is never a pointer or memory address. The pair preserves the source
runtime's generation-checked identity in a snapshot document.

Materialization does not force source IDs into the process-global object
registry. That would collide when a source and a loopback receiver coexist in
one process. Instead, `SnapshotLoadResult` owns a source-ID-to-local-object map.
References resolve through that map, and a missing, dangling, duplicate, zero,
or destroyed ID cannot resolve. Local receiver objects retain their own runtime
`ObjectId` values.

Consequently, `WireObjectId` is stable within the source object's lifetime and
within the snapshot/change stream that names it. It is not yet a durable UUID
that survives independently authored save histories. Generation exhaustion and
long-term identity migration remain future protocol concerns.

## Closed wire value schema

`WireValue` is a closed C++ variant with explicit encodings for:

- null, boolean, integer, float, double, and owned string;
- `Vector2`, `Vector3`, `Color3`, `UDim`, `UDim2`, and `CFrame`;
- enum type/item pairs;
- `WireObjectReference`.

`std::any` exists only behind reflection while converting native property
values. It is absent from `Snapshot`, `SnapshotObject`, `WireValue`, and the JSON
document. Unknown native property types fail capture rather than silently
crossing the boundary.

Reference read/write hooks are extensions of `InstanceProperty`, not a second
metadata registry. The existing `UseRead` and `UseWrite` templates populate
those hooks for `shared_ptr<Instance-derived>` and optional shared references.
Materialization constructs and maps every object before applying hierarchy,
properties, and references, so forward references are valid.

## Determinism and cursor transition

Object order follows the authoritative child order. Property maps use lexical
ordering, the envelope field order is fixed, and JSON is emitted without
format-dependent native type names. Capturing an unchanged DataModel produces
the same bytes, including the same cursor.

The consumer contract is:

```text
capture complete baseline
  -> Cursor.Scope = source DataModel ObjectId
  -> Cursor.NextSequence = N
  -> consume journal records beginning at N
  -> if Read returns ResnapshotRequired, discard incremental state and request a new snapshot
```

The snapshot and journal are coordinated by Main-domain serialization, not by
locking the hierarchy. Snapshot reads do not invoke callbacks, so no mutation
can interleave on the same authoritative thread during capture.

## Deliberate limits and next blockers

Versioned journal records and the in-process source/receiver session are now
implemented in `LoopbackReplication.md`. Receiver materialization and apply use
a scoped journal-suppression policy, so local receiver mutations do not pollute
the source cursor stream.

Persistence selection remains independent: `Saved` does not
implicitly put a property into a snapshot. The generated reflection schema is
the single source of truth for both policies.

Authentication, transport, durable world identity, hostile-input resource
limits, and schema migration remain unimplemented. These are required before
accepting snapshot or journal documents from an untrusted network peer.
