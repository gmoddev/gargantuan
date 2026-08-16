---
status: current
owner: editor-host
last_verified: 2026-08-16
related_code:
  - include/gargantuan/classes/DataModel.hpp
  - include/gargantuan/filesystem/Project.hpp
  - src/editor/EditorHost.cpp
  - src/filesystem/Project.cpp
related_adrs: []
---

# Project revision and persistence

## Authority and revision

Each loaded `DataModel` owns an explicitly initialized unsigned 64-bit
`AuthoritativeRevision`. A successfully loaded existing project starts at 1.
The counter advances once when a committed authoring mutation changes state
represented by project serialization. Exhaustion throws and never wraps.

Current revision-bearing mutations are saved reflected properties (including
`Name`), Attributes, Tags, class-extension values, custom-class property state,
and existing hierarchy/lifecycle changes. Rejected and detected no-op gateway
mutations do not advance it. Transient properties, viewport/camera state,
selection, diagnostics, networking, and Studio layout do not advance it.

The project revision is neither an undo transaction identifier nor an alias for
`ChangeJournal.Sequence`, network sequence state, or `ObjectId` generation.
Future persistent Create/Delete/Duplicate/Reparent operations must advance this
same counter when their committed state transition succeeds.

## Persisted revision and dirty state

EditorHost owns the active project destination and `PersistedRevision` for its
loaded session. Successful Open initializes both revisions to 1. Dirty state is
derived only as `AuthoritativeRevision != PersistedRevision`.

A save serializes one coherent authoritative state into an in-memory
`Project::PersistenceSnapshot` carrying the exact captured revision. Only a
successful atomic persistence operation assigns that value to
`PersistedRevision`; failure leaves it unchanged.

If revision N is captured, a later mutation advances the live project to N+1,
and the write succeeds, the result remains persisted N, authoritative N+1, and
dirty. Synchronous EditorHost persistence currently runs at its authoritative
safe point; the snapshot contract preserves this rule if scheduling broadens.

## Save and Save As

`SaveProject` takes no caller-supplied revision or dirty state and persists to
the engine-owned destination. `SaveProjectAs` accepts one absolute local
project-root destination. EditorHost rejects malformed, URI-like, non-absolute,
or file destinations after bounded normalization.

Save As constructs a candidate destination, preserves the existing instance
format, copies the PreRun schema file when present, and persists the snapshot.
The live destination and `DataModel` filesystem/root change only after success.
Failure leaves the old destination, persisted revision, and dirty state intact.

## Atomic persistence and errors

Persistence serializes before touching the target, writes a unique temporary
file beside it, flushes and closes it, then atomically replaces the target
(`MoveFileExW` with replace/write-through on Windows, same-filesystem rename
elsewhere). A pre-replacement failure removes the temporary and preserves the
prior valid file.

EditorHost normalizes missing project, invalid destination, serialization,
filesystem, and persistence failures into structured errors. The instance JSON
format remains version 4; no format or migration framework was added.

## Protocol propagation

Open, snapshot, journal polling, Save, Save As, and `GetProjectState` expose
authoritative revision, persisted revision, derived dirty state, and current
destination. Journal sequence remains independent. Studio cannot choose a
revision or mark the project clean.
