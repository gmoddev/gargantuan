---
status: implementation-under-qualification
owner: editor-host-runtime-schema
last_verified: 2026-10-29
---

# Canonical PreRun source authoring

PreRun remains `.gargantuan/prerun.luau`, a project bootstrap resource rather than
a Script Instance. The existing DefineSchema-only sandbox, source/VM/time/schema
bounds and transactional registry validator remain authoritative. This extension
adds no filesystem capability to Studio or gameplay, and no runtime schema swap.

## Source boundary

EditorHost advertises optional `PreRunSource` and `ValidatePreRun` capabilities.
All three operations require the process session token and explicit
EditorCommands. Reads and validation require ReadDataModel; saving requires
MutateDataModel. Only an established project is eligible.

`GetPreRunSource` takes no parameters and returns:

- `ProjectRoot`: generation-safe open-project ObjectId.
- `Path`: the constant `.gargantuan/prerun.luau`.
- `Exists`, opaque `SourceRevision`, `SourceBase64`, `MaximumSourceBytes`.
- `ActiveSourceRevision`, `SchemaGeneration`, `ReloadRequired`.

SourceBase64 is an array of one to six strings, each at most 65,536 bytes. The
concatenation is canonical Base64 for at most 262,144 UTF-8 source bytes without
embedded nulls. Chunking preserves existing JSON string/request bounds, including
the full existing PreRun source limit. Empty source has one empty chunk; `Exists`
distinguishes it from a missing resource. No file path is supplied by a caller.

`SetPreRunSource` and `ValidatePreRun` take exactly `ProjectRoot`,
`ExpectedSourceRevision` and `SourceBase64`. Root identity rejects a buffer from a
replaced/reopened project. SourceRevision binds the exact saved bytes/existence
and canonical project location, so external edits, stale buffers and Save As
cannot silently retarget a write. It is an equality token, not a sequence.

Saving reuses Project's native atomic temporary-file replacement. The fixed
resource path rejects linked/reparse components and paths outside the canonical
root. Its exact saved state and path are checked again immediately before
replacement; conflicts remove the temporary and preserve the winning saved
source. Source replacement does not advance DataModel revision/history or claim
that a candidate is valid. Invalid but bounded text can be saved for later repair.
Read/commit failures are bounded protocol errors, not modal native error windows.

## Validation and activation

ValidatePreRun runs the existing PreRun VM against a disposable native-seeded
RuntimeSchemaLifecycle, validates and freezes its candidate, then destroys it.
It never calls PublishCandidate and never changes the active registry, world,
source file or generation. The response includes `Validated`,
`ValidatedSourceRevision`, `ActiveSchemaChanged=false`, and bounded Diagnostics
with stable Code, canonical Source, Definition and Message (up to 4 KiB).
Compiler source locations are retained when supplied by the existing VM.

Deadline interrupts abort only at ordinary VM safepoints using an allocation-free
native cancellation tag, then the native execution boundary closes the disposable
VM before constructing the diagnostic. The tag deliberately does not derive from
std::exception, which pinned Luau converts into catchable script errors. Protected
calls and metamethods cannot consume the timeout and continue. GC callbacks cannot
break across a C boundary; allowing
an expired callback to re-enter during error-string allocation caused the
verified native stack overflow recorded as KI-010. The correction retains the
existing time/memory bounds and follows the pinned Luau interrupt convention.

Project bootstrap now returns the exact source bytes that it executed, allowing
EditorHost to report active versus saved source without rereading a potentially
changed file as its activation receipt. Only explicit project reload/reopen
publishes a new schema. Saved source that differs from the active source causes
Play/package capture to return `PreRunReloadRequired`, preventing a package or
runtime from combining pending declarations with an old world schema.

Save/validation are rejected during Play, an open authoring transaction or an
unfinished package job. An externally replaced schema generation requires
project reopen. Source changes remain outside DataModel Undo/Redo; a schema
reopen establishes a fresh world, snapshot, history and document identity using
the existing project lifecycle.

`PreRunEditorHostTests` covers successful/failed disposable validation, all three
Schema registration interfaces, stale/external/racing edits, atomic cleanup,
activation, custom-class authoring/save/reopen, exact source bounds and authority.
Existing PreRun/bootstrap/foundation regressions remain required. No physical
network qualification is implied.
