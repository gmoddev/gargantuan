---
status: current
owner: runtime
last_verified: 2026-11-07
related_code:
  - include/gargantuan/assets/InstanceSerialization.hpp
  - src/assets/InstanceSerialization.cpp
  - src/packaging/PackageBuilder.cpp
  - src/content/ContentAvailability.cpp
  - tests/InstanceReferencePersistenceTests.cpp
---

# Project Instance persistence v5

## Envelope and identity

`InstanceSerialization::Serialize(Json, Root)` writes version 5. Every emitted
Instance has a positive unsigned `NodeId` from 1 through 65,536, unique within
that document. The writer assigns IDs in hierarchy preorder. `NodeId` is a
document-local lookup key, not a runtime `ObjectId`, path, name, authority
token, or durable cross-save identity. Reopening or cloning allocates fresh
runtime ObjectIds. The root is always emitted. The existing Archivable rule
remains: a non-Archivable emitted node omits its descendants; it is not itself
omitted. The writer collects exactly this emitted tree before reading saved
properties, so a reference to an omitted descendant is external to the
document.

The v5 node retains v4's `Name`, `ClassName`, `ClassSchemaId`,
`ClassDefinitionVersion`, `Properties`, `Attributes`, `Extensions`,
`CustomProperties`, `Tags`, and `Children` fields. `Version` remains on the
document root. Native saved scalar and enum properties keep their existing
explicit typed encodings. A saved reflected Instance reference uses exactly
`{"InstanceReference": null}` or `{"InstanceReference": NodeId}`. Every
saved reflected reference property is explicit in v5. Nullable references to
objects outside the emitted tree encode as null; an external non-nullable
reference rejects serialization. No source ObjectId or external lookup key is
persisted.

## Loading and failure

The reader accepts versions 0 through 5. Versions 0 through 4 retain their
existing behavior; their previously omitted Instance references use the class
default. A v5 load requires bounded, unique, unsigned nonzero NodeIds and
bounded unsigned reference targets. It constructs and parents the complete
hierarchy before reference resolution. A missing target ID, duplicate ID,
wrong target class, cross-scope target, malformed value, or rejected setter
fails the entire load. The reader preflights each reference through the
existing reflected wire validation and then applies it with `Engine`
permission and `CoreTrusted` context. No name/path matching, global source-ID
resolution, direct field write, or new mutation authority is introduced.

The existing 8 MiB JSON document, 64-deep hierarchy, 65,536 object, and
1,024-property-per-object ceilings still apply. Pending reference records
have an additional 1,048,576-entry hard ceiling. Invalid input returns a
failed `DeserializationState`; a partially built hierarchy is destroyed and
never returned as a successful document. Clone and detached decode use the
same local map, so forward references point to their new document's objects.

## Package content

New package content manifests use Instance schema version 5. Existing schema
version 4 package content remains readable; new builds publish v5. A content
unit is a separate Instance document, so package partitioning retains any
static Workspace subtree that contains a non-null reference source or target
in the bootstrap project. Other qualified static subtrees may still become
units. This deliberately avoids cross-unit fixups and prevents a split from
silently replacing an internal authored reference with null. Package content
namespace, digest, admission, and runtime ObjectId rules are unchanged.
