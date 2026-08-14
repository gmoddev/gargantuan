# Instance attributes

## Implemented now

Every live `Instance` owns a deterministic, name-sorted map of bounded dynamic
attributes. Attribute names are runtime state, not schema definitions: setting
an attribute does not reopen the frozen `RuntimeSchemaRegistry`, allocate a
`SchemaId`, or change registry generation. The frozen native `Instance` schema
describes the `SetAttribute`, `GetAttribute`, `GetAttributes`, and
`GetAttributeChangedSignal` behavior only.

Names are case-sensitive UTF-8 byte strings. They must be non-empty, contain no
embedded null, and be at most 100 bytes. The authoritative limits are:

| Limit | Value |
| --- | ---: |
| Attributes per Instance | 64 |
| Encoded bytes per value | 4 KiB |
| Aggregate name/value bytes per Instance | 16 KiB |
| Replicated attribute bytes per Instance | 16 KiB |

The encoded-value limit measures canonical `WireValue` JSON. The stored subset
is boolean, integer, finite double/float, bounded UTF-8 string, `Vector2`,
`Vector3`, `Color3`, `UDim`, `UDim2`, and finite `CFrame`. `Null` is removal and
is never stored. Enums, object references, containers, blobs, functions,
userdata, and native pointers are rejected.

## Authority and changes

External and Studio writes use `UpdateAttributeCommand` through
`MutationGateway`. The authoritative Instance boundary rechecks Main-domain
ownership, `MutateDataModel`, names, values, count, and aggregate limits before
replacing its map. Reads require `ReadDataModel`; domains grant no authority.

Creation, replacement, and removal each commit one `AttributeUpdatedChange` and
fire the named signal once. Assigning the identical `WireValue`, or removing a
missing name, is a successful no-op. Rejection leaves prior state unchanged.

## Persistence and replication

Project JSON version 1 stores an `Attributes` object per Instance in sorted
order and accepts legacy version 0 documents without attributes. Snapshot
version 4 carries the same map alongside tags. Both loaders validate the complete collection.

Wire journal version 4 includes `AttributeUpdate` with stable `ObjectId`, name, and
`WireValue`; `Null` means removal. Snapshot plus subsequent records reconstructs
identical state. Loopback replication applies initial state, updates, and
removals to its isolated receiver. EditorHost exposes bounded `SetAttribute`
and otherwise uses the existing snapshot/journal document path.

## Deferred

Reference-valued attributes await an explicit cross-scope load policy. Enums,
attribute schemas/namespaces, broader migrations, and a full Studio
Properties UI are not implemented.
