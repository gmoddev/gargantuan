---
status: implemented-with-open-qualification-gates
owner: networking-and-runtime-host
last_verified: 2026-09-12
related_code:
  - src/host/server/ServerHost.hpp
  - src/host/server/ServerHost.cpp
  - include/gargantuan/network/GameSession.hpp
  - include/gargantuan/network/GameNetworkingSocketsTransport.hpp
  - src/network/GameNetworkingSocketsTransport.cpp
  - src/network/ReplicationPlanning.hpp
  - tests/ReliableEnvelopeContractFixture.hpp
  - tests/ReplicationRelevanceTests.cpp
related_adrs:
  - docs/src/content/docs/developing/networking-architecture.mdx
---

# Reliable deployment profile and atomic-group compatibility contract

## Approved service class and implementation (2026-09-12)

The user approved the proposed targets, 75/25 share, **no large-group exception**,
and unqualified treatment of incompatible low-rate profiles. This supersedes the
historical preflight/proposal below. Foundation 3L remains **B** until the official
path, client, overload, journal, security and current-source CI gates close.

The qualified gameplay targets are RPC p95/p99/max **150/250/500 ms**, reliable
Event ACK max service gap **250 ms**, and action-result max **250 ms**. These are
targets selected by product approval, not percentile claims inferred from a rate.
`ReliableServiceProfile::IsLatencyCompatible()` establishes necessary capacity
compatibility only; it does not certify an actual network/host or its p95.

Trusted startup `ServerHostConfiguration::ReliableService` owns R/A/N, backend
rate, share, finite bursts, admission thresholds and nonqueue allowance. The
session consumes the profile; GNS sets its rate on the listener/connection, never
process-globally. Clients, packages, Node content metadata and Luau cannot choose
it. No GSES/GRPL/GCHR wire fields or ordering domains changed.

The CLI requires `--reliable-rate`, `--reliable-aggregate-rate` and
`--reliable-peers` together (bytes/s, bytes/s, count). Supplying only part or
overriding an injected host profile fails. Native configuration can further tune
finite fields through the same validator. `--reliable-require-latency-compatible`
rejects an impossible class claim. Omission preserves legacy development behavior
and emits `unqualified-legacy`; it does not silently install a faster default.

An explicit CLI profile sets backend send min=max to **2R**, retaining R for
application reservation and an additional R for packet/realtime headroom. This
is a conservative configuration choice, **not measured path capacity**. Operators
must fund N backend ceilings and packet/retransmission/request load on their real
deployment; multiple Server processes must have separately funded envelopes.
No observed short-term throughput increases admission credit. Native profiles
may configure less backend headroom but remain capacity-unqualified. No send or
receive buffer was enlarged. Gameplay qualification additionally assumes finite
overlapping burst E, average gameplay within the reserved share, adequate actual
service and the declared host/path allowance. Overload retains existing finite
terminal-failure policy; there is no guarantee for unlimited concurrent Remotes.

Production numeric contract:

- `N*R <= A`; positive rates fit checked native/backend arithmetic. Session N is
  also at most 512. `1 <= StructuralPermille <= 750`: gameplay reserve >=25%.
- G=524,288 complete GNS bytes, of which 32 are adapter framing. The profiled
  session advertises at most 524,256 application bytes; it does not first accept
  a 524,288-byte application frame that the adapter cannot transmit.
- Peer/global structural bursts are finite and at least G. E defaults to
  262,176, covering one codec-ceiling Remote plus framing.
- `Bp <= Qp-2E`, `Bg <= Qg-2NE`, `Qg >= Qp`. The second E funds fresh gameplay
  headroom **while an existing gameplay burst is still pending**; a legal maximum
  group must not require a perfectly empty queue under a small continuous load.
  Defaults Bp=Bg=G, Qp=G+2E=1,048,640; CLI Qg=G+2NE.
- Qp/Qg cannot exceed the existing 64/256 MiB native resource ceilings. These are
  admission-exposure thresholds, not a claim of an exact total GNS memory cap.
  At N=500, Qg=262,700,288; a larger N that cannot fund this envelope fails rather
  than silently shrinking E or increasing memory limits.
- Nonqueue allowance defaults to 100 ms and must be in 1..249 ms. Compatibility
  requires Qp/R and Qg/A to fit the remaining part of the stricter 250-ms max,
  plus backend headroom >=R. This is necessary arithmetic, not proof of host,
  p95, loss/retransmission or actual bandwidth. With default thresholds/allowance,
  256/512 KiB/s and 1/2/4 MiB/s profiles are unqualified; 8 MiB/s is a candidate
  for actual path validation. No larger-group waiver changes those targets.

### Runtime byte boundary

`ReliableByteAdmission` is Main-owned resource accounting, not a scheduler of
semantic state. At the structural safe point after the early flush, GameSession
observes every admitted connection, including bootstrap. Exposure is provider
pending reliable bytes + unsent scheduler reliable bytes + 32 bytes per unsent
message. Missing feedback stops structural admission; absent data is never zero.
Already serialized but unacknowledged reliable bytes are not a second unsent FIFO
prefix; GNS still retains them under its existing finite provider limits. Actual
retransmission/physical shortfall remains part of path qualification.

Credit accrues from monotonic elapsed microseconds at S*R and S*A, with integer
fractional remainder, zero initial credit, finite caps and no refill per flush.
Each connection keeps one scalar required-cost hint, bucket, feedback and wait
age. No frame, object/Instance pointer, candidate list or semantic identity is
retained by this mechanism. The map is bounded by N generation-safe connections;
disconnect releases it. Memory diagnostics count logical state (not allocator
node overhead). One active synchronous reservation exists at most.

The existing 3J rotating consideration supplies peer order. A locally eligible
large group waiting for global credit earmarks the next global refill, preventing
smaller peers from consuming it forever. Local credit/backlog obstruction releases
the earmark; no-work, disconnect and accepted service release it too. A complete
group waits for finite credit, never borrows debt or bypasses the maximum.

Preparation takes a transient byte allowance **separate from the hard message
limit**. It uses the existing exact validation encoding. If insufficient, it
returns selected-operation cost plus a byte hint, with no PreparedCommit, Known,
sequence or emitting journal-cursor change. The READY plan survives; later
preparation revalidates current catalog/revision and re-encodes current payload.
Deferred sizing still consumes per-peer/global selection work for that step,
with one byte-deferred attempt per peer per step. No credit-induced hard-limit
replan or unbounded sizing retry is introduced. Nonemitting journal prefixes may
still progress under the independent existing journal caps.

When funded, peer and global exact frame+adapter bytes are reserved synchronously,
the already encoded bytes enter the existing scheduler, and matching 3J acceptance
commits Known/journal state. Failed scheduler admission refunds once and follows
existing terminal peer cleanup. Already accepted bytes are not refunded on a
later semantic failure. The 8,192 selection and 65,536 planning limits remain
independent. Late gameplay handoff, reliable FIFO order and unreliable GCHR remain
unchanged. There is no lane or new creator-facing priority.

Structural admission stops when exposure plus the group would exceed Qp-E or
Qg-NE. A threshold exceeded by intervening gameplay latches admission off until
exposure falls by min(quarter threshold, quarter G). Reserved bytes are added
immediately to this step's exposure, preventing several same-step submissions
from treating the same snapshot as empty. A fresh step replaces that snapshot.
The thresholds constrain structural contribution; they do not pretend to hard-cap
concurrent gameplay, provider protocol overhead or already-unacknowledged storage.

Current-source results and remaining gates are recorded in the
[validation ledger](ContentAvailabilityFoundation3L_3Validation.md).

The candidate 8 MiB/s application / 16 MiB/s backend profile passes the official
near-max Local and Node 100-call fixture: RPC p99/max 72.058/73.316 and
74.723/75.801 ms, respectively, no timeouts/errors, pending reliable peak 469,320 B.
This does not qualify full-size gameplay requests on the unchanged 256 KiB/s
Player request path, nor sustained gameplay bursts, overload/recovery or arbitrary
host/network conditions. The implementation is current; full qualification is not.

## Historical service-class selection preflight (2026-09-12)

The following preflight and proposal describe `e4739af17` before the approval
above. Statements that implementation was absent or selection unresolved are
historical, not current implementation status.

The implementation task over published `e4739af17` stops at its explicit
pre-implementation policy gate. **No production latency class has been selected
or installed.** Current networking contracts promise bounded queues, declared
ordering and terminal request outcomes, not an ordinary-gameplay p99/max bound.
The accepted networking design also leaves tuning values provisional.

The closest existing numeric requirements are the healthy differential's RPC
p95/p99/max of 150/250/500 ms, action-result max 250 ms and Event ACK max gap
250 ms. `GameSessionBenchmark.cpp` enforces them, but
[3L.2 validation](ContentAvailabilityFoundation3L_2Validation.md) explicitly
classifies them as fixture health gates, not Engine service guarantees. They
cannot be relabeled as an already-approved deployment promise. The prior
backend matrix's roughly 16/32/53 ms results are observations, not target-setting
authority either.

Recommended decision for confirmation, **not an implemented policy**:

- Adopt those existing healthy-fixture targets as the qualified Foundation 3L
  gameplay target, rather than choosing a number from the latest passing matrix.
- Start with 75% structural service and at least 25% gameplay reserve; prohibit
  a configured structural share above 75%. The measured small-message tradeoff
  is 7.316 s convergence at 75% versus 10.970 s at 50%, with approximately
  53 ms RPC maxima in both. This is a candidate policy, not a universally proven
  safe percentage independent of offered gameplay load.
- Do not waive the maximum for a legal atomic group. Reject incompatible
  **qualified profiles**; preserve lower-rate legacy compatibility as explicitly
  unqualified. The task's five requested rates are test profiles, not evidence
  that each must qualify for one latency class.
- Define ordinary gameplay's supported aggregate/per-peer burst and sustained
  demand, along with request-path, RTT and host-service assumptions, before
  claiming qualification. One codec-ceiling Remote allowance is a conservative
  single-message reserve, not a bound on overlapping calls. Existing Remote
  payload, concurrency and rate limits are not silently reduced.

The consequential missing decision is whether to promote those fixture targets
to the deployment contract **without a large-group exception**. A different
target or an explicit constrained-service exception changes startup compatibility,
backlog thresholds and deployment capacity requirements. No production admission,
rate, sizing, ordering, wire, 3J or planning change is made while this is unresolved.

For clarity, the measured 524,256-byte ceiling group is **eight Folder ancestry
operations**, not a Player/Character group. The separate demonstrated large
Player/Character group remains 123,183 bytes/two operations.

## Decision and implementation boundary

**B — FOUNDATION 3L PARTIALLY READY.** The preceding assessment was published as
`108200d077a8830ba2fd5fc96e518bdde6baf0c2` on
`foundation/3l-content-availability`, after committed-source validation. This
document defines a proposed implementation contract, not a deployed rate policy
or latency guarantee. The executable model and group observations are test-only.
No production config, admission, buffer, lane, wire, 3E, 3J, Known, planning bound,
content unit, client application, or authority behavior changes.

Decisions for the next implementation design:

1. Trusted **ServerHost** configuration owns one explicit per-server profile.
   Networking validates and consumes it. Per-connection reserved service and
   aggregate server egress are separate resources; both must fund admission.
2. Preserve the existing supported GNS complete-message ceiling: **524,288 bytes**
   including its 32-byte adapter envelope. A GRPL group must also fit its 36-byte
   frame header and, for bootstrap, the actual schema table. No new smaller
   content limit is silently imposed.
3. A complete group above the normal refill quantum waits for **finite accumulated
   credit**, capped at an explicitly configured burst ceiling sufficient for the
   supported group. No free oversized bypass, unbounded debt or semantic split.
4. A low-rate full-compatibility deployment has an honestly slower service class.
   Reject a profile that promises an incompatible queue window at startup. Do not
   truncate content, silently drop lifecycle, or advertise laboratory small-group
   latency for every legal group.

Implementation stops here as requested. In particular, preserving the full legal
group envelope at 256 KiB/s cannot guarantee subsecond FIFO service. If the product
requires both, that is an ordering-domain decision, not an admission optimization.

## Current ownership — SOURCE VERIFIED

`GameNetworkingSocketsTransportConfiguration` contains connection/event/receive
memory ceilings only. `TransportStartConfiguration` supplies role, endpoint,
network limits and opaque setup material; `GameSessionConfiguration` supplies
session and replication policy. None exposes rate or aggregate egress.

The adapter does not set `SendRateMin` or `SendRateMax`. The pinned GNS defaults
are inherited per connection from backend configuration, both 262,144 B/s;
equal bounds select fixed-rate behavior. These are neither a server-wide shared
budget nor a measured Internet capacity. `TrafficClass` remains metadata in the
single backend lane. Process-global GNS initialization is not rate-policy ownership.

`RunDedicatedServer` constructs the adapter with default configuration and the
session with `DefaultLimits()`. Server CLI accepts endpoint, content-provider,
residency, security-development and test controls, but no bandwidth profile.
`ServerHostConfiguration` is the appropriate trusted native composition seam:
it already distinguishes operator-supplied provider state from package/client
state. `EngineProviderConfiguration` is not a reason to put transport policy in
DataModel or content metadata. Scheduler per-peer queue bytes and its 256 MiB
manager ceiling are memory bounds; its per-tick bytes are not a rate limiter.

Self-hosted and platform/private deployments currently have the same inherited
GNS rate. A future platform launcher can supply this host input without changing
Node's content protocol. No Node/platform config is required to define a single
Server-process profile. Splitting a machine/NIC budget among multiple processes
remains the **operator's** obligation; this Engine cannot promise a node-wide
aggregate without a separate orchestrator contract. No secondary repository was
modified or needed for this finding. Studio, MCP, Telemetry, package authors,
clients and ordinary Luau do not select transport credit or service class.

## Profile semantics — PROPOSED, not wired into startup

Use an immutable native `ReliableServiceProfile` under `ServerHostConfiguration`
and pass its validated network portion to the owning session and adapter. No
serialized GSES field, public priority API or backend handle is required.
Install per-listener/connection backend values, not a process-global mutation
that unexpectedly changes another adapter's connections. Runtime updates require
drain/reconfiguration design; the first implementation should be startup-only.

| Model field | Meaning |
| --- | --- |
| `ConnectionRate` R | Reserved complete application-message byte service per second per admitted connection, including adapter bytes; not a throughput estimate |
| `AggregateRate` A | Reserved application egress per second for this Server process, separate from the NIC's wire budget |
| `MaximumConnections` N | Admission ceiling used for reservation feasibility, including bootstrap/handshake peers, not just currently Ready peers |
| `StructuralPermille` | Structural share S of R and A, strictly between zero and one; remaining service reserved for gameplay/control |
| `MaximumGroup` G | Maximum complete supported atomic message, including framing; at most 524,288 for current GNS |
| `PeerBurst`, `GlobalBurst` Bp/Bg | Finite structural credit caps, each at least G |
| `GameplayBurst` E | Bounded nonstructural reliable burst per peer; not permission to change Remote semantics |
| `PeerBacklog`, `GlobalBacklog` Qp/Qg | Separate admission exposure bounds, including reserved-but-not-yet-handed-off bytes |
| `QueueWindowMilliseconds` L | Conditional FIFO queue-service class, not an end-to-end RPC deadline |

Wire/encryption/packet headers, ACKs, retransmissions, incoming-request traffic,
GCHR/unreliable output and host-service interruptions need additional deployment
headroom. GNS's actual on-wire send setting is not automatically numerically
identical to an application-byte reservation. The initial implementation must
document that mapping and refuse a claimed envelope without supported headroom.
Short-term observed throughput never increases R, A or burst credit.

The executable **test model** validates necessary inequalities and finite arithmetic:

- `N * R <= A`, checked without overflow. R is a reservation, not an opportunistic
  ceiling. A future shared peak/minimum profile is a different contract.
- `G <= Bp <= Qp - E`; `G <= Bg <= Qg - N*E`; `Qg >= Qp`.
- `Qp <= R*L` and `Qg <= A*L` after explicit millisecond conversion.
- `1 <= N <= 4096`, matching the native connection ceiling; `0 < S < 1`.
- R fits the GNS signed integer setting; A is at most that ceiling times N.
- G includes all complete-message framing. Zero, oversized and overflow values fail.
- As conservative model safety ceilings, Qp/Qg may not exceed the existing
  64/256 MiB per-peer/manager constants. Reusing these **numeric safety ceilings**
  does not mean scheduler and backend occupancy are the same measurement. Total
  cross-layer memory still needs separate accounting and can be additive.
- The model accepts finite queue windows up to 60 seconds solely to bound its
  arithmetic input; this is **not** a supported gameplay latency recommendation.

An `IsValid()` result proves internal numeric consistency only. It does not prove
link capacity, root-motion/RPC health, arbitrary traffic liveness or deployment
qualification. Production validation must also ensure gameplay average demand
fits `(1-S)*R/A`, E fits the supported control/RPC messages, and the chosen L plus
RTT/service jitter/retransmission allowance fits the applicable request deadline.
Excess reliable demand retains the current finite terminal-failure policy; no
already-authoritative lifecycle is silently discarded on a healthy connection.

### Defaults and trust

Do not replace 256 KiB/s with an unexplained faster default. Omitted configuration
continues the legacy development behavior and must be reported as **unqualified**,
not as a production service guarantee. Future startup diagnostics should emit
bounded `[Network:ServiceProfile]` effective values, provenance/class and whether
the aggregate reservation and latency class are qualified; no credentials or
payloads. Development/local profiles are explicit test conveniences, not capacity
evidence. Self-hosted production and managed/private deployments require an
operator-specified A/R/N and measured network headroom. Customized profiles go
through the same validation, with no privileged bypass.

The fallback policy and effective-profile diagnostics are defined here but not
implemented. Existing production startup output and behavior remain unchanged.

## Aggregate egress and future fairness

Use hierarchical per-peer and server-wide structural credit, both based on
monotonic elapsed service time, not simulation ticks or flush count. One 3J group
needs both reservations. Bounded reliable gameplay/control consumes the remaining
reserved reliable service. GCHR and other unreliable traffic require additional
headroom in the encompassing deployment/NIC budget, not a second claim on R/A.
Ignoring those bytes would invalidate the capacity qualification. No bandwidth
budget can change Desired or Known.

500 peers reserving 1 MiB/s require **500 MiB/s** application aggregate, before
wire headroom. The model rejects that reservation against 64 MiB/s. A 64 MiB/s
deployment can instead reserve, for example, 131,072 B/s for each of 500 peers (62.5
MiB/s total), reduce N, or use a separately designed shared-peak service class.
It cannot grant all 500 a simultaneous 1 MiB/s allowance.

Future byte service must rotate generation-safe eligible peer identities before
global credit exhaustion. An eligible large group needs bounded reservation
progress: small groups must not consume every refill forever. Reserve/earmark
credit for the oldest fairly selected complete group, with at most one bounded
continuation/reservation per eligible peer, no copied peer-by-object candidate
matrix. Mutation/relevance invalidation, disconnect, epoch change or failed
encoding releases it. No Instance/session/Lua pointer is retained beyond its
owner. Final packing must revalidate cost/revision and fit both the unchanged
3J work allowance and byte allowance. The fairness/service-capacity proof is a
required implementation test, not claimed by this numeric model.

## Actual atomic-group measurements — MEASURED

`tests/ReliableEnvelopeContractFixture.hpp` uses the existing production
`PlanningCompletedGroups` counter under scoped `WorkCapture`. Every recorded
observation asserts **exactly one completed planner group**, encodes the resulting
frame with the actual codec, verifies Known did not advance, runs strict
`ReplicaApplier`, commits matching scheduler acceptance, and drains bounded plan
disposal. It does not infer a group from frame length or weaken a limit.

Measurements include GRPL's 36-byte header; add 32 bytes for GNS admission.
For one-sample rows, identical quantiles are a single observation, not a population
tail estimate. No names are padded in the representative rows.

| Isolated group / composition | Count | Bytes p50 / p95 / p99 / max | Operations/group |
| --- | ---: | --- | ---: |
| Ordinary canonical-style Part, parent already Known | 128 | 338 / 339 / 339 / 339 | 1 |
| Ordinary authoritative Part destruction | 128 | 45 / 45 / 45 / 45 | 1 |
| Player plus hard-referenced KinematicCharacter | 1 | 334 / 334 / 334 / 334 | 2 |
| Unknown ancestry closure, 32 Folders | 1 | 2,788 / 2,788 / 2,788 / 2,788 | 32 |
| KI-007 four reference replacements plus old-target removal | 1 | 157 / 157 / 157 / 157 | 5 |
| Fresh target recreation plus four reference restores | 1 | 440 / 440 / 440 / 440 | 5 |
| KI-007 four nil clears plus removal | 1 | 125 / 125 / 125 / 125 | 5 |
| Complete 32-Folder subtree eviction | 1 | 324 / 324 / 324 / 324 | 32 |

Separate legal-boundary cases deliberately use long names and are **not** typical
production size estimates:

| Complete group | GRPL bytes | Operations | Result |
| --- | ---: | ---: | --- |
| Player/Character, 60 KiB names | 123,183 | 2 | Actual single group, strict application/acceptance |
| Seven unknown ancestors, 64 KiB names | 459,292 | 7 | Below GNS complete-message ceiling |
| Eight ancestors, one name reduced by 644 bytes | 524,256 | 8 | Exactly 524,288 with adapter envelope |
| Eight ancestors, full 64 KiB names | 524,900 | 8 | Group fails the GNS-sized planner allowance without Known advance; valid under the larger GRPL codec ceiling |

Normal official Player/bootstrap is also observed by the existing hard-reference
fixture: its 1,867-byte/five-operation baseline contains **five planner groups**
in the measured run. It is not reported as an atomic-group percentile. Player/
Character may share a group or become two ordered groups depending on already-
Known prerequisites and identity order; the 123,183-byte case is a demonstrated
possible group, not a claim that every such Character requires one group.

These are isolated canonical schema/lifecycle fixtures, not a random authored
content survey. Actual mixed-group frequency distributions in the 200/500-peer
workload and official-network bootstrap groups are **not measured**. Existing
200/500-peer frame distributions remain in the preceding assessment. Target
recreation tests fresh ObjectIds and referrer restoration, not provider acquisition;
the previously established exactly-one provider acquisition/admission is unchanged.
No new production observer or queue was introduced. Each fixture retains at most
1,024 size/count samples and scoped fixed work counters, not an unbounded trace.

## Maximum legal versus supported group — SOURCE + MEASURED

The model has several different finite limits; none should be called a universal
atomic-group limit without its layer:

| Layer | Bound / implication |
| --- | --- |
| Protocol strings / identifiers | 65,536 / 256 bytes; WireValue is non-recursive and validates every string/scalar/reference |
| Native reflected properties | At most 1,024 per publication |
| Attributes | 64, name 100 B, value 4 KiB, aggregate 16 KiB/object |
| Tags | 64/object, 100 B/name; 1,024 distinct names/DataModel |
| Custom/extension schema | 64 definitions each; 64 properties/definition; inheritance depth 16; overrides capped at 128 and 32 KiB per category/object; schema payload 64 KiB |
| Protocol custom/extension maps | Bounded counts and strings; frozen-schema/live-state validation is additionally required, not bypassed by a syntactically valid DTO |
| Dependency closure | Depth 64, peer Desired <=65,536; references cannot recursively carry arbitrary nested payloads |
| Selected atomic work | Default peer quantum 512, hard maximum 4,096 operations including required fixups; larger groups explicitly fail |
| GRPL codec | 65,536 operations and **8 MiB** total frame including schema/header; bounded writer fails above it |
| Current GNS | **512 KiB complete message**, including 32-byte adapter wrapper; smaller negotiated limits still apply |

Thus the maximum **successfully encodable** GRPL group is bounded above by 8 MiB;
the maximum **current GNS-supported complete group** is bounded by 512 KiB, and
the exact latter ceiling is attained by the eight-operation fixture. The largest
possible frozen-schema authored dependency closure before these rejection gates
is not a useful deliverable guarantee and its tight raw byte supremum is
**not measured**. Valid object state can compose into a group that the selected
network deployment cannot support; today's bounded failure is real, not semantic
fragmentation or infinite deferral.

One existing integration mismatch must be accounted for in the next design:
`GameSessionConfiguration::DefaultLimits()` advertises 512 KiB **application**
messages while the adapter's 512 KiB limit includes its 32-byte wrapper. A future
profile must pass an effective application allowance no larger than **524,256**
to preparation and negotiation; the final 32 bytes of the present advertisement
are not an actual GNS send guarantee. No wire format change or larger buffer is
required; this task does not change that configuration.

## Oversized policy and low-rate compatibility

Choose **finite accumulated credit**, rather than a free oversized exception:

1. `g <= remaining peer/global credit`: reserve the entire group and admit only
   through unchanged 3J and scheduler acceptance.
2. `remaining credit < g <= G`: retain existing ready/pending semantic work and
   wait for fair credit accumulation. Caps must be >=G; the same full byte cost
   is charged once, regardless of transport fragmentation or flush count.
3. `g > G` or any negotiated/codec/operation limit: explicit bounded resource
   failure before acceptance. At startup, known incompatible required bootstrap
   fails startup. A later incompatible dynamic group uses peer-local structural
   failure. Never leave a live peer with falsely advanced Known.

Do not create a separate unlimited bootstrap exemption. The baseline schema table
and required closure count against its actual complete-message cost. A group
may be smaller than the enclosing frame; pack fewer complete groups, not a prefix
of a hard-reference or KI-007 group. A failure discovered after semantic world
commit is not a claim to roll back the authoritative world.

For full current GNS compatibility, configure G=524,288 complete bytes. A low-rate
deployment whose desired queue class cannot fund that must **reject the claimed
profile**, not silently lower G or discard legal content. Operators may explicitly
select a slower bounded compatibility class, increase proven reservation, or
reduce admitted peer count to fund the reservation. The laboratory G=2,000-byte
model is an experiment, not authorization to impose that content limit.

Accumulating a large idle burst can delay even small messages subsequently queued
behind it. Q and B, not median group size, define the worst case. At 256 KiB/s,
one maximum group costs **2.000 s** before path/jitter. The 123,183-byte group plus
adapter costs **0.47003 s**. No single FIFO admission policy can preempt either
after submission. With 4 KiB gameplay burst, a full-group Q=528,384 B needs at
least **2.015625 s** of ideal service; a 2.1-second test-model window is internally
feasible, but is **not** a passing Foundation 3L gameplay service class.

The 4 KiB reserve is only the small-probe workload. `MaximumRemoteFrameBytes`
already permits up to **262,144 application bytes** (32 arguments, strings up to
16 KiB), and manager-generated reliable traffic has separate finite rate bounds.
Using that codec ceiling conservatively for one Remote message requires at least
**E=262,176** with the adapter wrapper, before additional overlapping control/RPC
bursts. Then `Q >= G+E = 786,464 B` needs **3.000122 s** at 256 KiB/s. The model
rejects 2.1 seconds and accepts 3.1 seconds numerically; neither is an interactive
qualification. The exact attainable maximum Remote payload and its production
burst distribution are **not measured** here. Multiple outstanding responses
require a workload/burst bound and backpressure design, not merely space for one
message. Do not shrink legal Remote arguments or rates to make E=4 KiB appear
compatible. Sustained demand must also fit `(1-S)*R`; a burst reserve alone does
not fund throughput. This remains an explicit input to the next service-class
review.

## Quantitative envelope and limitations

For any elapsed interval t, future structural admission must satisfy both:

`Ap(t) <= Bp + S*R*t` and `sum Ap(t) <= Bg + S*A*t`.

Gameplay/control must fit its independently bounded reserve and burst E. The
ordinary 8,192 global operation cap and 65,536 planner-work cap remain separate.
Reservations must occur before Known acceptance and span scheduler/backend
handoff without double charging or loss of ownership. Accounting must distinguish
prepared reservations, scheduler bytes, backend pending bytes, sent-unacknowledged
bytes and retransmission exposure; `m_cbPendingReliable` alone is insufficient to
prove their sum or a strict memory high-water. Unavailable/invalid backend feedback
must fail closed for new admission, not be read as zero backlog.

A conditional FIFO queue bound is approximately `Dqueue <= Q/C + J`, where C is
a deployment-supported actual service lower bound and J covers bounded transport
and host service interruptions. Configured R is not evidence that C=R. End-to-end
RPC adds request-path service, RTT, handler and completion dispatch; sent-unacked
and loss behavior need separate supported assumptions. No zero-loss Internet
promise follows from localhost tests. Sustained total offered traffic must stay
below C and A for liveness; excess structure waits under existing finite
planning/backpressure bounds. Sustained gameplay overload uses bounded terminal
failure, not unlimited queue growth or permanent structural starvation.

The prior 50/75% matrix validated the small-message mechanism: all ten cases
delivered 905/905 reliable messages; at 256 KiB/s and 75%, RPC max was 52.82 ms,
structure 7.316 s (28% slower than the unpaced case). The 1 MiB/s configured
profile against 256 KiB/s actual service produced ~105 kB pending and 423 ms RPC,
despite feedback deferrals. This is why `Q/R` is conditional, not a measured
guarantee. Aggregate real-network service, strict backend accounting slack and
large-group FIFO experiments under the proposed production profile remain
**not measured**. This task does not modify the existing GNS matrix/scale observer.

## Lanes and next implementation gate

No lanes or ordering domains are implemented. Small-group same-stream service is
feasible; a full-compatible low-rate subsecond guarantee is not. If the product
requires full 512 KiB atomic groups **and** a 100 ms ideal queue window, it needs
at least **5,283,840 B/s** of per-peer service for the illustrative 4 KiB gameplay
burst, plus path headroom. That is beyond the prior 4 MiB/s matrix and cannot be
declared a safe Internet default. Reserving one full codec-ceiling Remote instead
raises that illustrative lower bound to **7,864,640 B/s**; larger concurrent
bursts raise it further. If that rate/convergence class is unacceptable,
a separate ordering-domain assessment must examine bootstrap/LocalPlayer,
publish-before-Remote, hard references, KI-007, control revocation and reconnect.
Independent ordering may improve unrelated traffic, never prerequisite-dependent
traffic. A lane is not a way to violate those dependencies.

Next: review the proposed single-stream admission design against the explicitly
chosen deployment/service class, implement trusted profile validation/effective
message limits and bounded reservation accounting, then test whole-group packing,
per-peer/global fairness, late sends, official Local/Node, physical shortfall,
overload/drain, client service and journal margin. Do not implement admission in
this architecture task. A future reservation model must not be confused with the
already-published diagnostic benchmark. Final security, current-source CI and
all Foundation service gates remain separate. Do not begin 3M.
