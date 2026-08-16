---
status: current
owner: runtime
last_verified: 2026-08-16
related_code:
  - include/gargantuan/runtime/AuthoritativeTransactions.hpp
  - src/runtime/AuthoritativeTransactions.cpp
  - src/runtime/MutationGateway.cpp
  - src/editor/EditorHost.cpp
related_adrs: []
---

# Authoritative authoring transactions

## Identity, ownership, and lifecycle

Each loaded `DataModel` owns one `AuthoritativeTransactionHistory`. A nonzero
unsigned 64-bit `TransactionId` is issued monotonically within the engine process;
zero is invalid and exhaustion fails closed. Identity remains session scoped for authority
and independent from `ObjectId`, project revision, journal sequence, and IPC
request ID. Open initializes empty project history without reusing an earlier
process identity.

The implemented lifecycle is `Open -> Committed` or an empty `Open ->
NoChanges`. There is at most one open transaction and its owner is a native
EditorHost session value that request JSON cannot choose. Duplicate commit,
unknown identity, and wrong owner fail without mutation. Project replacement
and EditorHost shutdown terminate the owner's open group; a group containing
changes is committed because Foundation 3A does not pretend those already
applied changes can be rolled back. An empty group is closed as `NoChanges`.
The one-open-group, change-count, and byte bounds prevent abandoned input from
growing memory. A five-minute lifetime is checked at request boundaries; an
expired changed group is committed and an empty group closes as `NoChanges`, so
timeout never claims to roll back already applied work.

## Commit-only grouping contract

This is Model B: commit-only grouping, not an abortable staging transaction.
Every mutation is validated and applied by the existing `MutationGateway` and
Instance lifecycle path. While an explicit group is open, the engine defers its
project-revision advance and captures its journal records. Successful commit
publishes one revision and the buffered records. A rejected later request does
not undo earlier valid requests; the group remains open and can be committed.
There is deliberately no `AbortTransaction` capability.

An EditorHost authoring request without `TransactionId` receives an implicit
transaction. It validates, applies, captures, and commits in the same request,
preserving the previous one-shot behavior. Studio-origin authoring enters this
history. Runtime/internal mutation paths remain outside Studio undo history and
retain their established revision/journal behavior.

Save and Save As reject while an explicit transaction is open. This prevents a
persistence snapshot from observing applied group state before its revision
boundary. Save never clears retained history and transaction commit never
changes `PersistedRevision`.

## Semantic change representation

History owns values and stable identities only; it contains no `Instance`, raw
pointer, callback, backend handle, or Studio reference. Current categories are:

- saved property/custom-property before and after values, property name,
  declaring `SchemaId`, and exact definition version;
- Attribute absence/presence and before/after `WireValue`;
- Tag before/after membership;
- Class Extension before/after `WireValue`, extension `SchemaId`, and version;
- Reparent object plus old/new parent `ObjectId`;
- Create and Duplicate persistent subtree JSON plus every newly allocated
  identity and the parent identity; and
- Destroy persistent subtree JSON captured before destruction plus every
  destroyed identity and the former parent identity.

The subtree representation reuses project JSON version 4, so it carries class
and definition identity, saved properties, hierarchy, Attributes, Tags,
Extensions, and custom-property overrides without inventing another serializer.
Duplicate records the clone snapshot rather than relying on the source surviving.
Object-reference values use `WireObjectReference`, never native pointers.

History restoration always allocates fresh generation-safe identities. A
history-local alias table maps each captured identity to the current live
generation after Create, Delete, or Duplicate restoration. The table is not an
ObjectRegistry override and is never exposed as request authority: the old ID
remains stale to every normal API. Repeated restore/destroy cycles update the
alias and never resurrect a generation.

Closed scalar `WireObjectReference` values are resolved through this alias table
before validation/application. Project-v4 subtree JSON does not currently encode
an arbitrary object-reference graph, so restoration makes no claim to rebuild
external inbound references or unsupported internal reference edges. Existing
generation-safe reference behavior is preserved; no missing reference is
silently substituted with another object.

## Revision and journal relationship

One committed transaction containing one or many persistent mutations advances
`AuthoritativeRevision` exactly once. A no-op group advances no revision and
creates no history entry. Rejected operations contribute no semantic change.
`PersistedRevision` changes only after Save.

Journal records remain the authoritative projection feed. A transaction may
release many ordinary journal records in operation order, but `TransactionId`
is not encoded as `ChangeJournal.Sequence` and no wire-journal format version
changed. Undo and Redo publish the same ordinary semantic records as forward
authoring; clients never invert cached state. EditorHost commit returns transaction identity, starting/resulting
revision, and change count; Studio then consumes the normal journal batch.

## Cursor, execution, and branch invalidation

The cursor is an applied-entry count in the retained deque. Entries before it
are undoable; the entry at it is the next redo. `CanUndo`, `CanRedo`, and bounded
next labels derive from that position. Undo executes the preceding entry in
reverse change order, Redo executes the next entry in forward order, and neither
appends a synthetic history transaction. A new authoring commit behind the tip
discards the redo suffix before retention.

Execution is Main-domain, requires trusted mutation authority, rejects while an
explicit group is open, and accepts no transaction selector from request data.
Semantic state, exact schema/version, live generations, hierarchy, scope,
resource bounds, and revision availability use the mutation/lifecycle paths.
Subtree reconstruction is detached and bounded before attachment. Incompatible
state returns a structured failure without moving the cursor or revision.

Every successful Undo or Redo advances `AuthoritativeRevision` once; it never
restores an old revision and never changes `PersistedRevision`. An Undo after
Save is therefore dirty even if values resemble an older persisted state. Save
does not clear history. Project open/replacement clears history, cursor, aliases,
and open grouping state.

EditorHost advertises `Undo`, `Redo`, and `AuthoritativeHistoryStatus`. Undo and
Redo take empty payloads. Project state and journal responses carry derived
history status; raw cursor indices, restoration aliases, and arbitrary history
selection are not protocol operations.

## Bounds and eviction

The limits are one open transaction with a five-minute request-boundary lifetime,
128 UTF-8 label bytes, 4,096 semantic
changes, 8 MiB semantic bytes per transaction, and 4 MiB per subtree snapshot.
Committed history retains at most 128 transactions and 32 MiB. A mutation that
would make its current transaction exceed a per-transaction bound is rejected
before state mutation. When retained-history count or aggregate bytes would be
exceeded, oldest entries are evicted deterministically; current project state
is unaffected.

Front eviction decrements the applied-count cursor when needed, preserving valid
availability without underflow. Diagnostics expose retained count, cursor count,
semantic bytes, availability, and bounded next labels only.
