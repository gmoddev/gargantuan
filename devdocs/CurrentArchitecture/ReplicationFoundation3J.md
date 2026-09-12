---
status: current
owner: networking
last_verified: 2026-09-11
related_code:
  - include/gargantuan/network/ReplicationCoordinator.hpp
  - include/gargantuan/network/GameSession.hpp
  - src/network/ReplicationCoordinator.cpp
  - src/network/ReplicationPlanning.hpp
  - src/network/GameSession.cpp
  - tests/ReplicationRelevanceTests.cpp
  - tests/GameSessionTests.cpp
  - tests/GameSessionBenchmark.cpp
related_adrs:
  - docs/src/content/docs/developing/networking-architecture.mdx
---

# Replication foundation 3J

Foundation 3J bounds selected structural operations and reliable submission
after 3E has decided a peer's desired objects. It does not bound all surrounding
Desired/dependency/fixup inspection by itself. Runtime peers now use the separate,
private dependency-complete continuation described in the measured
[3L.3 planning checkpoint](ContentAvailabilityFoundation3L_3.md) before offering
READY work to these unchanged selection limits. It preserves 3I's immutable
authoritative templates, GRPL v1, peer materialization epochs, and 3E.1's
reliable failure rules.

```text
3H candidate discovery
    -> 3E desired and required peer objects
    -> current dependency closure
    -> compact pending Enter/Leave state
    -> 3J peer/global work selection
    -> 3I template plus peer reference patches
    -> GRPL v1 frame
    -> reliable scheduler acceptance
    -> exact prepared structural commit
```

This is not the lossy newest-wins policy used by 3G. Budget exhaustion leaves a
semantic transition pending. It does not make an object irrelevant, drop an
operation, advance a peer cursor, or claim materialization.

KI-007 in `KNOWN_ISSUES.md` records the formerly omitted accepted soft edge when
a replacement precedes old-target removal. The current local correction tracks
accepted native reference targets beside accepted ancestry and charges their
clear/replacement work to removal groups. Current authoritative values alone are
not sufficient. See the [3L.3 correctness gate](ContentAvailabilityFoundation3L_3.md)
and validation ledger for exact-source results; no client validation, budget or
authority is weakened.

The subsequent call-local ordered planning lookup joins sorted Desired identities
to catalog/accepted maps. At most eight iterator advances precede a tree-search
fallback for each lookup. This bounds sparse lookup walking only, not total
planning. No iterator survives the non-mutating pass and no candidate becomes
accepted early. This remains the synchronous/reference path. Runtime continuation
state instead validates immutable selection and catalog/accepted revisions at
each service slice, and reacquires reference values by ObjectId after suspension.
Only complete, lifecycle-valid, fully costed batches become READY. Incomplete
scratch is neither Pending nor Known; READY is not acceptance. KI-007 never sees
partial groups. The separate default 65,536 planning-step allowance, 2,048 peer
slice, finite memory ceilings and cancellation rules do not raise any 3J budget.
The 3L.3 ledger records improved reload tails, slower convergence and still-failing
healthy service gates; bounded planning is not an end-to-end latency guarantee.

## Ownership and work limits

`GameSessionConfiguration::StructuralReplication` is trusted native session
configuration. It is validated before session startup and has no client,
replicated, wire, or ordinary Luau route. Production values are fixed for the
session:

| setting | default | hard validation |
| --- | ---: | --- |
| transitions per peer per simulation tick | 512 | `1..4096` and no greater than the global limit |
| transitions globally per simulation tick | 8192 | `1..65536` |
| peer service quantum | 512 | `1..per-peer limit` |
| pending transitions per peer | 65536 | `1..MaximumPeerDesiredObjects` |
| pending transitions across the session | 1048576 | `per-peer limit..1048576` |
| diagnostic transition deadline | 600 ticks | nonzero |

A work unit is one selected structural operation. Creates, updates, reparenting,
reference changes, ordinary unpublishes, and destroys all consume the same
global/per-peer limit when they use the structural frame path. The object-count
limit is deterministic and bounds selected template patching and encoding work;
it is not a promise of a fixed number of wall-clock milliseconds for arbitrary
legal payloads. Existing protocol string, property, object, frame, journal, and
scheduler byte limits remain authoritative.

The default lets a normal baseline of at most 512 transitions complete in its
first scheduling opportunity. A larger initial desired set prepares only the
required dependency closure before accepting the peer, then drains ordinary
world work incrementally. A required bootstrap closure that cannot fit the
bounded peer quantum fails admission rather than creating an unusable
half-session or silently exceeding the cap.

## Peer structural state

One peer owns:

- the existing committed `ReplicationView`, `JournalCursor`, replication epoch,
  and next reliable sequence;
- a sparse ObjectId-to-exclusive-journal-boundary map for accepted complete
  publications whose represented state is ahead of that peer's journal cursor;
- the current dependency-closed desired and required object sets;
- one `PendingTransition` per current `Desired - Known` Enter or `Known -
  Desired` Leave;
- accepted ancestry records with compact non-nil native reference targets from
  the actual accepted frame; these are replica bookkeeping, not semantic truth;
- critical and ordinary FIFO scheduling entries containing only ObjectId and a
  generation token; and
- at most one bounded prepared commit while synchronous scheduler admission is
  being resolved.

A pending transition stores its kind, original pending tick, critical bit, and
queue token. It stores no encoded frame, property history, template copy,
materialization epoch, timer, task, callback, bandwidth debt, or client state.
The current MSVC value payload is 32 bytes per live pending map entry before
ordered-map allocator/link overhead; a queue token is 16 bytes before deque
block overhead. Only current pending peer-object relationships allocate these
records--there is no `MaximumPeers * MaximumObjects` dense matrix.
Repeated queue promotion/cancellation uses a new token; stale entries are
ignored and the queues compact once total entries exceed twice live pending work
plus 64. Thus queue memory cannot grow with historical defer duration.
The session-wide pending counter rejects the newly affected peer before sparse
relationship state can exceed 1,048,576 live transitions; peer teardown releases
its entire contribution without scanning unrelated peers.

The enforced relationships are:

```text
Pending Enter -> Desired and not Known
Pending ordinary Leave -> Known and not Desired
RelevantObjects -> Desired intersect Known
```

`KnownObjects` changes only in the accepted-frame commit. The desired set may
change earlier because relevance and materialization are intentionally separate.

## Dependency-safe selection

3E still supplies server-owned desired and required identities. The coordinator
expands current parents and schema-declared hard/non-nullable object references
from the 3I catalog. A selected Enter includes every still-unknown prerequisite
needed by that frame. Enter encoding orders ancestors before descendants, while
the receiver's existing frame validation treats same-frame hard-reference
targets as available. Leaves collect pending dependents and encode deepest
descendants before prerequisites. Soft references retain 3I's bounded nil/fixup
behavior.

The peer quantum is also the maximum selected dependency group. A group that is
truly not representable within that explicit limit is a peer-local structural
resource failure; the scheduler never performs an arbitrary overshoot. Existing
closure depth/object limits and cycle validation still fail closed. Each pending
transition has one canonical map entry, and queue tokens prevent duplicate live
membership after promotion, replan, cancellation, or relevance churn.

Selection precomputes the peer's current reference-fixup cost once for an active
transition pass. Required same-frame nil/reference fixups consume work units
alongside the object transitions that caused them. A group plus its fixups that
cannot fit the peer quantum is detected as an explicit resource failure instead
of overshooting the cap or remaining silently unserviceable forever.

Old reference targets come from accepted object metadata, including peer-specific
nil patches and accepted journal property updates, not the current catalog alone.
Surviving referrers must receive a current available replacement or nullable nil
in the same group before a target Leave. Unavailable hard replacements defer the
Leave until existing prerequisite Enter work progresses. A surviving referrer's
clear is one charged operation. Hard referrers also pending Leave join dependency
closure; soft referrers may leave independently. If their departure can reduce a
temporary oversized clear set to a legal group, selection defers the target and
continues peer queue service. A genuinely oversized surviving-reference group
retains the existing peer-local failure policy. No unbounded retry/debt queue or
new semantic dependency graph is created.

Accepted reference records are prepared with the frame and installed only at its
matching scheduler acceptance. Parent-only updates preserve the references;
reference-only updates preserve the ancestry journal watermark. Leave/destroy and
peer/session teardown release them. Frozen canonical property pointers plus full
ObjectIds cannot resolve to replacement Instance identities. Three fixed debug
counters measure inspection/removal work; accepted ancestry logical bytes now also
include the reference vector capacity. This finite representation does not close
the separate whole-peer planning CPU bound.

Dependency and current-revision validation occurs again when work is selected.
Pending Enter work resolves the current 3I template, so property mutations while
unknown collapse into the eventual current materialization. It never pins or
replays obsolete templates.

## Critical work and bootstrap

Required 3E identities and their hard dependencies are the narrow bootstrap
class. They include the trusted LocalPlayer and the owner Character closure when
a Character exists. The world root is the fallback required baseline. Other
global Player shells, regional objects, NPCs, and decorative content remain
ordinary unless 3E makes them required for this peer.

Critical work is serviced before ordinary work within a peer, but remains
bounded by the same peer quantum and global cap. Unused capacity needs no
separate reserve. Normal small sessions materialize their whole baseline in one
frame, preserving the established startup availability of scripts and Remotes.
Large sessions may become accepted after required bootstrap commits while
ordinary structural convergence continues.

Authoritative destruction promotes an existing pending Leave to critical.
Control revoke, Character action/control semantics, session messages, and other
non-structural reliable traffic remain outside the ordinary 3J budget and retain
their current scheduler lanes and failure policy. A client never gains authority
merely because an ordinary spatial replica remains known while its Leave waits.

## Deterministic fairness

GameSession derives a sorted vector of active connections, begins at its
persistent fairness cursor, and gives each peer at most one quantum per pass.
When the global cap ends the tick, the next tick resumes after the last serviced
connection. Removing a peer is safe because the cursor is a value-semantic
generation-bearing `ConnectionId`; reconnect creates fresh peer state.
Connection ordering scratch is retained by the session and per-tick consumed and
submission accounting lives directly on each peer, avoiding fresh fairness maps
or sets on every scheduling tick.

Within a peer, critical entries precede ordinary entries. Entries of the same
class remain FIFO by pending age; ObjectId is used only by deterministic initial
insertion and compaction. A scheduling call examines at most
`max(64, 4 * allowance)` queue entries, so stale entries or a temporarily
unfittable group cannot create an unbounded scan or spin. Newly queued/unblocked
ordinary work is eligible on a later controlled scheduler pass, never recursively
from an acceptance callback.

Age is measured from the tick the transition first became required. Deferral,
queue rotation, revision refresh, promotion, and dependency re-evaluation do not
reset it. The 600-tick deadline is an honest diagnostic, not an impossible
delivery promise when offered work exceeds capacity indefinitely. No credit or
historical debt accumulates, and unused capacity does not create a later burst.

## Prepare, scheduler acceptance, and commit

For bounded GameSession peers, the transaction is exact:

```text
select pending transitions
    -> build current immutable GRPL frame and bounded commit identities
    -> scheduler rejects: FailPeer; no Known/cursor/sequence commit
    -> scheduler accepts matching frame sequence
       -> advance sequence and represented journal cursor
       -> add/remove exactly represented KnownObjects
       -> remove exactly represented pending transitions
       -> refresh materialization-dependent visibility
```

Only one prepared frame may exist for a peer. `CommitSchedulerAcceptance`
requires the same peer generation and reliable frame sequence. Selection,
encoding, or a mismatched acceptance cannot mutate committed materialization.
The legacy aggregate published/unpublished/destroyed counters advance from the
same commit record, never from selection or encoding.
Reliable rejection remains terminal under 3E.1, so 3J does not create a second
retry queue after submission. Multiple peers and frames commit independently.

The legacy direct coordinator helpers retain their in-process produce/commit
behavior for existing codec tests. Production GameSession uses only the bounded
explicit-acceptance entry point.

## Journal, mutation, and cancellation

Every peer retains its own committed journal cursor. The initial accepted
baseline advances that cursor to the catalog revision represented when bootstrap
was prepared. Later reads are limited by the remaining 3J allowance. While both
materialization transitions and journal work exist, GameSession alternates the
two bounded classes per peer; neither a dense Enter backlog nor a continuous
stream of known-object updates can monopolize that peer's quantum.

For an unknown pending-Enter object, journal property records are skipped and the
cursor may advance because eventual Enter reconstructs the current complete 3I
template. The converse scheduling order must also be safe: when Enter is accepted
first, its prepared commit records the catalog's **prepare-time** exclusive
journal boundary. Earlier records for that ObjectId are already represented by
the complete publication and are skipped on subsequent bounded journal reads.
Otherwise old Attributes/tags can roll current materialization backwards and
bulk content repeats its initialization history. A newer catalog refresh by
another peer before acceptance cannot move this captured boundary. Records at or
after it and all unrelated objects keep their normal handling. Only exact
scheduler acceptance installs the boundary; rejected preparation installs none.

This is native server materialization bookkeeping, not a wire field, new
scheduler, payload queue, relevance decision, or journal-cursor shortcut. Each
map entry is one generation-bearing ObjectId plus one uint64 boundary, bounded
to 65,536 current entries per peer; it contains no strings or snapshots. Entries
are removed with the corresponding accepted Leave/destroy and all are released
on peer/session destruction. Baselines whose cursor already represents the
publication need no entry. Unchanged resident objects may retain this compact
bookkeeping for their current materialization lifetime, never historical reload
lifetimes. Maximum map payload is 65,536 times sizeof(pair<const ObjectId,
uint64_t>), plus allocator/tree overhead; the validation failure is peer-local.

No per-peer historical property queue is required. For already-known objects,
the existing journal preserves ordered create/destroy/reparent/tag/attribute and
other structural barriers; ordinary property values use current authoritative
state where the established replication semantics permit coalescing. A peer that
falls beyond the bounded 16,384-record per-scope journal window receives the
existing resnapshot-required resource failure and is failed locally rather than
retaining unbounded history. Reads remain bounded to 4,096 records and emitted
operations remain bounded to the current 3J allowance; skipped unknown-object
records therefore cannot turn into an oversized frame.

Cancellation is current-state based:

- pending Enter plus undesired/destroyed object becomes no transition;
- pending ordinary Leave plus renewed desire remains in the same materialization
  lifetime;
- accepted create followed by destroy remains an ordered create/destroy;
- object or peer generation reuse cannot match old pending state; and
- disconnect, `FailPeer`, Stop, and shutdown discard pending/prepared work rather
  than draining it.

No cancelled transition mutates KnownObjects, consumes a materialization epoch,
or leaves an encoded retry buffer.

## Character, Remote, and simulation boundary

`RelevantObjects` is always the intersection of committed KnownObjects and the
current desired closure. Therefore a Character or Remote becomes GCHR/Remote
eligible only after accepted structural materialization. When ordinary relevance
is lost, runtime publication eligibility stops immediately while a bounded
structural unpublish may follow; authoritative control was never granted by the
stale replica. Destroy and control revoke stop authority/publication immediately.
Re-entry creates the established new peer materialization lifetime after commit.

3J executes on the existing server GameSession safe point after authoritative
simulation and 3E relevance evaluation and before 3F/3G Character publication.
The client has no structural scheduler, budget, queue, or priority knowledge. It
receives ordinary dependency-valid GRPL v1 frames and applies existing lifecycle
signals at actual commit order. Offline runtimes with no network GameSession do
no 3J work.

## Diagnostics and measured evidence

Saturating aggregate metrics distinguish offered, selected, prepared, encoded,
scheduler-accepted, committed, deferred, cancelled, replanned, deadline-missed,
pending Enter/Leave/critical work, active peers, oldest ages, fairness rotations,
global-cap ticks, backlog-limit failures, journal-lag failures/maximum lag,
dependency operations, encoded bytes, and selection CPU. They carry no peer or
ObjectId labels.

The deterministic Release saturation fixture used one peer, current Folder
templates, and a fixed 1,000-transition service limit:

| offered | max selected/tick | service ticks | mean service tick | p99/max | encoded bytes |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1,000 | 1,000 | 1 | 1.94 ms | 1.94 / 1.94 ms | 101,339 |
| 2,000 | 1,000 | 2 | 2.19 ms | 2.14 / 2.24 ms | 202,375 |
| 5,000 | 1,000 | 5 | 2.60 ms | 2.66 / 2.85 ms | 505,483 |
| 10,000 | 1,000 | 10 | 3.35 ms | 3.85 / 3.86 ms | 1,010,663 |

The 10,000-object initial desired-state/pending capture took 4.64 ms on this host;
it is cheap identity/dependency bookkeeping bounded by the 65,536-object peer
ceiling, while template patching, assembly, encoding, and commit remain tied to
the selected 1,000. Total convergence sends only current state and does not grow
a historical queue.

The representative 32-peer admission fixture selected 2,496 transitions at its
peak, recorded no budget deferral or deadline miss, and reached complete
structural convergence in the same four ticks as critical readiness. One peer
similarly completed in four ticks. The 100-peer run saturated the 8,192 global
cap for two ticks, reached p50/p95 critical readiness in tick 3 and p99/max in
tick 4, and fully converged in tick 6.

In the 500-peer production-session fixture, all 500 server-side critical
baselines were scheduler-accepted in tick 17; every simulated client completed
readiness by tick 21 / 2,269 ms. Full ordinary structural convergence completed
in 91 ticks / 7,367 ms. The maximum selected work was exactly the configured
8,192 global cap, the cap was exhausted on 89 ticks, pending transition
high-water was 475,256, final backlog was zero, selected and committed work both
totalled 737,280, and total structural encoded bytes were 83,050,502. Journal lag
peaked at 9,672 records with zero lag failure, validating the measured 16,384
retention bound. The longer convergence is the intended trade for bounded peak
work; it is not reported as admission failure.

Focused tests additionally prove deterministic constrained drain, exact
selection-versus-acceptance accounting, pending cancellation, ordinary Leave
cancellation without epoch churn, hard dependency ordering, deadline metrics,
configuration rejection, destroy/replacement cleanup, reliable terminal failure,
Remote visibility, Character materialization/control ordering, and full existing
GameSession action/Remote behavior.

## Surface and security audit

New native surface is limited to structural configuration, aggregate metrics,
the measured bounded journal-retention constant, bounded desired-state
recording/production, a pending-work query, and exact scheduler acceptance
commit. There is no new GRPL/GCHR version, packet field,
application ACK, client request, game Luau API/callback, renderer dependency,
Studio feature, Node route, MCP operation, or required Telemetry dependency.

The client cannot select relevance, enqueue an ObjectId, set a budget/deadline,
claim critical status, move the fairness cursor, alter pending age, or commit a
prepared frame. Critical classification derives only from server 3E/session
state. Existing ObjectId generations, connection generations, authority checks,
Remote bounds, action/input limits, scheduler ceilings, and peer-local terminal
failure remain the security boundary.

## Next foundation recommendation

Measured 3J work no longer points to a generic QoS abstraction: structural and
replaceable Character policies remain intentionally different, and the bounded
16,384-record journal retained a 9,672-record admission peak without becoming
the dominant unbounded resource. 3H candidate locality, 3I current revisioned
templates, and 3J incremental materialization are now the prerequisites for a
future **package-backed region content availability/streaming foundation**:

```text
packaged region manifest
    -> server region content availability
    -> 3H spatial organization
    -> 3E relevance
    -> 3J bounded structural materialization
```

That work should begin with package/runtime profiling and an explicit content
lifetime contract. It should not expose raw region IDs or structural budgets,
merge 3G and 3J policy, or be treated as already committed by this milestone.
