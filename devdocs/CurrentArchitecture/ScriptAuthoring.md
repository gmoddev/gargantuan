---
status: current
owner: editor-runtime
last_verified: 2026-08-16
related_code:
  - assets/classes/LuaSourceContainer.luau
  - src/classes/LuaSourceContainer.cpp
  - src/runtime/MutationGateway.cpp
  - src/editor/EditorHost.cpp
  - src/assets/InstanceSerialization.cpp
related_adrs: []
---

# Authoritative script authoring

## Supported classes and source state

Foundation 5 supports the existing native `Script` and `ModuleScript` classes.
Both derive from `LuaSourceContainer`, remain normally constructible through
`CreateInstance`, and start with an empty Source. There is no `LocalScript`
class or new execution category. Existing runtime meaning is unchanged and the
EditorHost source operations never execute source.

`LuaSourceContainer.Source` is saved, engine-readable/writable UTF-8 text with
an exact 65,536-byte maximum. NUL and malformed UTF-8 are rejected. No newline
normalization is performed: LF, CRLF, tabs, Unicode, and trailing newlines round
trip exactly. Syntax validity is deliberately not a persistence constraint, so
incomplete or invalid Luau may be committed and saved.

Source is not a replicated property and is omitted from ordinary snapshot and
journal values. `SourceVersion` is a positive, monotonic per-object integer
invalidation token. It is transient, replicated to the EditorHost projection,
and increments whenever authoritative Source changes. Exhaustion fails closed.
This token is not project revision, transaction identity, or journal sequence.

## EditorHost authority and conflicts

The handshake exposes `ReadScriptSource` and `WriteScriptSource`.
`GetScriptSource(ObjectId)` returns the exact Source, current SourceVersion, and
project AuthoritativeRevision only for a live `LuaSourceContainer` in the open
project. `SetScriptSource(ObjectId, ExpectedSourceVersion, Source)` requires
trusted EditorCommands plus MutateDataModel capability, a current journal
cursor, a live generation-safe identity, the correct class, valid bounded
UTF-8, and the exact current token. A stale token returns `SourceConflict`
without mutation. Non-scripts, stale generations, invalid input, missing
project state, and transaction limits use structured protocol errors.

The generic `SetProperty` route rejects Source. This prevents callers from
bypassing optimistic concurrency. Gameplay scripts and network peers have no
EditorHost token/capability and cannot invoke either source method.

## Transactions, revision, and journal

One successful Source mutation is one semantic `Edit Script` transaction and
advances AuthoritativeRevision once. Character edits in a Studio buffer do
nothing until commit. The history record owns ObjectId, exact declaring schema
and definition version, and old/new source. Both strings count against the
existing per-change, per-transaction, and retained 32 MiB semantic-history
bounds; an oversized history entry is rejected before commit.

Undo and Redo validate the expected live identity, schema/version, and current
semantic source, then use the same bounded mutation path while suppressing a
new history entry. Each successful traversal advances project revision once.
The ordinary journal publishes only the new SourceVersion, causing interested
Studio tabs to refetch Source. Source text is never copied into general journal
traffic.

## Persistence and privacy

Source uses the existing JSON project v4 saved-property representation. Adding
`std::string` serialization support fixed the existing serializer gap without a
format revision; older/minimal v4 projects still load and missing Source uses
the empty default. Duplicate/subtree transaction snapshots use the same
serializer, so duplicated scripts own independent copied Source.

Source may flow only through project-v4 persistence, bounded semantic history,
the dedicated local EditorHost response/request, and the existing script
runtime when that class is intentionally run. It is absent from normal gameplay
replication, generic journal payloads, telemetry, crash breadcrumbs, and
automatic diagnostics. RemoteEvent data remains developer-supplied and is not
populated from Source by this feature.

Studio diagnostics use the pinned Luau compiler against a local buffer. They do
not execute source, affect project state, or enter transaction history. LSP,
debugging, hot reload, filesystem synchronization, and MCP source
tools remain deferred.
