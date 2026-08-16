# Authoritative project creation

## Minimum project

`CreateProject` creates the smallest canonical persisted project: one `DataModel`
named by the request and its required `Workspace` service. It uses normal native
schema bootstrap, service construction, archivable-state marking, and project-v4
serialization. It does not add starter parts, scripts, gameplay services, assets,
or a template identity.

The initial project state is explicit:

```text
AuthoritativeRevision = 1
PersistedRevision = 1
Dirty = false
CanUndo = false
CanRedo = false
```

Creation is initialization, not an authoring transaction. The first subsequent
persistent authoring transaction advances the authoritative revision to 2 and
creates the first history entry.

## EditorHost authority

`CreateProject` requires the trusted Studio `EditorCommands` capability. Its
request contains exactly `Destination` and `Name`. The request cannot supply
serialized objects, `ObjectId` values, schema data, revisions, persistence state,
or transaction identity. The engine constructs and persists the `DataModel`.

The response contains the authoritative root identity and ordinary project
state. Schema discovery, snapshot, journal, viewport, structural editing,
persistence, and history then use the same paths as an opened project. There is
no special new-project session mode.

## Destination policy

The destination must be a normalized absolute local directory path with an
existing parent. The project name must be visible valid UTF-8, contain no control
characters, and fit the 100-byte bound. URI-like, relative, malformed, and
inaccessible destinations fail closed.

Foundation 4 uses a conservative directory policy:

- a nonexistent destination is created;
- an existing empty directory is rejected;
- an existing nonempty directory is rejected; and
- an existing Gargantuan project is rejected explicitly.

There is no overwrite or replace mode.

## Transactional filesystem establishment

Creation allocates an exclusive, uniquely named sibling staging directory. The
engine builds the minimum world, captures one coherent revision-one snapshot,
and uses existing atomic file persistence to write
`.gargantuan/project.instance.json` within that staging directory. It then closes
the temporary world and renames the sibling directory to the requested
destination on the same filesystem.

Before the final rename, failure removes only the exact staging directory whose
exclusive creation proves ownership. A pre-existing destination is never
removed, traversed for rollback, or modified. A successful rename leaves a valid
reopenable v4 project even if the requesting process disconnects before receiving
the response.

## Session adoption and failure

EditorHost stages and establishes the persisted destination before replacing its
active world. It then runs the canonical project schema bootstrap and normal
deserialize/open path against the new destination. Studio performs New Project
through a candidate EditorHost process, so its current session remains published
until creation, schema discovery, snapshot establishment, and viewport startup
all succeed.

Validation, persistence, and filesystem failures return bounded structured
errors and do not adopt the candidate session. A post-establishment activation
failure returns `ProjectActivationFailure`; the valid project remains on disk for
explicit reopen rather than being destructively rolled back.
