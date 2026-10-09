---
status: review-required-non-normative
owner: runtime-networking
evidence_date: 2026-10-09
---

# Foundation 3L — bounded networking architecture review

**FOUNDATION 3L — NETWORKING ARCHITECTURE REVIEW REQUIRED**

No qualified correction emerged within the two-strategy investigation. Stop
implementation and physical retries for human review. This document proposes
options; it does not adopt a larger redesign or change canonical F1. KI-006
remains OPEN, Foundation 3L B — PARTIALLY READY, Node32 unrun. No 3M or merge.

## 1. Exact remaining failure

Qualified native `c916b7d4f868177ae00b4b05dacfa7b8dbb1f563` reached actual Local32
in physical run `9953fd84-0d6f-444a-93eb-ff8efa1d96ce`, lifecycle
`981fbecc-0596-4f90-88b6-47d0cbb2df1c`. Of 7,133 completed Ready grants, 21
fail recurring F1 across 16 peers. Maximum is **96,951,490,816 byte-us**, against
**18,025,216,000**; finite shortfall is zero. All 1,994,813,588 accepted bytes
first-send, ACK and retire. Three recovery cases pass. These independent
successes do not erase F1 failure. The physical first-failure token/interval and
OS/software cause are NOT MEASURED.

Separate-process worker-loopback witness `native-f1-452aa71d-9953c` fails 117 of
6,905 completed grants across all 32 peers, maximum **82,736,720,768 byte-us**,
finite shortfall zero. All 1,928,005,711 B first-send/ACK/retire. First retained
Main-observed certificate: peer 3:1, token 2373, W=295,256 B, activation
1027058150896 us, first-send 1027058151043, completion 1027058169635. Its exact
maximum is 27,393,921,280. First violating retained checkpoint spans 4,068 us
with 50,028 B of intervening unique service, yielding 18,221,714,688 byte-us.
Its largest adjacent send gap is only 589 us: recurring under-rate service can
fail without a >1.074-ms blackout.

A fixed reconstruction of the retained native grant certificates establishes
three genuine common-four intervals. Time starts after all native events at
the last first-send timestamp and stops before events at first completion.
No exhausted member is charged afterward.

| Peer generations | Common interval | Unique bytes inside | Maximum pool deficit, byte-us | Bound 72,100,864,000 |
| --- | ---: | ---: | ---: | --- |
| 3:1–6:1 | 15,923 us | 982,368 | 95,947,087,872 | FAIL |
| 8:1–11:1 | 12,159 us | 761,790 | 70,936,894,784 | PASS |
| 18:1–21:1 | 8,333 us | 405,909 | 162,366,100,480 | FAIL |

Only the first failed retained grant per peer exists; other overlaps are NOT
MEASURED. This is diagnostic native evidence, not physical/provider acceptance.
Reconstruction SHA256: `732a4fb8e4250709d96bfb254c0a501b93b135e92872ed9b864e3c76d69672c3`.
Source raw log: `e0071212a046f4f495df91d1d3d59f90a016bb6e8edc0bd6b1295f25a07a6c46`;
original evidence index: `382a4d57a490b73dd9d801d0895283e6a035e0d4e99060bfb7e3f0f4255a00c4`.

## 2. Scheduling versus execution capacity

Strongest supported classification is **insufficient demonstrated aggregate
execution capacity in the current serial service-loop configuration**. This
is not a theorem that the hardware, all Windows environments or GNS's protocol
cannot satisfy F1. Sufficient capacity plus an ordering defect is not proven;
unavoidable platform jitter is not established as the cause.

The loss-free 40.13-second `native-f1-etw-window-e51-01` sample contains the first
failure observer and its 53,987.7-us native history. Service-thread scheduled
residency is 53,975.1 us, with only 12.6 us off-CPU. The 175 token-bearing
four-packet callbacks cover 700 native packets /821,978 native B in 12,545.8 us,
without observed off-CPU time. Separate interrupt analysis reports 32 us DPC,
zero ISR on their CPU. Deliberately overcharging 272 us for interrupts and
reporting precision still leaves about 63.87 MiB/s of native bytes, before
structural attribution and the rest of the actor's work. This is limited
diagnostic cost, not production WCET; loopback and instrumentation differ.
The later recovery exception lost that run's grant certificate, so an exact
ETW-to-failed-grant curve is NOT MEASURED. The minimal export correction is
already implemented; this investigation did not launch another farm.

One private four-grant component sample attributes 78.3% of sender elapsed
time to the synchronous raw UDP call. The prior WSASendTo substitution changes
that cost by only about 3.87% in one A/B pair, with identical packet/wire counts
and increased peer/pool deficits. It was rejected and rolled back before this
bounded investigation; it is not a third strategy or a qualified correction.

An isolated CPU model directly compiles the unchanged production
`FiniteGrantServiceCurve.hpp` (SHA256
`60742da729d878799fa2c6b37b7fe98b1c508231c4e127312481cf0299e3afda`).
Five expected outcomes pass: tiny 77 B, boundary 1,258 B and four 512-KiB grants
with illustrative 10-us packet cost pass; 20-us cost fails recurring peer/pool
service despite zero finite shortfall and maximum 80-us gap; a 1,302-us injected
blocking interval also fails. The two costs are synthetic inputs, not fitted
constants or measured native capacity. Packet eligibility is a conservative
no-banked-credit model, not the complete pinned GNS pacer. EDF failure is not
an optimality/infeasibility proof. ACK, retirement, protected service and real
native I/O are NOT MEASURED by this model. It adds no production verifier.
Model output SHA256: `66e3509b1a31c953c889b15cace9b3229b69b4e8b1a7e0866e72d31bf2eb23e5`.

## 3. Two strategies evaluated, and exact trial scope

**Neither strategy was implemented in production or run as a corrected native32
candidate.** Source counterexamples prevent a defensible narrow patch. The
private five-case model is not a substitute for that trial. These are failed
bounded-correction evaluations, not fabricated results from two native runs.
No timers, packet quanta, F1 bounds or production constants were tweaked.

### Strategy 1: deadline-aware selection in the current actor

For an unfinished grant, current F1 yields a last-safe finite service time:
`activation + floor((finiteIntercept + firstSentBytes*1,000,000)/R)`.
After first-send, the running deadline is
`lastObservation + floor((B-currentRunningDeficit)/R)` while healthy. The
earlier deadline matters; neither maximum-ever deficit nor token presence is
a valid substitute. Checked arithmetic and native/F1 clock conversion are
required. These follow the existing equations and add no allowance.

The current thinker heap selects wake eligibility, not service completion
deadlines. Connection callbacks bundle SNP work, ACK/retry, statistics,
keepalive and transport maintenance. Native SNP folds those reasons into one
next-think time. Due-at-entry fairness prevents socket-read starvation but does
not create packet-processing capacity. A token can survive through ACK wait.
The ordinary DATA pacer provides an upper cap and refunds ACK/control; it does
not supply a protected-service lower-bound ledger.

Consequently, prioritizing token-bearing callbacks or changing a generic heap
key cannot distinguish legitimately deferrable DATA from urgent ACK/control.
It can harm FIFO delivery, retirement and reserves. Genuine EDF needs separated
eligibility/reason metadata, protected-work deadlines and an execution-feasibility
model. No evidence-supported bounded local patch closes that gap. **Rejected
before native trial.** Source anchors: `ApplyServiceFairness.cmake`,
`OrdinaryWirePacer.hpp`, `FiniteGrantServiceCurve.hpp`, and pinned connection
`Think` / `SNP_GetNextThinkTime`.

### Strategy 2: bounded egress batching/offload

The most coherent candidate keeps GNS's protocol and gives immutable datagrams
to four persistent egress workers, derived from MaximumDrainGrants. A proposed
process-wide pool has at most four packet/result slots, at most one per
connection, and does not reuse a slot until actor commit or terminal
reconciliation. This bounds copied datagram storage to 4*1,300=5,200 B;
transaction/reference/ACK metadata needs separate explicit bounds. This is
proposed I/O backpressure, not a change to grant admission. Admission is
GameSession-owned; simultaneous sessions may have more than four accepted
grants, so worker count alone proves neither the storage bound nor multi-session
service capacity. That ownership/funding scope remains a design obligation.
Synchronous worker calls can record the same positive-return service
clock boundary as today's raw path. This is a possible architecture, not a
proven performance benefit.

It cannot be implemented by moving the current raw helper onto threads:

- Raw helpers require the global lock and access configuration, simulation,
  tracing and shared socket state. Workers need an OS-only operation with frozen
  arguments and a retained socket lease, not a connection pointer.
- SNP serialization already moves message/segment ownership and changes pending
  and sent-unacked counters. The packet/header/encrypted buffers are stack-local;
  UDP sequence/statistics changes precede the syscall. A pending transaction
  must distinguish preparation from native success and preserve failed-attempt
  retry/reference semantics.
- The ACK decoder currently finds packets only in the committed in-flight map.
  A real ACK can beat result publication. It must not be lost or free pending
  references; a remote ACK cannot manufacture native success. Draining results
  before receive is insufficient to eliminate the race.
- Feedback observes a copied curve at query time. An earlier unpublished worker
  success can make that copy report a false latched failure/max, then later
  truthful maxima appear to decrease. Advancing the actor's real curve past that
  success would also violate timestamp monotonicity. An unresolved evidence
  frontier must be explicitly unqualified, then replay the actual outcome and
  evaluate the entire elapsed interval. Freezing apparently healthy feedback
  would silently pause F1 and is forbidden.
- Same-connection sequence output, including ACK/control, needs ordering while
  a packet is unresolved. Socket/shared-owner destruction and sender purge must
  defer until results are reconciled. Close must stop assignment, resolve owned
  operations, release leases and join without holding a lock workers need.

Pinned sockets are already nonblocking on Windows and POSIX; the proposal does
not introduce buffer-credit waiting. That does not prove worker dispatch or
actor turnaround. At optimistic Q=1,248 B, each 16-MiB/s grant needs approximately
one packet every 74.4 us. The existing stack lifetime, ACK map, global ownership,
snapshot projection and teardown require a broader redesign. **Rejected as a
narrow correction before native trial; no larger redesign implemented.**
Relevant worker source: `steamnetworkingsockets_socketthread.cpp`,
`steamnetworkingsockets_snp.cpp`, `steamnetworkingsockets_udp.cpp`; repository
`ReliableServiceFeedbackBridge.cpp` and `PooledReliableServiceFeedback.hpp`.

## 4. Comparative implementation evidence

These are techniques, not equal-F1 guarantees or benchmark rankings. Current
external source reads were pinned where practical; Epic evidence is public API
and technical documentation, not inspected licensed engine implementation.

| Implementation | Established behavior | Implication for this investigation |
| --- | --- | --- |
| [Valve GNS at the pinned revision](https://raw.githubusercontent.com/ValveSoftware/GameNetworkingSockets/2cb93a06350bb065db53abdb0d87cf297e0bfd34/README.md) | ACK-vector reliability, fragmentation, AES-GCM and weighted/priority lanes. Our inspected integration runs serial native service work under the global lock. | Lanes address head-of-line behavior within a connection, not aggregate CPU reservation. Changing lanes also changes ordering domains; it is not a safe automatic fix. |
| [ENet design](https://raw.githubusercontent.com/lsalzman/enet/5a9c537fd464b3c6d3c55e1d3bd47588faf71b42/docs/design.dox), [Windows send](https://raw.githubusercontent.com/lsalzman/enet/5a9c537fd464b3c6d3c55e1d3bd47588faf71b42/win32.c) | Independent sequenced channels, reliable windows/retries, aggregation of protocol commands and ACKs into packets, bandwidth/throttle control. Windows issues a gathered synchronous send per datagram. | Command aggregation is not multi-datagram syscall batching. Simpler ownership and throttling do not establish a 16/64-MiB/s lower service bound. |
| [Godot channel documentation](https://docs.godotengine.org/en/stable/tutorials/networking/high_level_multiplayer.html#channels), [pinned integration](https://raw.githubusercontent.com/godotengine/godot/232e6b14abf6e590b41d60f11704e42de22bff72/modules/enet/enet_multiplayer_peer.cpp) | Regular `service(0)` polling and distinct reliable/unreliable channels. Godot's ENet modifications support IPv6; [ENetConnection](https://docs.godotengine.org/en/stable/classes/class_enetconnection.html) exposes DTLS setup. | Separating control/gameplay/bulk helps head-of-line behavior, but requires an explicit cross-channel ordering decision and does not prove CPU capacity. |
| [Unreal connection API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/OnlineSubsystemUtils/UIpConnection), [replication flow](https://dev.epicgames.com/documentation/en-us/unreal-engine/detailed-actor-replication-flow-in-unreal-engine) | Optional send tasks have prerequisite ordering, shared socket ownership, protected result storage, deferred cleanup and waits. Replication prioritizes relevant actors and defers work on saturated connections. | Send offload requires real ownership/result/lifetime machinery. Tick-level actor deferral is not permission to defer an already accepted F1 grant. No same-F1 claim follows. |
| [Quake III snapshot sender](https://github.com/id-Software/Quake-III-Arena/blob/dbe4ddb10315479fc00086f08e25d968b4b43c49/code/server/sv_snapshot.c#L480) | Per-client message-size/rate scheduling and next-snapshot times; fragments are serviced before a new snapshot. Unacknowledged server commands are repeated with snapshots. | This bounds/staggers offered work in a frame-driven protocol; it does not supply this finite-grant active-drain guarantee or service-thread parallelism. |
| [Yojimbo](https://raw.githubusercontent.com/mas-bandwidth/yojimbo/272153a10f32135bb44bb60e7467072baf48f762/README.md), [reliable channel source](https://raw.githubusercontent.com/mas-bandwidth/yojimbo/272153a10f32135bb44bb60e7467072baf48f762/source/yojimbo_reliable_ordered_channel.cpp) | Finite message/packet sequence buffers, ACK-driven reliable ownership and ordered receive IDs. All client/server calls belong to one thread; documented design assumes 100 players or fewer. | Application-owned pumping is clear, but a library switch is not automatic parallelism or F1 proof. Exact native send attribution would still need integration. |

GNS and yojimbo are BSD-3-Clause; ENet and Godot are MIT, with bundled notices
requiring separate attention. Quake III is GPL-2.0-or-later; it is comparative
input, not proposed copied code. Unreal reuse is governed by Epic's EULA, not a
permissive source license. Gargantuan publishes MPL-2.0. No foreign code was
copied into production. Sources: [GNS license](https://raw.githubusercontent.com/ValveSoftware/GameNetworkingSockets/2cb93a06350bb065db53abdb0d87cf297e0bfd34/LICENSE),
[ENet license](https://raw.githubusercontent.com/lsalzman/enet/5a9c537fd464b3c6d3c55e1d3bd47588faf71b42/LICENSE),
[Godot license](https://github.com/godotengine/godot/blob/232e6b14abf6e590b41d60f11704e42de22bff72/LICENSE.txt),
[yojimbo license](https://raw.githubusercontent.com/mas-bandwidth/yojimbo/272153a10f32135bb44bb60e7467072baf48f762/LICENCE),
[Epic EULA](https://www.unrealengine.com/eula/unreal).

## 5. Is GNS fundamentally limiting?

The current integration's serial packet construction and synchronous egress
form a structural execution bottleneck. The inspected loop has no demonstrated
aggregate CPU reservation or bound covering four finite grants plus protected
traffic. A configured byte rate does not provide it. Changing callback order
cannot create processing capacity, although it can improve a feasible schedule.

It is NOT established that GNS's reliability/encryption protocol is inherently
incompatible with F1. Separating native egress ownership could preserve the wire;
another deployment envelope could supply more processing capacity. Neither is
qualified. Replacing GNS with another single-threaded per-datagram loop can
recreate the same bottleneck with different security/ownership obligations.

## 6. Windows/Linux defensibility

F1 is mathematically coherent and useful as a strict measured deployment and
runtime-health contract. It is not presently established as an unconditional
guarantee on general-purpose Windows or default Linux. Windows documents that
timer expiry makes a thread ready without guaranteeing immediate execution.
Linux SCHED_DEADLINE provides a reservation model with runtime/deadline/period
and admission assumptions, including execution-cost funding; it does not
automatically bound this socket/driver/lock/path workload. See [Microsoft](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleep)
and [kernel documentation](https://www.kernel.org/doc/html/latest/scheduler/sched-deadline.html).

Those facts do not attribute the existing failures to unavoidable jitter.
Current Linux sanitizer jobs do not establish this wall-clock F1 service class.
An accepted deployment must demonstrate both processor/transport capacity and
dispatch/blocking behavior for unchanged F1, with failures retained. No measured
sample is converted into extra H_run or B_run.

## 7. Three architecture options for review

All options below are unimplemented and unqualified. Rank is engineering
judgment, not a performance measurement or an approved architecture amendment.

| Rank | Option | Correctness requirement | Complexity / potential performance | Integration risk |
| --- | --- | --- | --- | --- |
| 1 | Retain GNS wire/security; introduce an explicitly owned bounded native egress transaction and execution domain. | Pending packet/result, exact native timestamp replay, ACK/FIFO, socket leases, prepared/committed counters, close/purge and protected-work proof. | High complexity; directly targets measured serialized syscall cost. Dispatch/turnaround benefit unknown. | High native-lifecycle risk, but avoids protocol/security replacement. |
| 2 | Retain current wire and strict F1; define and qualify a stronger dedicated execution/deployment envelope. | Sufficient CPU/packet capacity plus declared dispatch/lock/driver assumptions; reject unsupported hosts. No admission reduction or F1 slack. | Lower code complexity, potentially substantial deployment cost; hardware/platform sufficiency unknown. | Lower transport regression risk, higher operational/support restriction risk. |
| 3 | Replace the transport behind the adapter with an explicitly scheduled, capacity-oriented backend. | Reprove reliability, encryption/authentication, FIFO, generation safety, exact first-send/ACK/retirement, reserves, resource bounds and provider compatibility. | Very high complexity; alternative pumping/batching/parallelism may help but has no measured benefit here. | Highest migration/wire/security and qualification risk. ENet/yojimbo alone are not drop-in solutions. |

Further serial EDF/quantum/timer tuning is not a fourth option. The evaluated
scheduling-only strategy lacks protected-work semantics and cannot fund CPU.
Any change to F1 itself requires a separately reviewed canonical decision.

## 8. Preferred direction

Prefer option 1 for architecture review: retain the already integrated transport
semantics and address the measured raw-send critical path with explicit bounded
ownership. Unreal's public send-task lifecycle is supporting precedent, not
proof of our result. Require a source-level transaction design and meaningful
capacity benefit before production adoption. If its actor/worker handshake
cannot supply the unchanged service envelope, reject it rather than enlarge
constants. Option 2 is the conservative alternative if deployment restrictions
are acceptable. Do not choose a new library merely because its API is smaller.

This preference is not authorization to implement the larger redesign now.

## 9. Unknowns and one discriminating next experiment

Unknown: clean native critical-path costs in a real four-grant interval,
actor/worker turnaround under protected traffic, safely bounded pending ACK
state, multi-session funding and slot contention, same-connection control
latency, driver/syscall scaling, and whether an
alternative execution environment supplies the required processor envelope.
The exact cause of the physical first failure and hosted 1,302-us gap remains
NOT MEASURED. No universal hardware/OS incapacity follows.

After human architecture review, the discriminating experiment would be one
private, fixed before/after trial of the reviewed prepare→native-send→commit
design in the existing native32 reproducer. It must preserve exact raw-success
timestamps, count no enqueue as service, run mixed/reserve and close/ACK/failure
races, and require every peer/pool F1 equation plus terminal conservation.
Use the existing bounded first-failure recorder. Predeclare rejection for no
meaningful gain, any accounting/order/cleanup error or genuine F1 failure; no
repeated quantum/timer tuning. This experiment has NOT been implemented/run.

## 10. Commits, CI, cleanup and evidence

- Starting integration head: `50de797bc91f43ec52487cf01fc841dc01c04bed`.
- Latest execution-changing source: `b56ae4372b1b0da85f6fd584990da40318939642`.
- Physical native/package source: `c916b7d4f868177ae00b4b05dacfa7b8dbb1f563`.
- Pinned GNS: `2cb93a06350bb065db53abdb0d87cf297e0bfd34`.
- This review/CI-receipt update changes documentation only; runtime source,
  installed services, coordinator pins and v0.1.0 remain unchanged.

All b56/968 original jobs are now terminal and retained. For b56, push Windows
fails funded ACK statistics 95/96, Linux skips; PR Windows 96+393 Python and
Linux 53 pass. Both GNS jobs 11/11 pass. For 968, push Windows 96+393 and Linux 53
pass; PR Windows 95/96 fails a distinct tiny-grant/retransmission case and Linux
skips; both GNS jobs 11/11 pass. Counterparts do not replace original failures.
No pending job, rerun or doc-CI substitution qualifies execution source.
Terminal manifest SHA256:
`c47f8961df38397a7f8e807181ef7442b68176b6d578667db7165aff8f849e02`.

The b56 token 36 raw 347-event reconstruction matches native running maximum
21,847,785,920 byte-us, zero finite shortfall and 1,302-us zero-service gap.
Reconstruction pin: `f07004a343a4b8203d681c93c06fcd3f903cdc151f0ae5840811a4cbeffc9fec`.

The model compiles/runs in 2.297 s on dockerbox at BelowNormal priority, with
owned job reaped, without sockets, captures or farms. Direct local/worker state
census verifies task native processes and UDP 39450/39451 absent. Existing raw
physical failures, unsealed failed client capture, sealed worker/server evidence
and the exact six-secret cleanup receipt remain preserved. No tools/services
were installed or replaced. The rejected UDP development binary is stale
against restored private source and remains ineligible to run/package until
rebuilt; no such run was made. No new physical attempt, Node32, 3M or merge.

The next task is architecture review of these options. Implementation and
physical qualification remain stopped until that review selects a direction.
