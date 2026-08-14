# Instance tags

## Implemented now

Each authoritative `DataModel` owns one bounded `TagIndex`. Tags are case-sensitive dynamic project state, not runtime-schema definitions. Tag changes do not allocate `SchemaId`, reopen the frozen registry, or change its generation.

Names are non-empty valid UTF-8 without embedded nulls and are limited to 100 bytes. An Instance may have 64 tags and a DataModel may have 1,024 distinct active tags. The index interns each active name to a session-local numeric `TagId`; that identifier is never persisted or sent over the wire.

The canonical in-memory index maintains both `ObjectId -> TagId` and `TagId -> ObjectId` sets. Reverse sets use generation-checked `ObjectId`, never Instance pointers. Single-tag queries and explicit AND intersections start from indexed sets, validate live scope membership, and return `ObjectId` order. There is no query language, namespace, tag metadata, inheritance, or OR/NOT evaluation.

## Authority and lifecycle

`AddTagCommand` and `RemoveTagCommand` use `MutationGateway`. Main-domain and `MutateDataModel` checks are enforced at the authoritative index; reads require `ReadDataModel`. Domains do not imply capabilities. Duplicate add and absent remove are successful no-ops with no record.

Destroy removes forward and reverse membership before ObjectId invalidation. Descendant destruction performs the same cleanup for every object. Leaving a DataModel scope removes that scope's memberships. Queries additionally validate the current ObjectId generation and world, so slot reuse cannot resurrect an old association.

## Persistence and state transfer

Project JSON version 2 persists one sorted `Tags` array per object and rebuilds the reverse index; the reverse index itself is not serialized. Versions 0 and 1 remain readable without tags. Snapshot and wire-journal version 4 carry sorted initial membership plus explicit `TagAdded` and `TagRemoved` records. Loopback replication and EditorHost use those same records and stable object identity.

The `Tags` service exposes bounded add, remove, membership, object-tag, single-tag, and explicit all-tags intersection operations. Studio stores membership in its replicated document and derives a local read index; edits still return through EditorHost authority.

## Deferred

Rich expressions, namespaces, tag metadata, ACLs, inheritance, schema-defined tags, plugins, and generalized collection queries are not implemented.
