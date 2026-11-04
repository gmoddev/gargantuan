---
status: accepted-design
implementation_status: editorhost-exposure-implemented
owner: runtime-authoring
last_verified: 2026-09-16
source_checkpoint: 25c4da3d72c18d473bffcd21c095189d255fb470
related_code:
  - include/gargantuan/InstanceProperty.hpp
  - include/gargantuan/classes/Instance.hpp
  - include/gargantuan/runtime/MutationGateway.hpp
  - include/gargantuan/runtime/AuthoritativeTransactions.hpp
  - include/gargantuan/runtime/ChangeJournal.hpp
  - src/classes/Instance.cpp
  - src/runtime/MutationGateway.cpp
  - src/runtime/AuthoritativeTransactions.cpp
  - src/runtime/ChangeJournal.cpp
  - src/datatypes/Signal.cpp
  - src/editor/EditorHost.cpp
related_architecture:
  - ../CurrentArchitecture/MutationGateway.md
  - ../CurrentArchitecture/AuthoritativeTransactions.md
  - ../CurrentArchitecture/EditorHostProtocol.md
  - ../../docs/invariants/Core.md
---

# Prepared property commit architecture

Implementation and qualification evidence for the internal foundation now lives
in [Prepared property commit validation](../CurrentArchitecture/PreparedPropertyCommitValidation.md).
The decision below remains normative. Descriptions of the ordinary mutation
blocker explain why the separate prepared path is required. The foundation was
qualified at `8cb1d7519`; the additive EditorHost adapter and its separate
final-source gate are recorded in the validation receipt.

## Decision

**B — a small reusable Engine-owned prepared-mutation primitive specialized to
bounded property batches.**

The primitive is shared by forward multi-object property mutation and by Undo/
Redo of history entries created by that operation. It is not a generalized
prepared transaction system. Existing ordinary transactions, single-property
setters and `SetTransform` retain their current semantics until separately
migrated.

Required semantic unit:

```text
bounded request
  -> validate
  -> prepare every ordinarily fallible resource
  -> final revision/identity validation
  -> non-failing authoritative commit
  -> one revision transition + one history action
  -> post-commit observer delivery
```

Oversized requests reject before live mutation and are never silently chunked.
Studio remains a non-authoritative client of this Engine primitive.

## Current blocker

The current implementation cannot prove all-or-none multi-object property state.

Generated ordinary setters are effectively:

```text
AssertCanMutate
ValidatePropertyMutation
backing field = value
NotifyPropertyCommitted
```

`NotifyPropertyCommitted` runs after the backing value changed and may encode the
new `WireValue`, allocate payload/name storage, publish a journal record, advance
the DataModel revision, create/find a signal and fire observers. A public setter
is therefore not a safe commit primitive.

`UpdateWirePropertiesCommand`/`SetTransform` preflight values but applies ordinary
setters sequentially and performs compensating writes on later failure. That is
useful compatibility behavior, not the required atomic commit proof.

The ordinary transaction path also contains post-mutation allocation/failure:
`RecordMutation` grows change/journal vectors, `Commit` advances revision before
journal/history installation is complete, journal grouping allocates, retained
history can allocate, and `ChangeJournal::CommitBatch` copies/grows retained
storage.

`ScopedSignalDeferral` is not sufficient because its thread-local deferred vector
may allocate during `BaseSignal::Fire`, after state mutation has started. Final
signal delivery also runs arbitrary native/Luau observer code.

Current Undo/Redo similarly applies changes sequentially and uses compensating
replay if a later change fails. Property-batch history must opt into the new
prepared replay path; legacy history remains unchanged.

## Why not A or C

**A**, a one-off forward-only property batch, would require a second unrelated
atomicity implementation for Undo/Redo.

**C**, a generalized prepared authoritative transaction layer, is unnecessary.
The requirement does not need atomic hierarchy/tag/attribute/subtree combinations,
and changing those paths would alter existing commit-only/partial-success
semantics.

B gives forward editing and property-batch history replay one proof without
redesigning unrelated mutation types.

## Prepared-safe property surface

V1 supports only reflected native properties that can prove a non-failing raw
state store.

The class generator already distinguishes generated backing properties from
`overrideWrite` properties. For an ordinary editable generated property whose
backing type is nothrow move-assignable, generation should register an internal
prepared-store hook conceptually equivalent to:

```cpp
struct PreparedPropertyValue {
    std::any NativeValue;      // decoded/normalized during preparation
    WireValue CanonicalValue;  // publication/history value
};

using PreparedStore = void (*)(Instance &, std::any &) noexcept;
```

The hook only installs already-validated backing state. It does not validate,
encode, journal, advance revision, allocate notification state or call observers.
Generation should fail/omit the capability if the backing operation cannot meet
that contract. `Name`, generated scalar/vector/enum/string state and generated
`BasePart.CFrame`/`Size` fit this shape.

`overrideWrite` properties are **not** prepared-batch-writable by default. They
may opt in later only with explicit fallible preparation, a nothrow state-apply
hook, and bounded post-commit effects. The initial implementation must reject an
unsupported property rather than invoke its ordinary setter.

V1 excludes attributes, extension/custom-class override maps, tags, hierarchy and
script Source. Current Studio reference-property editing remains read-only. If a
reference property is enabled later, preparation must resolve the full target
ObjectId, validate class/nullability/same-scope rules and hold the resolved target
strongly until commit.

## Shared property preparation

Factor current validation/decode rules into an internal resolver so the batch
does not duplicate property semantics:

```text
PreparePropertyWireMutation(
    Instance,
    canonical property identity,
    WireValue,
    security context
) -> PreparedPropertyMutation | MutationStatus
```

Preparation performs generation-safe target resolution, DataModel-scope checks,
canonical property and declaring-schema/version checks, Studio editability and
capability checks, native decode/type/range/domain/enum validation, reference
validation where applicable, property validators, prepared-safe capability and
Source restrictions.

It returns normalized native state plus canonical `WireValue`; it changes no live
property. Ordinary single-property APIs need not migrate in the first task, but
the decoder/validator should be shared so they can reuse it later.

## Prepare/commit state machine

The complete operation runs synchronously on Main. It does not yield or run
observer/user callbacks between preparation and commit.

### 1. Envelope and revision admission

Before per-entry growth:

- require 1–256 writes;
- enforce the existing 1 MiB EditorHost input envelope;
- reject duplicate `(ObjectId, declaring schema, property name)` writes;
- use checked accounting for all counts/bytes;
- require exact `ExpectedRevision` and reject a mismatch;
- ensure the revision can advance; and
- for V1 reject execution inside an explicit/open ordinary authoring transaction.

The operation owns exactly one implicit history action.

### 2. Prepare every write

For each request entry:

1. exact `ObjectRegistry::Lookup(ObjectId)`;
2. reject destroyed/destroying/wrong-world targets;
3. resolve property/schema metadata;
4. validate and normalize the requested value;
5. capture canonical current `Before` value;
6. discard exact no-op writes;
7. create the prepared native value for the raw store;
8. create exact old/new history representation;
9. create the exact journal payload ordinary committed state needs; and
10. capture an existing property-changed signal pointer, if one exists.

Do not call `GetPropertyChangedSignal` merely to prepare delivery. If no signal
exists, no observer can already be connected and there is no past event to retain
for a future signal.

### 3. Prepare notifications

Generated V1 properties produce at most one direct property notification per
changed write. The prepared operation owns that bounded list before commit. It
does not use `ScopedSignalDeferral` during the commit phase.

Observer delivery itself is post-commit and may still allocate/run arbitrary
callbacks. Delivery failure is not authoritative commit failure.

### 4. Prepare one history action

Construct the full retained action before mutation:

```text
CommittedTransaction
  Id
  Label = "Set Properties"
  StartingRevision = R
  ResultingRevision = R + 1
  Changes = complete PropertyTransactionChange set
  ReplayPolicy = PreparedPropertyBatch
  SemanticBytes = precomputed bounded size
```

It is one history cursor entry regardless of write count.

Current value-owned `deque` retention has no reservable insertion boundary. Make
history installation preparable without changing transaction semantics by storing
retained committed actions as immutable shared objects and preparing a bounded
candidate pointer/index container. Preparation applies the existing redo-suffix,
count and retained-byte eviction policy and precomputes any identity-mapping
pruning. Commit installs candidate pointer/index/mapping containers by no-throw
swap/integer assignment. Existing transaction payloads are not copied.

Ordinary transaction retention may use the same internal storage representation;
its external semantics remain unchanged. This is a storage/reservation refactor,
not a transaction redesign.

### 5. Prepare journal publication last

Journal reservation is the final fallible stage because it may hold the existing
journal mutex through the short commit window.

Add an internal `PreparedJournalBatch`. Under the journal mutex it:

- partitions already-built records by scope;
- checks sequence-space arithmetic;
- rejects if new records for a scope exceed configured journal capacity;
- ensures required stream entries exist;
- constructs each complete replacement retained deque, including the existing
  oldest-record eviction policy;
- assigns exact future sequence numbers in the candidate; and
- prepares lightweight render-dirty intents.

This moves the current `CommitBatch` copy/allocation work before live mutation.
If preparation had to create an empty stream entry and final validation fails, the
reservation erases the still-empty entry before unlock. No record/cursor becomes
observable.

After successful journal preparation, no expected fallible operation remains.

### 6. Final boundary validation

Immediately before the first live write:

- revision still equals `ExpectedRevision` and is advanceable;
- every full ObjectId still resolves to the same prepared target;
- every target is alive/not destroying/in the same DataModel;
- every property descriptor still matches schema identity/version;
- prepared history installation is still valid; and
- journal reservation still owns its exact prepared sequence boundary.

Failure releases prepared resources and returns with no batch mutation.

Runtime schema is frozen for an open project, authoritative graph mutation is
Main-owned, and the operation does not yield/reenter. No global DataModel lock is
required.

## Non-failing authoritative commit

After final validation succeeds, enter an internal commit body whose contract is
`noexcept` for ordinary recoverable conditions:

```text
COMMIT BOUNDARY
  1. apply every prepared raw property store
       no validation / allocation / journal / revision / callback

  2. install prepared history candidate
       bounded swaps/counters only

  3. install prepared journal stream candidates
       deque swaps / NextSequence assignment only
       journal mutex remains held

  4. advance AuthoritativeRevision exactly once through an internal
     prevalidated no-throw increment

  5. release journal reservation/mutex
COMMIT COMPLETE

POST-COMMIT
  6. publish prepared render-dirty intents
  7. deliver prepared property notifications
```

The revision increment is the Main/EditorHost publication fence. Journal readers
cannot observe newly installed records before it because the journal mutex stays
held until afterward. History is Main-owned and no callback runs during steps
1–5.

A process crash between instructions is outside this in-memory atomicity contract;
this is not durable transaction logging.

### Proof table

| Potential failure | Required disposition |
| --- | --- |
| property validation / native decode | preparation |
| schema/reference lookup | preparation + final identity equality check |
| backing-value allocation | fully prepared; commit uses nothrow raw store |
| journal payload encoding | preparation |
| journal sequence exhaustion | preparation |
| journal retained storage growth | preparation; commit swaps |
| history action allocation | preparation |
| history growth/pruning | preparation; commit swaps candidate metadata |
| revision exhaustion | preparation; commit uses prevalidated increment |
| notification queue allocation | no allocating deferral queue in commit |
| user/native callbacks | post-commit only |
| render dirty accumulation | existing `noexcept`/fail-open path, post-commit |
| journal synchronization | mutex acquired in preparation and merely released after commit |
| integer/resource bounds | checked during preparation |

If a raw property store cannot be made nothrow, that property is ineligible. If
journal/history candidate installation cannot satisfy the table, implementation
must stop rather than add compensating live-state rollback.

## Notifications and observer visibility

A supported Engine observer must never be invoked by this batch while object A is
new and object B is old.

The proof uses existing ownership rather than a broad lock:

- Main owns authoritative mutation;
- prepare/commit does not yield or execute callbacks;
- property signals are delivered only after commit;
- journal readers remain behind the journal mutex until journal/history/revision
  installation is complete; and
- renderer/runtime projections receive post-commit derived dirty/signal work.

Unsynchronized foreign-thread direct reads of mutable Instance backing fields are
outside the existing authority/threading contract.

Post-commit signal delivery can still fail because snapshot construction allocates
or native/Luau observers fail. That failure must be diagnosed separately,
continue later notifications where safe, and **must not roll back or report the
already-committed batch as an authoritative failure**.

`RenderDirtyAccumulator::RecordChange/Mark` is already `noexcept` and fail-open;
prepared journal work should preserve that derived invalidation post-commit.

## Revision semantics

Failed preparation:

```text
live values unchanged
revision unchanged
history unchanged
journal unchanged
no batch observer notification
```

Successful non-no-op batch from revision R:

```text
all values committed
one history action installed
corresponding journal records installed
AuthoritativeRevision = R + 1 exactly once
notifications afterward
```

An all-no-op request returns success/no-change without revision/history mutation,
matching current no-op conventions. No per-property revision authority is added.

## Undo/Redo

A property-batch transaction carries `ReplayPolicy = PreparedPropertyBatch`.
Undo/Redo for that entry bypasses legacy sequential replay and uses the same
prepared property machinery:

- Undo expects every current value to equal captured `After`, then prepares all
  `Before` values.
- Redo expects every current value to equal captured `Before`, then prepares all
  `After` values.
- historical reference aliases are resolved before the boundary;
- journal/notification resources are prepared exactly as for forward mutation;
- history traversal prepares a no-allocation cursor transition rather than a new
  history action; and
- successful replay advances revision once, moves the cursor once, then notifies.

Any divergent value, stale identity, incompatible schema or preparation resource
failure occurs before mutation and leaves the history cursor unchanged.

Legacy transaction entries retain their current sequential/compensating replay
semantics.

## Resource bounds

V1 bounds are deliberately below the general transaction ceiling and are derived
from existing EditorHost/history limits.

| Resource | V1 bound | Basis |
| --- | ---: | --- |
| writes | **256** | existing EditorHost journal batch scale; supports a 256-object one-property interactive selection without inheriting the 4,096-change transaction maximum |
| encoded EditorHost request | **1 MiB** | existing host input limit |
| identifier/property string | **256 bytes** | existing protocol identifier limit |
| individual string value | **64 KiB** | existing protocol string limit |
| aggregate prepared old/new values | **4 MiB** | existing bounded single-action/subtree staging scale; covers both old and new state |
| prepared journal records | **256** | max one ordinary property record per changed V1 write |
| prepared journal payload | **2 MiB** | new state already fits the 1 MiB request envelope; bounded metadata/normalization headroom |
| history semantic bytes | **8 MiB** | existing `MaximumTransactionSemanticBytes`; no new exemption |
| direct notifications | **256** | one direct signal per generated V1 write |
| retained history | **128 actions / 32 MiB** | unchanged current limits |

A configured journal whose capacity is smaller than the complete new record set
rejects before mutation. Existing retention may evict older records under its
current policy, but the newly committed batch itself must fit as one retained
suffix.

Prepared state is synchronous and destroyed at request completion. The existing
MutationGateway pending-command bound remains unchanged; failed requests cannot
accumulate retained prepared objects.

## Security / adversarial behavior

Treat requests as untrusted authoring input. Fail closed for count/byte overflow,
duplicate/conflicting canonical targets, stale generations, wrong world, schema
mismatch, non-editable/non-prepared-safe properties, invalid enum/reference/type/
range/domain values, Source through the generic path, and any batch whose value,
journal, history or notification budgets cannot be prepared.

Authority, capabilities, scope and prepared-safety are never accepted from request
data; they remain host/schema-owned.

## EditorHost operation

The sketch below records design intent, not the canonical serialized contract.
The implemented additive operation uses `DeclaringClassSchemaId`,
`DeclaringDefinitionVersion`, `TransactionId` in the success response, plus the
required `Scope` and `PropertyBatchVersion` preconditions. See the exact
[EditorHost protocol](../CurrentArchitecture/EditorHostProtocol.md#prepared-native-property-batches-capability-version-1).

```json
{
  "Version": 1,
  "Method": "SetPropertyBatch",
  "Params": {
    "ExpectedRevision": 42,
    "Writes": [
      {
        "Object": { "Slot": 10, "Generation": 3 },
        "DeclaringSchemaId": "...",
        "DefinitionVersion": 1,
        "Property": "Color",
        "Value": { "...": "canonical WireValue" }
      }
    ]
  }
}
```

There is no per-write success list. The operation commits the prepared changed set
or none. Structured failure may identify the offending entry for UX diagnostics.

Success returns at least:

```text
StartingRevision
ResultingRevision
ChangedWriteCount
HistoryTransactionId
```

`ExpectedRevision` is required. V1 accepts no caller transaction id and rejects
while an explicit authoring transaction is open.

The top-level EditorHost major version may remain 1 because the method is
handshake-negotiated/additive. Handshake should advertise a dedicated property-
batch capability/version. Current discovery is already v6; the implemented
additive `PropertyBatchVersion = 1` versions both the operation and the optional
`AtomicBatchWritable` metadata, preserving compatibility with existing v6
consumers. That flag is UX guidance only; every request is authoritatively
revalidated.

## Executable feasibility / failure-injection gate

Production work starts with a focused Engine test seam before EditorHost exposure.
Provide injectable preparation failures after:

1. value decode/normalization;
2. journal payload construction/encoding;
3. history action allocation/build;
4. notification preparation;
5. aggregate resource reservation;
6. journal retained-storage/sequence reservation; and
7. final revision/identity validation.

For each failure capture and compare all target WireValues, revision, history
entries/cursor/retained bytes, affected journal records/cursors and observer fire
counts. Required result:

```text
zero batch live mutation
zero batch revision change
zero journal publication
zero history action/cursor movement
zero observer notification
```

Then, with preparation injection disabled:

- make the commit boundary explicit and mechanically `noexcept` where possible;
- `static_assert` nothrow generated prepared stores;
- reject allocations beginning at the commit boundary and prove authoritative
  commit still completes;
- assert the first callback already sees all final values and new revision;
- deliberately throw from one post-commit callback and prove state/revision/
  journal/history remain committed;
- cover two objects, many objects, `Name`, strings and numeric/vector properties;
- cover duplicates, 257 writes, too-small journal capacity, revision exhaustion,
  stale generation, schema mismatch and invalid references;
- force a revision/identity change between test-only Prepare and final validation
  and prove this batch contributes no mutation; and
- prove one successful batch is one history action whose Undo/Redo atomically use
  the same prepared mechanism.

No production/test seam is implemented by this architecture-only task. These tests
are the first implementation gate, not optional follow-up hardening.

## Implementation sequence

1. **Property preparation surface:** shared wire decode/validation; generated
   prepared-safe capability; nothrow raw store; internal prepared value and
   notification types.
2. **Prepared journal install:** prepare/reserve token and allocation-free swap
   while preserving capacity/sequence/render-dirty semantics.
3. **Prepared history retention/replay:** immutable retained actions, candidate
   pointer/index/mapping install, `PreparedPropertyBatch` replay policy and
   prepared Undo/Redo cursor move.
4. **Engine orchestration + proof:** synchronous Main prepare/final-validate/
   commit path plus failure-injection/allocation-after-boundary tests.
5. Only after steps 1–4 are green: add `SetPropertyBatch` and schema/handshake
   capability metadata to EditorHost.
6. Studio consumes the public contract in a separate task.

Steps 1–4 are the exact next Engine implementation task and must qualify together
before the new operation is called atomic.

## Compatibility / non-goals

Preserved:

- existing single-property APIs;
- ordinary transaction commit-only/partial-success behavior;
- current history bounds/authority;
- current `SetTransform` behavior;
- legacy sequential history replay;
- journal/replication Engine authority; and
- Studio's non-authoritative role.

This decision does not create general ACID transactions, transaction abort,
atomic mixed structural mutations, prepared arbitrary `overrideWrite` setters,
durable crash recovery, or a public prepared object/API. It does not modify
Studio or unrelated networking/Foundation work.

Later cleanup may migrate single-property mutation or `SetTransform` onto the
prepared path after qualification; that is not required for the bounded batch.

## Accepted design checkpoint readiness

**READY FOR IMPLEMENTATION** was the accepted architecture checkpoint decision.
Current implementation readiness and proof results are maintained in the
[validation record](../CurrentArchitecture/PreparedPropertyCommitValidation.md).

The source provides a narrow feasible path: generated backing storage can be split
from the fallible `NotifyPropertyCommitted` tail; journal copy/swap work can move
to preparation; render dirty is already no-throw/fail-open; Main ownership keeps
preparation stable without a graph-wide lock; callbacks can be delayed until
post-commit; and history needs a localized reservable-storage/install refactor,
not a transaction semantic rewrite.

Stop and revisit this decision if implementation proves that a supposedly
prepared-safe property must execute arbitrary callback/user code during raw state
application, or that journal/history candidate installation cannot actually be
allocation-free after the boundary.

**Next task at design acceptance:** implement the Engine-only prepared property commit foundation
and failure-injection proof (steps 1–4). Do not add `SetPropertyBatch` to
EditorHost and do not modify Studio until that proof is green.
