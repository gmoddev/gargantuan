---
status: blocked-on-product-workload-decision
owner: runtime-networking-and-runtime-host
last_verified: 2026-09-12
---

# Foundation 3L reliable gameplay workload contract gate

## Decision and source scope

**B — FOUNDATION 3L PARTIALLY READY; overload/recovery gate not passed.**
This contract-first assessment continues from published documentation HEAD
`cfb21c46937f7cd523aa28012cb1dd53bb51d060` on
`foundation/3l-content-availability`. Native source and fixtures are unchanged
from `5ada43a5773a96b1f0a97e9e6299baa6f420b762`.

The repository establishes reliable semantics, finite denial/resource limits,
approved latency targets and server capacity policy. It does **not** establish
the intended production payload/rate/burst/concurrency combination. Neither the
[accepted networking design](../../docs/src/content/docs/developing/networking-architecture.mdx),
[minimum game](../FutureArchitecture/MinimumUsableGame.md),
[Remote API](LuauRemotes.md), nor current fixtures supplies that missing product
requirement. A pickup/action game and typed scalar/reference messaging motivate
small control messages, but do not justify a specific general-purpose KiB or
messages/s promise. Large inventory replies, nested calls, broadcasts and
Character observer fanout need explicit workload assumptions.

Accordingly, no numeric qualified payload, arrival or concurrency policy is
selected, no new runtime limit is introduced, and no overload fixture is run.
This is the task's explicit missing-requirements stop, not a failed benchmark or
evidence that a feasible gameplay contract cannot exist. The tables below make
the unresolved decisions and the qualification procedure reviewable.

## Legal, qualified and overload

- **Legal** means a representation permitted by the codec. Session limits,
  authority, visibility, publication lifetime, admission and resource checks
  still apply; legal does not mean every send must be accepted.
- **Qualified** means a complete workload and deployment combination for which
  measured service meets RPC p95/p99/max **150/250/500 ms**, reliable Event ACK
  maximum service gap **250 ms**, and action-result maximum **250 ms**. Sizes
  alone, reserve arithmetic, parser tests and small-message observations do not
  establish that combination.
- **Overload** means demand outside an accepted workload envelope or above
  sustainable configured service. Ordinary latency targets need not hold there;
  finite resource use, explicit failure, correct state progression and defined
  recovery remain obligations. Traffic cannot be relabeled overload after a
  qualified test fails. Until the envelope is accepted, a codec-ceiling stress
  run is only an unqualified diagnostic.

The 262,144-byte Remote frame can remain legal without promising continuous
ceiling-size RPCs from every client. No smaller parser limit, delivery lane,
ordering domain, wire field or application priority class follows from this
distinction. [Deployment capacity](NetworkingReliableDeploymentContract.md)
and qualified gameplay demand are separate contracts.

## Producer inventory — source verified, sizes calculated

Frame bytes below include the named protocol header but exclude the **32-byte
GNS adapter envelope**. Count that envelope once per message in service budgets.
Packet/encryption/ACK/retransmission bytes require separate physical headroom.
Calculated sizes are source arithmetic, not new executed size measurements.

| Producer / direction | Legal shape and tighter semantic/resource policy | Existing fixture and production frequency behavior |
| --- | --- | --- |
| RemoteFunction request, either direction | GRMT <=262,144 B including 52-B header; <=32 arguments; each string <=16,384 B; scalar/typed values and scoped object references, no arbitrary nested table serialization | Official client sends `node-content-ping`: 74 B frame / 106 B with adapter. One outstanding call, waits for completion then `task.wait`; retained run requests 100 calls. Application may invoke repeatedly or nest calls within the finite limits below; no qualified larger size or fixed arrival rate. |
| RemoteFunction response, reverse direction | Same GRMT size/value limits; one accepted reply for a live request; application chooses result values independently of request size | `node-content-pong` plus double clock: 83 / 115 B. A small request can induce a large reply. Completion bunching and nested request/response combinations are not qualified. |
| Reliable RemoteEvent, either direction / server broadcast | Same GRMT limits, visibility and materialization checks; broadcast charges every recipient | Official probe string `materialization-probe` plus integer sequence: 83 / 115 B each direction (the Luau adapter encodes in-range integral numbers as int32). Eligible every fourth simulation callback with at most one pending echo; additional one-shot lifecycle strings. At 60 callbacks/s eligibility is 15/s, not a guaranteed actual rate. Application may fire repeatedly within shared caps. |
| Event ACK | No GRMT Event-ACK opcode or automatic application acknowledgement | The official ACK is the matching echoed RemoteEvent and consumes another Event's bytes/call budget. Backend ACKs are different. Record probe RTT and ACK-to-ACK service gap separately; do not substitute one for the other. |
| RPC error / cancellation, either direction | Error frame <=1,212 B (52+4+128+4+1,024); cancellation 52 B with no arguments | Generated on handler rejection/error or cancellation; official passing runs report zero RPC errors/timeouts. Repeated application cancellation and rejected requests consume generated-control budget; they cannot be ignored in workload totals. |
| Owner action request, client to server | GCHR 256-B codec guard; current fixed request 48 / 80 B; owner/control epoch and monotonic sequence validation; eight pending client actions and eight pending server actions per Character | Official `PackageLunge` request uses the fixed shape; one matched accepted result in retained runs. Application can request actions repeatedly; a full pending array rejects further work. The input-command four-per-tick limit is not an action-rate contract. |
| Action result, server to owner | GCHR fixed 40 B rejected or 116 B accepted with authoritative action state (72 / 148 B including adapter); no application argument payload | One result per processed valid action request, including semantic rejection. A full/unauthorized incoming queue need not produce that result. Supported action rate, overlap and handler cost remain undecided. |
| Character control bind/unbind, server to owner | Fixed GCHR 40 / 72 B, within 256-B guard; trusted control/lifecycle transitions | Owner creation, bind/unbind/replacement; not a periodic message. Application-driven Character lifecycle can create repeated transitions, subject to normal authority and resource limits. No qualified lifecycle-churn rate. |
| Reliable Character semantic state, server to materialized observers | StateFrame codec <=1,200 B, <=15 states; tighter configured/negotiated application limit (default 1,168 B). Current forced semantic path sends one compact state: 108 B without action or 180 B with action, plus adapter | Action transitions/endings, control changes and discontinuities can force reliable publication to **every materialized observer**. The ordinary 20/10/5 Hz replaceable stream and 60-tick absolute refresh do not bound this reliable fanout. Its actual official byte/rate distribution is **not measured** here. |
| Session handshake, client/server | GSES <=256 B codec guard; current Hello/Accepted/Ready frames 48/84/32 B, plus adapter; one legal state-machine progression per connection | Bootstrap competes as reliable Control. Reconnects create new lifetimes; total admitted peers are bounded, but qualified join/rejoin burst and frequency are unspecified. |
| Structural bootstrap/materialization, server to peer | GRPL shares reliable transport; complete group <=524,256 application B / 524,288 with adapter in profiled GNS; dependencies stay atomic | Charged to structural credit, not miscounted as free gameplay/control. Readiness and object-dependent Remotes can wait for accepted materialization. Retain 65,536 planning bound, exact 8,192 structural selection cap, and acceptance-only Known. |

Source: [GRMT encoder](../../src/network/RemoteProtocol.cpp),
[value encoder](../../src/network/BinaryCodec.cpp),
[Luau value conversion](../../src/network/RemoteLuau.cpp),
[Remote manager](../../src/network/RemoteManager.cpp),
[GCHR encoder](../../src/network/CharacterProtocol.cpp),
[Character producer](../../src/network/CharacterNetwork.cpp),
[GSES encoder](../../src/network/GameSessionProtocol.cpp),
[session composition](../../src/network/GameSession.cpp), and
[official fixture](../../tests/OfficialNodeHostVertical.ps1).
The [Remote microbenchmark](../../tests/RemoteBenchmark.cpp) also uses small
scalar/string messages, advances an artificial clock to avoid rate denial and
injects responses directly. Its iteration count is not a real arrival-rate claim.
Unreliable Character input/state and unreliable Remotes are outside reliable
message totals but still consume host/path capacity. Node content acquisition
uses the separate provider path; admitted content's later GRPL traffic is counted.

## Existing concurrency and denial ceilings

There is **no missing unbounded outgoing RPC-count surface** in the inspected
manager. [StartRequest](../../src/network/RemoteManager.cpp) checks both bounds
before allocating pending tracking:

| Resource | Existing bound; not qualified service |
| --- | --- |
| Outgoing pending RPCs / peer | Negotiated `MaximumInFlightRemoteRequests`; official default 1,024, native ceiling 4,096 |
| Outgoing pending RPCs / manager | 8,192, shared across peers and Remotes |
| Incoming handlers / peer | Minimum of negotiated request limit and 64 |
| Incoming handlers / manager | 4,096 |
| Request deadline | Default 10 s, maximum 30 s; not an ordinary latency target |
| Shared Remote call admission | 1,024/peer, 256/Remote/peer, 8,192/manager per one-second reset window; incoming and outgoing calls share the respective manager/peer counters |
| Generated responses/errors/cancellations | 4,096 messages and 32 MiB encoded GRMT bytes per manager per one-second reset window |
| Broadcast | 4,096 recipient submissions and 16 MiB encoded bytes per manager per one-second reset window, in addition to other applicable admission checks |
| Dispatch queue | 8,192 messages / 32 MiB manager-wide; deferred reliable messages also have existing count/byte/lifetime checks |

The [limits](../../include/gargantuan/network/RemoteManager.hpp) are independent
and all apply. Reset windows can admit traffic on both sides of a boundary;
they are not a smooth token bucket or a short-window latency promise. Unreliable
Remote calls also compete for the shared call budget. Count broadcast fanout,
reverse RPCs, nested requests and terminal messages at their actual destinations.
Cancellation/short deadlines do not immediately release incoming handler-work
ownership; the bounded work lease prevents churn from evading that resource cap.
[Remote tests](../../tests/RemoteTests.cpp) explicitly check a negotiated
two-request cap, rejection before tracking, rate limits and lifecycle behavior.
No new resource limit is justified by this inventory.

This count/lifetime boundedness is not a handler execution-time bound. The
current Remote contract explicitly lacks handler-specific Luau instruction
preemption. Arbitrary application handler computation cannot receive an implicit
latency guarantee; qualification needs an explicit handler/host service
assumption. A general script watchdog belongs to scripting policy and would be
a separate architecture decision, not a hidden networking benchmark limit.

## Required workload decision — values intentionally unresolved

The product/platform owner must select supported gameplay behavior; runtime
networking must cost it and the trusted host/operator must fund it. A platform
decision does not require a Node change: native ServerHost already owns the
deployment input. Prefer one common qualified GRMT frame-size limit unless
actual request/response requirements justify an asymmetry; do not add a large
message class merely because the codec permits one.

| Decision | Required content | Current status |
| --- | --- | --- |
| Payloads | Maximum encoded RPC request, response and Event bytes, whether their maxima may coincide, plus full-size application behavior | **Not selected**; larger qualified fixtures **not measured** |
| Per-peer arrivals, each direction | Message and complete-message byte rates, finite count/byte burst, window/replenishment rule, maximum peak duration | **Not selected** |
| Aggregate arrivals | Independent server ingress/egress rates and bursts; actual recipient fanout; simultaneous active versus merely connected peers | **Not selected** |
| RPC overlap | Qualified pending requests per peer and server, both directions, nested-call allowance and response-bunching bound | **Not selected**; resource ceilings above are already implemented |
| Engine traffic | Action mix, materialized observers, reliable semantic/control churn and joining peers sharing the nonstructural reserve | **Not selected** |
| Path and host | Client uplink service, server ingress/egress, RTT/loss/retransmission, host tick/handler/client-application allowances and measurement start/end | **Not selected** beyond the scoped official observations and server profile |
| Materialization dependency | Whether qualified RPC timing includes waiting for a newly referenced object's complete 3J admission, and its finite supported demand | **Not selected**; no clock-start change may hide that wait |

A compact contract can use, for every direction and every interval of length
`T > 0`, admitted workload bounds `Bytes(T) <= BurstBytes + ByteRate*T` and
`Messages(T) <= BurstMessages + MessageRate*T`, with independent peer and server
totals. Specify maximum frame sizes as well. A transaction is not one message:
request, response, probe echo, error, control and every fanout recipient consume
their own budgets. If a peak rate is allowed for a finite window, specify both
and its replenishment; a one-second average alone permits an uncontrolled burst.
This is a proposed specification form, not an implemented gameplay shaper or
permission to choose token values from passing tests.

## Deployment compatibility preflight

Keep `N*R <= A`, structural share `S <= 0.75`, finite structural bursts `Bp/Bg`,
and gameplay/control reserve `(1-S)*R` per peer and `(1-S)*A` globally. Qualified
steady nonstructural traffic must fit both reserves, including engine fanout.
The full structural group remains atomic. Player egress and server ingress need
their own funded service; ServerHost R/A does not raise the Player send rate.

The following are **calculated candidate deployment dimensions**, not qualified
workloads. Use 1 and 32 peers from the existing GameSession benchmark and
200/500 for the retained stress architecture. For this arithmetic only, take
R=8 MiB/s, S=0.75, A=N*R and the existing CLI backend ceiling 2R per connection.
These reserve N connections, including bootstrap; an active subset does not
permit reducing A below N*R under the present profile.

| Admitted N | Minimum A, MiB/s | Gameplay/control reserve, MiB/s aggregate | Sum backend ceilings, MiB/s | Qg bytes | Qg/A, ms |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 8 | 2 | 16 | 1,048,640 | 125.008 |
| 32 | 256 | 64 | 512 | 17,303,552 | 64.461 |
| 200 | 1,600 | 400 | 3,200 | 105,394,688 | 62.820 |
| 500 | 4,000 | 1,000 | 8,000 | 262,700,288 | 62.633 |

Here `G=524,288`, `E=262,176`, `Qp=G+2E`, `Qg=G+2NE` and
`Bp=Bg=G`. Qp/R is 125.008 ms for every row, within the profile's 150-ms queue
allowance after its 100-ms nonqueue allowance. Qg remains within 256 MiB even
at N=500. These rows pass the existing necessary backlog arithmetic, **not**
client, CPU, p95/p99, NIC or fairness qualification. In particular, the 500-peer
reservation is 4,000 MiB/s application service and 8,000 MiB/s summed backend
ceilings; the stress architecture does not prove an operator has that capacity.

Additional necessary checks before admitting a workload:

1. Both byte and call-count totals must fit. For example, hypothetical 500 peers
   completing 10 RPCs/s each would require 5,000 server responses/s, exceeding
   the existing 4,096 generated-message cap even with tiny responses and ample
   transport bytes. This is a rejected example, not a proposed product rate.
2. E is admission headroom for one ceiling Remote, **not** a measured burst
   obligation or an enforced aggregate gameplay bucket. A chosen mix must fit
   peer/global burst exposure including simultaneous completions and Character
   fanout; otherwise reject or explicitly redesign the contract before testing.
3. With service `C`, continuing rate `r<C`, burst `b` and existing relevant
   backlog `q`, `(q+b)/(C-r)` is a conservative fluid drain estimate; FIFO
   residence also includes work already accepted ahead and nonpreemptible
   structural groups. Apply peer, aggregate and client bottlenecks. Measured
   service interruptions and scheduler/backend behavior must validate the model.
4. RPC time includes client request transmission, both endpoint queues, handler
   execution, response transmission and client completion. The default Player
   backend is 262,144 B/s: one 262,176-B complete Remote needs about **1,000.122
   ms** of ideal serialization at that rate alone. Continuous ceiling traffic
   cannot satisfy the unchanged 500-ms max/250-ms p99 targets. Backend token
   bursts do not make that a sustainable qualified workload.
5. Qp/R plus 100 ms is about 225 ms and does not prove the stricter 150-ms RPC
   p95 or budget a substantial uplink separately. Reserve the complete round
   trip allowance once; do not give every direction a fresh 100-ms allowance.
6. The deployment validator leaves default 256/512 KiB/s and 1/2/4 MiB/s
   full-group profiles unqualified. A higher class must be explicit when
   required. Do not weaken the targets or fund overload by increasing buffers.

**Result:** server candidate arithmetic is compatible; complete mandatory
gameplay-profile compatibility is **not established** because the workload,
active-peer mix and client/path/host budgets are unresolved. No mathematically
impossible combination is accepted as qualified.

## Canonical qualification matrix — specification pending decision

These seven fixture purposes are fixed; their missing workload parameters must
come from the accepted decision above before implementation. They are not seven
new service classes or a Cartesian product of all dimensions.

| Fixture | Required setup | Question / gate |
| --- | --- | --- |
| Ordinary small | Preserve official small RPC, echo and action probes with the approved arrival schedule; 1 peer, Local and Node | Does the existing ordinary service remain healthy? Prior evidence passes its recorded scope; no new run. |
| Qualified upper payload | Exact approved request/response/Event maxima and approved pairings; at least one request-heavy and one response-heavy boundary | Does upper qualified size pass unchanged latency gates without hiding uplink cost? **Not measured.** |
| Qualified burst | Approved count/byte burst and window, include coincident responses, echo, actions and reliable Character state | Are queue exposure and service bounded at the supported burst boundary? **Not measured.** |
| Concurrent RPC | Approved peer/aggregate overlap, nested calls if supported, delayed response completion within the handler allowance | Do overlapping requests terminate exactly once and meet service without leaking tracking? **Not measured.** |
| Mixed structural | Qualified gameplay plus dependency-complete streaming and supported lifecycle churn; Local and Node providers | Does admission defer structural demand while gameplay passes, preserving Known and selection/planning bounds? **Not measured.** |
| Multi-peer aggregate | 32-peer moderate case and 200/500 stress dimensions, explicit active subset/all-active scenario, funded R/A/N and observer fanout | Do peer gaps, latency and convergence spread pass without treating connected peers as all codec-max senders? **Not measured.** |
| Overload then recovery | Supported baseline, explicit demand above accepted byte/count/burst or structural capacity, then return below sustainable capacity | Do resources remain bounded, failures stay explicit and surviving/replacement lifetimes recover and converge? **Not measured.** |

For overload, fix offered demand and phase duration before the run; record actual
admitted/rejected work. Separate excess structural demand with qualified gameplay
from deliberate gameplay overload. A disconnected peer is a terminal outcome,
not evidence of that peer's convergence; report replacement bootstrap separately.
Define finite recovery deadline and acceptable terminal policy in the workload
decision, not after seeing drain results.

Retain per-phase RPC p50/p95/p99/max, Event RTT and ACK gap, action-result latency,
pending reliable bytes, peer/global credit and reservations, structural deferrals,
pending groups/ages, memory high-water, per-peer fairness, recovery time and
convergence. Report offered, accepted, completed, rejected and timed-out counts;
do not exclude failed calls to improve latency percentiles.

## Journal and validation gate

The historical retained margin **170 / 16,384 entries** is unchanged. Supported,
overload and recovery journal production rate, oldest required entry and owning
peer/consumer, minimum margin, low-margin duration, and correlation with deferred
groups/transport admission are **not measured**. Collect them on the same timeline
as service/backlog. Diagnose valid delay, unfair service, stale/lifecycle ownership
or configuration insufficiency before proposing any retention change. Do not
increase journal capacity to manufacture a passing result.

The [prior overload checkpoint](ReliableOverloadQualification3L.md) and
[validation ledger](ContentAvailabilityFoundation3L_3Validation.md) retain official
Local/Node RPC p99 72.058/74.723 ms and terminal successful native/sanitizer CI at
`5ada43a57`. This assessment is documentation and source arithmetic only: new
native tests, overload/recovery, security scan, client/scale guarantees and
production journal qualification are **not measured**. Later documentation-HEAD
CI must be reported independently; old green runs do not certify a new HEAD.

Next task: product/platform and runtime-host owners accept the small common
payload or justified asymmetric payload policy, both-direction rate/burst and
RPC overlap, active-peer/observer mix, and client/path/handler allowances. Then
validate that precise contract against mandatory deployments, implement only
any demonstrated missing bound, and run this matrix and journal qualification.
Foundation 3L stays partially ready until these and its independent current-source
security, client/scale and terminal-validation gates close. No 3M or merge.
