---
status: measured-assessment
owner: runtime-networking
last_verified: 2026-09-11
---

# Foundation 3L: derivation and serial commit assessment

This is an attribution/design assessment, not an implemented parallel replication
contract. The current [3L.3 checkpoint](ContentAvailabilityFoundation3L_3.md) and
[validation ledger](ContentAvailabilityFoundation3L_3Validation.md) identify the
measured native sources. Existing runtime ownership and replication contracts
remain authoritative; no JobSystem, execution-domain or wire change is made here.

## Source-traced phase map

The runtime host/fixture polls input, advances Engine simulation, then calls
`GameSession::Step`. That call refreshes derived spatial positions, evaluates 3E,
requests/processes bounded planning, runs rotating structural/journal publication,
flushes structural submissions, synchronizes Character/Remote graph membership,
steps Character authority, pumps Remotes, and flushes remaining sends. Failure
drains occur between those service points. This is the implemented order, not a
proposal to move simulation after networking.

| Phase / implementation | Reads | Writes and classification | Current worker safety |
| --- | --- | --- | --- |
| 3K refresh in `ReplicationRelevance` | Main-owned Instance pose/size and lifecycle | Projection/index transactions; MIXED derived maintenance | Main only; reads live Instances |
| 3H `SpatialRegionIndex::Query` | Cells, memberships, bounds/space identities | Shared query generation, per-object dedup marks, metrics, caller scratch; MIXED derived computation | **Not a const/concurrent query** |
| `UpdatePeer` focus/hysteresis | Current projection, prior relevant roots, trusted focus, owner identity | Local candidate set plus peer roots/metrics; MIXED | Predicate computation is derivable, but the current call mutates the peer |
| `BuildSelection` | Global membership, root members, required lifecycle | Character candidate vector and immutable 3E selection installation; MIXED | Construction can be private; semantic installation stays Main |
| `PlanningContinuation::Run` | Selection lease, catalog, Known/accepted maps, dependency/reference revisions | Private closure/fixup/group scratch **and shared reservations/metrics**; MIXED derivation/resource accounting | Existing coroutine is Main-owned, not a worker job |
| Planner validation/install/cleanup | Selection identity, PlanningCursor, AcceptedRevision | Stale cancellation, complete Pending/projection swap, credit release | Serial resource/install boundary; no acceptance |
| 3J `ProducePlannedFrame` / incremental preparation | Complete READY groups, current templates, peer budgets, journal | Prepared frame/commit, candidate accounting; MIXED | Not just a selector: includes construction and validation encoding |
| `EncodeReplicationFrame` | Complete frame, immutable 3I templates and frozen schema | Private bounded byte vectors; READ-ONLY DERIVATION with owned output | Candidate for a bounded read/derive job; frame must remain alive |
| `QueueReplicationFrame` | Frame epoch/sequence and negotiated limits | Second encoding, intent, scheduler queue/accounting; MIXED | Encoding is separable; admission remains serial |
| `CommitSchedulerAcceptance` / `ApplyPreparedCommit` | Matching prepared sequence and accepted outcome | Known, accepted ancestry/references, publication/journal watermarks, pending removal; SERIAL AUTHORITY/COMMIT | Must remain Main and acceptance-only |
| Graph synchronization / Remote materialization | Current lifecycle, 3E candidates, accepted replicas | Character authority/Remote registrations and materialization epochs; MIXED lifecycle reconciliation | Not a generic read-only graph scan |
| Scheduler flush / transport handoff | Accepted queues, delivery/order, remaining allowance | Queue/bandwidth/lifecycle state and backend submission; SERIAL service | Pool work cannot replace ordered admission/handoff |
| Client poll / replica apply | Received frame and receiving graph | Decode/preflight scratch then DataModel/identity/signal changes; MIXED | Decoder derivable; application remains Main; independent preflight blocker |

Object and connection identities carry generations. The planner additionally
checks immutable selection identity, PlanningCursor and AcceptedRevision before
service/READY use. Those checks prevent stale *serial* resumptions; they are not
locks and do not permit concurrent map access or scratch-accounting mutation.

## 3E finding and local correction

The measured repeated work is peer × query/root traversal, not repeated Desired
set allocation on every evaluation. In the diagnostic Local 200 load, 9,864 of
10,064 peer evaluations retain the same roots. Queries allocate zero sampled
bytes in that phase; hysteresis still examines 5,060,921 old-root and 5,553,168
candidate visits. A no-op result does **not** prove the query was unnecessary:
other Characters move, and membership WorldRevision deliberately does not track
every pose change. No revision-only skip was introduced.

The retained candidate uses a call-local ordered join of sorted unique query
ObjectIds with retained root ObjectIds, instead of a tree membership search for
every candidate. Projection lookup no longer first repeats a lookup in
`SpatialObjects`. Successful `AddSpatialRoot` registers both stores; exception
paths remove the projection, and `UnregisterObject` removes both at the same Main
safe point. A failed projection transaction marks the relevance owner unhealthy
and prevents further evaluation. No projection can independently grant relevance.
The sorted join uses an explicit loop, not an order-dependent algorithm predicate.
No cursor outlives the call, no new cache/queue exists, and current enter/leave
distance tests, owner pinning, membership revisions and selection installation
are unchanged. A 128-object/48-step independent reference fixture exercises mixed
membership, three focus volumes, movement, hysteresis and fresh identities.

## Encoding and acceptance are different boundaries

Source and nested timing confirm two encodes: coordinator validation checks the
actual byte limit, then `QueueReplicationFrame` encodes the same immutable frame
for scheduler submission. The 200 Local load spends 62.70/58.58 ms in these paths
over 301 ticks; total encoding is 121.22 ms (nested wrapper timing differs slightly).
The 500 reload spends 195.86/185.95 ms, total 381.67 ms. Both-pass encoding is only
4.48%/6.46% of measured server busy time respectively, despite burst maxima of
11.39/13.94 ms. It is not the dominant recurring owner.

The encoder itself validates the frame, writes a payload vector, appends that
payload to a header/output vector, and returns the output. Independent copy CPU
inside that routine is **not measured**. Allocation bytes are cumulative traffic,
not retained memory. Intent creation and scheduler queueing move the byte vector;
the source does not perform another deep payload copy there. Their measured
separate scopes are small. Template/reference patch and accepted-metadata
preparation are not encoded-byte copying and must not be folded into encoding.

Reusing the validated encoding is a smaller future candidate than parallel
encoding, but requires a bounded owning result contract, equality/error-path
coverage and a measured gain. It was not combined with the 3E correction.
Current acceptance follows byte validation and scheduler admission. There is no
existing post-acceptance encoding stage to jobify: moving validation past commit
would weaken the failure transaction. A future encoder batch must prepare/encode
before serial admission/commit and preserve per-peer sequence/dependency order.

## Existing JobSystem: capability, not permission

`JobSystem` owns a fixed `std::thread` pool (zero means hardware concurrency), a
mutex/CV FIFO ring vector initially sized 64 and grown when full, and
`Submit(std::function<void()>, JobGroup)`. There is no work stealing, affinity,
queue ceiling, admission backpressure or cancellation token. JobGroup supports
Wait/completion and first-exception inspection. Shutdown rejects new submissions;
draining completes queued work, non-draining shutdown drops queued jobs while
joining active jobs. Group-less exceptions are caught but have no result channel.

The pool has no DataModel read lease. Worker execution rejects Main-only mutation;
that check does not make ordinary live reads thread-safe. A bounded caller can
submit a fixed number of coarse jobs and wait before releasing input/output
storage. Animation already uses this pattern with prepared pose work, bounded
batches and a merge after Wait. Pools are subsystem-owned; replication does not
currently own an Engine-wide shared pool or frame graph.

A source-level exception-safety caveat needs coverage before expanding usage:
`Submit` increments a group's pending count before allocating an expanded queue.
If that allocation throws, no queued job can decrement the added count. This is
a code-path finding, **not an injected/reproduced failure or completed security
review**. A dedicated <=16-job drained batch fits the initial queue and avoids
that growth path; it does not prove general pool allocation-failure safety.
No job-system changes were made in this slice.

## Proposed read/derive boundary — NOT IMPLEMENTED

3E is the strongest future candidate. Before submitting jobs, establish a 3H/3K
same-tick **read epoch**, rather than calling today's mutable Query concurrently:

1. Main completes spatial/lifecycle refresh and selects at most the existing 64
   eligible peers with the current critical/rotating order. Capture resolved
   focus, owner/LocalPlayer, full connection identity and prior roots.
2. Pin the projection/index and root-membership view through a mandatory same-tick
   barrier. No mutation, callback, teardown, index registration/removal or pose
   update may run against it while borrowed. No worker resolves an Instance.
3. Give each worker independent bounded query dedup/scratch. Today's shared
   per-object QueryGeneration writes must be replaced for this read API, not
   protected by a single query mutex that serializes the dominant work again.
4. Workers produce private root/selection candidates and local metric deltas.
   Main validates epoch, full peer identity, owner/control lifecycle and input
   selection/focus revision, then installs results in deterministic peer order.
   A result is not 3E semantic truth until that serial installation.
5. Wait/join before releasing the read epoch or allowing session Stop. Exceptions
   drain already-submitted work before storage release. Invalid results are
   discarded and the existing bounded evaluation queue is dirtied, not appended
   to an unbounded retry queue. Epoch exhaustion must fail closed, never wrap.

This is a new internal read contract requiring implementation and adversarial
tests, not an assertion that today's APIs already supply it. A full world copy
per peer would be the wrong boundary. Under the current production query limits,
one query scratch is about 640 KiB before any new dedup representation; sixteen
would already cost about 10 MiB. At most 64 result slots and explicit aggregate
byte/object ceilings are needed; 64 × 65,536 IDs alone is 32 MiB per full result
set. These are design sizing calculations, **not measured retained allocations**.
Do not allocate per-worker copies of the maximum peer × world graph.

The 64 evaluations remain global, not per worker. A future planner variant would
similarly reserve slices from **65,536 total** before dispatch, make credit
reservation/metrics per-job or serialized, pin catalog/accepted state, validate
revisions, and install on Main. No worker-count multiplication of 3J's 8,192
global operations, peer quantum, bytes/messages or memory ceilings is permitted.
Planning remains a lower-priority candidate because its shared reservations and
borrowed accepted maps are not private computation today.

With a strict same-tick no-mutation epoch, ordinary concurrent semantic stale
results should be absent by construction. Destroy/recreate, Character replacement,
disconnect/reconnect and focus changes injected before merge must still reject
the captured generation/revision. Actual discard/retry rate, worker utilization,
queue wait, barrier cost and 2/4/8/16-worker scaling are **not measured**. There is
no cross-tick networking pipeline in this proposal.

## Decision and remaining owners

The [ledger](ContentAvailabilityFoundation3L_3Validation.md) supplies the measured
before/after decomposition and Amdahl bounds. Those ideal bounds apply to captured
server busy work, not recipient gaps, idle pacing, client frames or transport RTT.
They do not assume that all unclassified remainder is parallelizable, or that
separate phase p99 values occurred in the same tick.

No jobified production phase is retained. The existing pool can execute bounded
derive batches, but the current spatial API lacks the pinned/read-only query
contract required to dispatch the dominant 3E work safely. The next justified
parallelism step is the narrow read-epoch/query boundary and serial-reference
validation above, not workerizing `UpdatePeer` or the current coroutine directly.
Encoding and planner jobification have smaller measured ceilings and are not
substitutes for that prerequisite. This is a **not-yet-met implementation gate**,
not a claim that safe 3E parallelism is impossible or has no potential value.

Post-selection construction/encoding/commit and client/observer draining still
form expensive consecutive publication iterations. Client whole-world preflight,
official reliable transport/client service, overload/recovery, startup journal
margin, current-source security and CI remain separate open gates. Loopback
measurements do not close the historical ~3.8-second official Remote delay.

**B — FOUNDATION 3L PARTIALLY READY.** No commit, push or Foundation 3M work.
