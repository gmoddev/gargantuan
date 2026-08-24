---
status: current
owner: project-filesystem
last_verified: 2026-08-21
related_code:
  - include/gargantuan/filesystem/BaseFilesystem.hpp
  - include/gargantuan/filesystem/SourceMount.hpp
  - src/filesystem/SourceMount.cpp
  - src/classes/FileLink.cpp
  - src/services/AssetService.cpp
  - src/Engine.cpp
related_adrs: []
---

# SourceMount and FileLink compatibility boundary

## Ownership decision

`BaseFilesystem` is the engine filesystem backend boundary, not only a test
helper. `Project` and `DataModel` retain the backend and project-root association.
For source import, `SourceMount` is the single semantic policy layer above that
backend: it owns one canonical project/source root and is the only facility that
`FileLink` and `AssetService` use to resolve, enumerate, or read project source
content.

`SourceMount` uses host path facilities only to canonicalize and verify paths.
Actual metadata, enumeration, and file reads pass through `BaseFilesystem`.
Neither consumer calls SDL file APIs or opens a user-selected path directly.
`AssetService` receives only project-relative provenance and reads it through a
mount before dispatching bounded bytes to a private importer. This decision is intentionally limited to the source-import
path; existing persistence and explicitly selected CLI file operations are not
being rewritten as part of this boundary.

The filesystem backend and mount are native engine implementation details. They
are not exposed to experience Luau, and `FileLink.Path` remains a project-relative
compatibility value rather than host-path authority.

The generic `DiskFilesystem::GetDescendants` helper also treats symbolic links
and Windows reparse points as non-directories and enforces the same 32-level and
16,384-entry traversal ceilings. This is defense in depth for future backend
callers; current source-import authority continues to flow through
`SourceMount`, which performs the stronger per-operation confinement checks
below.

## Root confinement

Construction canonicalizes an existing directory owned by the project
filesystem. Every operation then:

1. rejects absolute, drive-rooted, null-containing, and oversized paths;
2. lexically rejects any `..` sequence that would escape the mount root;
3. rejects traversal deeper than the mount limit;
4. rejects every symbolic link or Windows reparse point in the path, including
   links that would currently resolve inside the root;
5. canonicalizes the existing candidate;
6. compares canonical path components against canonical root components; and
7. revalidates the canonical identity after enumeration or bounded read.

Containment never uses a string-prefix comparison. Directory entries returned by
the backend contribute only one validated filename; their backend path cannot
select a different parent. Missing, wrong-type, inaccessible, link, traversal,
and limit failures return `SourceMountError` with a stable code and a
mount-relative path.

The hard production limits are:

| Resource | Limit |
| --- | ---: |
| Directory traversal depth | 32 |
| Enumerated entries | 16,384 |
| UTF-8 path bytes | 4,096 |
| Individual imported file | 8 MiB |
| Aggregate imported bytes | 32 MiB |
| Candidate Instances | 65,536 |
| Nested `.instance.json` Instance depth | 32 |

Script source remains subject to the narrower 64 KiB
`MaximumScriptSourceBytes`. Callers may lower limits for tests, but cannot raise
them above the engine hard limits. Instance JSON also remains subject to the
canonical persistence parser's document, JSON-depth, and object limits.

## Transactional synchronization

`FileLink::Synchronize` is a synchronous Main-domain compatibility importer:

1. An RAII guard owns `Synchronizing`, so every result and exception clears it.
2. The source directory is resolved and enumerated through `SourceMount`.
3. Recognized scripts, folders, and nested Instance JSON are read and constructed
   as a detached candidate forest.
4. Aggregate bytes, entries, objects, filesystem depth, nested Instance depth,
   and model validity are checked before a candidate enters the DataModel.
5. Nested models containing `DataModel`, already-owned Instances, hierarchy
   cycles, or another `FileLink` are rejected. This prevents recursive mounts and
   closes the former nested-model self-recursion gap.
6. Only after every candidate succeeds are all new top-level siblings parented.
   A parenting failure destroys any newly published candidates and leaves the
   prior owned siblings intact.
7. `OwnedSiblings` changes to the accepted set, then the superseded linked roots
   are destroyed.

The old linked tree therefore remains authoritative during preflight and
candidate construction. Recognized-source failure cannot publish a partial
candidate or erase last-known-good content. New Scripts can be queued only after
the complete candidate has been accepted; the engine does not step script
execution inside synchronization.

## Compatibility and deferred work

Current visible compatibility remains startup-time, directory-to-sibling import
for folders, `.luau`, `.client.luau`, `.server.luau`, and `.instance.json`.
Unknown file suffixes remain ignored.

Asset Foundation 1 separately uses the same boundary for explicit PNG, BMP, OBJ,
TTF, and OTF import/reimport. Its canonical artifacts are loaded for runtime
consumption; source access is required only for trusted authoring reimport.

This boundary does not add filesystem watching, two-way synchronization,
incremental diffs, rename tracking, conflict resolution, collaboration,
deterministic source maps, trust prompts, arbitrary external roots, or Studio/MCP
filesystem authority. Normal runtime project trust and automatic execution of
project-contained scripts remain tracked separately from root confinement.
