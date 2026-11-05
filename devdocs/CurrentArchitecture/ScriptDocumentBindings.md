---
status: current
owner: editor-runtime
last_verified: 2026-11-03
related_code:
  - assets/classes/LuaSourceContainer.luau
  - src/classes/LuaSourceContainer.cpp
  - src/assets/InstanceSerialization.cpp
  - src/classes/Instance.cpp
  - src/runtime/MutationGateway.cpp
  - src/editor/EditorHost.cpp
  - tests/ScriptDocumentBindingTests.cpp
related_adrs:
  - docs/adr/D04-durable-script-resource-bindings.md
---

# Script document binding v1

This additive EditorHost capability restores a document's intended resource,
not unsaved text. The ordinary Engine source APIs, mutation gateway and project
Save remain authoritative. No Studio buffer, name/path resolver, sidecar ID
registry, gameplay replication field or new source authority is introduced.

`Handshake.Capabilities` advertises `ScriptDocumentBinding`. Both operations
require the existing session token, `ReadDataModel` and `EditorCommands`. They
operate only on the current authoring world; runtime ObjectIds cannot be used to
obtain an authoring binding. They are reads and remain available during Play.

## Exact wire representation

`GetScriptBinding` takes exactly `{ "Object": { "Slot": 1, "Generation": 1 } }`.
`ResolveScriptBinding` takes exactly `{ "Binding": <binding> }`.

A binding has exactly these fields:

| Field | Type / rule |
| --- | --- |
| Version | unsigned integer, exactly 1 |
| ProjectId | canonical nonzero 32 lowercase hexadecimal characters |
| SourceId | canonical nonzero 32 lowercase hexadecimal characters |
| ClassSchemaId | existing canonical 128-bit SchemaId hexadecimal encoding |
| ClassDefinitionVersion | nonzero unsigned 32-bit integer |

Both success results contain `Binding`, `Object`, `Scope`, `ProjectId`,
`AuthoritativeRevision`, `PersistedRevision`, and `IdentityOrigin`. Object is
the currently live full generation-safe ObjectId; Scope is the current authoring
DataModel ID. ProjectId equals Binding.ProjectId and the open project's existing
durable identity. Revisions use the existing unsigned 64-bit representation and
are not source versions or binding versions. `IdentityOrigin` is `Generated` if
the current Instance generated its identity, or `Loaded` if it reconstructed it
from serialized state (including authoring history). This provenance is not a
claim that current source or project changes are saved.

Clients must check current request/project/scope and generation fences before
using the returned Object in the existing `GetScriptSource` API. A binding is
not a source-conflict token; source edits still use the newly read SourceVersion
and normal project revision checks. Do not persist response Object/Scope as the
durable identity. Names are presentation only and are not part of a binding.

## Identity, persistence and lifetime

`LuaSourceContainer.SourceId` is canonical saved schema state with Engine-only
read/write permissions, no ordinary editor editability and no gameplay
replication. Native hydration validates its exact encoding before adoption;
identity cannot change after hydration or DataModel adoption. The constructor
uses the existing ProjectId generator, with no new random-number mechanism.
Identifiers are not secrets and possession never grants authority.

Persistence validates uniqueness across the bounded document on write and read.
An invalid or duplicate ID rejects the whole candidate, including detached
prepared-document materialization. A legacy missing field receives a new ID in
memory. The original file remains byte-for-byte unchanged until explicit Save.
New and legacy-generated IDs survive restart only once saved. If an unsaved ID
is later unavailable, resolution returns ResourceNotFound and never guesses.

Source edits, Name changes and hierarchy moves retain identity. Instance.Clone
and gateway Duplicate regenerate Script resource IDs before publication. Their
source text and other persistent properties retain existing semantics. Duplicate
history captures its own regenerated identities; Undo/Redo retains the logical
resource identity while allocating fresh live ObjectIds. Deleted resources do
not bind to newly created same-name/same-path Scripts. Undo deletion may restore
that same resource intentionally.

Save As and copied projects retain their existing ProjectId lineage and SourceIds.
A new independent project has a different ProjectId and rejects the old token.
This contract does not distinguish two deliberate copies of the same lineage by
filesystem location. Clients may scope their workspace intent separately, as
long as they resolve the actual resource through Engine.

## Bounds and rejection

Resolution traverses the canonical current tree once and requires every ancestor
to be Archivable. It visits at most MaximumPersistenceObjects (currently 65,536)
and uses at most that many temporary traversal entries; no per-event work or
retained lookup registry exists. Request/response byte, UTF-8 and JSON depth
limits remain the existing EditorHost envelope limits. One operation resolves
one binding; clients restoring a bounded document list should serialize requests
through their normal queue and cancel stale completions.

Errors include MalformedRequest (shape/encoding/version/bounds), Unauthorized,
ProjectRequired, StaleObject, NotScript, ResourceNotPersistent, ResourceNotFound,
AmbiguousResource, ResourceLimit, IncompatibleBinding and StaleProject. Duplicate
matches fail closed. A mismatched project never triggers a name/path search.
No read creates services, mutates source, advances project revision/history or
writes files. These operations do not reinterpret journal order as a persistent
resource version and do not change networking or Foundation 3L contracts.

## Qualification

The focused `gargantuan_script_document_binding` test covers native/schema
ownership, persistence, malformed/duplicate identities, missing legacy fields,
save/reopen, fresh live identity, rename/reparent, Clone/Duplicate, Undo/Redo,
delete/recreate, project-lineage Save As, cross-project/denied/malformed requests
and read-only project state. The source-bound Windows worker passed all five
focused suites, including 465 checks in this fixture; see
[the qualification receipt](ScriptDocumentBindingQualification20261103.md).
Native Studio restoration, draft recovery and end-to-end UI latency are separate
gates.

The same fixture prints bounded 2/32/128-Script resolution samples (eight warmups,
64 individual requests each). Timing includes in-process EditorHost request JSON,
the authoritative lookup, response encoding and test JSON decoding. It excludes
pipes, Studio scheduling, source read, display and physical input. Allocations
are not measured. Concurrent gate output is smoke evidence; isolated results
must be identified separately before making performance comparisons.
