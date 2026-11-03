---
status: accepted
date: 2026-11-03
supersedes: null
superseded_by: null
---

# D04 — Durable Script resource bindings

The owner authorized the minimum Engine identity mechanism for Studio Script
restoration in the LUI blocker-resolution campaign. Engine remains resource,
source and persistence authority; Studio persists document intent only.

Existing `ProjectId` identifies a project lineage, including copies and Save As.
`ObjectId` identifies a live scoped generation, and `SourceVersion` is a transient
edit-conflict token. Neither identifies an ordinary Script across deserialization.
`AssetId` identifies imported assets and cannot identify an ordinary project
Script. There is no existing persistent Instance ID to reuse for this purpose.

Add one saved native-owned `LuaSourceContainer.SourceId`, a nonzero canonical
128-bit hexadecimal identifier generated with the existing ProjectId identity
generator. It is an identifier, not a secret or capability. This is canonical
per-resource state, not another identity registry. It is not gameplay-replicated
or writable through ordinary Luau/EditorHost properties.

The version-1 binding contains ProjectId, SourceId and exact class identity/version.
EditorHost resolves it against the bounded current authoritative hierarchy and
returns a fresh scoped ObjectId. It never uses a name, hierarchy path, filesystem
path, source hash or previous ObjectId as a fallback. Ambiguity rejects instead
of choosing a match. Resolution grants no source-write or runtime/server authority.

Rename, reparent and source edits preserve SourceId. Create, Clone and Duplicate
create fresh IDs; Undo/Redo reconstruct the same logical resource identities.
Duplicate history must therefore capture the duplicate's regenerated identities,
not the source subtree's identities. Malformed or duplicate persisted IDs reject
the candidate atomically.

Project format v4 remains compatible. Missing legacy fields receive a fresh ID
in memory and persist on the next explicit Save. No binding read writes project
files, changes revision/history or silently migrates disk. An identity first
generated in an unsaved session may not resolve after reopen; this fails safely.
Successful Save/reopen establishes durable binding. Save As preserves existing
ProjectId lineage and resource IDs, while a genuinely new project has another ID.

The initial implementation scans at most MaximumPersistenceObjects per request,
without a retained resolver index. A later measured need may justify a disposable
derived index using committed lifecycle changes; it must not become configuration
authority or weaken duplicate/stale rejection. Studio restoration, draft recovery,
durable identities for other resource kinds, filesystem source mounts and network
protocol changes are outside this decision's implementation slice.

The exact additive protocol is [Script document bindings](../../devdocs/CurrentArchitecture/ScriptDocumentBindings.md).
