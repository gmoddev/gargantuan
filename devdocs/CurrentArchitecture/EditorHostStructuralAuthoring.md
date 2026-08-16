---
status: current
owner: editor-host
last_verified: 2026-08-16
related_code:
  - include/gargantuan/runtime/MutationGateway.hpp
  - src/runtime/MutationGateway.cpp
  - src/editor/EditorHost.cpp
  - src/classes/Instance.cpp
related_adrs: []
---

# EditorHost structural authoring

## Authority and identity

EditorHost exposes `CreateInstance`, `DestroyInstance`, `DuplicateInstance`, and
`ReparentInstance` only to its assigned Studio `MutateDataModel` authority.
Requests use exact generation-bearing `ObjectId` values and remain confined to
the loaded `DataModel`. Create additionally requires the active class
`SchemaId` and exact `DefinitionVersion`; Studio never supplies a new ObjectId.

The engine registry allocates every created or duplicated identity. A stale
generation, cross-scope target, malformed identity, or absent schema fails
before mutation. DataModel and registered services are protected from create,
destroy, duplicate, and reparent. Only editor-visible, constructible classes are
published as constructible through schema discovery, including validated Custom Classes and
their approved native host.

## Operations

- Create constructs a detached candidate, initializes its persistent
  `Archivable` state and optional Name, then attaches it to a validated parent.
  The normal replication-subtree journal publishes class identity, initial
  properties, parent, Attributes, Extensions, custom state, and Tags.
- Destroy uses the existing recursive `Instance::Destroy` lifetime order.
  Descendants are engine-owned; callers send only the root identity. Object
  registry invalidation makes every old generation fail safely.
- Duplicate serializes one authoritative source subtree with project v4 and
  deserializes it as a detached candidate under journal suppression. Every node
  receives a fresh registry identity. Persistent properties, descendants,
  Attributes, Tags, Class Extensions, and Custom Class overrides are copied;
  display names are preserved. The clone is attached beside the source.
- Reparent validates both identities, project scope, protected state,
  self-parenting, and descendant cycles before calling the existing hierarchy
  primitive. Same-parent requests are successful no-ops.

The duplicate path deliberately follows existing persistence semantics for
object-reference properties; it does not add a new graph-reference remapper.

## Atomicity, bounds, and revision

Create and Duplicate stage detached state with journal suppression, then publish
their complete committed subtree through one atomic `ChangeJournal::CommitBatch`.
Failure destroys the candidate and emits no project journal state. Reparent
performs complete semantic preflight before its single hierarchy change. A
rejected Delete never enters lifecycle mutation; accepted recursive destruction
uses the established deterministic teardown path.

The project object-count and persistence depth/document bounds are reused.
Duplicate preflights its full subtree count so it cannot allocate until the
project limit is hit. Schema, Attribute, Tag, Extension, and Custom Class limits
remain enforced by their existing validators.

`DataModel` supplies a narrow revision batch used only for logical revision
accounting. Successful Create, recursive Delete, subtree Duplicate, and changed
Reparent each advance `AuthoritativeRevision` once even when they emit multiple
journal records. Rejected operations and same-parent no-ops do not advance it.
Revision exhaustion fails closed. This batching is not an undo transaction and
does not provide user-visible history.

## Journal, persistence, and errors

Studio treats structural responses as correlation only. Durable hierarchy state
comes from the existing Create/property/Attribute/Extension/Tag/Reparent/Destroy
journal records. Project state rides the poll response, preserving authoritative
dirty-state reconciliation.

Loaded persistence trees are marked archivable as session state, and new
authoring objects enter that state explicitly. Save and reopen therefore retain
created and duplicated subtrees, reparenting, state overrides, and deletion
without changing project JSON version 4.

EditorHost maps failures to structured statuses including stale object, invalid
class, invalid parent, protected object, resource limit, validation failure,
revision exhaustion, unauthorized, and internal error. Native exceptions do not
become protocol semantics.
