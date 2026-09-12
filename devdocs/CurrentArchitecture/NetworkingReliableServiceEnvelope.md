---
status: historical-assessment-with-implemented-follow-up
owner: networking
last_verified: 2026-09-12
related_code:
  - src/network/ReplicationPlanning.hpp
  - src/network/GameSession.cpp
  - src/network/Scheduler.cpp
  - src/network/GameNetworkingSocketsTransport.cpp
  - tests/GnsCapacityBenchmark.cpp
  - tests/StructuralBytesFixture.hpp
  - tests/ReplicationRelevanceTests.cpp
---

# Networking reliable service envelope

## Current follow-up (2026-09-12)

The assessment below preserves its original measured source state. The approved
[deployment contract](NetworkingReliableDeploymentContract.md) now implements
hierarchical byte admission with no large-group latency exception. Its
[validation ledger](ContentAvailabilityFoundation3L_3Validation.md) records the
production-accountant matrix and official Local/Node results. Earlier statements
that admission was absent apply to the historical diagnostic slice, not current
runtime behavior. Full path/client, overload, journal, security and CI qualification
remain separate gates; this assessment does not certify a deployment.

## Published assessment and follow-up

This diagnostic slice is published as `108200d077a8830ba2fd5fc96e518bdde6baf0c2`
on `foundation/3l-content-availability`. Its original checkpoint below is retained
as evidence. The subsequent [deployment/atomic-group contract](NetworkingReliableDeploymentContract.md)
defines trusted per-connection plus aggregate reservation semantics and finite
oversized-group credit without implementing production admission. Actual isolated
planner groups now include a 123,183-byte Player/Character group and an eight-op
group reaching the GNS complete-message ceiling. Mixed canonical group-frequency
percentiles remain unmeasured; frame percentiles below must not be relabeled.

## Decision checkpoint

**B — FOUNDATION 3L PARTIALLY READY.** This is a measured design assessment over
`1bbcd948991a398c7a0613dcfea90668e10b68db`, not an implemented transport guarantee.
Only tests/diagnostics and documentation change. Production rates, buffers,
ordering, wire, 3E, 3J, Known and all planning/operation limits remain unchanged.
No lanes, client transaction redesign, new replication pipeline, merge or 3M.

Small complete-message pacing works in the controlled backend experiment. It
does not establish a universal production rate, a supported aggregate server
egress profile, or a policy for every currently legal atomic group. Those are
the implementation gate: do not quietly shrink the accepted content/Remote
contract or add an oversized exception that reinstates seconds of FIFO blocking.

## Resource ownership

| Resource | Current owner / bound | Not equivalent to |
| --- | --- | --- |
| Planning work | Main-owned continuation, 65,536 steps/tick; peer slice 2,048 | Operations or bytes |
| Accepted structural operations | 3J, 8,192 globally/tick plus peer/quantum caps | Link service or time |
| Encoded frame | GRPL codec and negotiated message limit; GNS complete message <=512 KiB including 32-byte adapter envelope | A sustainable byte rate |
| Pre-backend queued messages | NetworkScheduler; session 16 MiB reliable, manager 256 MiB ceilings | Backend queue or delivery |
| Backend capacity | GNS congestion/send mechanics; current inherited fixed 262,144 B/s configuration | Observed instantaneous delivered B/s or a path guarantee |
| Backend backlog | GNS; pending, sent-unacked and unreliable work are different observations | Desired/Known or application completion |
| Gameplay latency | Entire request/publication-to-recipient chain | Handler time, local acceptance or a queue estimate |

GameSession shares a negotiated 2 MiB byte allowance across its flush opportunities
in one Step. That bounds local submissions, not bytes per wall-clock second.
`SchedulerTickBudget::IsValidFor` must fit any one negotiated message; reducing
that budget below a valid message is not a substitute for production byte admission.

## Current preparation and group boundary

The private planner constructs dependency-complete candidate groups under a work
count, accumulates `Cost`/`LargestGroup`, then exposes READY. 3J constructs current
peer-patched operations, encodes the entire frame, and checks `MaximumFrameBytes`.
An oversized frame disposes its plan and rebuilds with a smaller operation limit;
the floor preserves the largest group and required bootstrap closure. A truly
oversized group fails the peer rather than accepting an invalid prefix. This is
a message-validity retry, **not** byte pacing. Repeating it against fluctuating
available credit could repeatedly invalidate planning and defeat liveness.

Known, accepted parent/reference metadata, journal watermarks and sequence advance
only at matching scheduler acceptance. An admission controller must defer before
that acceptance, not mark work Known while withholding its prerequisite frame.
Simply returning WouldBlock for bulk messages in the adapter is insufficient:
GameSession stops later flush attempts after a non-drained flush, and newly
materialized Remote/Character traffic may require the queued structural prefix.
Do not bypass that ordering to make an RPC test pass.

KI-007 reference clears/replacements must remain in the same accepted group as
the removal they make safe. A group is not a frame, and a frame is not a packet.
Fragmenting a large already-admitted frame does not reduce its FIFO byte debt.

## Structural byte evidence

The canonical scale package is 512 objects: one Folder and 511 ordinary Parts,
no padded names, Attributes or custom state. The selected real-client recipient's
load frame is **154,851 bytes / 512 operations**. Individually encoded Part Enters,
conditional on their parent already being accepted, are at most **339 bytes
including the 36-byte GRPL header**. The legacy diagnostic label `flatEnterGroup`
describes this conditional encoding, not a captured planner group partition.
An unknown Folder can join its first child's group; the flat fixture's decoded
90-byte Folder operation plus largest 303-byte Part operation and header imply a
conservative **429-byte** Enter-group envelope (INFERRED from this graph only).
Actual planner group percentiles remain **not measured**. The load publication contributes 109,354 bytes of reflected
property entries and 45,461 bytes of identity/name/count fields, plus 36 header
bytes. Removal of the complete flat region fits a **4,644-byte / 512-operation**
frame. This is a conservative complete removal envelope, not a generic KI-007
group decomposition. Other graph shapes are not classified by this flat fixture.

The official near-max profile is deliberately different. `OfficialNodeHostVertical.ps1`
starts with 20 authored CollectionCourse objects, then adds **492 Parts with 1,536
padding characters in each Name**. Two content publications therefore contain
**1,511,424 padding bytes**, 83.35% of the previously measured four-frame total
1,813,406 bytes. The codec writes each name once, with a four-byte length; this
is measured fixture payload amplification, not evidence of duplicated encoding.
The all-padded 256-operation frame has 393,216 padding bytes in its 469,284 bytes.
Its remainder is 76,068 bytes. Tags/Attributes are not the padding owner; the
property-heavy profile is a different fixture. No encoding optimization is made.

The 339-byte scale Part, substituting the official 1,549-character MemoryPart name
for its 19-character scale name, predicts a **1,869-byte** standalone frame.
That is a source/codec size inference, not a captured official atomic-group
percentile. Exact official group decomposition and arbitrary KI-007 encoded
group percentiles remain **not measured**.

Large groups are not only large collections of small Parts. The extended existing
Player.Character hard-reference regression exercises both reference and production
planned paths with 24/60 KiB legal names. The planned 60 KiB case produces a
**123,183-byte, two-operation** dependency-complete frame after its ancestors are
Known. A 40 KiB attempt cannot falsely commit it. This exceeds the proposed burst
quantum for the 256 KiB/s through 2 MiB/s profiles below. The exact group is tested
through codec, strict replica application and scheduler acceptance; it is not an
opaque transport payload. These are sample sizes, not the maximum legal graph.

The existing upper bound remains the negotiated/native message ceiling with
explicit oversized-group failure, not a proven sub-50-ms service quantum. There
is no new claim that every legal group fits 2,000 bytes or that a percentile
substitutes for its hard bound.

## Candidate policy — NOT IMPLEMENTED

Prefer an explicit **trusted host/deployment profile**, installed at connection
construction, with both a per-connection budget and an aggregate server egress
budget. It is not client-negotiated authority, a package field, ordinary Luau
priority or Node relevance policy. The five values below are laboratory profiles,
not recommended Internet-wide defaults. Even 1 MiB/s per peer implies 500 MiB/s
aggregate at 500 peers; single-loopback capacity cannot authorize that deployment.

The profile must distinguish nominal budget `S` from a defensible supported service
lower bound `C` over a specified healthy-path interval. Pinned GNS treats equal
SendRateMin/Max as a fixed rate and does not automatically discover a larger one.
Merely raising Max does not establish an adaptive capacity contract. Recent output
statistics never create extra credit. Packet/framing/encryption, retransmissions,
GCHR, gameplay reliable traffic and host service interruptions need headroom.

The experiment uses these explicit hypotheses, not product latency thresholds:

- Structural token rate `rho = 0.50S` or `0.75S`.
- Credit cap `sigma = floor(0.05S)` bytes; monotonic elapsed time replenishes it.
  An idle session cannot accumulate a later unbounded catch-up burst. Multiple
  flushes do not refill it. The trace reports actual charged bytes per 10 ms.
- Before each whole 2,000-byte test group, pending reliable plus pending unreliable
  plus that group must fit `H = floor(0.10S)`. Feedback must be available/nonnegative.
  `H` is an admission threshold; backend framing/retransmission can make observed
  pending bytes slightly larger. It is not an exact backend hard-cap claim.
- The remaining work is one bounded source cursor into 1.45 MB of offered work,
  not a persistent payload/candidate queue. Every reliable byte and probe must
  arrive before success; a case has a 25-second deadline.
- Gameplay is explicitly finite: 60 RPC, 60 event-ACK and 60 action-result probes,
  115 bytes each, scheduled over six seconds. These are tagged backend probes,
  **not** RemoteManager authority/action validation. All share the existing FIFO.

A production design would reserve complete-group encoded cost (including adapter
overhead) before acceptance, then consume the same reservation through encoding
and handoff. The planner/encoder remains group/dependency owner; networking owns
credit and bounded transport feedback, never relevance or acceptance. Scalar
changes between planning and preparation require current-size revalidation. A
ready group larger than transient remaining credit waits; a group larger than
the profile hard maximum cannot wait forever. No second payload queue is proposed.

Oversized policy must be explicit: retain peer-local failure above the existing
negotiated/native maximum; **do not yet impose a new smaller maximum**. For groups
between a service quantum and today's legal maximum, the deployment/compatibility
decision must choose either a documented bounded long-burst service class (and its
real latency cost), a smaller supported group/message contract with explicit
failure, or separate bootstrap semantics. Splitting KI-007 or promising immediate
gameplay service during a large same-FIFO atomic transmission is not an option.

## Conditional guarantee and limits

The token contract is `A_struct(t) <= sigma + rho*t`. Stable bounded service also
requires sustained total offered load (including nonstructural traffic and wire
overhead) below actual available service, not just below the configured estimate.
A FIFO residence bound needs a service lower bound, finite ahead-of-message bytes,
bounded gameplay bursts, finite retransmission/host scheduling assumptions and
the maximum complete group. Nominal `H/S` alone is not a delivery guarantee.

If actual service falls below the profile, backlog feedback can preserve bounded
admission but cannot preserve the nominal latency target. The capacity-shortfall
experiment configures a 1 MiB/s admission profile against a 256 KiB/s backend:
pending peaks at about 105 kB, with thousands of deferrals; RPC max is about
423 ms, not the nominal 100-ms queue estimate. No unbounded source queue is used.
Physical bandwidth exhaustion/loss must have an explicit finite failure/degrade
policy, not an unlimited promise of low RPC latency.

## Ordering-domain gate and required decision

**Lanes are not demonstrated necessary for the tested small-group workload.**
At the unchanged baseline rate, byte admission reduces the controlled trailing
RPC from seconds to tens of milliseconds while structure converges more slowly.
The 16 kB-message experiment without admission did not do that.

No same-FIFO guarantee is established for all current legal groups, sustained
gameplay overload, path loss, or 500 real network peers. If the required latency
cannot tolerate `largest atomic group / supported service`, or the resulting
convergence is unacceptable, a separate ordering-domain assessment is justified.
It must analyze bootstrap/LocalPlayer, publish-before-Remote references,
Player.Character/control bind/revoke, KI-007 removals, reconnect generations and
cross-class reference lifetimes. No lane or protocol change is implemented here.

**Stop before production implementation:** agree the trusted deployment/aggregate
rate contract and oversized-group compatibility/service policy. Reusing the
current 512 KiB-valid message contract with a universal 13 KiB burst cap would
silently reject legal work. Giving every oversized group a free bypass would
restore FIFO latency. This is the task's rate-policy/broader-contract gate, not a
claim that GNS is incapable or that a faster unexplained default fixes 3L.

Next implementation should use approved fixed profile bounds during planning,
exact complete-group packing, shared pre-acceptance credit and backlog gating,
then validate official Local/Node, lifetime/dependencies, journal retention,
aggregate 500-peer egress and overload/recovery. Raw scale observations and
backend probes cannot close those gates. Detailed current measurements and
validation are in the [3L.3 ledger](ContentAvailabilityFoundation3L_3Validation.md).
