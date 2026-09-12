---
status: investigation-in-progress
owner: runtime-networking
last_verified: 2026-09-11
related_code:
  - src/runtime/RuntimeWorkDiagnostics.hpp
  - src/network/ReplicationCoordinator.cpp
  - src/network/ReplicationPlanning.hpp
  - src/network/GameSession.cpp
  - tests/GameSessionBenchmark.cpp
---

# Foundation 3L.3: runtime work isolation

## Publication checkpoint (2026-09-11)

The retained source is being preserved on `foundation/3l-content-availability`,
from `a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1`. This is a partially-ready
development branch, not a merge or Foundation 3M authorization. The
[publication ledger](ContentAvailabilityFoundation3L_3Validation.md#publication-checkpoint-2026-09-11)
defines the scope and committed-source validation gate. The historical sections
below describe their original dirty-source checkpoints; their no-commit/no-push
statements are historical, not the current publication instruction.

After publication, the next investigation is due-to-recipient latency, including
cadence/tick dilation, client preflight, transport and observer work. The read-epoch
proposal remains unimplemented and is not the next authorized production change.
No latency gate is closed by publishing the accumulated corrections.

## Previous slice: derivation versus serial commit (2026-09-11)

**B — FOUNDATION 3L PARTIALLY READY.** Final native checkpoint `derive-lookup-v3`
retains a measured, serial 3E lookup correction and bounded attribution scopes.
No replication job, JobSystem change, wire change or logical-budget change was
introduced. Source is local and uncommitted; no push or Foundation 3M work.

The [derivation assessment](ReplicationDerivationAssessment3L.md) maps actual
reads/writes and Main ownership through 3E, private planning, 3J, frame preparation,
encoding, scheduler admission, Known/journal commit, transport and client apply.
The [validation ledger](ContentAvailabilityFoundation3L_3Validation.md) records
exact-source before/after scopes, service metrics, Amdahl estimates and gaps.

The remaining repeated 3E work was not primarily Desired-set reconstruction:
9,864 of 10,064 Local load evaluations retained the same roots, but still walked
5,060,921 old roots and 5,553,168 candidates. The same-root check now joins sorted
candidate/root identities with a call-local iterator instead of searching the
root tree per candidate, and avoids a duplicate root-map lookup before projection
lookup. The healthy owner maintains transactional root/projection membership;
current distance checks, owner pinning, hysteresis and semantic installation are
unchanged. No "nothing changed" shortcut, persistent cursor or queue was added.

Local cumulative 3E/hysteresis time falls from 1,542/879 to 853/332 ms over 301
ticks with identical visit counts. Final Control/Local/Node tick p99 is
4.07/30.82/30.74 ms. Local/Node Character throughput loss remains 9.15%/9.05%,
and recipient maximum gaps remain 652.59/649.24 ms. The 500-peer reload converges
in 4.257 s / 158 ticks, p99/max 43.27/52.41 ms, recipient max gap 425.41 ms;
load still has an 846.56-ms gap. Streaming health still fails.

There is a material *average server busy-time* opportunity for future 3E
parallelism, but not a safe current API to dispatch: spatial queries mutate shared
dedup marks and metrics, while planner resumes mutate shared reservations.
Revision checks alone are not thread synchronization. The assessment proposes
a same-tick pinned 3H/3K read view, private bounded scratch, Main result validation
and installation, and unchanged global allowances. That new read contract is
**not implemented**. Existing JobSystem batches/barriers suffice as execution
machinery, not as an input-lifetime contract. No jobified phase passed the complete
implementation gate in this slice.

Average cost and tail ownership differ: in the recorded 652.59-ms Local gap,
3E derivation totals only 33.11 ms, while twelve consecutive iterations each
select/accept 8,192 structural operations and simulated-recipient drain totals
234.54 ms. Client structural apply contributes a separate 23.57-ms spike. Even
ideal eight-worker relevance would save only about 28.97 ms in that window.
Encoding occurs twice per frame, but is a smaller average owner; no encoding or
transport redesign was combined with the lookup change.

Final MSVC targeted tests pass 6/6 and Clang 19 ASan/UBSan/LSan pass 7/7 with leak
detection, including the new independent hysteresis/lifecycle reference fixture.
The prior planner's 65,536/2,048 work limits, 8,192 selector cap, 16-tick maximum
measured 500-peer planning interval, finite reservations and acceptance-only Known
remain unchanged. Shared acquisition/initial admission remain exactly one each.
The 170-entry startup journal margin, client whole-world preflight, official
reliable delivery, overload/recovery, current-source security and CI remain open.

## Previous slice: dependency-complete continuation (2026-09-11)

**B — FOUNDATION 3L PARTIALLY READY.** Final checkpoint `planning-boundary-v13`
passes six targeted MSVC tests and seven Clang 19 ASan/UBSan/LSan tests with leak
detection enabled; measured streaming health still fails. The ledger
distinguishes each executable. The continuation supersedes
the previous slice's scope stop, not its retained measurements. Source remains
local and uncommitted; no push, wire change or 3M work is part of this slice.

### Ownership and completeness barrier

`GameSession::Step` supplies an immutable 3E selection through `RequestPlanning`,
then calls `ProcessPlanning` before the existing rotating 3J publication passes.
`ReplicationCoordinator::PlanningContinuation` owns Main-thread execution state,
implemented privately in `src/network/ReplicationPlanning.hpp`. Its stages are:

1. Capture the selection lease and catalog/accepted-state revisions.
2. Build/reuse ancestor and hard-reference closure privately.
3. Reconcile candidate Enters/Leaves, reverse Leaves and current Relevant view.
4. Examine current and accepted referrers; construct parent fixups and KI-007
   clear/replace/removal dependencies; calculate complete group costs.
5. Finish at most one peer-quantum batch, including its restore operations.
6. Install only the complete projection/Pending replacement and expose READY.
7. Let 3J defer or prepare the READY batch; only scheduler acceptance changes
   Known, accepted ancestry/references and materialization lifetimes.
8. Dispose consumed/invalidated scratch incrementally; restart if work remains.

An incomplete projection or group is never Pending, Desired, Known, or accepted
truth. 3E remains semantic relevance authority; these private sets are merely
execution scratch. READY is neither accepted nor a transport commitment. Current
publication templates supply scalar values when a READY batch is submitted, so
a yield cannot replay an obsolete CFrame or ordinary property value.

The final candidate reads accepted state in place under `AcceptedRevision`;
it does **not** copy a second Known/accepted graph. An earlier measured variant
did copy it; removing that redundant state reduces allocation and disposal work.
The old synchronous coordinator helpers remain a semantic test oracle and retain
the reference index/ordered-lookup correction. They reject attempts to mutate a
continuation-managed peer. The resumed path reacquires catalog/reference values
by identity rather than retaining unsafe property nodes across ticks.

### Work, memory and fairness contract

| Resource | Default / enforced ceiling |
| --- | --- |
| Charged planning work per tick | 65,536; native configuration range 2–262,144 |
| One peer's work before rotation | 2,048; no larger than the global allowance |
| Private reservation credits per continuation | 262,144 |
| Shared credits, including retained input leases | 8,388,608 |
| Live plus detached continuation peers | 1,024 |
| READY batch | Existing 3J peer quantum, normally 512 complete operations |
| Existing 3J global acceptance/selection limit | 8,192 operations/tick, unchanged |
| Existing Pending limits | 65,536/peer; 1,048,576/session, unchanged |

A charged unit is a peer visit, coroutine resumption over an identity/edge,
candidate/referrer/fixup/group step, or scratch cleanup step. Nested loops yield
too, including empty-reference container visits. Tick zero has one allowance;
same/older ticks cannot refill it. A one-unit global configuration is rejected
because it cannot fund both peer selection and progress. Bootstrap critical
counts are accumulated during discovery rather than rescanning Pending at 3J.
Tree lookup and per-object native property/vector costs additionally retain
their existing finite cardinality bounds; this is not wall-clock preemption.

The ordered peer cursor rotates after each slice. Detached cleanup consumes the
same allowance. A peer's READY wait at 3J is not counted as waiting for planning
service. The final maximum measured planning-service interval is 16 ticks at
500 peers and seven at 200 (15 and six intervening ticks without service).
Fair selection is not a promise of sufficiently low wall-clock latency.

Credits conservatively reserve input vectors, private sets/maps/frontiers,
pending queues, dependency/referrer entries, old projection state awaiting
disposal, and bounded group scratch. They are not exact live entry or byte counts:
released/reused scratch remains charged until its continuation is disposed.
Variable reference/group vectors are charged before growth, with bounded element
types; no publication payload is copied into the frontier. Container capacity and
allocator overhead are not reported as exact bytes. There is no preallocated
maximum-peers × maximum-world-objects matrix. At a ceiling, planning fails closed
through the existing structured peer resource-failure path; it cannot silently
drop a lifecycle operation or wait forever while all peers hold the memory limit.

### Invalidation and lifetime

Catalog identity/ancestry/hard-reference changes advance `DependencyCursor`.
Identity/ancestry and native hard **or soft** reference changes advance
`PlanningCursor`. Ordinary scalar changes advance neither planning revision.
Accepted Known/ancestry/reference changes advance a peer's saturating revision;
revision exhaustion fails closed. Each service slice and READY submission checks
the full selection lease and revisions. A complete but stale batch cannot enter
3J. Selection lease identity is an immutable input version, never object identity.

Peer disconnect cancels the coroutine borrowing that peer before erasing its node;
only owned scratch enters detached disposal. Peer generation and ObjectId scope/
generation are preserved. Eviction/reacquisition cannot bind old work to fresh
identities. Accepted map iterators are guarded by the accepted revision. Reference
vectors are indexed afresh after every yield, including when same-value journal
acceptance replaces their storage without changing semantic revision. Schema
property pointers refer only to frozen metadata, not Instances or Lua states.

An important integration regression was measured and corrected: a destroyed
required Character can still appear in the last evaluated 3E selection while
that peer awaits 3E's own turn. The planner discards that private projection and
parks until a different selection lease arrives; it neither disconnects a healthy
peer nor repeatedly rebuilds obsolete input. A refreshed 3E selection wakes it.
The benchmark now detects disconnects during initial drain instead of trusting
cumulative Ready counts. The original failed traces remain retained.

### Journal interaction and scope limits

A READY structural backlog can consume the operation cap before journal readers
run. The existing rotated journal pass now uses any remaining **journal** allowance
to skip only a leading prefix of non-replicated property records, with zero
operations. It stops before any replicated or lifecycle record. Acceptance,
retention, read caps and the global 3J operation cap remain unchanged.

This does not bound every runtime phase: 3E spatial queries, journal/catalog work,
post-selection encoding/materialization, diagnostic full-Pending snapshots and
client preflight retain their own contracts. Nor does successful finite-load
drain establish sustained overload capacity. Healthy service, official reliable
transport, client preflight, overload/recovery, startup journal margin, security
and current-source CI remain open. Convergence latency and the worsened final
healthy recipient gap must not be hidden by improved reload maxima.

### Final native measurement and tradeoff

The unchanged 500-peer reload reduces tick p99/max from 95.95/178.46 to
48.89/56.29 ms and Character/root recipient max gap from 1,147.77 to 459.61 ms.
Charged planning never exceeds 65,536/tick; reload planner CPU p99/max is
9.374/9.745 ms. Maximum reconciliation examinations fall from 138,752 to 56,007;
Desired referrer visits fall from 536,510 to 31,562 per tick. All peers converge,
but maximum convergence grows from 3.233 s / 33 ticks to 4.743 s / 158 ticks.
The complete planning scope is broader than the old fixup-only timing scope.

The healthy 200-peer final Control/Local/Node load tick p99 is 4.03/35.71/39.07 ms.
Local/Node Character throughput loss is 10.15%/11.23%, and recipient max gaps
**worsen** from 395.76/402.02 to 686.79/703.88 ms. The final 500-peer load gap also
worsens to 905.92 ms. Existing health gates still fail. The correction is retained
for its explicit resource boundary and primary reload-tail benefit, not claimed
as a uniformly beneficial latency optimization or readiness closure.

Maximum global/per-continuation reservations are 4,574,325/8,635 credits, below
the enforced ceilings. Journal capacity is unchanged at 16,384; observed minimum
startup margin is 170 entries versus 26 previously, still not a proven production
envelope. Measured load/reload margin is 10,249, eviction 15,340, with zero
retention failures. The validation ledger distinguishes these phase-local
measurements from startup-cumulative high-water counters.

Remaining recurring server time is dominated by 3E query/hysteresis and
post-selection publication; saturated 3J batches plus client/observer processing
still produce expensive consecutive iterations. This does not reopen the settled
static physics, typed Character inspection, KI-007 or late-send corrections.
Official reliable transport and client whole-world preflight are separate,
unmodified blockers. Recipient p95/p99 remain not measured.

## Previous slice: remaining referrer/discovery attribution (2026-09-11)

**B — FOUNDATION 3L PARTIALLY READY.** KI-007 remains the correctness baseline.
The new phase decomposition attributes the indexed reload tail before selecting
another correction. No general discovery frontier or new planning authority is
introduced.

### Measured amplification and the retained narrow correction

Diagnostic `referrer-attribution-v2` reproduces a 227.578-ms reload maximum.
At tick 1115, 496 planning calls perform 536,510 Desired referrer visits and
535,950 accepted-object visits. Selected and accepted operations are both 4,016.
The Desired pass takes 138.570 ms, of which only 5.103 ms is indexed reference
cost construction; the 133.467-ms exclusive remainder is primarily object
traversal, Known/catalog/accepted-map lookups and parent inspection. It finds
**zero parent changes**, while 49,600 reference-cost candidates contain **zero
pending Enter targets**. A second Desired pass spends 31.194 ms restoring
references and finds **zero matching Enter targets**. Accepted-edge inspection
takes 6.628 ms and finds the 496 required old-edge removal dependencies. KI-007
still requires that last work; 492 clears are emitted, with departing referrers
accounting for the difference. These are sibling/nested timings, not additive
causal throughput percentages.

Tick 1109 owns the separate discovery peak: 64 peers, 69,280 Desired examinations
(all already Known), 69,472 Known examinations and 192 inserted transitions.
Dependency preparation takes 0.522 ms and the subsequent walk 13.516 ms, with
66,376 seeds, 72,580 edges and 69,454 duplicate identity visits. The 138,752
reconciliation examinations are not 138,752 selected operations: selected and
accepted work that tick is 8,192, including previously pending/journal work.

The retained correction is a call-local ordered lookup join. Desired is
already a sorted unique full-ObjectId set. Catalog and accepted ancestry are
ordered maps. Restarting a tree-root search for each successive referrer is not
needed: `detail::FindPlanningObject` retains the current iterator within that
one synchronous pass. It advances at most eight nodes per lookup, then uses
`lower_bound` for a sparse gap. Thus a sparse selection cannot accidentally
walk the whole unrelated catalog. Exact key equality, including generation,
still determines success. The same join is used for catalog lookup in the
restoration pass. No semantic check, candidate, ordering or budget is removed.

This is **not** a cross-tick discovery cursor. Iterators exist only inside each
non-mutating planning loop after catalog refresh. No iterator survives catalog
replacement, retirement, peer disconnect, acceptance or the next call. There is
no invalidation subscription, dirty bit, copied future-work set or heap allocation.
The maximum consecutive linear probes is a locality bound, not a new 3J work
allowance and not a proof that complete planning is bounded per tick. Eight is
a search fallback threshold, not a semantic service budget.

With identical diagnostic scopes, `referrer-join-v1` reduces that tick's Desired
pass to 105.575 ms (100.758 ms exclusive), restoration to 16.227 ms and combined
fixup work from 176.494 to 128.261 ms. The reload tick maximum falls from 227.578
to 178.456 ms. All three referrer-visit counts, the 99,692 property examinations,
492 fixups and 4,016 accepted operations remain unchanged. The causal correction
therefore removes repeated search cost, not required reference semantics. The
remaining 100.758-ms exclusive pass is still not individually decomposed into
Known lookup, map iteration and parent inspection; do not attribute all of it to
one container without another measurement. General planning remains the owner.

The unchanged reload converges in 3,233.36 ms / 33 ticks, versus indexed baseline
3,312.04 ms / 33 ticks. Its Character/root recipient gap remains 1,147.77 ms,
versus 1,200.67 ms: a sequence of expensive ticks still delays service. This is
a useful narrow CPU reduction, not a non-starvation closure. The new helper has
two simultaneously live map iterators plus a local advance count, no retained
entries and no heap allocation. Sparse/dense and missing-generation reference
tests compare it to independent tree lookup. Existing lifecycle tests exercise
the complete coordinator, including KI-007 and fresh identities.

### General continuation decision

KI-007 fixes which references removal must clear; it does not make incomplete
Pending/reverse-Leave discovery safe. `CollectGroup` still treats an absent
Pending dependency as absent work, and removal still requires complete accepted
reference knowledge. A safe cross-tick continuation needs an explicit
revision-validated group-completeness boundary, bounded scratch, critical-work
fairness and liveness proof. Adding an outer examination cap or saving one
iterator without that design would expose invalid prefixes or turn ordinary
incompleteness into peer failure. That larger transaction/scheduling change is
not silently installed by this narrow lookup correction. Final validation and
before/after measurements are recorded in the ledger.

## Previous slice: KI-007 accepted-reference removal groups (2026-09-11)

**B — FOUNDATION 3L PARTIALLY READY.** The KI-007 correctness gate passed before
restoring the measured reference-property index. The previous diagnostic
checkpoint and its failed regression remain historical evidence below. General
planning, client preflight cost and official transport remain separate blockers.

### Ordering contract and correction

The old planner compared the current server target B with Leaving, overlooking
the accepted client target A. `PrepareIncremental` could correctly clear that
edge through journal service, but `ProduceRelevanceFrame` could remove A first.
`ReplicaApplier` correctly rejected the resulting dangling reference during
snapshot preflight. The fix belongs in server group construction; client
validation and incremental transaction semantics remain unchanged.

`CaptureAcceptedParents` now prepares accepted native reference targets alongside
the existing accepted ancestry record. It reads actual Publish values (including
peer-specific nil patches), native property updates and Reparent operations from
the exact prepared frame. Only scheduler acceptance installs the replacement
record. This is a description of accepted replica state, not another semantic
reference graph: DataModel and frozen native property definitions remain the
source of current values and policy. Scalar updates do not change this metadata.

`ProduceRelevanceFrame` derives call-local removal requirements from those old
targets, including referrers that are Known but no longer Desired. Each surviving
referrer receives a current peer-valid replacement or nullable nil before the
old target is removed. An unavailable hard replacement defers removal until its
existing Enter prerequisite progresses. Accepted hard referrers also leaving join
the existing dependency group. Soft edges do not expand hard dependency closure;
they may be cleared while the referrer's own Leave waits. No wire or client
preflight rule changes.

A property clear/replacement costs one ordinary 3J operation, as does its target
removal. Four required property updates plus one removal need five operations:
an allowance of four emits no prefix; a quantum of four invokes the existing
oversized-group failure. Byte-limit retries reconstruct complete groups without
changing Known. Same-frame Enter restoration retains its existing charged work;
overlapping clear/restoration can conservatively reserve two updates. This does
not establish a new general planning budget or change the 512/8,192 defaults.

### Ownership, invalidation and bounded representation

Each existing accepted object record adds one vector of `(frozen native property
pointer, full target ObjectId)` pairs. It stores only currently accepted non-nil
references, never Instances, strings, payload history, peer pointers or callbacks.
There can be no more entries than the object's validated native properties
(at most 1,024). Records exist only for accepted Known objects: do not mistake
the 65,536 Desired cap for an identical instantaneous Known cap while old Leaves
remain pending. In successful bounded 3J reconciliation, Known is contained in
Desired union pending Leaves, giving the conservative 131,072-record bound from
the two existing 65,536 per-peer caps. Empty vectors allocate no reference
entries. Capacity may remain
until that object's accepted lifetime ends; it cannot grow with reload history.
Preparation copies only affected object records; acceptance moves allocated
vectors and merges prepared nodes. Leave, destroy, disconnect and Stop release
the owning records. A per-peer byte total is updated at the same commit points,
so reporting reference storage does not add an all-object diagnostic scan.

Destroy/recreate uses full generations. A prepared frame represents its immutable
prepare-time values even if authority changes before acceptance; subsequent
reconciliation removes obsolete identities normally. Referrer and target lifetime
do not turn the schema pointers into live-object pointers. Parent updates preserve
accepted references; reference updates preserve the Parent-only journal watermark.
Reference-valued Attributes/custom/extension properties remain unsupported by the
existing canonical schema contract, not silently omitted supported edge types.

The call-local removal map contains only accepted edges into pending Leaves;
there is no persistent reverse index or future-work queue. It is cardinality
bounded by accepted reference state, but inspecting that state still scales with
Known objects/reference fields. **General low-latency planning boundedness remains
open.** Three fixed diagnostic counters expose accepted-object visits, accepted
reference visits and removal requirements without retaining identity history.

### Correctness gate and reference-property index

Correctness-only v5 passed targeted MSVC **6/6**, Clang 19 ASan/UBSan/LSan
**7/7 with leak detection**, the separate late-handoff case, and all **90**
physics matrix cases. The unchanged 500-peer fixture converged with exactly one
acquisition/initial admission and the exact 8,192 accepted selection maximum.
Its gameplay health gate still failed. That closes KI-007, not Foundation 3L.

Only then was the `CatalogEntry::ReferenceProperties` index restored. Each
immutable publication owns a compact vector of borrowed property-map node
pointers. Membership requires both a `WireObjectReference` value and canonical
native `ObjectReference` metadata. This avoids scalar and nil traversal in the
first Enter-fixup and restoration passes. Nil values need no Enter restoration;
accepted old-reference metadata still supplies required nil clears on removal.
There is no hand-maintained schema, reverse-reference graph or per-peer index.

The index is built transactionally with the immutable catalog entry. Any payload
replacement, including scalar-only replacement, remaps its borrowed pointers;
this is storage invalidation, **not** dependency-topology invalidation. Reference
changes, create and resnapshot likewise replace entries. Parent/reparent updates
retain existing ancestry semantics. Retirement releases the owning entry;
fresh full ObjectIds cannot reuse an old entry. Target lifecycle and peer
relevance do not edit the index: selection resolves those against current
Catalog/Desired/Known/Entering/Leaving state. Retained prepared publications own
their values but do not retain a separate old index history.

On this native layout, index storage is 24 bytes per catalog entry plus 8 bytes
per allocated pointer, with at most 1,024 property slots per validated object.
Accepted-reference capacity is explicitly capped at 1,024 pairs per accepted
object. The measured 500-peer reload catalog index is **30,872 bytes**; accepted
ancestry/reference storage is **35,212,704 logical bytes**, versus 21,633,320
before KI-007. That approximately 12.95 MiB increase is the correctness metadata,
not index cost. Allocator overhead is not included. No new persistent candidate
queue is introduced, and these finite cardinality bounds are not a proof of a
small per-tick CPU bound or of complete pipeline overload safety.

The indexed reload performs **99,692** property examinations / **170.473 ms**
combined fixup CPU at its worst planning tick, versus **8,517,714 / 433.633 ms**
originally. Tick maximum is **221.427 ms**, recipient Character/root maximum
**1,200.67 ms**, convergence **3,312.04 ms / 33 ticks**. The index has measurable
benefit, but 536,510 Desired referrer visits, 535,950 accepted-object visits and
138,752 peak general discovery examinations remain outside a narrow planning
work budget. See the validation ledger for exact-source final validation,
healthy differential and the separate correctness-only counterfactual.

## Previous slice: fixup attribution and correctness stop (2026-09-11)

**B — FOUNDATION 3L PARTIALLY READY.** The reload amplification is reproduced
and separated into its three actual passes. A new lifecycle test exposes an
existing soft-reference removal defect on both the candidate optimization and
the original full-scan path. Optimization work stops at that correctness gate.
No performance correction is retained or claimed validated in this slice.
The accepted late-send behavior and all preceding Foundation changes remain.

### Exact measured owner

`ProduceRelevanceFrame` does complete Desired/Known referrer inspection before
building dependency groups. It constructs Enter/Leave reference costs, pending
Parent moves and typed hard-nil costs. After selection it scans Desired again
to clear references for Leaves, then again to restore references to Enters.
Each pass looks up immutable catalog publications; none is covered by the
selector's `max(64, 4 * allowance)` queue-examination limit. `FindNativeProperty`
searches canonical `AllProperties`; the clearing pass calls it even for scalar
values. Restoration scans even when the Enter set is empty.

In the unchanged 500-peer reload at tick 1116, the new scopes record:

| Pass | Property examinations | CPU, ms |
| --- | ---: | ---: |
| Pre-selection fixup costs | 2,840,760 | 187.899 |
| Pre-removal reference clearing | 2,836,194 | 190.840 |
| Post-Enter reference restoration | 2,840,760 | 54.894 |
| All three disjoint passes | 8,517,714 | 433.633 |

The first pass has 498 planning calls and 538,670 referrer visits. Of its
property examinations, 2,566,860 are scalars, 49,800 are reference values and
224,100 are typed hard nils. These are repeated visits, not unique properties.
Only 494 fixup operations are emitted. No byte-limit retry occurs. Full tick
maximum is 488.404 ms and the 12-tick Character/root recipient window remains
1,524.95 ms. This corroborates the earlier 478.171-ms / 1,508.28-ms failure.
The separate discovery peak remains 138,752; only 4,326 discovery examinations
occur on the worst CPU tick. Reducing discovery's peak alone misses this owner.

**Measured:** the named CPU/counts and the first-pass scalar share (90.36%).
**Inferred from source and repetition:** scalar property inspection and repeated
whole-peer fixup reconstruction are avoidable amplification candidates.
**Not measured:** distinct referrer/property identities, exact overlap between
peer sets, and a causal percentage of throughput loss. Busy-time shares must
not be substituted for a measured before/after correction.

### Candidate assessed, then withdrawn

A private catalog-entry index was built from each immutable publication's
reference-valued and nil-valued native property entries. It filtered the three
passes without caching hard/soft policy: canonical schema metadata remained
consulted at use. It retained no Instance/peer pointer, future transition list,
or second dependency state. Scalar/reference/Parent changes rebuilt the entry
through existing publication replacement. The candidate was finite per existing
catalog entry (at most 1,024 borrowed value-node pointers), not a global planning
budget. It did not solve whole-peer scanning or general catalog overload.

The candidate compiled after correcting a diagnostic-field forwarding error,
but the new lifecycle test failed. Reinstating the original three full-property
loops reproduced the same semantic failure. The index, its metric, and its
forwarding were then removed. Source manifests and patches preserve the trial;
there is **no measured after-performance result and no retained cache**.

### Correctness gate: accepted soft edge versus current value

The minimal regression is `TestSoftReferenceReplacementBeforeLeave`:

1. The client has accepted a Character whose soft RootPart reference targets A.
2. The server destroys A, creates fresh B and changes RootPart to B.
3. B is not Desired/Known for this peer; the Character remains Desired.
4. Pending structural removal is serviced before ordinary property journal work.

The current clearing predicate tests whether the **current authoritative**
reference target is Leaving. It sees B, not A, and emits no clearing update.
The client candidate still contains the previously accepted edge to A when A
is removed. Its unchanged preflight correctly rejects that candidate with
`Snapshot reference is outside the receiving scope`.

This is a server fixup-completeness/ordering defect, **not** a justification to
weaken client preflight or a stale ObjectId resolving to B. Current publication
state does not by itself establish which old edge the client still observes.
Ordinary journal production already substitutes nil for a soft target that is
not Known; relying on that path winning an incidental service race is not a
complete structural-removal guarantee. The minimal test retains both service
orders so the next correction can prove independence from that race.

Before resuming optimization, establish a bounded proof for clearing an accepted
old soft edge: either sufficient accepted-reference metadata, or conservative
current-reference fixups with complete 3J cost/dependency accounting. Do not
silently add metadata or broad nil updates without bounds and tests: conservative
updates can enlarge atomic groups and change which legal workloads fit a quantum.
No chosen implementation or claim that a wire change is necessary follows here.

### Retained scope and remaining bounds

This slice retains only two fixed timing scopes, eleven saturating counters,
the regression, analysis helpers and documentation. No queue/frontier or new
semantic state remains. Existing 512/8,192 per-peer/global selection, group
completion, acceptance-only Known, relevance authority, send allowances and wire
semantics are unchanged. **General planning remains unclosed:** complete Desired
inspection, up to 1,024 properties per publication, dependency edges and repeated
per-peer service are not charged to a new per-tick planning budget. Existing
cardinality limits are not a defensible low-latency service guarantee.

The validation ledger distinguishes the new failing correctness gate from prior
green evidence. Client whole-world preflight (~89–101 ms for one update at 8,193
replicas), historical official reliable Remote delay (~3.8 s), overload, startup
journal margin, current-source security and CI remain independent open gates.
No commit, push or Foundation 3M work is performed.

## Previous slice: reviewed owners and late transport handoff (2026-09-11)

**Foundation 3L remains Partially Ready.** This slice measures three independent
paths, not a general discovery-frontier implementation. Local uncommitted source
at published base `a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1` remains authoritative.
The preceding static-physics, typed Character and bounded retirement corrections
remain intact. No secondary source, 3J limit, wire, content limit or authority
changes; no commit, push or 3M work.

### Local-source verification before behavior changes

| Published review mechanism | Exact local classification and finding |
| --- | --- |
| Global catalog cursor invalidation | PARTIALLY PRESENT: a separate dependency cursor already excludes ordinary scalar/attribute/soft-reference payload changes. Actual topology still advances one global revision. |
| Full Relevant rebuild on acceptance | ALREADY CHANGED LOCALLY: `ApplyPreparedCommit` applies accepted Enter/Leave deltas. `RefreshRelevantObjects` rebuilds on a dependency-plan miss, not every acceptance. |
| Unconditional incremental peer-view copy | ALREADY CHANGED LOCALLY: `ProduceIncremental` constructs its candidate view lazily. Actual structural legacy paths and standalone `PublishObjects` can still copy state. |
| Remote materialization cleanup scan | PRESENT: after structural publication, `ApplyServerRemoteMaterialization` scans the peer's tracked Remote materialization collection against Known. |
| Unaccounted dependency/fixup traversal | PRESENT: closure, reverse Leave dependency index and both fixup passes surround selection; the 8,192 cap does not count their inner edges/properties. |
| Duplicate encoding | PRESENT: coordinator size/validation encoding and transport-submission encoding both remain. Byte-limit retry can repeat planning. |
| Already-flushed suppression | PRESENT before this correction: early/structural flush could suppress the final opportunity after Character authority and Remote pumping. |
| Whole-world incremental preflight | PARTIALLY PRESENT: changed native values and non-property operations copy/index/preflight the candidate world. All-identical native-property frames skip LoadSnapshot, but still copy/index. |
| Per-removal receiver scan | PRESENT: `RemoveReplicaObject` destroys the object then scans the receiver mapping to remove destroyed descendants. |

### Measured discrimination: recurring server work

The diagnostic-before canonical 401-load-tick runs record only **200 dependency
plan misses for 200 streamed peers**, with both selection and topology changed
on those same misses (the reason counters overlap). Control has zero misses.
Local/Node retain 13,320/13,264 plan hits. One payload-only catalog batch occurs
in each case without a topology change; streaming adds two topology batches.
No lazy peer-view identities are copied and no encoding retry occurs in these
load phases. These measurements contradict a recurring scalar-invalidation or
whole-view-copy explanation for this workload; they do not prove global
topology invalidation optimal under unrelated structural mutations.

Server relevance totals are 295.38/1,959.50/1,949.33 ms (Control/Local/Node), of
which `RelevanceQuery` accounts for 253.37/822.03/816.63 ms. Local closure,
pending discovery, full Relevant rebuild and first-pass fixup scopes total
31.15, 25.96, 6.42 and 26.15 ms respectively. These nested scopes must not be
added to their parents. Remaining recurring relevance work includes per-peer
current-root checks, exact spatial predicates and derived selection containers;
those individual predicates are **source-traced, not independently timed**.
Local Remote reconciliation totals 26.82 ms and duplicate structural encoding
114.48 ms. They remain concrete costs, but are not the largest measured owner.

The complete path remains ready peer -> bounded peer relevance opportunity ->
3E spatial/current-root evaluation -> `RecordDesiredState` -> dependency plan /
Desired-Known pending discovery -> dependency/fixup preparation -> 3J selection
-> prepared frame encoding -> scheduler admission -> acceptance-only Known /
journal advancement -> Remote materialization reconciliation -> publication.
Peer evaluation count and retirement work are bounded; whole-peer root checks,
closure, pending discovery, fixup properties and encode work are **not newly
bounded**. Maximum Desired/Known examinations remain 68,258 at 200 peers and
106,176 at 500 peers. Installing a partial Pending prefix without proving
dependency completeness would weaken the established 3J contract. No frontier
or another semantic truth set is introduced to conceal this gap.

### Narrow correction: shared step allowance and late service

`GameSession` keeps its existing relevance update, early service, structural
production and structural flush, then Character graph synchronization/authority, Remote pump and
final service order. Its private per-peer `SessionSendAllowance` now shares one
negotiated byte/message allowance across those existing service points. Reset
occurs once per `GameSession::Step`, not on each flush. Successful transport
submissions consume the allowance; scheduler acceptance is not charged as Send.
After an earlier flush fully drains, late-produced gameplay can use the residual
allowance in that same step. No queue, new transport lane or semantic priority
is added; the existing scheduler still performs all selection and Send calls.

An earlier budget-limited/backpressured/error flush stops further attempts for
that peer in the same step. This avoids retry spinning and does not let newly
queued gameplay bypass unsent structural prerequisites. The scheduler's existing
minimum valid quantum (enough bytes for any one negotiated legal message and a
nonzero message allowance) is retained: a smaller residual is intentionally
unused until the next step. This is a conservative capacity tradeoff, not a
promise of same-step delivery under backlog or exhausted capacity.

Bounds: at most the three existing service sites per peer/step; aggregate
successful bytes/messages at most that peer's negotiated limits; fixed private
allowance fields (two counters and one stop flag), no heap ownership or history.
The allowance contains no peer/ObjectId pointer and disappears with its peer.
Existing scheduler/global queue bounds and the separate global 8,192 structural
selection cap are unchanged. This closes a missed local handoff opportunity,
**not** official reliable backend head-of-line blocking or recipient completion.

### Client stop gate: preserve validation, define the required seam

A one-property native Name update with 65/513/2,049/8,193 existing identities
copies, indexes and preflights all those identities. At 8,193 the ten before
samples take 87.30--94.65 ms, with 70.49--77.85 ms in validation LoadSnapshot.
The source also rebuilds the receiver reverse map; removal scans the mapping.
Object counts are measured input identities, not allocator-hook counts; copied
bytes, allocation counts and isolated temporary-world destruction CPU remain
**not measured**. The residual outside named phases is not all called cleanup.

No client preflight is removed. Localizing it requires an accepted transaction
design: resolve touched identities and dependent references against the existing
validated semantic state; validate class/property/Parent/reference effects;
preflight native setter and lifecycle constraints without publishing partial
Instances; then commit with the same deferred-signal/journal-suppression and
rejection/rollback contract. Cross-object setter effects, destruction closure,
hard references and failure after live mutation must be covered before replacing
whole-world LoadSnapshot. A property-name fast path alone does not establish
that proof. Implementation stops at this client design gate for this slice.

### Remaining owning subsystems

Replication relevance and whole-peer dependency/fixup planning still own server
CPU/work isolation. Player replica transaction/preflight owns fixed-increment
world-size scaling. Official transport/backend and client service still own the
historical ~3.8-second Remote gap, independently of this local handoff test.
The validation ledger records measurements, residuals and unexecuted gates;
loopback improvements cannot close official reliable transport, overload,
current-source security or CI. A general frontier must follow a dependency-safe
continuation design, not precede it.

The after 500-peer reload strengthens that warning: selection-only invalidation
adds 499 dependency rebuilds, discovery reaches **138,752** examinations, and one
478.171 ms server tick is dominated by 240.773 ms exclusive structural-selection
preparation plus 185.967 ms fixup planning. Its 12-tick recipient window is
1.508 seconds. This is a measured remaining replication-planning failure, not a
closed non-starvation envelope. The shorter RPC-completion-driven phases change
cadence/spatial alignment, so that run is not used to claim a pure causal
regression percentage from the late-send helper. Its bad tail remains a blocker.

## Previous slice: bounded retired-catalog reclamation (2026-09-10)

This measured correction follows the retired-cleanup owner identified below.
It **does not complete general bounded Desired/Known/dependency/fixup planning**.
The previous typed Character and settled-static physics changes are preserved.
Source remains local and uncommitted; no push, secondary-repository modification,
wire change, selection-budget change, content-limit change, or 3M work is included.

### Measured owner and scope

The new `CatalogRetirement` scope isolates the old nested loop inside
`RefreshCatalog`: for every refresh/batch, every retired identity searches peers
until a Known owner is found. In the unchanged 500-peer eviction it performed
1,945,801 retired-object examinations and **143,447,770 peer-Known probes**.
Retirement alone consumed 2,045.601 ms, p99/max 85.512/184.170 ms, versus
108.047 ms for dependency closure, 146.007 ms for candidate discovery,
57.896 ms for the reverse Leave index and 222.040 ms for fixup planning.
These are measured inclusive scopes, not additive causal percentages.
The earlier, less-instrumented catalog maximum was 140.002 ms. The extra probe
counters are intrusive; preserve both baselines, not just the slower one.

The 68,258/106,176 Desired/Known examinations remain legitimate complete
reconciliation, not duplicate candidate insertion. The exact source path and
dependency-completeness warning in the preceding checkpoint below still apply.
No partial Pending prefix has been installed.

### Implemented ownership proof

`ReplicationCoordinator::CatalogEntry` wraps the unchanged immutable 3I template
with one private `shared_ptr<const ObjectId>` retention lease. The lease contains
only the full identity, not an Instance, peer, session, provider, or Lua pointer.
It is ownership metadata, not a semantic relevance or acceptance flag.

`CaptureAcceptedParents` retains that lease in the existing prepared ancestry
record before scheduler acceptance. `ApplyAcceptedParents` moves/merges those
records into the existing accepted-ancestry map. Consequently every Known
identity has a lease, and a prepared Enter also protects its catalog identity.
Destroy after prepare cannot let maintenance discard a template before a late
accepted Enter receives its subsequent ordered Leave. Rejection/disconnect drops
prepared ownership; accepted Leave/destroy removes the accepted record. Copies
made by the existing standalone transactional helpers conservatively retain it.

Scalar, Parent and reference publication revisions reuse the same identity lease.
A catalog resnapshot preserves it for matching full ObjectIds. Destroy/recreate
uses a different identity and a different lease. Immutable publication fields,
3E selections, 3J Pending/Known and the wire never contain the lease.

`ReclaimRetiredTemplates` may erase a retired catalog identity only when the
catalog is its lease's sole owner. This replaces the cross-peer search with one
ownership check. It does not select or accept any structural operation.

### Work, continuation, fairness and memory

| Property | Implemented contract |
| --- | --- |
| Work unit | One retired identity: ordered lookup, lease test, optional template/tombstone removal |
| Default and hard tick cap | 4,096 examinations, shared across every maintenance call in that coordinator/tick |
| Service point | `GameSession::Step` on Main; direct coordinator `RecordDesiredState` shares the same budget |
| Refresh behavior | Catalog refresh no longer performs the retired-object/peer search |
| Refill | Only a strictly newer trusted simulation tick; repeated/older ticks cannot refill |
| Continuation | One full-ObjectId `upper_bound` cursor, tick and remaining count; 24 logical bytes on the tested 64-bit targets |
| Fairness | Ordered rotation with wrap; a retained identity never stops later eligible identities |
| Quiet path | Empty retired set returns without allocation or peer/graph scan |
| Cancellation | No iterator/raw object pointer survives a call; removed cursor keys are safe; leases follow existing prepared/accepted ownership |
| Retired-set ceiling | 1,048,576 identities, matching the existing global Pending cardinality ceiling, without raising that ceiling |
| Ceiling behavior | Before committing the catalog read, fail with `Structural retired catalog limit exceeded`; existing replication resource-failure/terminal-peer handling applies; no silent truncation |

For a fixed retired set of R identities, a complete rotation takes at most
ceil(R/4,096) serviced ticks. The 4,128-object test reaches every unowned identity
within two ticks despite Known owners at both ends. This is **retirement fairness**,
not a new peer-discovery fairness result. Sustained arbitrary churn/overload and
the million-entry limit boundary have not been benchmarked. At the measured
512-object unit, 131,584 eviction examinations drain all 512 templates; the
maximum tick is exactly 4,096. No candidate queue or copied future transition
set is introduced. Cold resnapshot and coordinator/peer teardown remain their
existing ownership paths, not certified incremental by this maintenance cap.

The lease adds 16 bytes to each **existing** accepted-ancestry record and 24
logical bytes per catalog identity (shared-pointer member plus ObjectId payload),
excluding shared-pointer control blocks and allocator bookkeeping. At final
500-peer load this is 8,677,280 added ancestry bytes and 30,072 catalog-retention
logical bytes. The existing ancestry metric now includes the larger record;
`CatalogRetentionLogicalBytes` reports the catalog increment. There is no new
dense peer-by-world frontier, but the per-Known metadata cost is real. Persistent
retired identities remain finite; this does not certify total pipeline overload
or the whole-world catalog byte envelope. No RSS/leak conclusion follows from
these logical byte counts.

### Result and remaining planning design

500-peer retirement CPU falls to **8.569 ms total, 0.301/0.604 ms p99/max**;
peer-Known probes become zero. Eviction convergence stays 32 ticks while wall
time falls from the counter-instrumented 3,493.52 ms to 1,456.53 ms; the recipient
Character/root gap falls from 1,269.08 ms to 449.639 ms. Relative to the previous
checkpoint's 2,951.69 ms and 1,054.79-ms gap, the improvement also remains large.
Load convergence remains 33 ticks, 2,690.48 ms. The healthy Local/Node load
still has 39.901/42.373-ms tick p99 and 621.101/680.095-ms recipient gaps.

**Not implemented:** a global examination/edge/fixup budget for general plans.
The current load owner remains recurring 3E query/hysteresis work plus complete
coordinator reconciliation; retirement has no load work after drain. The
dependency-complete continuation design must treat closure, pending reconciliation,
accepted reverse-Leave information and fixup costs as a coherent preparation
transaction. A peer/group may reach 3J only after those proofs are complete;
partial discovery must not masquerade as absence of a prerequisite. Rotating
charged slices need full peer/ObjectId validation, bounded scratch reclamation
and a progress proof under changing selections/dependencies. Blanket restart on
every global catalog revision is insufficient: continuous unrelated admissions
could prevent any multi-tick plan from completing. This is a concrete remaining
design/liveness requirement, **not a claim that continuation requires a new
authority or a wire change**, and not a completed staging implementation.

The [validation ledger](ContentAvailabilityFoundation3L_3Validation.md) separates
this successful retirement bound from the unclosed general-discovery objective,
journal startup margin, official transport/client, overload, security and CI gates.

## Previous slice: pre-3J attribution and typed Character inspection (2026-09-10)

This is a **partial correction, not completed pre-3J work isolation**. Foundation
3L remains B; no commit, push, wire change or 3M work is included. The settled
static physics correction below remains intact. Exact measurements and remaining
unproved gates are in the [validation ledger](ContentAvailabilityFoundation3L_3Validation.md).

### Actual upstream path

`GameSession::Step` updates trusted owner focus, then `ReplicationRelevance::Update`
refreshes dirty 3K/3H projections and rotates at most 64 peer evaluations. For
each evaluated peer, `UpdatePeer` queries spatial candidates, checks the previous
roots against leave predicates and query candidates against enter predicates.
`BuildSelection` visits global identities and all members of relevant roots when
the semantic selection requires rebuilding. These are indexed memberships, not
a fresh DataModel descendant walk for every peer.

`RecordDesiredState` then refreshes the shared catalog. A changed selection or
dependency cursor rebuilds two closures (Desired and required bootstrap), scans
pending transitions, scans the new Desired set, copies/sorts/scans Known, and
builds the reverse Leave dependency index. Only **after all of that** does 3J
select dependency groups under its per-peer/global budgets. Fixup planning also
scans desired known referrers before the selector's queue examination cap.
Scheduler acceptance commits Known, ancestry and journal watermarks. Remote
argument registration follows accepted frames, including a scan of its prior
known-argument registry. Character graph synchronization precedes Character
authority/publication and final Remote pumping.

Potential multipliers remain explicit:

- evaluated peers times queried roots, hysteresis checks and selected members;
- evaluated peers times complete dependency-closed Desired and Known;
- pending Leave identities times reverse dependency discovery;
- structural service opportunities times desired known reference-fixup scans;
- catalog refresh calls times retired identities times peer Known checks;
- accepted structural frames times prior Remote argument registrations.

Before this correction, graph synchronization additionally examined every
registered Character for every ready peer on every tick. It performed registry
lookup and RootPart inspection before the spatial relevance predicate. The
200/50 load produced 4,010,000 visits and no Character membership changes across
401 ticks; 3,753,360 visits did not qualify for materialization.

### What 68,258 means — MEASURED

At the reproduced Local burst, 64 dependency-plan refreshes inspect 50,513
Desired identities and 17,745 Known identities. No pending entries need a
cancellation scan on that tick. They create 32,768 distinct pending transitions,
with zero duplicate insertion attempts. The remaining 35,490 examinations are
the already-known intersection visited from both directions. Thus this is not a
68,258-element duplicate candidate queue; it is synchronous whole-set discovery
running four times ahead of the 8,192 selection capacity. Across the load phase,
213,030 examinations / 102,400 new candidates = 2.0804 examinations/candidate.
The 500-peer burst reaches 106,176 examinations as global Player/Character-shell
dependencies enlarge each peer's sets. A count can be legitimate and still need
a scheduling boundary.

### Retained correction: derived typed inspection candidates

3E now builds an ordered Character-root subset while it already walks the
current root membership to build a selection. `GameSession::SynchronizeServerGraph`
inspects that subset rather than all server Characters. This is an inspection
projection, not a second relevance set, a pending queue or an acceptance system.
Every candidate still passes current server registration, generation-safe lookup,
live RootPart, `IsRuntimeRelevant`, and accepted shell/RootPart Known checks.
The existing all-removals-before-additions order, materialization epochs and
control reconciliation remain unchanged.

No new dirty boolean, revision counter or asynchronous event queue was invented.
Creation/removal/reparenting use the existing DataModel membership subscriptions
and coalesced 3E WorldRevision. Owner changes use existing CriticalDirty handling.
The existing unchanged-selection path reuses the typed subset only under the same
membership/owner conditions as the complete selection. Movement still executes
current enter/leave predicates. RootPart changes are inspected live, even when
the subset is unchanged. Destroyed generations can remain temporarily in the
last evaluated subset, but cannot pass live registration/relevance; peer removal
destroys its subset. The span returned to GameSession is borrowed only within
the owning Main-thread service point, never retained by a callback or worker.

Storage is a sparse vector per peer, no dense maximum-peer/object matrix. Entry
count is bounded by the existing root/Desired ceilings. The diagnostic reports
actual retained vector-capacity bytes separately; vector objects and the one
typed-root flag per spatial entry are additional fixed/container metadata.
There is no retained future transition list. Tests compare the subset to an
independent semantic reference during bounded peer evaluation, movement,
disconnect/reconnect, destruction/recreation and subtree removal/return.

This eliminates the all-world Character polling multiplier. It does **not** make
the complete graph service event-driven: currently relevant Characters are still
checked each tick, and transient desired-materialization sets still allocate.
A dense relevant-Character workload does not gain a new examination guarantee.

Final `character-candidates-v2` load graph visits fall from 4,010,000 to 256,640
at 200 peers; Local graph time falls from 1,455.09 to 262.08 ms. Control/Local/Node
tick p99 becomes 3.989/38.738/38.553 ms, and Local/Node throughput loss is
9.00%/8.89%. Recipient maxima remain 630.103/622.587 ms. RPC/action maxima do
not uniformly improve. General discovery still reaches 68,258 examinations;
500-peer load still reaches 106,176 and eviction still has a 1,054.79-ms
Character/root gap. This is measured graph-work reduction, not work-isolation
closure. Final MSVC 6/6 and Clang 19 ASan/UBSan/LSan 7/7 targeted tests pass,
as does the 90-case physics matrix; remaining gates stay open.

### Correctness gate for resumable Pending discovery — NOT IMPLEMENTED

A cursor over Desired/Known alone is unsafe with the current selector.
`CollectGroup::Enqueue` treats a dependency absent from Pending as no additional
pending work. An Enter exposed before its unknown parent/hard target is
discovered can therefore omit that prerequisite. Leave selection likewise
depends on a complete reverse-dependent index: absence from a partial index is
not proof that no dependent remains. Fixup accounting also requires complete
current referrer knowledge. Merely adding a per-tick examination counter around
these loops would weaken 3J's dependency guarantee.

This is a proposed-cursor hazard, not a demonstrated current authority bypass:
the current complete discovery path remains unchanged. For an omitted hard
property target, the later `PublishReferencesKnown` guard rejects the produced
frame; a naive frontier would turn ordinary incompleteness into a production
failure rather than bounded defer/progress. Unknown Parent ordering and reverse
Leaves likewise need a complete proof before enabling partial discovery.

The proposed safe boundary must retain identity/revision cursors, not a copied
future transition list, and expose only dependency-complete groups to existing
3J acceptance. Rotating peer slices must charge candidate, edge and fixup work;
stale revisions must invalidate incomplete discovery before selection. The
reverse accepted-dependency proof must be complete (or the affected group must
remain blocked), including hard references and accepted ancestry. Global and
per-peer scratch ceilings, cancellation cleanup, critical progress, service
capacity and reference-model equality must be demonstrated before selecting a
production budget. **No budget value or general liveness guarantee is claimed
from this design assessment.** No partial frontier was installed.

### Journal finding and remaining ownership

The old `StructuralMaximumJournalLagRecords` is a session-cumulative maximum,
not the current streaming consumer lag. New phase-local gauges observe maximum
catalog-minus-peer cursor distance before and after service. Both 200 and 500
fixtures peak at 6,135 records during load/reload and 1,044 during eviction:
the corresponding capacity margins are 10,249 and 15,340 against 16,384. The
16,358 historical maximum already exists before the recorded 500-peer phases.
Its 26-entry startup margin remains a separate bootstrap concern; it is not
evidence that eviction is currently 26 records from overflow.

The cursor advances through accepted publication/coalesced journal consumption,
not network ACKs. These loopback results therefore do not prove anything about
official reliable transport acknowledgements. Before the graph correction the
500-peer eviction also exposes catalog refresh p99/max of 60.037/138.230 ms,
with repeated retired-object/peer Known scans in that owning function. That
work, complete dependency/discovery/fixup planning and recurring relevance
evaluation remain replication-owned investigations, not ContentAvailability
provider work. The official approximately 3.8-second reliable Remote delay,
client service, overload, current security and CI gates remain open independently.

## Previous slice: settled-static physics (`static-world-v3`)

The measured correction is retained; **B — FOUNDATION 3L PARTIALLY READY**.
No commit/push or Foundation 3M work is authorized by this checkpoint.
The exact native manifest is `evidence/static-world-v3-source.json` (44 Engine,
two unchanged Node files). Benchmark
`1082D651CCC94304189DDBEDD11A4D70AF88FEB9676F775369E975E2BF759EF6`
is identical for v1/v2/v3; v2/v3 only correct/extend the physics test. The paired
200-peer results use `static-world-v2` log names. v3 adds explicit dynamic-body
admission/removal counting coverage. No semantic scheduling change was stacked
on the physics correction before measurement.

The [physics adapter contract](PhysicsBackend.md) now permits a fresh empty
step result only when no dynamic body exists and a prior full step has
processed all participating mutations. Native body/shape/proxy activation
remains synchronous. Anchor/collision/touch changes, create/destroy,
constraints, gravity and impulses invalidate this no-work path. Non-touching,
non-colliding anchored Character transforms still reach native query proxies
immediately without needlessly repeating every scenery sensor query.

| Load result | Instrumented control / Local / Node | Corrected control / Local / Node |
| --- | --- | --- |
| Tick p99 ms | 6.572 / 61.340 / 62.262 | 6.798 / 42.230 / 41.360 |
| Tick max ms | 10.365 / 103.939 / 99.554 | 9.614 / 96.870 / 96.580 |
| Accepted Character states/wall-second | 8,208.66 / 4,250.77 / 4,234.95 | 8,208.52 / 7,386.52 / 7,385.51 |
| Loss versus same control | baseline / 48.22% / 48.41% | baseline / 10.01% / 10.03% |
| Recipient Character/root max gap ms | 203.073 / 1,100.640 / 1,203.500 | 202.815 / 695.362 / 659.970 |

Local server native-step total falls from 3,714.34 to 5.1404 ms per load;
client native-step total falls from 3,692.75 to approximately 5.15 ms. Each
side performs one real step after materialization and skips 437 unchanged
static steps. Relevance/dependency/discovery still consumes about 2,052 ms,
graph synchronization 1,536 ms, and 3J/encode/accepted commit 307 ms over 401
Local ticks. Thus the correction recovers throughput without pretending that
pre-3J bursts or all tail latency are solved. No 3J cap, ordering, Known rule,
content limit, wire protocol, transport service precedence or fixture semantic
property changed. Control's 0.226-ms p99 difference is not evidence of a
meaningful overhead claim from one run; wall throughput is unchanged.

The existing fixture gates remain unchanged: tick p95/p99/max at
16.667/33.334/100 ms, recipient Character/root maximum at 250 ms, and existing
Remote/action guards. Both streaming cases still fail. These are investigation
guards, not a newly established all-workload product contract. The final legal
unit stays 512 objects / 1 MiB as a compatibility limit, not a proved safe
end-to-end service envelope. Pre-3J object-work bounds, actual transport
head-of-line behavior, client/overload validation, final security and CI remain
open. The [validation ledger](ContentAvailabilityFoundation3L_3Validation.md)
records measured and missing metrics separately.

## Owner attribution: owner-attribution-v3

This instrumentation-only checkpoint preserves the accepted-membership behavior.
Its source manifest has 43 Engine and two Node files; benchmark SHA-256 is
`2B8D135B9C4FADD7FF151C1FC8B2C7A9C9B36A0F310F0306D8A3E02CBD1B1860`.
Raw logs and `owner-attribution-v3-analysis-v3.json` are under `build-3l3-worker/evidence/`.
`AnalyzeAttribution.ps1` reproduces the disjoint accounting from exclusive
scopes, separately recording pacing and the unscoped fixture remainder.

Important metric correction: `characterStatesPerWallSecond` divides the
**server scheduler-accepted state count** by actual fixture duration. It is not
a count of recipient-applied states. Recipient wall-clock gaps are measured
independently. Control, Local and Node each accept exactly 54,918 states and
4,596,300 state bytes over the same 401 load ticks, with zero scheduler
rejections. Wall durations are 6,690.25 / 12,919.6 / 12,967.8 ms and accepted
rates are 8,208.66 / 4,250.77 / 4,234.95 states/s. The approximately 48% loss
is iteration dilation, not a corresponding rejection/drop of accepted states.
The serialized fixture includes server, one Engine client, 199 protocol
observers and pacing; client CPU delays the next server iteration too.

| Added busy work versus control | Local ms / share | Node ms / share |
| --- | ---: | ---: |
| Recurring server rigid physics | 3,721.75 / 34.19% | 3,727.11 / 34.12% |
| Recurring client rigid physics | 3,702.55 / 34.01% | 3,791.69 / 34.72% |
| Pre-3J relevance/catalog/dependency/discovery | 1,697.33 / 15.59% | 1,691.11 / 15.48% |
| Server graph synchronization | 936.86 / 8.61% | 906.31 / 8.29% |
| 3J selection/encode/accepted commit | 308.93 / 2.84% | 303.27 / 2.77% |
| Protocol-observer work | 246.52 / 2.27% | 245.32 / 2.24% |
| Server incremental journal preparation | 83.18 / 0.76% | 81.72 / 0.75% |
| Server scheduler/backend submission | 7.07 / 0.06% | 5.98 / 0.05% |
| Body/constraint creation, both sides | 3.18 / 0.03% | 3.12 / 0.03% |
| Unscoped fixture remainder | 33.05 / 0.30% | 32.10 / 0.29% |

Remaining measured minor categories and exact values are in the JSON, not
silently attributed to transport. Shares normalize positive added busy time;
they are **not counterfactual causal percentages**. Saved pacing absorbs
4,657.64 / 4,644.66 ms, and extra physics catch-up creates feedback. Proportional
bookkeeping assigns about 32.88 / 33.33 percentage points of total throughput
loss to recurring physics, and 7.52 / 7.49 points to pre-3J work. There is no
unique additive marginal attribution obtainable from these observations.

### Largest owner: recurring sensors, not admission

Local server `b3World_Step` costs 3,714.34 ms across 749 actual steps in 401
ticks (up to four/tick); client backend costs 3,692.75 ms across 750 steps.
More than 99.9% of each backend duration is Box3D's reported sensor phase.
Server creation of 511 bodies costs only 1.0488 ms total. Client creation
includes preflight temporary worlds and live apply, so its 1,032 creates must
not be mislabeled 1,032 resident bodies. No activation staging is justified by
this result. Existing larger/moving/assembly activation limits remain open.

Box3D queries/sorts every enabled sensor each step even when the world is
entirely static. The fixture's scenery is anchored, non-colliding and touching;
Character roots are anchored, non-colliding and non-touching. Repeating the
same static overlaps dominates sustained iteration cost after admission.
The first correction therefore belongs to the physics adapter. It must not
disable touch, change the fixture, skip dynamic simulation, or defer activation.

### Pre-3J and transport are still independent gates

Local pending discovery examines 213,030 candidates, retains 102,400 and rejects
110,630 (2.080 examinations per needed transition); a tick reaches 68,258
examinations and 32,768 retained intentions **before** the 8,192 selector cap.
Dependency closures make 317,450 visits (158,485 unique retained; 158,965
duplicate/invalid), reaching 101,720 visits/tick. Their p99 is zero because
only four ticks rebuild, but maxima are 10.0751 ms closure and 7.9181 ms pending
discovery. They allocate at most 5,682,760 and 4,075,016 requested scalar-new
bytes/tick respectively. These are not bounded object-work cursors yet.
Graph synchronization examines 10,000 Characters/tick and allocates 1,440
scalar-new objects/tick in both control and Local; the larger Known/relevance
containers increase its cost despite identical visit counts.

Loopback Local GCHR, reliable events and RPC responses have zero scheduler
budget/capacity deferrals. Maximum enqueue-to-backend-acceptance ages are
101.392 / 100.932 / 100.925 ms. Semantic producer annotations are native-only
diagnostics: no queue-selection, wire, acceptance or authentication change.
They do not provide packet-send timestamps. Queue-depth percentiles represent
per-tick maxima observed at enqueue; service-age percentiles represent maxima
of **serviced** messages, not oldest still-pending age. Producer inter-service
gaps include idle/application cadence and are not backlogged-starvation claims.
Player/Character/scenery operations may coexist inside a GRPL frame; the
diagnostics do not invent a separate Player lane. Root motion uses GCHR and
provider acquisition is outside the game transport scheduler. Unreliable
RemoteEvent and packet-builder deferrals are not measured by this fixture.
The earlier official same-reliable-lane multi-second gap remains unclosed.

### Instrumentation resource contract

Exclusive durations subtract nested scopes within one capture, not across
independent nested captures. Backend-reported subprofiles have zero exclusive
time and no fabricated start/end timestamp. Counters saturate; allocation-byte
capture measures requested scalar `new` only (not native C/aligned/worker
allocations). Detailed output occurs after the bounded debug fixture, never
per tick/object in production. A sample is now 5,200 bytes: 52 phase records and
13 producer records. The fixture's four retained plus one current 1,201-tick,
two-sided blocks cap sample payload at 62,452,000 bytes, excluding container
overhead. Transport annotations add approximately 16 bytes/queued intent and
208 bytes/connection, bounded by existing queue/connection ceilings; they add
no producer-specific queue. Final instrumentation/security review remains open.

## Preserved checkpoint: accepted-membership-v1

This supersedes the earlier checkpoint summaries, not their retained evidence.
The Engine and Node published baselines remain respectively
`a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1` and
`f4440423c0701ff396f589fd51b6ce41edc63539`. Local uncommitted source is canonical;
there has been no commit, push, reset, or worker-to-local source replacement.
The benchmark binary SHA-256 is
`657360DA4A2D017C85F4EB97B1C4822B5D6362B175AB2146F1BE81CCC994CE46`.
The 41 Engine / two Node runtime-source manifest is
`build-3l3-worker/evidence/accepted-membership-v1-source.json`; the complete
58 Engine / two Node changed/new-file security inventory is separately retained
under `evidence/security-accepted-membership-v1/`. Node production code has not
changed to address Engine scheduling.

### Checkpoint 11: bounded dependency-group scratch

The selector now reuses a call-local group vector and checks its cardinality
before appending another unique dependency. The bound is the lesser of the
configured peer quantum and the existing 4,096-operation frame ceiling.
Oversize dependency groups fail without changing Known. No persistent staging
queue, alternate Desired/Known set, semantic priority, or wire change was added.
The 129-folder regression deliberately chooses the smallest **actual ObjectId**
as parent; allocation-slot reuse makes creation order an invalid substitute.
The earlier incorrect fixture and its failure are retained, not counted as a pass.

Relative to `discovery-profile-v1`, the 200-peer Local group-selection p99/max
falls from 6.211/6.592 to 4.801/5.070 ms. Inclusive scalar-new calls for that phase
fall from 715,800 to 409,600 per load, and maximum per-tick calls from 57,264 to
32,768. The 10,000-object probe falls from 489,941 to 469,950 calls and from
61.013 to 59.088 ms total. Four targeted MSVC suites pass in 18.40 s.
This is a measured allocation/CPU improvement, **not** a bound on all pre-3J work.

### Checkpoint 12: accepted membership deltas

`ApplyPreparedCommit` now updates Relevant only for accepted Enters and Leaves,
instead of rebuilding the complete intersection on every accepted frame.
An Enter adds Relevant only when Desired contains that identity; a Leave erases
it and its metadata. Desired cannot change while a prepared commit is pending.
The semantic Desired-update path still refreshes the intersection; the legacy
native `SetRelevant` path is not silently assigned this new policy-path contract.
Known still changes only through accepted structural work.

The new test compares Relevant with independently computed Desired intersect
Known after each seven-operation acceptance, cancellation and removal. A
property-only frame performs zero **membership work units**. This is not a claim
of zero heap allocations: MSVC still allocates empty map sentinels in preparation.

| Affected phase | Before | After | Qualification |
| --- | ---: | ---: | --- |
| 200-peer Local accepted-commit maximum | 9.133 ms | 3.237 ms | Compared with group-scratch-v2 |
| Same phase maximum scalar-new calls/tick | 55,515 | 24,624 | Inclusive benchmark counter |
| Control property-only commit maximum | 8.122 ms | 0.038 ms | Compared with discovery-profile-v1 |
| Same control tick scalar-new calls | 55,515 | 200 | Empty-container overhead remains |
| 500-peer Local accepted-commit maximum | 43.526 ms | 5.150 ms | Compared with discovery-profile-v1 |
| Same 500-peer maximum scalar-new calls | 286,830 | 24,624 | Not all native allocations |

Semantic tradeoff: none intended; this removes redundant reconstruction of an
already-defined set. Convergence and cancellation remain governed by 3E/3J.
It does not bound Desired reconstruction, pending discovery, fixup scans or physics.

### Exact healthy differential after checkpoint 12

The workload remains 200 peers, 50 active Characters, five deterministic trusted
neighborhoods, staggered five-tick owner input, ten Animator/root-motion tracks,
the same action/Remote schedule and 512-object / 273,032-byte content fixture.
One full Engine client plus 199 protocol observers is not the graphical GNS
Player fixture. The server and that client run sequentially in the benchmark;
wall-clock tick spacing includes both, observer draining and pacing.

| Load-phase metric | Control | Local | Node |
| --- | ---: | ---: | ---: |
| Server tick p50 / p95 / p99 / max, ms | 4.418 / 6.085 / 6.760 / 9.928 | 19.233 / 29.409 / 61.582 / 98.811 | 19.483 / 29.547 / 60.887 / 103.078 |
| Character states / wall-second | 8,209.58 | 4,327.99 | 4,188.38 |
| Throughput delta from control | baseline | -47.28% | -48.98% |
| Recipient Character maximum gap, ms | 201.788 | 1,072.83 | 1,220.62 |
| Recipient root-motion state maximum gap, ms | 201.788 | 1,072.83 | 1,220.62 |
| Lua reliable-event ACK maximum service gap, ms | 20.778 | 123.797 | 103.227 |
| RPC round-trip p50 / p95 / p99 / max, ms | 48.882 / 51.957 / 52.539 / 52.554 | 91.498 / 115.631 / 299.798 / 321.277 | 90.954 / 120.541 / 290.611 / 300.186 |
| Maximum action request-to-observed-result, ms | 53.634 | 148.751 | 115.251 |
| Peer convergence p50 / p95 / p99 / max, ms | 16.881 / 16.884 / 16.884 / 16.884 | 733.962 / 1,218.94 / 1,300.37 / 1,300.37 | 848.701 / 1,336.95 / 1,419.79 / 1,419.79 |
| Maximum convergence ticks | 1 | 14 | 15 |

These are failures of the existing healthy-service gate. The Node maximum gap
is worse than the preceding checkpoint even though p99 server CPU is lower;
neither is averaged away. All 100 RPC calls per phase complete without error,
timeout or crash. Load/evict/reload converge with zero journal failures and zero
Character scheduler rejections. Ten root-motion cell crossings remain correct;
load root-motion requests equal commits (2,000 / 2,110 / 2,050 respectively).
Those functional assertions do not establish a wall-clock root-motion guarantee.

### Preserved after-timeline and causal qualification

`AnalyzeWork.ps1` produced adjacent `.timeline.csv`, `.service-points.csv` and
`.character-endpoint-envelope.json` for the current Local and Node logs. Local's
maximum recipient gap is 9,060.61–10,133.4 ms, with endpoint authoritative ticks
548–560. Those two states' acceptance scopes are separated by 1,059.629–1,060.413
ms. Across the window, consecutive tick spacings are approximately 58–123 ms,
not one 1.1-second Character call. Node's corresponding endpoint acceptance
separation is 1,219.221–1,219.989 ms, with recipient gap 1,220.62 ms.

Character publication intervals are measured in authoritative ticks
(`CharacterNetworkConfiguration::PublicationIntervalTicks`); the ordinary low
tier is 60/5 = 12 ticks. Slow ticks therefore stretch a tick-based cadence in
wall time without requiring scheduler rejection. The observed 12-tick endpoint
separation is consistent with that mechanism, but the fixture does not record
the worst relationship's tier or every generated/accepted/sent state. These are
**recipient gaps and endpoint bounds**, not a complete server-publication-gap
distribution or proof that no intermediate state was coalesced/lost.

Local remaining inclusive phase p99/max: relevance 10.432/14.908 ms,
structural selection 16.504/16.865 ms, peer journal preparation 2.443/2.606 ms,
rigid/world physics 19.942/20.513 ms. Desired-state max remains 20.540 ms.
Node has the same dominant domains, not a new provider-owned CPU bottleneck.
The earlier real-Player 3.8-second RPC case remains evidence of same reliable
ordered-stream backlog; it has **not** been rerun after these server changes.

### 500-peer decomposition and retained bounds

The current 500-peer/50-active-character Local load has server p50/p95/p99/max
46.333/79.372/111.267/140.044 ms, Character throughput 2,054.45 states/wall-second
and a 1,294.4-ms maximum recipient gap. Convergence p50/p95/p99/max is
2,163.77/3,875.22/3,997.68/4,081.94 ms (17/31/32/33 ticks). One acquisition and
one authoritative admission still serve all 500 peers. The exact selection
maximum is 8,192; 31 load ticks reach the global ceiling and pending high-water
is 190,464. All peers converge; journal failures and Character rejections are zero.
The 500-peer control load is healthy (p99 12.818 ms), but its later eviction phase
fails the service gate; the overall control process exits unsuccessfully. This
scale fixture is not substituted for the established healthy differential.

Pre-selection work is still not sufficiently bounded: current load maximum
dependency visits 139,404, pending-discovery candidates 106,176, and 32,768
possible new pending transitions before selection. Corresponding closure and
discovery maxima are 14.281 and 11.259 ms. Relevance evaluates at most 64 peers
per tick, defers at most 436 here, and reaches eight ticks oldest pending age;
this does not cap the objects traversed within one peer evaluation.

Current 500-peer journal reads reach 32,768 globally / 896 per peer in load;
3,077,500 records are charged, aggregate lag high-water is 3,067,500 peer-records
and maximum backlog episode 124 ticks. The cumulative individual lag high-water
is still 16,358 against 16,384 retention, including bootstrap: only 26 records of
observed margin, not a proven final retention envelope. Relevance cursor storage
is 16,064 bytes; journal staging 12,000 bytes; accepted ancestry 542,330 entries /
13,015,920 logical bytes excluding allocator overhead. There is no new dense
peer-by-object staging matrix, and no claim these values equal whole-pipeline RSS.

### Current verification and security artifact qualification

Exact `accepted-membership-v1` passes four targeted MSVC Release suites in
18.09 s and six Clang 19 ASan/UBSan/LSan suites in 76.02 s with
`ASAN_OPTIONS=detect_leaks=1`. The latter includes current ancestry, Parent
watermark, group-expansion and membership-delta tests; it is no longer limited
to the earlier lazy-journal checkpoint. Full final CTest, fault-injection,
overload/Stop, real-Player and CI/deployment matrices remain unclosed.

Security scan `2fb6b60a-c9f3-4496-baa8-b6b1d004635e` now has a successful native
launcher and complete reproducible manual coverage: all 19 inventory pages,
58 Engine plus two Node files, and 20 hand-authored diagnostic helpers were
reviewed. 9,083 generated/vendor/evidence rows were explicitly excluded;
no tracked diff was excluded. Independent coordinator and relevance/journal
reviews plus the parent review found no plausible new reachable security
candidate in this snapshot. Complete inventory and receipts are retained under
`build-3l3-worker/evidence/security-context/`.

The final draft explicitly requested complete coverage with no deferred rows.
The finalizer succeeded, but its sealed `coverage.json` retained the earlier,
superseded seven-file pending-review row and therefore says **partial**. Do not
edit the sealed artifact or call it complete coverage. Exact sealed copies are
under `evidence/security-sealed-accepted-membership-v1/`; the manual completion
receipt identifies every reviewed file. A separate tool warning says the working
tree changed during scanning: all 60 canonical source hashes were reverified
unchanged after sealing; new diagnostic/receipt artifacts had been added.
These two qualifications are preserved, not hidden by a security-clean claim.
The scan's measured usage is 16,306,159 total tokens, 16,251,911 input tokens,
including 15,592,832 cached input tokens, as reported by the workbench.

The current documentation site builds locally with the existing Node 24 and
Astro dependencies: 19 pages, 2.96 s, exit 0, isolated output
`build-3l3-worker/docs-accepted-membership-v1`. The nonfatal missing `docs/404`
content-entry warning remains. This does not deploy Pages or validate every
devdocs Markdown file; the expanded ledger's 157 rows / 78 answers and numbering
were checked separately, and Engine/Node `git diff --check` passes.

**B remains mandatory.** The next implementation owner is pre-3J replication
discovery/dependency work, followed by rigid physics envelope and transport
arbitration. No final legal unit reduction, safe partial physics activation,
new transport lane, client apply queue, publication or Foundation 3M is authorized
by these measurements. The existing 512-object / 1 MiB legal limit is unchanged
and is not yet a measured-safe end-to-end service envelope.

## Pre-3J causal decomposition after checkpoint 9

`discovery-profile-v1-source.json` is instrumentation-only relative to checkpoint
9 (41 Engine / 2 unchanged Node files); its benchmark SHA-256 is
`18C8217657BB0A888CFEA8322AA08A8BB464EB4D3647A05388D05D06F867AD1E`.
Four targeted MSVC suites pass in 18.22 s. The existing bounded debug capture
now separates dependency closure, pending discovery, leave-index construction,
reference-fixup planning and dependency-group selection. Thread-local benchmark
`operator new` counts are inclusive scalar-new allocation attempts, not all
native/C/physics allocations, not allocated bytes, and not worker-thread counts.
Sanitizer builds explicitly mark allocation capture unavailable. Production has
no allocation hook or per-tick output. The test trace still retains at most four
completed plus one current block of 1,201 server/client samples: the 42-phase,
48-byte duration payload has a 24,212,160-byte maximum, excluding containers.

| Local load phase | 200 peers max ms | 200 peers max work units/tick | 200 peers max new calls/tick | 500 peers max ms | 500 peers max work units/tick | 500 peers max new calls/tick |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Dependency closure | 9.676 | 101720 | 51597 | 13.924 | 139404 | 70452 |
| Pending discovery | 7.438 | 68258 | 66112 | 10.029 | 106176 | 66112 |
| Leave dependency indexing | 0.138 | 32768 | 0 | 0.156 | 32768 | 0 |
| Fixup cost planning | 2.025 | 12655 | 2416 | 4.073 | 17374 | 2416 |
| Dependency-group selection | 6.592 | 8192 | 57264 | 9.699 | 8176 | 57248 |
| Accepted commit (includes membership refresh) | 8.731 | not counted | 55515 | 43.526 | not counted | 286830 |

Closure units include queued dependency visits, including duplicates. Discovery
units include pending cancellation checks, Desired candidates and Known
removal candidates; they are not all newly allocated transitions. During one
load, 200 peers create 102,400 region Enters; 500 peers create 256,000. Each
tick can populate 32,768 before the independent 8,192 selected-operation cap.
The captured 200/500-peer loads rebuild 200/500 peer dependency plans (400/1000
closure calls including required dependencies). Fixup units count candidate
referrers, not emitted fixups. Leave indexing is cheap on Enter-only admission;
this does not prove eviction indexing cheap. CPU and allocation maxima need
not occur in the same tick and inclusive phases must not be summed twice.

The 200-peer control has zero closure/discovery work, but its ordinary property
commit tick still allocates 55,515 membership nodes and takes 8.122 ms. This
isolates a separate whole-Relevant-set rebuild at every accepted frame, even
when no membership changed. It is not necessary to 3E or 3J authority.

The unchanged 200-peer Local workload remains a health failure: load p99/max
63.931/99.848 ms, 4,355.01 Character states/wall-second, 1,086.85 ms maximum
observed gap and 1,324.84 ms/14-tick convergence. Control p99/max is
5.883/17.571 ms. The additional 500-peer/50-active-Character run is scaling
decomposition, not a replacement healthy acceptance workload. Its baseline
p99/max is 12.784/68.612 ms; load p99/max becomes 114.208/135.051 ms, observed
Character gap 1,337.82 ms, convergence p50/p95/p99/max
2,251.99/4,036.00/4,162.61/4,248.66 ms (17/31/32/33 ticks).
One acquisition/admission, 8,192 selection ceiling and zero journal failures
hold. Pending high-water is 190,464; aggregate journal lag high-water is
3,067,500 peer-records and the maximum backlog episode 124 ticks. The reported
16,358 maximum individual lag is cumulative, including bootstrap, versus
16,384 retained records; it is not a load-only lag measurement. That narrow
historical bootstrap margin needs explicit final retention validation.

## Implementation checkpoint 9: accepted ancestry, not current ancestry

The retained red regression `reparent-regression-red-v2-test.log` fails all four
combinations of new parent Known/unknown and 2/64-operation budgets. A
still-Desired child moved to another parent can be recursively removed by the
old parent's accepted Unpublish before its journal Reparent arrives. The first
frame can apply successfully while deleting the child: this is not merely a
decoder rejecting an inconsistent frame. Server Known then disagrees with the
replica. A second case removes both child and old parent after an unobserved
reparent; removal must follow accepted, not current, ancestry.

The coordinator now retains one parent identity and Parent-only journal
watermark per accepted Known object. Metadata is prepared before acceptance,
committed with the existing accepted frame, and erased with Known/disconnect.
It is derived replica metadata, not a new Desired or Known set. Entering a new
parent or leaving an accepted old parent includes the necessary current-parent
fixup before removal; an unknown new parent must enter first. Fixups count
against the existing selection/peer quantum. Historical Parent journal entries
covered by the fixup are coalesced without suppressing ordinary property changes.
Leave ordering follows accepted ancestry. No wire or protocol version changes.

`accepted-ancestry-v2-source.json` records 41 Engine / 2 unchanged Node source
hashes. Benchmark SHA-256:
`3C35C50C6E04E05D4464399CCB623D6C08CA7F3AAB2DE5C3DD461958985A8391`.
Four targeted MSVC suites pass in 18.26 s, including rapid repeated Parent
changes, a concurrent Name update, small journal slices, pointer-preserving
child survival and one-operation child-before-parent removal. This checkpoint
does not yet carry a fresh sanitizer pass or complete cancellation matrix.

| Load metric | Control | Local | Node |
| --- | ---: | ---: | ---: |
| Tick p50 / p95 / p99 / max (ms) | 4.280 / 5.474 / 6.320 / 16.951 | 19.139 / 29.842 / 63.135 / 103.392 | 19.186 / 28.671 / 63.063 / 105.498 |
| Character states / wall second | 8206.19 | 4203.51 | 4310.14 |
| Character max observed gap (ms) | 202.839 | 1099.69 | 1089.52 |
| Structural convergence max (ms / ticks) | 21.235 / 1 | 1340.63 / 14 | 1334.90 / 14 |
| RPC p50 / p95 / p99 / max (ms) | 48.911 / 51.810 / 52.130 / 52.512 | 93.205 / 131.364 / 305.936 / 325.789 | 90.967 / 119.833 / 301.735 / 329.603 |
| Reliable event ACK p99 / max (ms) | 52.657 / 53.688 | 300.790 / 337.334 | 301.880 / 330.809 |
| Action result p99 / max (ms; ten samples) | 53.235 / 56.060 | 113.502 / 159.146 | 112.058 / 160.630 |

This is a correctness fix, not a demonstrated performance improvement over
checkpoint 8. Local selection p99/max is 18.226/19.183 ms, Desired max 21.056 ms,
relevance p99/max 9.839/14.988 ms, journal 2.469/3.053 ms and physics
19.969/21.032 ms. Local's observed gap spans 9081.60–10181.3 ms, state ticks
548–560; endpoint server acceptance separation is 1086.186–1086.950 ms, subject
to checkpoint 7's sampling qualification. Both streaming health gates fail.

At load end, accepted ancestry contains 55,315 entries in control and 157,715
in each streaming case: 1,327,560 / 3,785,160 logical payload bytes, excluding
map nodes/allocator overhead. Local RSS/PrivateUsage is 143,847,424 / 142,757,888
bytes and Node 146,415,616 / 142,065,664. These are point-in-time whole-process
measurements, not an attribution of all RSS to the map or a leak conclusion.
For a successfully planned policy-managed peer, Known is contained in Desired
plus pending Leaves, giving a conservative 65,536 + 65,536 = 131,072-entry
bound during overlap, not a standalone 65,536 Known ceiling. The session's
pending cap further bounds aggregate overlap. This argument does not supply a
new hard limit for legacy manual replication APIs; those remain review scope.
No maximum-peers by maximum-objects dense allocation or ancestry history is
added. All 100 RPCs
per phase complete with zero errors/timeouts. Shared acquisition/admission,
8,192 selection cap and zero journal failures/scheduler rejections remain.
Unbounded pre-3J population/fixup scans and the broader lifecycle matrix remain
open; the legacy manual SetRelevant path is not claimed independently closed.

## Implementation checkpoint 8: read-only journal views

Journal preparation now borrows accepted peer membership until a legacy
journal create/destroy actually needs a mutable candidate view. Ordinary
property/reference checks and skipped/coalesced records do not mutate that
view and no longer copy it. Mutable legacy paths retain their speculative copy
and existing commit rules. The bounded-read test asserts zero PeerViewCopy
calls across no-op history, property frames and byte-limit retries; all four
targeted MSVC suites pass (17.95 s).

Source: `lazy-journal-view-v1-source.json`; binary SHA-256:
`A3A8BD82DB659B32D3D77D9A116E225C3C774816B57F5C452D8EEE31ECCD2E1E`.
Local journal preparation mean/p99/max becomes 0.286/2.511/2.595 ms, versus
0.884/9.212/9.849 at checkpoint 7 and 0.339/1.695/90.130 before the work bound.
The differential records zero peer-view copy calls, down from 2,962.
Both the work bound and the lazy view are retained as a measured combination.

| Load metric | Control | Local | Node |
| --- | ---: | ---: | ---: |
| Tick p50 / p95 / p99 / max (ms) | 4.208 / 5.643 / 6.225 / 17.672 | 19.459 / 32.255 / 61.783 / 98.047 | 18.908 / 27.749 / 61.104 / 99.192 |
| Character states / wall second | 8206.24 | 4194.78 | 4376.85 |
| Character max observed gap (ms) | 202.579 | 1072.35 | 1082.17 |
| Structural convergence max (ms / ticks) | 22.008 / 1 | 1306.86 / 14 | 1320.88 / 14 |
| RPC p50 / p95 / p99 / max (ms) | 49.154 / 51.498 / 51.786 / 52.025 | 93.176 / 136.454 / 299.096 / 318.343 | 90.671 / 111.415 / 301.204 / 322.064 |
| Reliable event ACK p99 / max (ms) | 52.738 / 53.186 | 299.254 / 327.696 | 299.906 / 324.040 |
| Action result p99 / max (ms; ten samples) | 50.636 / 57.081 | 109.743 / 153.278 | 103.535 / 154.209 |

The earlier journal spike is materially eliminated, not merely relabeled.
However, Local still loses approximately 48.9% of Character throughput and
Node 46.7%; both streaming gates fail. The Local observed gap spans
9073.08–10145.4 ms, state ticks 548–560. Its endpoint server acceptance-window
separation is 1058.241–1059.048 ms, with the qualification in checkpoint 7.
Dominant Local phase p99/max remains relevance 10.661/14.506 ms, Desired
1.862/20.031 ms, selected structural preparation 17.434/18.368 ms, rigid
physics 19.834/21.680 ms and graph synchronization 5.627/6.099 ms.
The paired official Engine replica additionally has 19.996/23.534 ms physics
and a 22.371 ms maximum structural application call. These are simulated
transport measurements, not a fresh real-GNS official Player run.

Each streaming load still examines 1,231,000 journal records with 32,768 global
and 910 peer read high-water, 6,314 maximum peer lag, 1,227,000 total lag
high-water, a 49-tick maximum backlog episode and zero remaining lag at phase
end. The new per-peer execution state is 4,800 bytes for 200 peers. Selected
and committed structural work agree; 8,192 selection saturation, one shared
acquisition/admission, fresh reload admission, zero RPC errors/timeouts and
zero Character scheduler rejections remain intact. The fresh six-suite Clang 19
ASan/UBSan/LSan pass on this exact source passes in 76.43 s with leak detection
enabled. This is targeted checkpoint coverage, not the complete final matrix.

## Implementation checkpoint 7: bounded peer journal reads

`bounded-journal-v2` compiles and passes four targeted MSVC suites (18.29 s).
Binary SHA-256: `C6DD745C21DCFFBB558D48BD6CF4191686090EF6C17ED282CB7C6D5311C6B70B`.
The first candidate's production
tests passed except a new fixture that incorrectly assumed Archivable emits
journal records. The corrected native fixture explicitly injects skipped
non-replicated records and verifies exact total consumption across 192 peers.
Its 512-record global test ceiling saturates, the per-peer ceiling is respected,
all peers drain, and no journal lag failure occurs. The exact test consumes
6,144 skipped records in 13 ticks with 512 global / 68 peer read high-water.

| Load metric | Control | Local | Node |
| --- | ---: | ---: | ---: |
| Tick p50 / p95 / p99 / max (ms) | 4.416 / 5.967 / 6.718 / 22.130 | 20.703 / 38.508 / 62.152 / 105.229 | 19.794 / 39.474 / 62.815 / 103.999 |
| Character states / wall second | 8201.45 | 3990.52 | 4128.76 |
| Character max publication gap (ms) | 202.767 | 1098.93 | 1118.21 |
| Structural convergence max (ms / ticks) | 26.336 / 1 | 1336.06 / 14 | 1351.86 / 14 |

Local journal preparation max falls 90.130 to 9.849 ms, but mean rises
0.339 to 0.884 ms. Splitting history causes 2,962 full peer-view copies instead
of 600; their total per-tick mean rises 0.095 to 0.517 ms, despite a smaller
6.843 ms maximum. Character throughput regresses versus checkpoint 6.
This exposes redundant read-only peer-view copying; a following candidate
will allocate a mutable view only when legacy journal create/destroy actually
needs membership mutation. The new work bound is justified, but this combined
implementation is not ready as a performance result.

Local and Node each examine 1,231,000 records during load; the exact global
high-water is 32,768 and per-peer high-water is 910. Total lag high-water is
1,227,000 peer-record relationships (shared history is not copied into a new
queue), maximum single-peer lag remains 6,314 against 16,384 retained records,
and all cursors reach zero lag by the phase end. The longest observed continuous
nonempty backlog episode is 49 ticks. All phases converge with zero journal
failures, RPC errors/timeouts or Character scheduler rejections; the streaming
health gates still fail. The Local publication gap spans 9100.29–10199.2 ms,
state ticks 548–560; its old journal spike is gone, but repeated structural,
relevance and physics work still stretches the entire interval.

Measurement terminology: the benchmark's `maxCharacterGapWallMs` is the
recipient's wall-clock observation interval, not a timestamp taken at every
server scheduler acceptance. For its two endpoint states, the existing server
CharacterPublication scopes bound acceptance-window separation to
1085.379–1086.190 ms (versus 1098.93 ms observed). `BuildState` stamps the current
authoritative tick and queues within those scopes. This demonstrates that the
span is already present on the server timeline; it does not prove whether any
intermediate accepted state was coalesced/lost. A complete per-relationship
generated/accepted/queued/sent/received/applied trace remains outstanding.

The runtime uses existing peer journal cursors; no record queue is added.
Native defaults are 1,024 records per peer per tick and 32,768 session-wide;
hard limits are 8,192 per peer and 65,536 session-wide. These count returned
records even when skipped/coalesced, plus every repeated read during a
byte-limit retry. Each call reserves a geometric retry tail (at most half
the remaining allowance, rounded up, in its initial read). Each returned
record is visited by at most the existing batch-publication and operation
passes. Structural operation selection retains its separate 8,192 default cap.
When journal service exhausts its cap, the existing full-ConnectionId rotation
continues after the last reader, including readers that produced no frame.
This is an explicit bound on peer journal readers, not a claim that catalog
refresh, full peer-view copying or all pre-3J work has been closed.

No-op/coalesced cursor progress commits immediately because it changes no
materialized Known state. A frame-bearing cursor advances only on existing
3J scheduler acceptance. Retention, resnapshot failure and reliable ordering
are unchanged. Disconnect erases the generation-scoped peer cursor.
Additional persistent execution state is one counter plus an optional pending
tick per actual peer (24 bytes per peer on the measured 64-bit host); no dense
peer/object staging matrix is introduced. Aggregate diagnostics report reads,
per-peer/global high-water, deferred opportunities and total cursor lag.
Backlog age is explicitly a **continuous nonempty backlog episode**, not a
timestamp of the oldest individual record. Exact oldest-record age is not measured.
The extra no-work cost is scalar bookkeeping and an existing-peer cursor lookup;
control p99 rises 6.034 to 6.718 ms in this single paired run; repeated controls
are needed to distinguish the small fixed bookkeeping cost from run variance.

## Implementation checkpoint 6: selected ancestry ordering

Ancestry depth is now computed once per selected object, not once per sort
comparison. The 64-edge dependency-depth bound permits fixed stack cycle detection;
the selected-object scratch retains cached depths and reports their storage.
Parent-first Enter and child-first Leave ordering still use ObjectId tie-breaking.
An independent nested-tree reference and all four targeted MSVC suites pass (7.56 s).
This does not yet bound transition discovery before selection.

Source: `ancestry-once-v1-source.json`; binary SHA-256:
`D164EED41DBBD2A6B248E84BC8759B1A3B1CB835C5BBB67F77B20BAD954D759D`.
The 10,000-object structural probe retains ten convergence ticks and identical
selected/committed counts, while allocations fall 853,100 to 479,840 and mean
scheduling-call CPU falls 4.155 to 2.151 ms. Local load selection p99/max falls
39.793/41.179 to 17.454/17.925 ms.

| Load metric | Control | Local | Node |
| --- | ---: | ---: | ---: |
| Tick p50 / p95 / p99 / max (ms) | 4.172 / 5.656 / 6.034 / 21.621 | 18.698 / 26.910 / 64.088 / 137.243 | 18.711 / 26.245 / 63.217 / 136.095 |
| Character states / wall second | 8204.44 | 4402.95 | 4387.35 |
| Character max publication gap (ms) | 202.374 | 1164.56 | 1163.38 |
| Structural convergence max (ms / ticks) | not recorded here | 1411.81 / 14 | 1405.89 / 14 |

The exact Local gap spans 9083.03–10247.6 ms (state ticks 549–561).
Repeated 17 ms selection calls remain inside stretched ticks; tick 559 adds a
90.130 ms journal-preparation burst, including 29.485 ms of peer-view copying.
Local relevance p99/max is 10.099/14.267 ms and server physics 19.745/20.164 ms.
Character service itself peaks at 1.311 ms with zero scheduler rejections.
All phases converge, but both streaming healthy gates still fail; verdict is B.
The next candidate bounds per-peer and global journal reads, including coalesced
records and byte-limit retries. It is not validated by this checkpoint.

## Implementation checkpoint 5: exact stable relevance selection reuse

Every budgeted peer evaluation still queries current 3K projections and evaluates
the existing enter/leave hysteresis predicates. When those predicates yield the
same spatial roots, the existing selection can be reused only if world membership
revision and required owner lifecycle are unchanged. No second Desired set or
asynchronous result is retained. Admission, eviction, descendant membership and
owner replacement still rebuild; motion across either hysteresis boundary still
changes relevance. Tests explicitly cover each of these cases, including a new
descendant under an already relevant root and owner replacement when both roots
were already nearby. All four targeted MSVC suites pass (7.75 s).

Source: `stable-selection-v1-source.json`; binary SHA-256:
`ECB9BA336C261A569AF90E13406DEE2938EF6EABDF6D229079D24D8FBB5CDA4B`.

| Load metric | Control | Local | Node |
| --- | ---: | ---: | ---: |
| Tick p50 / p95 / p99 / max (ms) | 4.234 / 5.666 / 6.382 / 24.203 | 19.035 / 26.705 / 86.873 / 149.219 | 19.181 / 26.363 / 88.101 / 150.532 |
| Character states / wall second | 8198.42 | 4201.49 | 4203.15 |
| Character max publication gap (ms) | 202.632 | 1376.77 | 1395.05 |
| Structural convergence max (ms / ticks) | 28.866 / 1 | 1693.75 / 14 | 1728.63 / 14 |

Local relevance mean/p99 falls from 11.394/23.627 to 4.779/10.109 ms.
Only 200 actual selection reconstructions remain, with 13,264 cache hits;
the 64-peer cap, 136 deferred-peer high-water, three-tick pending age and
6,464-byte cursor/revision storage are unchanged. Server/client fixed-step counts
fall to 737/739 over 401 ticks, reducing physics means to 9.247/9.188 ms.
The semantic precision change is retained for this measured benefit. Streaming
still fails: Local/Node Character throughput losses are about 48.75%/48.73%,
and consecutive structural selection (~40 ms) plus a 90.456 ms journal burst
still dominate the admission service gap. This is not final non-starvation closure.

## Implementation checkpoint 4: direct journal cursor lookup

`ChangeJournal::Read` previously iterated from the oldest retained record on
every call, including an already-current catalog/peer cursor that returned zero
records. It now addresses the deque by `cursor - oldest` after unchanged lag and
empty/future-cursor checks. Successful commits assign consecutive sequence
numbers, retention removes only a prefix, and clear removes the whole retained
tail without reusing sequences. Thus this is an exact O(1 + returned records)
lookup, not journal truncation, reordering or a new authority. The bounded result
vector reserves only the actual requested range. Unscoped legacy `ReadSince`
is unchanged; no production callers of it are present in the inspected source.

The independent cursor-range test covers every sequence around a 500-record
stream with 137 retained records, five read limits including zero, future/end
cursors, clear, batch commit and zero retention. All four targeted MSVC suites
pass in 7.74 s. Source is `journal-direct-cursor-source.json` (41 Engine / 2 Node
hashes); binary SHA-256 is
`72E3C88A1FDA518386ECCACA6525A7DE864DDD48FA18C8F9F595614EE1DA345D`.
The new benchmark also fixes the 500-peer relevance benchmark's accounting:
measure one complete observed peer rotation, not one capped call divided by 500.

With 16,384 retained records and 10,000 read calls, native microbench results:

| Cursor lag / maximum return | Before total ms / allocations | After total ms / allocations |
| --- | ---: | ---: |
| 0 / 64 | 132.356 / 0 | 0.2034 / 0 |
| 1 / 64 | 138.233 / 10,000 | 0.6017 / 10,000 |
| 512 / 64 | 156.965 / 120,000 | 5.4171 / 10,000 |
| 4096 / 64 | 124.206 / 120,000 | 5.3490 / 10,000 |

The before probe links checkpoint 3 production source with the same newly added
probe (`5223760FBB97D29701425E48A7A4F40937E21DB0F43FA99D78FD2B2925C706EA`).
No other workload ran concurrently with these performance measurements.

| Load metric | Control | Local | Node |
| --- | ---: | ---: | ---: |
| Tick p50 / p95 / p99 / max (ms) | 4.629 / 6.057 / 6.770 / 21.296 | 32.056 / 49.136 / 98.350 / 166.698 | 31.844 / 48.656 / 97.477 / 161.434 |
| Character states / wall second | 8205.86 | 2831.97 | 2904.32 |
| Character max publication gap (ms) | 202.632 | 1486.07 | 1477.47 |
| Structural convergence max (ms / ticks) | 25.488 / 1 | 1782.15 / 14 | 1762.07 / 14 |

Local catalog-refresh mean falls 3.698 to 0.040 ms/tick and journal-preparation
mean/max falls 6.578/146.154 to 0.348/93.060 ms. Control p99 improves 12.137 to
6.770 ms. Local Character throughput improves but still loses about 65.49%
against control; Node loses about 64.61%. Relevance remains 11.394 ms mean,
23.627 ms p99; structural selection remains about 41 ms p99 across the admission
window. All phases converge with zero journal failures/rejections/RPC errors,
but both streaming health gates still fail. This change is retained for its
measured benefit without claiming full journal service bounds: per-peer returned
history, view copying and cross-peer draining still require isolation.

## Implementation checkpoint 3: typed nil fixups; relevance alone is insufficient

The typed nil correction is now exercised by a dedicated 32-object Leave group
with 20 unrelated nil `Player.Character` references. The group costs exactly 32
operations, applies successfully and changes Known only after acceptance. Existing
Character retirement tests still pass. The three targeted MSVC suites pass (8.71 s).
The worker build helper is pinned to its existing CMake/CTest 3.31.10: a prior
regeneration incorrectly mixed PATH CMake 3.29 with the cached 3.31 module directory;
that failed tool invocation is retained, not counted as a source test failure.

Source: `typed-nil-fixups-source.json` (40 Engine / 2 Node native/fixture hashes).
Benchmark SHA-256: `FC3C57EF1FB08ACBDFD2D8224E47533E4FCC355F10791D25D197BB253AEF8F69`.
All streaming phases now converge, including eviction and fresh-identity reload,
but the gameplay health gate still fails. No wire, byte ceiling or quantum changed.

| Load metric | Control | Local | Node |
| --- | ---: | ---: | ---: |
| Tick p50 / p95 / p99 / max (ms) | 8.468 / 10.502 / 12.137 / 30.391 | 44.379 / 58.269 / 102.103 / 219.695 | 44.146 / 61.976 / 101.523 / 223.558 |
| Character states / wall second | 8190.10 | 2234.21 | 2234.86 |
| Character max publication gap (ms) | 207.526 | 1563.34 | 1583.42 |
| Structural convergence max (ms / ticks) | 35.080 / 1 | 1844.40 / 14 | 1869.20 / 14 |

Local relevance p99/max falls from checkpoint 1's 71.029/73.144 ms to
24.379/28.400 ms; Desired-recording max falls from 63.918 to 20.975 ms.
However, structural selection remains 40.439/42.012 ms p99/max and journal
preparation reaches 146.154 ms. Steadier load also increases fixed-step physics
catch-up: 1,301 server / 1,303 client rigid steps over 401 ticks, compared with
the original 930/992. Server/client physics means become 16.324/16.270 ms.
Thus the relevance candidate is phase-effective but **not yet justified as a net
production non-starvation improvement**. Final retention requires later decisive
evidence; it is not accepted merely because its per-peer work is bounded.

The preserved Local gap spans state ticks 549..561, wall time 9,205.82..10,769.2 ms.
Ticks 549..558 repeatedly spend about 40 ms in structural selection before ordinary
service; tick 559 adds a 146.154 ms journal pass and lasts 251.1 ms including the
client/observer path. Character simulation itself remains at 1.4605 ms maximum,
with zero Character scheduler rejections. This is repeated delayed service,
not a 1.56-second Character computation. Full before/after service-point CSVs
remain next to raw logs under `build-3l3-worker/evidence/`.

### Zero-peer physics matrix (not an end-to-end unit-size approval)

The new test-only `PhysicsActivationBenchmark` uses one coherent Main-owned
Folder attachment, then 120 fixed 1/60 s world steps, three repetitions per case.
The backend is Box3D. Anchored `CanCollide=false, CanTouch=true` Parts are sensors;
their overlap queries still run each fixed step. Disabling touch in this isolated
comparison is diagnostic only and is not an authorized change to authored behavior.

Values below are worst atomic attach / worst run p99 step (ms), across repetitions:

| Parts | Anchored, touch off | Anchored sensors | Simple rigid | Representative rigid | Weld-chain assembly |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 0.64 / <0.01 | 0.54 / 0.18 | 0.57 / 0.06 | 0.90 / 0.21 | 1.09 / 1.22 |
| 128 | 1.12 / <0.01 | 1.10 / 0.87 | 1.35 / 0.13 | 1.66 / 0.68 | 2.77 / 2.42 |
| 256 | 2.16 / <0.01 | 2.06 / 2.25 | 2.08 / 0.22 | 3.26 / 2.33 | 4.26 / 6.42 |
| 384 | 3.69 / 0.01 | 2.83 / 3.76 | 3.10 / 0.33 | 4.64 / 4.96 | 6.78 / 10.78 |
| 512 | 4.30 / 0.01 | 4.91 / 5.94 | 4.88 / 0.45 | 6.75 / 6.86 | 10.72 / 15.40 |

All 90 cases pass body/constraint registration and post-destroy cleanup checks.
Non-spatial Folder cases create zero bodies/constraints, with worst measured
steady step 0.0342 ms and first step 0.0367 ms. These are zero-peer WorldRoot costs, not complete ContentAvailability,
3K/3H/3E, transport or Player costs. Individual registration/broadphase/constraint
subphase timings are not separately measured. One Folder plus N Parts is N+1
content objects; an N-Part weld chain is 2N objects. Thus the 512-Part cases and
384/512-Part chains exceed the current legal 512-object package unit and are
scaling data, not legal-content test passes. The 256-Part chain is exactly 512
objects. Physics remains atomic; final A/B/C envelope decision is not yet closed.
Legal limits remain 512 objects / 1 MiB pending end-to-end evidence with margin.
No package partitioning or delayed-physics semantic change has been made.

## Implementation checkpoint 2: bounded relevance candidate (not accepted)

The current candidate replaces admission/eviction's per-object all-peer dirty
loop with a coalesced world revision. It evaluates at most 64 complete peers per
simulation tick (native valid range 2..512); duplicate calls cannot reset that
tick's budget. A rotating full `ConnectionId` cursor selects ordinary work;
owner-Character changes receive up to one quarter of the calls ahead of it.
Each evaluation reads the current spatial projection, focus and live identities
and commits one coherent 3E selection. No candidate list or Instance/peer pointer
survives across ticks. Removed roots immediately cease runtime relevance.
Revision checks defer obsolete 3J plans until `RecordDesiredState` refreshes
them, rather than publishing an evicted generation. Existing global query,
membership and per-peer Desired limits still bound a single evaluation; this is
not a claim that every maximum-sized query already meets a latency envelope.

With 200 peers, observed evaluation cap is exactly 64, deferred-peer high-water
136, maximum pending age three ticks and conservative additional cursor/revision
storage 6,464 bytes. With sustained critical work, the ordinary reservation gives
a full 512-peer rotation in at most ceil(512/48) = 11 service ticks; without it,
ceil(512/64) = 8. These are scheduling opportunities, not proven wall-clock
guarantees while other runtime phases remain expensive. No-work updates refresh
only already-dirty 3K projections and avoid a new all-peer/world selection scan
or allocation. The existing synchronous initial peer bootstrap is unchanged.

The staged tests pass: current-world reference equality, movement during
backlog, disconnect and reused connection generation, eviction/reload, critical
Character arrival, same-tick repeated calls, zero-work behavior and 64 seeded
interleaved mutations. The tests also reject a stale structural Enter before
relevance reevaluation. This is not the complete required randomized/failure or
sanitizer matrix.

A real integration defect appeared when the relevance pass stopped draining all
peers at once: under the existing 192-peer / 256-transition-per-tick admission
test, two eligible six-operation bootstraps remained behind ordinary structural
work beyond 83 ticks. A test-only preparation probe confirmed both frames were
valid. An explicit bootstrap pass reserves up to one quarter of the existing
3J cap, followed by the unchanged ordinary rotation. It is disabled when the
reservation would leave less than one full ordinary quantum, and its no-work
path adds no separate peer scan. All 192 peers now become ready in **21 ticks**,
with exact 256 saturation, 5,376 selected = committed transitions, no removal or
protocol reject, and the original 80-tick test bound restored. The temporary
mutating probe was removed. This does not establish all critical/control
fairness requirements; other lifecycle/reverse-pressure tests remain required.

MSVC targeted tests pass 3/3 in 7.23 seconds on the cleaned candidate. Source is
`bounded-relevance-v5-source.json`; benchmark SHA-256 is
`B216AFB71B23399CF10CA8E2C8F91D2F287A4457A218ECA3CC084CD49EFE3ED9`.
However, the full differential **rejects this candidate**:

| Load metric | Control | Local | Node |
| --- | ---: | ---: | ---: |
| Tick p50 / p95 / p99 / max (ms) | 7.410 / 9.170 / 10.021 / 27.480 | 42.089 / 54.957 / 100.138 / 215.972 | 42.126 / 55.351 / 99.998 / 214.792 |
| Character states / wall second | 8194.90 | 2402.52 | 2404.16 |
| Character max publication gap (ms) | 202.573 | 1544.21 | 1538.68 |
| Structural convergence max (ms / ticks) | 32.090 / 1 | 1821.04 / 14 | 1819.85 / 14 |

The unchanged workload still uses 200 peers, 50 active Characters, five trusted
neighborhoods, the same 512-object / 273,032-byte region, 12 Hz owner inputs,
ten Animator root-motion tracks, scheduled actions/events and 100 RPCs per
phase. The harness terminates a phase when those calls finish after its minimum
tick count, not at a fixed duration: baseline takes 401 ticks here versus 301
in checkpoint 1; load remains 401 in both. Do not conceal that difference.
Streaming throughput degradation is approximately 70.7%, worse than checkpoint
1. Load retains one acquisition/admission, zero Character scheduler rejections,
zero journal failures and no RPC errors, but these do not override the failure.

Both streaming paths terminate during eviction with `Structural dependency
group and reference fixups exceed the peer quantum`. Inspection identifies a
conservative 3J overcharge: every known spectator's nil hard `Player.Character`
is charged as a fixup against a Part/Folder Leave group, although schema typing
prohibits that reference from targeting either class. A schema-precise filter
and a dedicated legal-max-quantum regression are being validated next. This is
not solved by enlarging the quantum or changing Known before acceptance.

The failed v5 streaming runs did not emit their deferred phase traces, because
the benchmark only printed them after all four phases completed. Their raw load
summary remains valid, but relevance CPU attribution for v5 is **not measured**.
The benchmark now has bounded exceptional-exit trace emission for subsequent
runs; no production high-volume logging was introduced. All failed output is
retained as `bounded-relevance-v5-*` in `build-3l3-worker/evidence/`. This candidate
must not be presented as a production non-starvation closure or a net improvement.

## Implementation checkpoint 1: precise dependency invalidation

`RefreshCatalog` now advances a separate dependency cursor only when its
coalesced authoritative catalog changes live object identity, Parent, class,
or a hard ObjectId reference. `RecordDesiredState` also invalidates when the
3E selection/required lifecycle set changes. Scalar reflected properties,
transform-only updates, Attributes, non-replicated properties and soft references
do not change structural dependency closure. Catalog templates and ordinary
journal replication still refresh normally. Destroy, admission, eviction,
resnapshot and fresh-generation reload invalidate; no raw Instance lifetime is
retained by this revision.

The corrected pre-fix test fails four unnecessary-rebuild assertions; after the
fix, all three targeted MSVC Release suites (replication, relevance, GameSession)
pass in 7.70 seconds. Tests cover Parent, hard Player.Character replacement,
required LocalPlayer dependency, scalar/CFrame/Attribute/soft-reference changes,
destroy and fresh-generation reload, including acceptance-only Known and actual
client application. The separate old-parent unpublish issue below remains open.

This is a precise invalidation contract, **not the dominant starvation fix**.
Identical 200-peer / 50-active-Character / 512-object differential results:

| Load metric | After precision: control | Local | Node |
| --- | ---: | ---: | ---: |
| Tick p50 / p95 / p99 / max (ms) | 5.837 / 13.267 / 15.271 / 27.104 | 20.735 / 92.223 / 94.944 / 274.320 | 20.036 / 89.971 / 91.784 / 271.556 |
| Character states / wall second | 8244.06 | 2716.39 | 2847.41 |
| Character max publication gap (ms) | 203.097 | 1093.48 | 1073.34 |
| Structural convergence max (ms / ticks) | 31.911 / 1 | 1930.58 / 14 | 1926.68 / 14 |

Local phase p99/max (ms): relevance 71.029/73.144; Desired/dependency recording
0.262/63.918; structural selection/preparation 42.238/49.256; incremental journal
preparation 6.162/151.892; physics 21.236/23.400; Character service 1.257/1.440;
Remote pump 0.039/0.053. Durations are inclusive and must not be summed as
independent CPU. Local RPC p50/p95/p99/max is 176.370/211.984/421.279/467.315 ms;
reliable event ACK round-trip p99/max 433.988/552.765 ms (not one-way service gap);
owner action result p99/max 194.462/211.104 ms. All 100 RPC calls per phase finish;
zero errors/timeouts, Character scheduler rejections and journal failures.
Root-motion accepts every request, retains 11 load spatial crossings, but shares
the unacceptable Character publication gap. Ordinary control remains healthy.

The approximately 67% Local wall-throughput loss persists. The 104-to-95 ms
comparison with an older run is not attributed as a significant improvement:
the immediately preceding timeline-only Local p99 was already 92.874 ms.
The cache fix demonstrably eliminates redundant closure work in targeted tests;
it does not remove synchronized full-peer relevance or journal/selection bursts.

Exact source/patch artifact: `dependency-precision-source.json`; final rebuilt
benchmark SHA-256 `518FD82EE90CB8536BF374DE46B58CEDBF2B9504DD32B72D637068550A8D8AAD`.
Logs are `dependency-precision-{none,local}-200-50.out.log`,
`dependency-precision-node-200-50.log`, and `dependency-precision-rebuilt-tests.log`
under `build-3l3-worker/evidence/`. An earlier mixed incremental build is invalid
evidence: `scp -p` preserved a changed source timestamp older than an object built
while local editing continued. MSBuild skipped that source, mixing header/core
layouts. Refreshing only the two changed file timestamps and rebuilding both
configurations corrected it; the final build explicitly recompiled the core.
Future transfers use changed-file hashes **and** fresh changed-file timestamps.

## Implementation continuation: pre-fix service timeline

The implementation continuation starts at the same Engine/Node HEADs and the
47-file tracked Engine / two-file Node dirty checkpoint. Complete tracked diffs
were inspected before production changes; required architecture and retained
source-linked evidence were reread. `build-3l3-worker/evidence/timeline-before-source.json`
and adjacent Engine/Node patches preserve the starting native timeline candidate.
It changes diagnostics only: fixed first/last monotonic timestamps per existing
phase plus action validation and action-root-motion scopes. There is no event
history in production; disabled capture takes no clocks. Multiple calls within
one phase retain their first start, last end, total inclusive CPU and count.
The first-to-last envelope must not be mistaken for contiguous exclusive CPU.

The worker verified 38 Engine and two Node hashes before compiling with four
jobs. Executable SHA-256 is
`819F67E21637138D48CE330F12926BE465AAA1D4CE31365A1ABF8587304D8D7B`.
The unchanged Local healthy differential again converges but fails streaming
health: load tick p50/p95/p99/max = 20.089/89.427/92.874/272.901 ms;
Character wall throughput = 2,826.10 states/s; load recipient gap = 1,078.37 ms
(8,263.05 to 9,341.42 trace milliseconds, state ticks 447 to 453).
Reload reaches 1,120.17 ms. No scheduler rejection, journal failure or RPC error.
These are another **before** sample, not an improvement from an implementation.

The exact service-point CSV covers every captured tick including the complete
gap. Representative server ticks, milliseconds from the same trace origin:

| Service | Tick 448 first / last | Tick 454 first / last |
| --- | --- | --- |
| Tick start | 8263.320 | 9047.310 |
| Content | 8263.333 / 8263.334 | 9047.320 / 9047.321 |
| Physics | 8263.335 / 8287.075 | 9047.322 / 9067.011 |
| Animator root-motion admission | 8287.138 / 8287.250 | 9067.066 / 9067.159 |
| Relevance | 8287.625 / 8356.182 | 9067.492 / 9133.267 |
| Desired/dependency planning | 8356.350 / 8362.586 | 9133.418 / 9136.366 |
| 3J structural preparation/selection | 8362.591 / 8413.165 | 9136.370 / 9160.755 |
| Journal incremental preparation | no call | 9161.346 / 9315.291 |
| Character service (includes publication) | 8417.839 / 8418.749 | 9319.127 / 9320.075 |
| GCHR publication | 8418.385 / 8418.749 | 9319.645 / 9320.075 |
| Active-action root motion | 8417.898 / 8417.905 | no active action call |
| Remote pump | 8418.749 / 8418.780 | 9320.075 / 9320.104 |
| Last scheduler flush end | 8418.853 | 9320.209 |
| Server session end | 8418.853 | 9320.209 |

Actual source order remains Poll -> Engine content -> simulation/physics ->
Animator root-motion admission -> render publication -> Session relevance ->
Desired recording -> alternating structural/journal selection -> structural
flush -> graph synchronization -> Character action/input/action-root-motion and
publication -> Remote pump -> remaining scheduler flush. Action request
validation only runs when a request is pending; absence of a scope in these
two rows is not a measured zero request latency. GRPL encode, accepted commit,
all other rows, the client, and observer drain remain in the raw/derived CSVs.
This confirms consecutive stretched ticks plus service ordering, not a giant
Character call. The independently retained official multi-second RPC trace and
continuous client Poll samples remain applicable; this simulated run does not
re-measure GNS delivery.

Artifacts: `timeline-before-local-200-50.out.log`, `.timeline.csv`, and
`.service-points.csv` in `build-3l3-worker/evidence/`.

The first pre-fix dependency regression also exposed an independent 3J case:
reparent a still-desired child to a newly desired parent while the old parent
leaves in the same structural slice. Current preparation can unpublish the old
parent before the journal reparent reaches the client. The receiver can accept
the removal and recursively delete the still-Desired child. This is an existing
ordering defect, not caused by the invalidation change; checkpoint 9 reproduces
and fixes the policy-managed path. The invalidation-only Parent test keeps the old parent
desired so it tests dependency revision and required new-parent publication
without conflating these mechanisms. No readiness claim waives the remaining
staging/cancellation and legacy-path review.

This is an investigation checkpoint, not a completed implementation or readiness
claim. Foundation 3M remains gated. The prior [3L.2 closure](ContentAvailabilityFoundation3L_2Closure.md)
retains its historical lifetime, memory, transport, and sanitizer evidence.

## Starting source and reproduction

Canonical Engine HEAD is `a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1`.
The starting tracked working-tree diff contains 39 files, 1,397 added lines and
111 removed lines, including unrelated documentation that must be preserved.
Untracked files are recorded separately in
`build-3l3-worker/evidence/starting-source.json`; the counts above do not pretend
to include them. The initial tracked patch is retained alongside that manifest.

All 29 native/fixture hashes from the preceding closure match the pre-existing
worker snapshot under
`C:\Sandbox\Codex\Workspaces\gargantuan-runtime-host-f1-1\engine`.
The existing `gargantuan_node_content_scale.exe` reproduces the healthy
200-connected / 50-active-Character fixture with five Characters per spatial
neighborhood, 12 Hz staggered owner input, ten root-motion tracks, real actions,
RemoteEvent and 100 RemoteFunction calls per phase. One client is an actual
client Engine; remaining peers are protocol consumers. This is not a
500-connected or graphical Player validation.

Reproduction arguments:

```text
--content-differential <existing-scale-package> none 50 5 200
--content-differential <existing-scale-package> local 50 5 200
```

The package contains one 512-object / 273,032-byte region. The same package was
used by the earlier healthy differential; it is distinct from the near-1-MiB
official Player fixture. Measurements below compare the equivalent load phase.

| Metric | No streaming | Local OnDemand |
| --- | ---: | ---: |
| Server tick mean ms | 7.525 | 36.038 |
| Server tick p50 ms | 6.317 | 20.665 |
| Server tick p95 ms | 13.384 | 90.958 |
| Server tick p99 ms | 14.104 | 94.939 |
| Server tick max ms | 31.978 | 278.298 |
| Character recipient states / wall second | 8,228.13 | 2,730.15 |
| Character maximum publication gap ms | 202.071 | 1,081.29 |
| Character scheduler rejections | 0 | 0 |
| Authoritative root-motion requests / commits | 1,920 / 1,920 | 2,110 / 2,110 |
| Acquisitions / admissions | 0 / 0 | 1 / 1 |
| Structural selected / committed operations | 2,440 / 2,440 | 104,840 / 104,840 |
| Structural global selected cap | 8,192 | 8,192 |
| Journal failures | 0 | 0 |

Local Character wall throughput falls approximately 66.8%. All no-streaming
phases pass the existing health gate. Local streaming converges but fails the
gameplay health gate, as expected from the previous closure. This independently
reproduces the server interference. It does not yet reproduce or causally
resolve the separate 3.8-second official Player transport/service gap.

Raw evidence is retained in `build-3l3-worker/evidence/before-none-200-50.log`
and `before-local-200-50.log`. Remote logs are under
`C:\Sandbox\Codex\Logs\gargantuan-3l3`. No scheduling correction, content-limit
change, final commit, or publication is claimed at this checkpoint.

## Diagnostic candidate

The local candidate adds internal `WorkCapture`/`WorkScope` diagnostics and
opt-in benchmark tracing via `GARGANTUAN_CONTENT_TRACE`. Each capture uses a
fixed array of counters; normal operation retains no trace queue. A disabled
scope reads no clock. The active capture is thread-local, cannot migrate
DataModel work to a worker, and restores the previous capture on scope exit.

The benchmark retains at most four completed phase vectors plus one current
vector, each capped at 1,201 tick samples. It emits trace output after measured
phases, so printing does not inflate the next phase's Engine time delta.
Each sample contains separate server/client counters. Reported scope durations
are inclusive: nested scopes must not be summed as independent costs. Calls
equal to zero mean the scope was not entered in that capture, not proof that a
subsystem can never run or incur cost.

Scopes cover content polling/preparation/commit, Engine activation and major
steps, spatial/relevance updates, desired-state planning, catalog refresh,
structural selection, incremental preparation, peer-view copying, encoding,
accepted structural commit, graph synchronization, Character processing,
Remote pumping, scheduler flush, GNS send, and client decode/apply. Finer
activation, handler, transport-delivery, and overload instrumentation still
needs to follow the measured dominant paths.

## Compiled diagnostic reproduction

The user authorized scoped source synchronization to the verified worker on
2026-09-09. Local working trees remain canonical; no worker source was copied
back, and no commit or push was used for synchronization. The existing combined
Node/Engine MSVC Release build is reused with four parallel jobs. The diagnostic
target compiles successfully; the linker reports its existing LNK4098 CRT warning,
which is not being silently classified as a clean warning-free build.

The first instrumented executable SHA-256 is
`CD13FF84AF03F55CFB2D17703C9DA8D8EB3D05FBB4794A6839BC70048B397156`.
The no-stream control exits 0 and passes all four health gates. Local exits 1
after all four phases because gameplay health fails despite content convergence.
The load-phase tick p99 is 95.3195 ms, max 287.063 ms; Character wall throughput
is 2,697.23 recipient states/s and maximum recipient publication gap is
1,146.56 ms. This reproduces, rather than closes, the earlier regression.

Inclusive durations below are accumulated per measured tick, not per call.
Each load phase has 401 ticks. A once-only operation therefore has a zero p99;
its maximum and call count are important. Nested rows must not be summed.

| Scope | No-stream mean / p99 / max ms | Local mean / p99 / max ms | Local calls |
| --- | --- | --- | ---: |
| Content step | 0.0010 / 0.0017 / 0.0097 | 0.0484 / 0.0023 / 18.9307 | 401 |
| Detached preparation | 0 / 0 / 0 | 0.0209 / 0 / 8.3649 | 1 |
| Authoritative SetParent commit | 0 / 0 / 0 | 0.0225 / 0 / 9.0212 | 1 |
| Server Engine step | 0.3224 / 0.5121 / 0.5652 | 12.2648 / 21.3869 / 37.7166 | 401 |
| Server physics | 0.0300 / 0.0541 / 0.0646 | 11.8771 / 20.8750 / 22.7682 | 401 |
| Server session step | 7.0688 / 16.0720 / 32.1362 | 24.1725 / 84.6438 / 266.5940 | 401 |
| 3E relevance (includes spatial update) | 1.0237 / 6.5481 / 7.7393 | 11.3966 / 70.6306 / 72.7560 | 401 |
| Dirty spatial update | 0.0281 / 0.0402 / 0.0439 | 0.0563 / 0.0786 / 0.0996 | 401 |
| Desired/pending planning | 0.0675 / 0.1417 / 18.9326 | 0.2221 / 0.2393 / 70.9508 | 13,400 |
| Catalog refresh | 1.4524 / 2.7057 / 3.9048 | 2.7594 / 5.0277 / 8.3332 | 91,784 |
| Pending structural selection | 0 / 0 / 0 | 1.3972 / 44.1245 / 51.2341 | 200 |
| Incremental preparation | 2.5024 / 3.2087 / 13.5810 | 5.0227 / 6.5826 / 161.8200 | 78,184 |
| Graph synchronization | 2.1959 / 3.0752 / 3.2437 | 4.0776 / 4.7703 / 5.4959 | 401 |
| Character simulation (includes publication) | 0.7517 / 1.1296 / 1.3226 | 0.9082 / 1.2825 / 1.4600 | 401 |
| Character publication | 0.3098 / 0.4548 / 0.5203 | 0.3527 / 0.5067 / 0.6103 | 401 |
| Remote pump | 0.0209 / 0.0355 / 0.0423 | 0.0286 / 0.0418 / 0.0960 | 401 |
| Real client Engine step | 0.1526 / 0.3050 / 0.6409 | 13.0915 / 22.0209 / 25.8502 | 401 |
| Real client structural apply | 0.0067 / 0 / 2.6676 | 0.0708 / 0 / 25.6098 | 2 |

The dominant repeated p99 server cost is the synchronous 3E pass over peers,
not detached preparation or the Character publication routine. It recurs every
six ticks after all peers become due together. Resident physics is a second
major recurring cost on both Engine instances. Client physics mean/p99/max is
12.8410/21.6853/23.9283 ms (the recorded values remain in the raw trace).
The admission burst also includes 70.9508 ms desired-state planning and up to
51.2341 ms pending selection; the 8,192 selected-operation cap does not bound
this entire upstream and downstream work chain.

At tick 454, the server consumes 287.06 ms: 66.60 ms relevance, 161.82 ms
incremental preparation, and 24.41 ms pending selection are major components.
The entire fixture iteration consumes about 319.99 ms, including the actual
client and protocol-only observer processing. Subsequent ordinary relevance
ticks still take approximately 94–96 ms on the server and 115–118 ms for the
fixture iteration. These are repeated delays, not one 1.15-second server tick.
Zero Character scheduler rejections and preserved simulated-tick recipient
throughput are consistent with CPU/service cadence interference in this
simulated-transport fixture. This does **not** rule out separate GNS contention
in the official Player fixture.

The earlier suspicion that scalar journal changes continually rebuild all
dependency closures is not the dominant steady-state cost in this trace:
desired-state p99 is only 0.2393 ms after the burst. Keep that distinction when
choosing a correction. Likewise the tiny `EngineActivation` scope measures only
the Engine's descendant callback, not every observer invoked by SetParent.

Evidence: `build-3l3-worker/evidence/instrumented-{none,local}-200-50.out.log`;
the analysis helper emits lossless parsed summary JSON and a tick timeline CSV
alongside each input. The refined diagnostic adds rigid/soft physics phases,
relevance query/selection scopes, protocol-observer drain time, and exact
maximum-recipient-gap endpoints. No scheduling or legal-unit correction is
claimed by these diagnostic additions.

## Refined worker results: equivalent load phase

The refined diagnostic binary SHA-256 is
`852A4704C40F5BAF363838A5D66A618DC62AA270C57939D030734EB62CE28648`.
Compilation exits 0. All 38 scoped Engine files and both scoped Node files
match their canonical local SHA-256 values on the worker; see
`build-3l3-worker/evidence/instrumented-source.json`. Node HEAD remains
`f4440423c0701ff396f589fd51b6ce41edc63539`.

These are repeated diagnostic measurements, **not post-fix results**. The Node
case acquires content through real TLS; gameplay transport in this differential
fixture remains simulated. Each load phase has 401 ticks. No performance run
overlapped the subsequent native builds or sanitizer/race workloads.

| Metric | No streaming | Local | TLS Node |
| --- | ---: | ---: | ---: |
| Server tick mean ms | 7.643 | 35.845 | 36.044 |
| Server tick p50 ms | 6.342 | 20.544 | 20.728 |
| Server tick p95 ms | 14.073 | 91.222 | 91.183 |
| Server tick p99 ms | 15.887 | 104.447 | 96.707 |
| Server tick max ms | 32.830 | 297.256 | 276.047 |
| Character recipient states / wall second | 8,216.18 | 2,742.93 | 2,724.38 |
| Maximum recipient Character gap ms | 208.430 | 1,117.750 | 1,075.960 |
| RPC p50 / p95 / p99 / max ms | 49.514 / 51.223 / 51.668 / 52.592 | 174.313 / 199.592 / 452.913 / 497.539 | 175.043 / 210.985 / 420.468 / 454.213 |
| RemoteEvent ACK p50 / p95 / p99 / max ms | 50.164 / 58.000 / 59.107 / 76.909 | 162.515 / 201.816 / 453.081 / 570.001 | 164.088 / 199.518 / 423.131 / 549.947 |
| Action result p50 / p95 / max ms | 50.543 / 51.109 / 52.979 | 177.961 / 194.244 / 199.178 | 177.208 / 194.920 / 207.846 |
| Root-motion requests / commits | 1,930 / 1,930 | 2,060 / 2,060 | 2,040 / 2,040 |

Relative to the equivalent no-stream load phase, Character wall throughput
falls 66.62% Local and 66.84% Node; tick p99 rises 88.560 ms Local and 80.820 ms
Node. All control phases pass their health gates. All streaming runs converge
but fail gameplay health. RPC has 100 completions per phase, zero errors and
timeouts; no access violation occurs. All action submissions succeed, and all
root-motion requests commit, with eleven tested cell crossings and minimum Y
2.495 in each load phase. Correctness is not acceptable wall-time service.

Both providers use one shared acquisition and one initial admission; cached
reload makes a second admission, not a second acquisition. Both retain the
8,192 structural selection cap, have zero journal failures, and record 94,208
pending transitions at load high-water. Decoded-document high-water is
1,623,360 bytes for this package; it is not total pipeline memory.

The finer scopes locate resident Engine cost in the rigid backend step:
Local server mean/p99/max is 11.63/20.77/22.44 ms across 930 backend steps;
client is 12.66/21.76/24.77 ms across 992 steps. Those are accumulated per
fixture tick, not single-physics-step durations. Existing wall-time catch-up
can execute up to four fixed steps per Engine tick; the expensive iterations
therefore amplify physics work. Soft-body, collider preparation, neutral
event delivery and physics update scopes are small in this case. The rigid
scope wraps the backend, including its neutral event conversion; it does not
identify a particular internal Box3D algorithm as the cause.

Local relevance mean/p99/max is 11.53/74.71/78.87 ms. Within it, indexed query
p99 is 14.54 ms and selection construction p99 is 17.50 ms. Both are totals
over peers in that tick. Exact-distance/hysteresis/candidate processing and
other enclosing work also matter. No new policy authority or spatial cache is
introduced. Node reproduces relevance p99 70.75 ms and rigid-step p99 21.19 ms.

### Exact Local Character gap

The worst recipient gap spans trace time 8,287.94 to 9,405.69 ms: 1,117.75 ms.
The recipient advances from authoritative state tick 447 to 453; the latter is
observed during fixture tick 454. This is several stretched service intervals,
not a one-second Character simulation call.

| Fixture tick | Start ms | Whole iteration ms | Server ms | Relevance ms | Structural selection ms | Incremental preparation ms | Real client ms | Observer drain ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 448 | 8,288.21 | 205.38 | 155.51 | 69.18 | 43.40 | 0 | 24.60 | 23.37 |
| 449 | 8,493.59 | 129.32 | 85.45 | 0.07 | 46.79 | 0 | 22.91 | 19.40 |
| 450 | 8,622.91 | 116.26 | 74.66 | 0.06 | 40.77 | 0 | 21.03 | 19.42 |
| 451 | 8,739.17 | 116.59 | 74.94 | 0.06 | 40.59 | 0 | 20.80 | 19.76 |
| 452 | 8,855.76 | 116.00 | 74.65 | 0.06 | 40.36 | 0 | 21.06 | 19.51 |
| 453 | 8,971.76 | 115.29 | 74.47 | 0.06 | 40.47 | 0 | 20.74 | 19.55 |
| 454 | 9,087.05 | 329.00 | 297.26 | 78.35 | 23.87 | 160.57 | 20.96 | 10.37 |

The gap starts just before row 448 and ends before row 454 completes; summing
all full rows is not an exact reconstruction of the clipped gap. The timeline
CSV retains every measured phase. Observer draining is fixture orchestration,
**not production server CPU**. Character simulation is about 1 ms in these
rows; Remote pumping is about 0.025 ms. Their work is reached late. Session
structural flushing also precedes Character/Remote work, and a flush already
performed in that step defers newly queued traffic to another step.

## Official Player: separate reliable-delivery delay

Rebuilt Player, Server and packager pass `TestOfficialContentMemorySoak` for
Local and real-TLS Node, with a legal 512-object / 1,048,197-byte unit, eight
same-process churn cycles and 100 RPCs per provider. This is the official
headless Player over real GNS, not a graphical/GPU responsiveness test. The Go
test asserts functional completion, not the Foundation latency acceptance gate.

| Metric | Local | TLS Node |
| --- | ---: | ---: |
| Player frame p99 / max ms | 18.224 / 43.895 | 18.229 / 43.172 |
| Event-loop gap p99 / max ms | 19.557 / 57.984 | 21.183 / 57.292 |
| Structural apply p99 / max ms | 13.583 / 37.816 | 13.500 / 37.434 |
| Decode max ms | 3.398 | 3.118 |
| Character service maximum gap ms | 3,620.652 | 3,640.422 |
| Remote service maximum gap ms | 3,785.027 | 3,795.658 |
| RPC p50 / p95 / p99 / max ms | 50.052 / 51.323 / 3,658.651 / 3,794.322 | 34.305 / 52.543 / 3,592.982 / 3,790.770 |
| Reliable queued bytes maximum | 1,443,536 | 1,444,046 |
| RPC errors / timeouts | 0 / 0 | 0 / 0 |

Player event-loop samples continue throughout both service-gap windows. They
do not contain a three-second synchronous apply stall. Local RPC 2 has these
monotonic timestamps in microseconds:

| Event | Timestamp / observation |
| --- | --- |
| Client request starts | 517,627,139,596 |
| Server structural sends | 517,627,176,403: 437,419 bytes; 517,627,176,603: 469,284 bytes |
| Server handler timestamp returned to client | 517,627,177,507 |
| Accepted application sends | 517,627,177,620 and 517,627,177,637: 83 bytes each, behind approximately 904,880 reliable bytes |
| Client completion | 517,630,798,247 |

Request-to-handler is 37.911 ms; handler-to-completion is 3,620.740 ms. Local
RPC 3 takes 12.352 ms to reach its handler and another 3,781.970 ms to complete;
an application send 15.699 ms after the returned handler timestamp encounters
936,191 queued reliable bytes. The application-kind log does not contain an
RPC request ID, so these sends are temporal correlations, not uniquely matched
reply-delivery spans. Luau's Windows `os.clock` and native steady time use QPC;
these are elapsed monotonic clocks, not CPU-time samples.

The existing GNS adapter submits structural and reliable application traffic
on the same default reliable lane. The measurements locate the dominant RPC
delay after server handling and alongside that accepted reliable backlog,
while the client event loop continues. This supports head-of-line/transport
service contention as the mechanism, independent of the differential's CPU
fan-out problem. Exact backend pacing/retransmission contributions and a
per-reply enqueue-to-delivery trace remain unmeasured; it would overclaim to
assign every microsecond to a particular GNS internal operation. GCHR is also
delayed in this window, but the current logs do not isolate each GCHR packet's
queue residence from its publication/materialization prerequisite.

## Legal-unit experiment

New Local-only size fixtures keep the healthy 200/50 gameplay shape unchanged.
All load/evict/reload phases converge. None of the tested sizes passes every
health gate; the 64-object load already exceeds the 16.667-ms tick-p95 limit.

| Objects / encoded bytes | Tick p95 / p99 / max ms | Character max gap ms | Character states / wall second |
| --- | ---: | ---: | ---: |
| 64 / 33,784 | 19.914 / 24.204 / 97.434 | 217.991 | 7,733.38 |
| 128 / 67,904 | 26.312 / 31.248 / 108.158 | 251.330 | 7,112.91 |
| 256 / 136,216 | 48.732 / 62.296 / 140.724 | 464.591 | 5,416.86 |
| 384 / 204,592 | 70.246 / 75.315 / 178.389 | 933.136 | 4,023.79 |
| 512 / 273,032 | 91.222 / 104.447 / 297.256 | 1,117.750 | 2,742.93 |

The legal maximum stays **512 objects / 1,048,576 encoded bytes** as the
existing compatibility ceiling, not an empirically accepted production-service
envelope. No limit reduction is justified as a complete correction by this
sweep. The entire size/shape/provider/500-peer matrix is not newly established.
Disabling touch/physics, changing relevance meaning, or weakening health gates
would change the workload rather than close its demonstrated interference.

## Owning corrections still required

These are implementation requirements, not implemented guarantees:

1. Bound **3E/3J work before and after selection**, not just selected output.
   Round-robin peer/candidate progress and pending discovery need count/byte
   budgets, an explicit freshness contract, and generation-safe cancellation.
   Preserve 3E as the only relevance authority and 3J as the only structural
   authority. Account for selection construction, catalog/journal scanning,
   dependency closure, encoding and accepted commit. Do not publish partial
   authoritative content or advance peer materialization before acceptance.
2. Profile and bound the **resident rigid-physics cost** without changing touch,
   collision, deterministic stepping or accepted simulation semantics. A 3E
   correction alone cannot erase the measured ~20-ms physics contribution on
   slow ticks. Box3D internals need attribution before choosing an optimization.
3. Isolate **critical transport service** from accepted bulk structural backlog,
   including ordering/materialization dependencies and byte admission. A higher
   scheduler priority cannot preempt already queued reliable bytes. Validate
   eventual delivery, critical bootstrap, channel semantics and bounded backlog
   under pressure before accepting any lane/queue design change.
4. Retain atomic Main-thread content admission. If client staging becomes
   necessary, specify total operation/byte ownership, epoch cancellation,
   disconnect/Stop cleanup and identity checks before introducing it. This trace
   does not justify attributing the current 3.8-second gap to client CPU apply.

Do not create 3L.4 or a separate Character-capacity foundation automatically.
The demonstrated primary seam is 3E/3J fan-out plus transport service, with an
independent resident-physics contribution. Dense 500-Character capacity remains
a different, already-unhealthy baseline.

## Validation and remaining gate

Remote source synchronization is authorized and working. The remaining gates
are engineering/validation work, not a source-transfer permission blocker.

Compiled phase attribution, Local/Node reproduction and targeted Windows
validation are complete. The [validation ledger](ContentAvailabilityFoundation3L_3Validation.md)
records exact coverage, historical carry-forward, the 110 requested report
items and 42 direct answers. Functional passes must not override the measured
health failures. A bounded-work correction, full affected acceptance matrix,
final security review and publication gates remain incomplete. Current results
support only **B — FOUNDATION 3L REMAINS PARTIALLY READY**.
