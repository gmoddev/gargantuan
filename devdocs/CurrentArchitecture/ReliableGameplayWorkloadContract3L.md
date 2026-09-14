---
status: engine-default-selected-partially-qualified
owner: runtime-networking-and-runtime-host
last_verified: 2026-09-13
---

# Foundation 3L reliable gameplay workload contract gate

## Decision and source scope

**B — FOUNDATION 3L PARTIALLY READY; qualification remains evidence-gated.**
The engine-owned defaults below supersede the September 12 missing-product-
decision stop. The September 13 task explicitly authorizes conservative defaults.
Initial local HEAD, upstream and remote were all
`b866a89e741b4b4df3419c74856d293b650bf475`; its native and dedicated GNS sanitizer
workflows both completed successfully before source changes.

These are workload assumptions for qualification, not new parser restrictions,
application traffic shaping, or a claim that every legal application qualifies.
The inventory distinguishes the existing hard safety limits from this smaller
ordinary service workload. Measured failures cannot be reclassified by silently
reducing these defaults.

## Selected engine default

All byte budgets include the 32-byte GNS adapter envelope. A payload ceiling
below means the entire encoded GRMT frame, including its 52-byte header.

| Quantity | Selected qualification default |
| --- | --- |
| RPC request, response, reliable Event and echoed Event | **16,384 B encoded frame**, 16,416 B complete message; the response may equal the request ceiling |
| Per-peer arrivals, independently in each direction | For every interval T seconds: **Bytes(T) <=20,480 +32,768*T** and **Messages(T) <=16 +64*T**, including engine control, actions, responses and every reliable observer recipient |
| RPC overlap | **Four outstanding requests per peer per direction**, 64 per server manager aggregate; nested calls consume the same allowance |
| Actions | At most two requests/s per owner, burst one, one unresolved action; result and semantic observer fanout count against shared budgets |
| Control lifecycle | At most one Character bind/replacement per peer per second, burst one; one new connection progression per second aggregate during steady gameplay |
| Moderate aggregate | N=32 connected, at most eight active; aggregate each direction **256 KiB/s, 160 KiB burst, 512 messages/s, 128-message burst** |
| High peer-count stress | N=200 or 500 connected, at most 16 active ordinary producers; aggregate each direction **512 KiB/s, 320 KiB burst, 1,024 messages/s, 256-message burst**; all-recipient fanout still charged |
| One peer | The per-peer bound; aggregate outstanding at most four |
| Host/path assumption | 60 Hz service, no unbounded/yielding handler work; <=5 ms handler completion; combined path/host/client nonqueue allowance <=100 ms; client backend remains 256 KiB/s, server R=8 MiB/s and backend=16 MiB/s |
| Dependencies | Qualification begins only with published Remote identities and already materialized argument references. New dependent-object waits are measured from invocation and are outside this ordinary latency promise until separately qualified. |
| Recovery deadline | After 480 offered service opportunities (at least eight seconds; longer under host saturation), ordinary service returns within 20 seconds and subsequent ordinary probes satisfy unchanged latency targets. For pooled service, complete structural convergence uses the independent [retained-work bound](PooledReliableServiceRecoveryContract3L.md#structural-convergence). |

Idle peers are not free: incidental reliable state/control bytes and broadcast
recipients count against the aggregate limits. Active fraction alone does not
bound fanout. Unreliable inputs/state and content-provider IO require additional
path and host headroom. A host unable to fund N*R cannot claim this deployment
merely because most peers are idle; 200/500 dimensions are scale stress, not an
assertion of commodity-NIC service capacity.

The burst is shared, not multiplied by concurrency. Four simultaneous 16 KiB
requests exceed it. A canonical concurrent burst uses four 3 KiB requests plus
one 3 KiB Event: 15,520 B, leaving 4,960 B for control/actions. A single full-size
message leaves 4,064 B. Requests and responses are charged independently; a
handler that bunches replies must keep egress within its own burst.

Rationale: 16 KiB accommodates ordinary typed game state while staying one
sixteenth of the legal frame ceiling. At the unchanged 256 KiB/s client backend,
the entire 20 KiB burst serializes in 78.125 ms, and its fluid drain bound at
32 KiB/s continuing demand is 89.286 ms. This leaves useful, but not guaranteed,
headroom in the fixed latency gates; the server FIFO and complete structural
group must still be measured. Per-peer steady gameplay is only 1/64 of the
2 MiB/s server reserve and 1/8 of client backend capacity. Four RPCs support
independent gameplay operations without treating the 1,024 storage ceiling as
a service promise. Aggregate calls and responses remain below existing manager
limits, including the 4,096 generated response/control messages/s and 32 MiB/s
generated byte ceilings. These engineering arguments precede measured pass/fail;
they do not turn capacity arithmetic into latency proof.

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

## Producer inventory — source verified, historical fixture sizes

Frame bytes below include the named protocol header but exclude the **32-byte
GNS adapter envelope**. Count that envelope once per message in service budgets.
Packet/encryption/ACK/retransmission bytes require separate physical headroom.
Calculated sizes are source arithmetic, not new executed size measurements.
The fixture-frequency column records the pre-selection baseline. Its statements
about unselected workload values describe that earlier fixture; the selected
table above supplies the current qualification assumptions for every producer.

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

## Applying the three limit classes

The inventory's codec and hard safety ceilings remain unchanged. The selected
table supplies the separate service limit for each producer. Applications may
control Remote sizes/rates and request Character actions or lifecycle changes;
their ability to encode a message does not bypass authority, pending-request,
dispatch, scheduler or backend bounds. Legal overload may receive explicit
rejection or terminal disconnect; accepted live-lifetime work must not silently
disappear. Report these outcomes separately from successful completions.

The workload inequalities are qualification assumptions, not an implemented
gameplay shaper. Qualification fixtures must account for request, response,
echo, action and every fanout recipient. Neither a one-second average nor an
RPC-only tally establishes the complete workload boundary.

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

**Result:** the selected rates and bursts fit the mandatory server reservation
and unchanged client ceiling mathematically. End-to-end qualification remains
measured: Qp/R plus the nonqueue allowance does not prove the RPC p95 gate.

## Canonical fixture matrix

`gargantuan_game_session_real_transport_tests --reliable-workload` exercises
real GameSession bootstrap, exact GRMT encoding, native Remote dispatch, matched
response/echo payloads, Luau-authorized Character actions, mixed structural
application, deliberate overload and recovery. The fixture keeps the Character
on a floor and holds trusted structural relevance at the test region; the
original short lifecycle mode remains separate.

| Case | Demand | Purpose |
| --- | --- | --- |
| Small | 128-B RPC and echoed Event every eight service opportunities | Sequential baseline with action results |
| Upper | 16,384-B request and response; equally sized Event/echo halfway between RPC arrivals; period 80 opportunities | Symmetric upper encoded-frame ceiling |
| Burst/concurrent | Four 3,072-B RPC requests plus one 3,072-B Event every 40 opportunities | Simultaneous requests and reply bunching within the shared byte burst |
| Mixed | Same gameplay; anchored Parts enter fixed relevance every four opportunities | Real structural acceptance and client application during gameplay |
| Gameplay overload | 16 concurrent 16,384-B RPCs and upper-size Event probes | Legal traffic beyond qualified concurrency/burst; terminal accounting and recovery |
| Structural overload | 32 materialized Parts, 16 names of 24 KiB changed each opportunity; small gameplay | Stress complete-group admission and journal retention without resizing the journal; actual host-paced throughput must be reported |
| Mixed overload | Same structural changes and overloaded gameplay | Shared FIFO pressure and recovery |
| Recovery | Small ordinary traffic after demand ends and accepted work drains | Ordinary latency restoration within a fixed 20-second drain/convergence deadline |

Service opportunities are paced at least 16.667 ms apart; report actual elapsed
time and offered rate, not an assumed 60 Hz under host saturation. Action
commands are eligible once per 60 opportunities with one pending. Completion
latency includes the fixture-to-Luau dispatch interval. Overload is run only
after the qualified cases pass. Rejected calls are reported and do not become
successful latency samples.

`--reliable-workload-32` provides 32 actual GameSession clients sharing one
server and manager, eight active qualified RPC producers, all-active overload,
then ordinary recovery. Per-peer accepted/completed/error counts and latency
distributions prevent an aggregate mean from hiding an unserved peer. This
does not by itself qualify all 32 peers at maximum burst or 200/500 peer host
performance.

`--reliable-workload-32-structural` preserves the combined aggregate structural
stress that exposed [KI-008](GnsPacketSequenceAttribution3L.md). The unchanged
workload passes overload and recovery after the narrowly attributed GNS timer
fairness correction, and is now included in GNS sanitizer CI. Overload latency
is reported separately from ordinary qualified targets; the workload is not
reduced to avoid the failure.

## Journal and client observations

The read-only `GameSessionTestAccess::GetJournalRequirements` diagnostic reports
the catalog cursor and every live peer cursor with its connection identity and
prepared-commit presence. These are the coordinator's actual raw-journal readers.
Dependency/planning revision stamps and publication watermarks do not themselves
read old journal entries and must not be reported as retention owners. Other
consumers, such as a simultaneously attached EditorHost, require separate
qualification; this dedicated-server fixture has none.

The fixture reads the tail and oldest retained sequence without copying journal
payloads. It reports production, retained high-water, required high-water,
minimum margin, and the owner/sequence at that margin. Capacity remains 16,384.
Required-entry age starts at the first post-step observation, so it can
underestimate commit-to-release retention by one service interval. Report that
sampling limitation. Remote offered/admitted byte counters exclude engine
control and action fanout; they are not complete gameplay arrival accounting.
Transport-deferral duration and correlation must be evaluated on the same
timeline; a full retained ring is not evidence of an endangered live reader.
Historical 170/16,384 evidence is not promoted to production qualification.

RPC completion and Event echo validate client semantic application. Structural
convergence is the replicated instance's current value on the client. No
rendered-visibility guarantee follows from the headless fixture. Existing client
decode/application counters and the whole-world preflight concern remain
separate from presentation/rendering.

## Evidence status and remaining gates

Numeric defaults are selected; the [qualification report](ReliableGameplayQualification3L.md)
records executed cases, scope and the unresolved combined aggregate failure.
Unavailable evidence remains **not measured**. Required final validation includes MSVC Release,
Linux Clang ASan/UBSan/LSan, dedicated GNS, official Local/Node, admission and
relevant scale/lifecycle regression fixtures, documentation build, and terminal
published-source CI. Independent security and client/scale gates remain open.
Foundation 3L is not Ready; no 3M or merge is authorized by this contract.
