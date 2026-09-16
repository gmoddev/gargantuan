---
status: implemented
owner: runtime-authoring
last_verified: 2026-09-16
source_base: 1e067f2f32e904130c5065022974a71ad2f31ab6
---

# Internal prepared property commit foundation

The governing decision is the accepted
[Prepared Property Commit Architecture](../FutureArchitecture/PreparedPropertyCommitArchitecture.md).
This record covers its internal implementation and qualification only. EditorHost
does not expose `SetPropertyBatch`, and schema discovery has no new batch flag.
Studio and Foundation 3L are unchanged.

## Implementation ownership

- `src/runtime/PreparedPropertyCommit.{hpp,cpp}` owns synchronous Main admission,
  preparation, final validation, authoritative commit, and post-commit delivery.
  Its request and qualification hooks are private Engine source interfaces, not
  wire requests, Luau APIs, or queued prepared objects.
- `tools/classgen.luau` registers backing-member hooks through
  `InstanceProperty::UsePreparedStore`. A hook exists only for an ordinary
  generated editable backing member whose type is nothrow move-assignable.
  Typed `std::any` validation precedes the pointer-form `any_cast` and raw move.
  The hook never calls the ordinary setter.
- Existing `Instance::ValidatePropertyWireMutation`, `InstanceProperty` validators,
  `DecodeNativeWireValue`, and the canonical wire encoder remain the shared
  permission/type/domain/range/enum/normalization rules. Preparation additionally
  verifies the exact native backing type, protocol value validity, frozen schema,
  full ObjectId, world ownership, and declaring schema/version.
- `ChangeJournal::PreparedJournalBatch` partitions prepared records by their
  existing scope, constructs replacement deques, validates sequence/capacity,
  computes eviction, and owns the journal mutex. A failed reservation removes
  only empty stream entries it created. Existing records remain untouched.
- `AuthoritativeTransactionHistory` retains immutable shared actions. Preparing
  a candidate copies bounded handles and identity mappings, then runs existing
  retention/pruning against the candidate. Ordinary entries keep legacy replay.
  Prepared entries use `TransactionReplayPolicy::PreparedPropertyBatch`.

The supported surface is ordinary generated editable native scalar, string,
vector, color, UDim/UDim2, CFrame and native-enum storage when the concrete type
passes the generated nothrow check and the canonical wire decoder supports it.
This includes `Name` and generated `BasePart.CFrame`/`Size`. Handwritten and
`overrideWrite` setters, references, Source, sparse custom/extension maps,
Attributes, Tags and hierarchy are excluded. No arbitrary setter is annotated
`noexcept` or advertised as safe.

## Boundary and visibility

After final exact revision, generation, world, schema descriptor/generation and
history checks, the explicitly `noexcept` commit body performs only raw stores,
history-container swaps, journal-container swaps/sequence assignments, one
prevalidated revision increment, and journal unlock. No callbacks or signal
deferral queue run in this window. Journal readers acquire the same mutex and
cannot observe the new publication with the prior revision.

Prepared render-dirty work and existing captured property signals run afterward.
No signal is created to retain a past event. Observer delivery failures are
counted separately in `PreparedPropertyResult` and native `TransactionResult`;
they cannot reverse authoritative success, and later direct notifications are
still attempted. Callbacks can initiate a later ordinary mutation.

Forward commits retain one semantic action. Exact no-ops retain none and advance
no revision. Undo/Redo resolve history identity aliases, compare all current
values with captured After/Before, and prepare their inverse/forward values
through the same coordinator. Cursor changes install with the prepared history
candidate. Open ordinary groups and revision deferral/batching reject admission.

## Resource accounting

The primitive enforces 1–256 writes, 256-byte identifiers, 64 KiB strings,
4 MiB aggregate encoded old/new values, at most 256 direct notifications and
journal records, 2 MiB prepared journal payload accounting, and the existing
8 MiB semantic action limit. Retained history remains 128 actions / 32 MiB by
default, including configured smaller test limits.

Before per-write storage growth, forward admission conservatively charges the
eventual 1 MiB request envelope using conservative encoded value bytes, six bytes
per identifier byte for escaping, 512 metadata bytes per entry, and 128 envelope
bytes. It does not accept a caller-supplied size assertion. Replay has no request
envelope but retains all prepared value/journal/history limits. Bounds reject;
there is no chunking. Journal replacement cost includes copying the currently
retained records as required by the accepted design; that work occurs outside
the bounded authoritative commit window. The samples below use an empty journal
and do not establish worst-case preparation cost at full retained capacity.

Each value's accounting reserves 512 fixed bytes (covering the largest closed
compound, including numeric JSON text) plus its dynamically escaped string/enum
bytes. A temporary JSON tree is deliberately avoided: sustained allocation
rejection exposed allocation in that tree's destructor during preparation
unwinding. The closed `WireValue` payload itself is still the existing canonical
journal representation; no serialization format changes.

## Executable proof and measured evidence

`tests/PreparedPropertyTests.cpp` is registered as `gargantuan_prepared_property`
in the complete headless CTest contract. The 2026-09-16 supplemental MSVC Release
run passes with the following evidence:

- 80 deterministic stage-failure cases cover aggregate reservation, every
  property/value, journal encoding and notification index, history allocation
  and retention, final validation and journal reservation, including replay.
- Sustained allocation rejection sweeps every allocation position until the
  first authoritative success: 29 fresh-forward, 41 Undo, 48 Redo, 50 forward
  behind the cursor with identity-map pruning, and 31 absent-journal-stream
  failures (199 total). Each rejected attempt preserves live values, revision,
  journal payload/sequence/stream existence, shared history handles/bytes/cursor
  and notification count. Counts are allocator/platform dependent.
- 26 successful authoritative commits reject every C++ allocation from the
  explicit boundary through journal unlock. Forward, Undo and Redo all pass,
  including strings, native enums, vectors, colors, UDim2, bool and CFrame.
  This exercises replacement `new`/`new[]`, aligned and nothrow forms; raw
  stores are additionally restricted by exact native types and static nothrow
  assignment checks. No boundary allocation or stop condition was observed.
- The first direct observer sees every affected object/property, the new
  revision, complete journal and history. Replay observers see complete inverse
  and forward states. Throwing delivery preserves success and attempts later
  property notifications. A concurrent journal reader sees the committed
  publication with the new revision after acquiring the journal mutex.
- Denials cover stale revision/identity, incompatible schema, wrong world/domain,
  permissions, invalid types/enums/ranges/nonfinite values, unsupported stores,
  duplicate/oversized input, old+new and replay journal budgets, sequence/revision
  exhaustion, open groups, revision deferral and insufficient history capacity.
  Final revision/identity invalidation contributes no batch state. Retention,
  redo-suffix pruning, front eviction, legacy destroy/restore identity aliases,
  divergent replay, no-op and no-signal-creation cases pass.

The affected Windows CTest selection (`gargantuan_prepared_property` and
`gargantuan_foundation`) passes. Existing foundation assertions cover ordinary
setters, partial-success ordinary groups, legacy Undo/Redo, journal capacity and
eviction, property notifications and atomic SetTransform. Existing assertions
were preserved; five history accesses only changed to dereference shared actions.

The separate Linux Clang 19 Release run also passes both affected tests with
ASan/UBSan (`halt_on_error=1`) and leak detection (`detect_leaks=1`), with no
sanitizer or leak report. Its sustained allocation sweeps reject 34/43/48/49/37
positions in the same five modes (211 total), and all 26 boundary-denial commits
pass. This run uses the canonical C++ sanitizer coverage with uninstrumented
vendor C dependencies. An initial supplemental configuration additionally
instrumented SDL C objects and failed SDL's `--no-undefined` shared-library link;
correcting those build-only flags to match CI resolved that harness issue.
No repository build configuration was changed for the workaround.

The documentation site builds all 19 pages; affected relative Markdown links
resolve and the staged patch passes `git diff --check`.

Single-run Release samples on the shared remote Windows worker, with one
96-byte new Name per object and an initially empty retained journal:

| Writes | Allocation calls | Total requested allocation bytes | Old+new budget bytes | Journal budget bytes | History semantic bytes | Total time | Commit window |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 16 | 1,600 | 1,128 | 1,144 | 380 | 3.6 us | 0.2 us |
| 32 | 205 | 42,998 | 36,096 | 36,608 | 12,160 | 51.8 us | 0.2 us |
| 256 | 1,552 | 342,379 | 288,768 | 292,864 | 97,280 | 551.0 us | 1.4 us |

Allocation bytes are cumulative requests, not peak resident memory. Encoded
budget charges are conservative upper bounds, not exact serialized lengths.
Total time includes preparation, commit, post-commit dirty work and cleanup;
the independently instrumented commit window excludes those fallible stages.
These are smoke measurements, not a stable latency guarantee or optimization
claim. Every size also completes prepared Undo and Redo.

## Per-revision certification and next task

The authoritative hosted gate is [Native engine CI for this implementation
branch](https://github.com/gmoddev/gargantuan/actions/workflows/native-ci.yml?query=branch%3Aprepared-property-foundation).
It builds and runs the complete registered suite with MSVC 19.50+ Release and
Clang 19 ASan/UBSan with `detect_leaks=1` (LSan). Its result must match the exact
source revision under consideration; a previous green revision is insufficient.
The task completion report records that immutable revision and run URL.

The remote supplemental Windows worker has MSVC 19.44. Its build disables
precompiled headers to avoid that compiler's existing `/pathmap` PCH failure.
This is supplemental evidence; it does not replace the hosted MSVC 19.50+ gate.

**Readiness rule:** the internal foundation is **READY FOR EDITORHOST EXPOSURE**
only with both hosted jobs green on its final source and passing documentation,
link and whitespace checks. This document does not grant permission to bypass
that gate, and the implementation does not expose the request.

The exact next implementation task after certification is a separate Engine-only
EditorHost `SetPropertyBatch` admission/response integration: parse a bounded
canonical request, enforce the exact document revision and authority, call this
coordinator once, expose only properties with the proven prepared capability,
and qualify protocol errors, no-op, journal/replica visibility, history and
post-commit notification failure reporting. Preserve SetTransform and ordinary
transaction semantics. Studio integration remains a later task.
