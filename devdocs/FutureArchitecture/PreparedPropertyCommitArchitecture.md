---
status: accepted-design
implementation_status: not-started
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
  - ../../../docs/invariants/Core.md
---

# Prepared property commit architecture

## Decision

**Selected architecture: B — a small reusable Engine-owned prepared-mutation
primitive specialized to bounded property batches.**

The primitive is reusable by the new multi-object property operation and by the
Undo/Redo replay of history entries created by that operation. It is **not** a
general prepared transaction mechanism. Existing ordinary transaction grouping,
existing single-property APIs, and the current `SetTransform` compatibility path
retain their current semantics until separately migrated.

The required semantic unit is:

```text
one bounded property-batch request
    -> validate and prepare everything that can ordinarily fail
    -> final revision/identity check
    -> non-failing authoritative commit
    -> one revision transition
    -> one retained history action
    -> post-commit observer delivery
```

Oversized batches reject before live property mutation. They are never silently
chunked.

This decision exists to support the accepted Studio requirement that one
multi-object Properties gesture be one revision-checked authoritative action,
all writes or none, and one Undo/Redo entry. Studio remains a non-authoritative
client of this Engine primitive.

## Why the current implementation is insufficient

The current source has several independent late-failure boundaries.

### Ordinary property setters

Generated ordinary properties currently execute:

```text
AssertCanMutate
ValidatePropertyMutation
backing field = value
NotifyPropertyCommitted
```

`NotifyPropertyCommitted` runs **after** the backing field has changed. It may:

- resolve property metadata;
- encode the committed native value to `WireValue`;
- allocate/copy property-name and payload storage;
- publish to `ChangeJournal`;
- advance the DataModel authoritative revision; and
- create/find and fire the property-changed signal.

Therefore the public reflected setter is not a valid atomic commit primitive.
The live value can already be new when encoding, journal publication, signal
queueing, or callback machinery fails.

### Current atomic `SetTransform`

`UpdateWirePropertiesCommand` preflights the requested values but then applies
them sequentially through the ordinary setter path. If a later write fails, it
runs compensating writes under journal suppression to restore earlier values.
That is useful compatibility behavior, but it is not the required proof that the
commit boundary itself cannot partially fail.

The new property-batch operation must not use compensating live-state rollback as
its atomicity mechanism.

### Current authoritative transaction commit

The current transaction path also has unsafe post-mutation work:

- `RecordMutation` grows the open transaction change/journal vectors after the
  underlying mutation may already have happened;
- `Commit` advances `AuthoritativeRevision` before journal/history retention is
  completely installed;
- journal grouping allocates temporary vectors;
- `ChangeJournal::CommitBatch` copies/grows retained storage; and
- retained history insertion/pruning may allocate.

The prepared property path therefore must not call the current open-transaction
`RecordMutation`/`Commit` sequence after changing live values.

### Signal deferral

`ScopedSignalDeferral` prevents callbacks from running immediately, but deferred
`BaseSignal::Fire` uses a thread-local `std::vector::emplace_back`. Queue growth
may allocate while setters are already mutating state. Its final delivery also
runs arbitrary native/Luau observer code.

Prepared property commits must prebuild their notification representation and
must not depend on this allocating queue during the authoritative commit.

### Undo/Redo

Current history replay validates and applies transaction changes sequentially,
captures journal records, and uses compensating replay if a later change fails.
It then advances the history cursor/revision before captured journal publication.
That preserves current compatibility semantics but does not provide atomic
multi-object replay.

A property-batch history entry must explicitly opt into prepared atomic replay.
Legacy history entries remain on the current sequential path.

## Rejected alternatives

### A — one-off dedicated batch with unrelated replay logic

A dedicated batch implementation can commit the forward edit, but the same
all-or-none requirement applies to Undo and Redo. Reimplementing preparation and
commit rules inside history replay would create two atomicity implementations.
That is unnecessary duplication.

### C — general prepared authoritative transaction subsystem

The current requirement does not need atomic reparent + tags + attributes +
subtree lifecycle + arbitrary property combinations. Generalizing all transaction
change types would also alter existing commit-only/partial-success semantics.
There is no source evidence that such a rewrite is necessary for property
batches.

The selected B primitive is therefore deliberately narrower: **property state
only**.

## Scope of v1 prepared property batches

The first implementation must support only reflected properties that can prove a
non-failing raw state write.

### Automatically eligible properties

The class generator already distinguishes ordinary generated properties from
`overrideWrite` properties. An ordinary generated property has a generated
backing member and a setter whose only semantic tail is
`NotifyPropertyCommitted`.

The generator should add an **internal prepared-store hook** for an ordinary
editable generated property when its backing type is nothrow move-assignable.
The hook is not a Luau/public API.

Conceptually:

```cpp
struct PreparedPropertyValue {
    std::any NativeValue;      // decoded/normalized during preparation
    WireValue CanonicalValue;  // immutable publication/history value
};

using PreparedStore = void (*)(
    Instance &,
    std::any &preparedNativeValue
) noexcept;
```

The generated store performs only the already-validated backing-state update.
It does not validate, encode, journal, advance revision, allocate notification
state, or call observers.

Generation should fail at compile time rather than mark a property prepared-safe
when the backing assignment cannot satisfy the no-throw contract.

`Name`, canonical generated scalar/vector/enum/string state, and generated
`BasePart.CFrame`/`Size` fit this model. `std::string` is prepared before the
boundary and transferred by nothrow move under the default allocator.

### Handwritten `overrideWrite` properties

`overrideWrite` is **not prepared-batch-writable by default**. A handwritten
setter may perform arbitrary secondary work, callbacks, validation, allocation,
physics/runtime activity, or multiple property changes.

Such a property may opt in later only with an explicit internal prepared hook
that separately defines:

1. fallible preparation;
2. a nothrow authoritative state application; and
3. any bounded post-commit effects/notifications.

The initial implementation must reject a batch containing a property without the
prepared-safe capability. It must not call the ordinary setter and hope it does
not fail.

### Custom/extension/attribute state

The v1 primitive is for reflected native property writes. Attributes, extension
properties, custom-class override maps, tags, hierarchy and source editing remain
on their existing APIs. They can adopt the primitive later only if their storage
and publication can satisfy the same proof.

Current Studio object-reference editing remains read-only. If reference-valued
native properties are made editable later, preparation must resolve the full
generation-bearing target, validate class/nullability/same-scope constraints, and
hold the resolved target strongly until commit; no reference lookup is deferred
to the commit phase.

## Shared property preparation

Preparation should factor existing validation rather than duplicate it.

Add an internal resolver conceptually equivalent to:

```text
PreparePropertyWireMutation(
    Instance,
    canonical property identity,
    WireValue,
    security context
) -> PreparedPropertyMutation | MutationStatus
```

It performs the same authoritative checks currently used by
`ValidatePropertyWireMutation` / `ApplyPropertyWireMutation`:

- exact live generation-bearing `ObjectId`;
- target belongs to the open DataModel;
- canonical property exists;
- declaring schema id and definition version match;
- Studio editability and write capability;
- semantic type and native decoding;
- numeric/domain/range validators;
- enum identity;
- reference nullability/class/scope where applicable;
- property-specific validator;
- prepared-batch-writable capability; and
- source/script restrictions.

It returns both the normalized native representation and canonical `WireValue`.
No live property is changed.

The ordinary single-property setter path does not have to migrate in the first
implementation task. The validation/decode helper should nevertheless be factored
so ordinary mutation can reuse it later without changing public semantics.

## Batch preparation

The complete operation runs synchronously on the Main execution domain. It does
not yield and does not run observer/user callbacks between preparation and
commit.

### 1. Envelope admission

Before per-write preparation:

- reject zero writes;
- reject more than `MaximumPreparedPropertyWrites`;
- reject an encoded EditorHost request above the existing 1 MiB input envelope;
- reject duplicate `(ObjectId, declaring schema, property name)` targets;
- reject arithmetic overflow while accounting bytes/counts; and
- reject use inside an explicit/open ordinary authoring transaction for the v1
  EditorHost operation.

The dedicated operation owns one semantic history action. Ordinary explicit
transactions keep their existing grouping behavior.

### 2. Revision admission

The eventual EditorHost operation requires an exact `ExpectedRevision`.
Preparation rejects with conflict if it differs from the current DataModel
`AuthoritativeRevision`.

`EnsureAuthoritativeRevisionAvailable()` runs before any prepared resource is
committed.

### 3. Resolve and prepare every write

For every requested write:

1. exact `ObjectRegistry::Lookup(ObjectId)`;
2. reject destroyed/destroying/wrong-world identities;
3. resolve canonical property/schema metadata;
4. validate and normalize the requested value;
5. capture the current canonical `Before` value;
6. omit exact no-op writes;
7. construct the prepared native value consumed by the raw store hook;
8. construct the exact `PropertyTransactionChange` old/new representation;
9. construct the exact journal payload that ordinary committed state requires;
10. capture the existing property-changed signal if one already exists.

Calling `GetPropertyChangedSignal` merely to prepare notification delivery is not
required. If no signal object exists at preparation time, no observer can already
be connected to it, and no past event needs to be delivered to a signal created
later.

All vectors/maps used by preparation have their final bounded capacities before
the commit boundary.

### 4. Prepare notifications

The v1 generated-property path produces at most one direct property notification
per changed write. Preparation owns a bounded list of existing signal pointers.
No `BaseSignal::Fire` occurs yet.

Notification delivery itself is deliberately **outside** the authoritative
commit. Callback allocation, reentrancy or exceptions are observer-delivery
failures, not mutation rollback conditions.

### 5. Prepare one history action

Build the complete retained action before mutation:

```text
CommittedTransaction
    Id
    Label = "Set Properties"
    StartingRevision = expected revision
    ResultingRevision = expected revision + 1
    Changes = all PropertyTransactionChange records
    ReplayPolicy = PreparedPropertyBatch
    SemanticBytes = precomputed bounded size
```

The transaction is one history cursor entry regardless of write count.

A failed preparation must not make it visible in history.

#### Reservable history installation

Current value-owned `deque` retention does not expose a no-allocation insertion
boundary. The implementation should make retention cheaply preparable without
changing transaction semantics:

- store retained committed actions as immutable shared transaction objects;
- keep at most the existing 128 retained actions;
- build a bounded candidate vector/index of shared transaction pointers during
  preparation, applying the existing redo-suffix and byte/count eviction policy;
- precompute any identity-mapping pruning into a candidate mapping vector; and
- install the candidate containers/counters by `swap`/integer assignment after
  live state application.

Only pointer/index state is copied when preparing retention; existing transaction
payloads are not duplicated. Ordinary transaction retention can use the same
internal storage representation while preserving all existing public semantics.
This is a storage/reservation refactor, not a new transaction model.

The implementation must statically/runtime-prove that the prepared install path
contains no capacity growth.

### 6. Prepare journal publication last

Journal reservation is the final fallible preparation stage because it may hold
the existing journal mutex through the short final commit window.

Add an internal `PreparedJournalBatch` concept. Under the journal mutex it:

- partitions the already-built records by scope;
- verifies sequence-space arithmetic for each scope;
- rejects when the new records for a scope exceed the configured journal
  capacity, because the complete committed batch must be retainable together;
- ensures any required stream entry exists;
- constructs the complete replacement retained deque for each affected scope,
  including ordinary oldest-record eviction under the existing capacity policy;
- assigns the exact future sequence numbers in the candidate; and
- records lightweight render-dirty intents for post-commit delivery.

This reuses the copy-then-swap shape `CommitBatch` already performs, but moves all
copy/allocation work before live mutation.

If preparation created an otherwise-empty stream entry and final validation
fails, the reservation token erases that still-empty entry before releasing the
mutex. No journal record/cursor becomes observable.

After successful journal preparation, no expected fallible operation remains
before the commit boundary.

## Final boundary validation

Immediately before the first live write, while all resources are prepared:

- current authoritative revision still equals `ExpectedRevision`;
- authoritative revision is not exhausted;
- every exact ObjectId still resolves to the same prepared target;
- every target remains alive, not destroying, and in the same DataModel;
- every prepared property descriptor still matches its schema identity/version;
- the prepared history reservation is still valid; and
- the journal reservation still owns the exact source sequence boundary it
  prepared.

Failure here releases prepared resources and returns without mutation.

The runtime schema is frozen for the open project, and authoritative graph
mutation is Main-owned. The production operation performs preparation and commit
synchronously with no callback/yield, so successful final validation cannot be
invalidated by another ordinary authoritative mutation before the commit body.
No global DataModel lock is added.

## Authoritative commit boundary

After final validation succeeds, enter one internal commit function whose contract
is `noexcept`/non-failing for all ordinary recoverable conditions.

The exact order is:

```text
COMMIT BOUNDARY
    1. Apply every prepared raw property store.
       - no validation
       - no allocation
       - no journal call
       - no revision call
       - no observer callback

    2. Install the prepared history candidate.
       - bounded container swap / counters only

    3. Install all prepared journal stream candidates.
       - retained deque swap / NextSequence assignment only
       - journal mutex remains held

    4. Advance AuthoritativeRevision exactly once through an internal
       prevalidated no-throw increment.

    5. Release the journal reservation/mutex.
COMMIT COMPLETE

POST-COMMIT
    6. Publish precomputed render-dirty intents (already noexcept/fail-open).
    7. Deliver prepared property notifications.
```

The revision increment is the Main/EditorHost publication fence. Journal readers
cannot observe the newly installed journal records before that increment because
the journal reservation keeps the journal mutex until afterward.

History is Main-owned and no observer/user callback runs during steps 1–5.

A process crash between machine instructions is outside this in-memory atomicity
contract; this design is not a durable transaction log. The proof obligation is
that no **ordinary expected recoverable failure** exists after step 1 begins.

## Non-failing commit proof obligations

| Concern | Resolution before/at commit |
| --- | --- |
| property validation | completed during preparation |
| native/wire decoding | completed during preparation |
| schema/reference lookup | completed during preparation; final identity equality check before boundary |
| backing-value allocation | native value fully built before boundary; raw store only nothrow move/assignment |
| journal payload encoding | complete canonical payload built before boundary |
| journal sequence exhaustion | checked during journal preparation |
| journal retained storage growth | replacement storage allocated before boundary; commit swaps |
| history action allocation | action allocated/built before boundary |
| history container growth/pruning | candidate pointer/index/mapping containers built before boundary; commit swaps |
| revision exhaustion | checked before boundary; commit uses prevalidated increment |
| notification queue growth | no allocating deferred queue is used during commit |
| signal callbacks/user code | run only after commit |
| render dirty accumulation | existing API is `noexcept` and fail-open; invoked post-commit |
| synchronization | journal mutex acquisition occurs in preparation, not in commit; commit only owns/releases the prepared lock |
| integer/resource accounting | checked with bounded arithmetic during preparation |

If an implementation cannot make a prepared raw store nothrow, that property is
not eligible. If journal/history cannot supply the prepared install contracts
above, implementation must stop rather than add live-state rollback.

## Notification failure model

Post-commit notification delivery observes an already-complete authoritative
batch. Every callback must therefore see all committed property values and the
resulting project revision.

A native callback may throw; Luau materialization/execution may fail; signal
snapshot construction may allocate. These are **delivery failures after a
successful authoritative commit**.

The prepared property operation must:

- never roll back authoritative state because an observer failed;
- log/diagnose delivery failure through the existing Engine diagnostics path;
- continue later prepared notification delivery where safely possible; and
- return the mutation as committed/successful.

This is intentionally different from treating a callback exception as an
authoritative transaction failure.

## Observer visibility

Atomic means no supported Engine observer can be invoked by this batch while one
prepared property is new and another is old.

The proof relies on existing ownership rather than a new global lock:

- authoritative mutation runs on Main;
- the operation does not yield/reenter during preparation or commit;
- direct property signals are delivered only post-commit;
- journal readers remain behind the journal mutex until journal installation and
  revision publication are complete; and
- renderer/runtime projections receive post-commit dirty/signal state and are not
  authoritative graph owners.

Unsynchronized foreign-thread direct reads of mutable `Instance` backing fields
are outside the Engine's authority/threading contract and are not made safe by
this feature.

## Revision semantics

### Failed preparation

```text
live property state: unchanged
AuthoritativeRevision: unchanged
history cursor/entries: unchanged
journal records/cursors: unchanged
observer notifications: none
```

Internal temporary allocations or an unobservable empty journal stream reserved
and then erased do not constitute committed state.

### Successful non-no-op batch

```text
StartingRevision = R
all prepared values become authoritative
one history action is installed
all corresponding journal records are installed
AuthoritativeRevision becomes R + 1 exactly once
notifications run afterward
```

A batch whose requested values are all already current returns success/no-change
without a revision increment or history action, matching existing no-op mutation
conventions.

No per-property revision authority is introduced.

## Undo and Redo

A retained property-batch transaction carries
`ReplayPolicy = PreparedPropertyBatch`.

Undo/Redo for that entry does **not** enter the legacy sequential replay loop.
Instead it constructs a new prepared property batch from all captured entries:

- Undo expects every current value to equal captured `After` and prepares every
  `Before` value.
- Redo expects every current value to equal captured `Before` and prepares every
  `After` value.
- historical reference identities are resolved before the commit boundary.
- journal/notification resources are prepared exactly as for a forward batch.
- history traversal prepares a no-allocation cursor transition instead of a new
  history action.

Successful Undo or Redo applies the whole property batch, installs its ordinary
journal records, advances the project revision once, moves the existing history
cursor once, then delivers notifications.

Any divergent value, stale identity, incompatible schema, resource-preparation
failure, or revision exhaustion fails before mutation and leaves the history
cursor unchanged.

Legacy transactions retain their current sequential/compensating replay semantics.
This design does not silently strengthen or change them.

## Resource bounds

The initial profile is deliberately below the general transaction ceiling and is
derived from existing EditorHost/history limits.

| Resource | v1 bound | Derivation |
| --- | ---: | --- |
| changed writes per batch | **256** | matches the existing EditorHost journal batch scale and supports an interactive 256-object one-property selection without inheriting the much larger 4,096-change transaction ceiling |
| encoded EditorHost request | **1 MiB** | existing EditorHost input limit |
| property/identifier string | **256 bytes** | existing protocol identifier limit |
| individual protocol string value | **64 KiB** | existing protocol string limit |
| aggregate prepared old/new value storage | **4 MiB** | reuses the existing bounded single-action/subtree staging scale; includes both captured old values and normalized new values |
| prepared journal records | **256** | at most one ordinary property journal record per changed v1 write |
| aggregate prepared journal payload | **2 MiB** | requested new state already fits the 1 MiB host envelope; 2 MiB leaves bounded canonical metadata/normalization headroom |
| retained history semantic bytes | **8 MiB** | existing `MaximumTransactionSemanticBytes`; the new action gets no larger exemption |
| direct prepared notifications | **256** | generated v1 property path emits at most one direct property notification per changed write |
| retained history count/bytes | **128 / 32 MiB** | unchanged existing history limits |

All accounting uses checked/saturating arithmetic before resource allocation based
on attacker-controlled counts.

A journal configured below the number of records that the complete batch would
insert rejects the batch during preparation. Existing retention may evict older
records according to its current bounded policy, but the newly committed batch
itself must fit as a complete retained suffix.

The `MutationGateway` pending-command bound remains unchanged. No prepared batch
is retained after its synchronous request completes; failed requests cannot build
an unbounded prepared-state backlog.

## Security and adversarial input

The eventual EditorHost request is untrusted authoring input.

Preparation must fail closed for:

- count or aggregate-byte overflow before per-entry growth;
- duplicate/conflicting writes to the same canonical property target;
- stale generation-bearing ObjectIds;
- wrong DataModel scope;
- declaring-schema/version mismatch;
- non-editable or non-prepared-safe properties;
- invalid enum/reference/type/range/domain values;
- script `Source` through the generic property path;
- references that are stale, wrong-class, nullable-invalid or wrong-scope; and
- batches whose journal/history/notification/value budgets cannot be reserved.

The operation never accepts capability, authority scope, or prepared-safety claims
from request data. Those remain host/schema-owned.

## Eventual EditorHost contract

Do not implement this protocol in the design task. The intended method is
conceptually:

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

`ExpectedRevision` is required, not advisory. There is no per-write success list:
the operation either commits the prepared changed set or commits none of it.
Structured failures may identify the failing request index/ObjectId/property for
UX diagnostics without implying a partial success.

A successful response should include at least:

```text
StartingRevision
ResultingRevision
ChangedWriteCount
HistoryTransactionId
```

No transaction id is accepted from Studio in v1; this operation is one implicit
history action and rejects use while an explicit authoring transaction is open.

### Capability/versioning

The top-level EditorHost major version can remain 1 because the method is an
additive, handshake-negotiated capability. Handshake should advertise a dedicated
`PropertyBatchVersion = 1` (name subject to normal protocol naming review).

Schema discovery needs a version increment from the current v5 shape if Studio is
to filter compatible properties before submission. Each property should expose a
read-only Engine-derived capability such as `AtomicBatchWritable`.

That metadata is advisory for UX; the Engine revalidates it authoritatively on
every request.

## Executable feasibility and failure-injection gate

Production implementation must start with a focused test-only harness around the
prepared primitive before exposing EditorHost.

### Injectable preparation failures

Provide test-only failure points after each ordinary fallible stage:

1. value decode/normalization;
2. journal payload construction/encoding;
3. history action allocation/build;
4. notification representation preparation;
5. aggregate resource accounting/reservation;
6. journal retained-storage/sequence reservation; and
7. final pre-boundary revision/identity validation.

For every injected failure capture and compare:

```text
all target WireValues
AuthoritativeRevision
history entries + cursor + retained bytes
journal cursor/records for every affected scope
observer fire counts
```

Required result:

```text
zero batch live mutation
zero batch revision change
zero journal publication
zero history action/cursor movement
zero observer notification
```

### Commit proof

With injection disabled:

- instrument the code so the boundary is explicit;
- make the commit function `noexcept` where mechanically possible;
- compile-time assert nothrow generated prepared-store hooks;
- run an allocation-failure harness that begins rejecting allocations at the
  commit boundary; authoritative commit must still complete;
- connect callbacks to each property and assert the first callback already sees
  all batch values and the resulting revision;
- make one post-commit callback deliberately throw and prove state/revision/
  journal/history remain committed;
- test two objects, multiple objects, `Name`, string state and canonical numeric
  properties;
- test duplicate requests, 257 writes, journal capacity smaller than the batch,
  revision exhaustion, stale ObjectId generation, schema mismatch and invalid
  references;
- force a revision/identity change between explicit test-only Prepare and final
  validation and prove the prepared operation contributes no mutation; and
- assert a successful forward batch is one history action whose Undo and Redo use
  the same prepared mechanism atomically.

The allocation-failure test is important: merely having no explicit error return
from commit is not proof that hidden container growth disappeared.

This design task does not add that production/test seam. The first implementation
change must add it before EditorHost exposure.

## Implementation ownership and sequence

The smallest implementation sequence is:

1. **Property preparation surface**
   - factor shared wire validation/decode;
   - add generated prepared-safe capability and nothrow raw store hook;
   - add internal prepared property value/notification structures.
2. **Prepared journal install**
   - add prepare/reserve token and no-allocation candidate swap;
   - preserve existing capacity/sequence semantics and render-dirty behavior.
3. **Prepared history retention/traversal**
   - make retained committed actions immutable/shareable internally;
   - add candidate pointer/index/mapping installation;
   - add `PreparedPropertyBatch` replay policy and prepared Undo/Redo cursor move.
4. **Engine batch orchestration + failure injection**
   - add synchronous Main-domain prepare/final-validate/commit path;
   - add focused failure-injection tests and allocation-after-boundary proof.
5. **Only after the Engine proof passes:** add bounded `SetPropertyBatch` to
   EditorHost and expose prepared-batch capability metadata.
6. Studio may consume the public contract in a separate task.

Steps 1–4 are the next Engine implementation task. They may be split into commits
but must be qualified together before the new operation is called atomic.

## Compatibility and migration

This architecture intentionally preserves:

- ordinary single-property setter behavior;
- existing ordinary transaction commit-only/partial-success semantics;
- current transaction bounds and history ownership;
- existing `SetTransform` behavior;
- existing sequential history behavior for legacy transaction entries;
- journal/replication as Engine authority;
- Studio as a non-authoritative client; and
- current Source/structural/tag/attribute/extension operations.

After qualification, ordinary single-property mutations and `SetTransform` may be
migrated to reuse the prepared path, but that is a separate cleanup decision and
must not be required to ship the bounded multi-object operation.

## Non-goals

This decision does not:

- create a general ACID transaction system;
- add transaction abort/rollback semantics to ordinary transactions;
- make mixed hierarchy/tag/attribute/property transactions atomic;
- make arbitrary `overrideWrite` setters prepared-safe;
- make concurrent unsynchronized Instance reads supported;
- define durable crash recovery;
- expose an Engine prepared object to Studio or Luau;
- modify Studio in this task; or
- change unrelated networking/Foundation work.

## Readiness verdict

**READY FOR IMPLEMENTATION DESIGN EXECUTION.** No stop condition is presently
triggered.

The source demonstrates a narrow path to a non-failing commit:

- generated ordinary property storage can be separated from the fallible
  `NotifyPropertyCommitted` tail;
- journal publication already uses a candidate-copy/swap pattern whose fallible
  work can be moved before mutation;
- render-dirty publication is already noexcept/fail-open;
- authoritative graph mutation is Main-owned, so preparation can remain valid
  without a new global graph lock;
- notification callbacks can be held until after commit; and
- history requires a localized reservable-storage/install refactor, not a change
  to ordinary transaction semantics.

The design must be reconsidered if implementation proves that a supposedly
prepared-safe property requires arbitrary callback/user code during its raw state
change, or if journal/history candidate installation cannot actually be made
allocation-free after the boundary.

The exact next task is the Engine-only implementation of the **prepared property
commit foundation and its failure-injection proof** (steps 1–4 above). Do not add
`SetPropertyBatch` to EditorHost or modify Studio until that proof is green.
