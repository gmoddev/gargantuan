---
status: review-required-non-normative
owner: runtime-networking
evidence_date: 2026-10-10
---

# Foundation 3L — bounded networking architecture review

**FOUNDATION 3L — NETWORKING ARCHITECTURE REVIEW REQUIRED**

No qualified correction emerged within the two-strategy investigation. Stop
implementation and physical retries for human review. This document proposes
options; it does not adopt a larger redesign or change canonical F1. KI-006
remains OPEN, Foundation 3L B — PARTIALLY READY, Node32 unrun. No 3M or merge.

The subsequent authorized execution-environment study is recorded in section
11. It evaluates a bounded diagnostic deployment candidate; it does not adopt
either rejected narrow patch or a larger production redesign.

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

## 11. Execution-environment feasibility study (October 9 local / October 10 UTC)

**OPTION 2 INSUFFICIENT — REVIEW BOUNDED EGRESS OWNERSHIP**

This verdict rejects the specified **instrumented pin19 candidate** on HOSTPC.
All six affinity repetitions fail individual F1, and the retained B04 pool
failure localizes insufficient unique service to a continuously scheduled,
SNP-dominated interval. Affinity alone did not supply a passing envelope.
This is not a theorem that every stronger host fails, that clean production has
the same measured cost, or that egress offload will pass. No offload, new
scheduler, admission change or F1 amendment is adopted.

### Fixed experiment and provenance

The predeclared six paired comparisons use the existing 32 separate packaged
Players, real UDP/GNS loopback `127.0.0.1:39450`, five-phase scale plus recovery
workload, 18,000 configured client frames and unchanged production F1 verifier.
Order is `A01,B01,B02,A02,A03,B03,B04,A04,A05,B05,B06,A06`. Each key is consumed
once, with its predeclared run UUID. No replacement run or third variant was
performed. Grant sizes/counts vary through real planning/coalescing and failed
workload termination; these are comparable workload profiles, not identical
grant traces or independent statistical trials.

A leaves placement unchanged. B sets only the actual GNS service thread to
processor group 0, logical processor 19 (`0x80000`), with SMT sibling 18.
HOSTPC is a Ryzen 9 5900X, 12 physical/24 logical processors, one group/NUMA
node, 32-GiB memory class, Windows 11 Pro `10.0.26200`. Processes remain Normal
and the existing GNS relative `THREAD_PRIORITY_HIGHEST` remains unchanged.
Main and Players are unchanged. The selected CPU is **not reserved**; full
context-switch evidence retains its competing work and sibling activity.
Every B records and verifies restoration of the original `0xFFFFFF` affinity.
No unrelated process, OS security, NIC, power-plan, driver or installed service
setting changes.

- Starting integration/review: `c57e7cc0b32c3ef1c9fb9efffae93b3944366982`.
- Execution base: `b56ae4372b1b0da85f6fd584990da40318939642`.
- GNS: `2cb93a06350bb065db53abdb0d87cf297e0bfd34`.
- Player: verified original `c916b7d4f868177ae00b4b05dacfa7b8dbb1f563` package;
  unchanged Player production source across the later base. Server is C57 plus
  the private diagnostic overlay. They are not described as identical builds.
- Final plan SHA-256:
  `2089e33a3fb0a2ffa876c664b2e0f7b500fd3ebe929f6f3e11d28af821219837`.
- Diagnostic overlay ZIP:
  `1a951af184386c04d48f68bb54a6e25b2feba9eb00f42fcb782f1f424223019f`.
- Fresh C57 source ZIP:
  `5b0396af91a1950ef0014b4e3be9b90692a7e72d216925084e341266291d2f86`.
- Server executable:
  `72376d988dc3d67f37a8340a1ef48b39643cfd8e32c0ab205f1b038a24269193`.
- Player executable:
  `16edd167237e34afc5042f77c10966da60bccd645dd8f8dbc053906f9d5b3700`.
- Server/Player package descriptors:
  `e571e7a3a00ead2b1c63be6d51c63e429a4dbfb5711218b7bea9846bf6f29c80` /
  `ceacd4a7912c240858599eeb093aa7739eabd2dec6fd92440ad0834bda9c2871`;
  all 57 content members of each role are verified before every run.
- Fixed trace helper:
  `c8f3b9be60cc7d185c3d970d13388d25f567a99e6ebc8dc1db817a13d317ed36`.
- Final offline reader:
  `eb667037d2979eadf887d6947028ca4a50b4e74494102e28ccfb0cd3ed80e4e0`.

The isolated fresh Release build uses MSVC 14.51, installed CMake 4.3.1-msvc1,
Ninja, GNS on, Tracy/benchmarks off and PCH disabled after a build-tool path
failure. Existing pinned dependencies/caches are reused without installation
or cache deletion. The original Player/installed physical packages remain
intact. Private diagnostic source is retained with its overlay and worktree;
this documentation commit does not put it into production.

### Complete measurements

| Key | Failed / completed grants | Failing peers | Maximum peer deficit, byte-us | Failed / common-four cohorts | Maximum pool deficit, byte-us |
| --- | ---: | ---: | ---: | ---: | ---: |
| A01 | 52 / 5,688 | 27 | 39,329,644,992 | 1 / 633 | 81,345,913,728 |
| B01 | 4 / 5,096 | 4 | 23,202,889,728 | 0 / 549 | 50,548,977,472 |
| B02 | 27 / 5,920 | 17 | 52,797,898,752 | 0 / 619 | 60,191,731,392 |
| A02 | 66 / 6,670 | 27 | 45,701,136,384 | 6 / 719 | 109,105,923,072 |
| A03 | 38 / 5,151 | 21 | 58,317,602,816 | 3 / 585 | 96,546,082,368 |
| B03 | 13 / 5,183 | 11 | 35,165,044,736 | 0 / 593 | 48,204,530,496 |
| B04 | 19 / 5,787 | 15 | 47,596,961,792 | 1 / 739 | 94,297,880,704 |
| A04 | 78 / 5,725 | 27 | 33,757,700,352 | 7 / 677 | 119,782,242,880 |
| A05 | 50 / 5,384 | 24 | 31,394,872,768 | 8 / 619 | 117,008,809,920 |
| B05 | 44 / 6,211 | 21 | 37,727,584,576 | 2 / 974 | 129,054,818,176 |
| B06 | 20 / 6,017 | 14 | 46,321,893,376 | 0 / 598 | 41,970,573,056 |
| A06 | 44 / 5,746 | 22 | 34,196,111,488 | 5 / 650 | 126,141,091,392 |

Baseline totals are **328/34,364** failed grants and **30/3,883** failed
pool cohorts; pin19 totals are **127/34,214** and **3/4,072**. Every repetition
fails peer F1. Failure frequency improves in this sample, but maxima and pool
failures do not improve uniformly. These correlated counts are descriptive;
they are not a future-failure probability estimate.

The bounds remain exactly 18,025,216,000 byte-us per grant and 72,100,864,000
byte-us for common-four running service. Finite shortfall and exact terminal
conservation are separate from those running curves. Passing pool intervals
cannot hide a failed peer. Failed raw outputs and exceptions are retained.

Seconds below are rounded for display; the retained JSON contains the exact
recorded microsecond values. Switches are switch-ins (switch-outs match).

| Key | Thread CPU, s | ETW scheduled / off-CPU, s | Switches | SNP, s | Raw within SNP, s | Encryption within SNP, s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| A01 | 52.609 | 53.672 / 172.260 | 103,870 | 22.225 | 17.987 | 1.379 |
| B01 | 47.750 | 48.373 / 164.001 | 96,161 | 18.246 | 14.841 | 1.086 |
| B02 | 65.000 | 65.484 / 180.801 | 102,812 | 21.706 | 17.698 | 1.289 |
| A02 | 68.672 | 69.182 / 182.197 | 107,767 | 26.578 | 21.528 | 1.606 |
| A03 | 48.609 | 49.691 / 163.069 | 105,828 | 19.375 | 15.663 | 1.162 |
| B03 | 48.313 | 49.384 / 165.021 | 107,245 | 18.789 | 15.311 | 1.135 |
| B04 | 54.188 | 54.454 / 170.686 | 114,005 | 21.207 | 17.257 | 1.266 |
| A04 | 55.250 | 55.782 / 171.054 | 108,091 | 22.893 | 18.479 | 1.414 |
| A05 | 49.750 | 50.101 / 166.392 | 100,094 | 20.833 | 16.808 | 1.273 |
| B05 | 51.438 | 51.786 / 188.285 | 121,039 | 25.514 | 20.733 | 1.521 |
| B06 | 65.109 | 64.300 / 176.191 | 99,226 | 22.129 | 17.916 | 1.327 |
| A06 | 54.125 | 54.442 / 171.849 | 94,792 | 22.533 | 18.194 | 1.404 |

Thread CPU from `GetThreadTimes` and ETW scheduled residency are separate
measurements. Residency includes interrupt time. Whole-run utilization includes
legitimate idle, credit and ACK waits and cannot prove active-window capacity.
B06's CPU accounting exceeds its ETW residency by about 0.809 s; the cause is
NOT MEASURED, so these totals are not an exact instruction-time closure. The
covered B04 attribution uses its synchronized short-interval ETW/phase records,
not a subtraction of whole-run API totals.
Callback/thinker/SNP spans nest: do not add their totals. The raw component scope
is SNP sender work, not all socket calls or inline receive-triggered replies.

All rows have **zero finite shortfall** and exact terminal
`accepted = first-sent = ACKed = retired` on the complete attributed byte basis.
The equality column includes bootstrap; the last column contains qualified
Ready structural bytes. UDP bytes and structural unique bytes are different
bases and must not be substituted.

| Key | Accepted = first-sent = ACKed = retired, B | SNP positive packets | SNP native UDP bytes, B | Qualified unique structural bytes, B |
| --- | ---: | ---: | ---: | ---: |
| A01 | 1,504,235,818 | 1,696,223 | 1,850,091,775 | 1,503,834,294 |
| B01 | 1,283,125,684 | 1,497,779 | 1,621,063,096 | 1,282,725,685 |
| B02 | 1,607,327,092 | 1,786,821 | 1,956,558,511 | 1,606,925,873 |
| A02 | 1,917,226,097 | 2,061,418 | 2,277,030,194 | 1,916,824,573 |
| A03 | 1,301,448,555 | 1,511,281 | 1,639,531,762 | 1,301,049,471 |
| B03 | 1,325,035,089 | 1,535,709 | 1,664,510,479 | 1,324,634,785 |
| B04 | 1,569,637,967 | 1,752,577 | 1,917,420,736 | 1,569,237,358 |
| A04 | 1,571,045,789 | 1,753,832 | 1,918,983,278 | 1,570,644,265 |
| A05 | 1,395,250,118 | 1,597,499 | 1,737,035,062 | 1,394,847,679 |
| B05 | 1,650,750,036 | 2,075,903 | 2,272,407,433 | 1,650,350,342 |
| B06 | 1,655,248,713 | 1,830,292 | 2,006,307,078 | 1,654,849,019 |
| A06 | 1,546,197,540 | 1,733,622 | 1,893,560,594 | 1,545,797,846 |


RAM is the actual native host-preflight sample. RSS is the sampled Server
working-set high-water, not a continuous peak bound. Combined DPC/ISR totals
are for each whole host trace on LP18/19, **not** this thread or a failed grant.
The retained interrupt table includes all 24 processors; unknown modules
remain unknown.

| Key | Observed probes / 96 | Maximum RPC / Event, us | Available RAM, GiB | Server RSS, MiB | Whole-trace IRQ LP18 / LP19, us |
| --- | ---: | ---: | ---: | ---: | ---: |
| A01 | 64 / 96 | 770,009 / 249,891 | 19.342 | 386.0 | 84,460 / 65,577 |
| B01 | 64 / 96 | 489,179 / 66,735 | 19.317 | 372.1 | 62,959 / 77,792 |
| B02 | 64 / 96 | 536,456 / 149,953 | 19.310 | 387.2 | 63,794 / 95,006 |
| A02 | 64 / 96 | 688,184 / 183,613 | 19.332 | 406.9 | 76,641 / 41,897 |
| A03 | 64 / 96 | 485,524 / 99,486 | 19.328 | 370.6 | 74,531 / 29,002 |
| B03 | 64 / 96 | 485,021 / 83,634 | 19.321 | 374.1 | 62,740 / 74,525 |
| B04 | 64 / 96 | 629,449 / 133,875 | 19.339 | 385.1 | 63,018 / 86,781 |
| A04 | 64 / 96 | 521,205 / 133,243 | 19.322 | 394.2 | 75,878 / 42,138 |
| A05 | 64 / 96 | 634,424 / 250,133 | 19.311 | 378.9 | 70,884 / 33,978 |
| B05 | 96 / 96 | 750,306 / 266,094 | 19.321 | 533.8 | 74,223 / 90,409 |
| B06 | 64 / 96 | 635,523 / 299,810 | 19.309 | 392.7 | 62,868 / 92,094 |
| A06 | 64 / 96 | 572,524 / 167,226 | 18.740 | 389.9 | 72,876 / 35,223 |

Eleven runs retain 64 gameplay/structural RPC/Event summary rows. B05 also
reaches mixed recovery and retains all 96; that complete set still fails the
existing RPC/Event latency bounds. Partial observations do not establish a
qualified protected campaign. Native ACK-due-to-send latency, whole-wave bidirectional
wire/reserve accounting and complete native reliable-order/retransmission
coverage are **NOT MEASURED** by this study. Exact eventual structural ACK and
retirement do not substitute for those facts. No physical Local32 or real-TLS
Node32 acceptance follows from native loopback.

### Capacity attribution and uncertainty

B04 is the decisive covered affinity counterexample. Its common-four witness
runs from `1131951597336` to `1131951601147 us`: **3,811 us**, with
`347,922 - 186,468 = 161,454 B` unique structural progress, approximately
**40.402659 MiB/s**. The service thread is scheduled throughout on LP19, with
zero observed off-CPU time. Existing timing records localize **3,654.8 us
(95.9%) to SNP sender work**; the enclosing callback/thinker times are
3,712.8/3,741.9 us. Timer wait is 14.7 us, global-lock wait 3.9 us and receive
drain 22.3 us. Repeated sender callbacks complete four positive native packets,
4,696 native bytes, in about 100–108 us. This is serial sender-path evidence,
not a generic timer blackout or an inference from average CPU utilization.

The independently decoded expanded interval contains zero reported LP19 DPC or
ISR time. Conservatively charging six microseconds for all-CPU event presentation
precision plus two for endpoint uncertainty still leaves
`93,761,009,792 > 72,100,864,000 byte-us`. These counterfactual subtractions are
for causal analysis only; actual F1 never pauses or discounts this time.
Critical phase artifact SHA-256:
`2d485ae1fab4e07dd26095cccaf3eee9d436aa1483343bc1cc00b04decf31acb`;
interrupt-bound artifact:
`f4f419628b377beedd9bbc1bf629f0826e986a41921621504fde0b6f1dc09a6d`.

A01 separately has a 7,752-us failed pool interval at about 53.99258 MiB/s,
with 7,748.6 us scheduled and 3.4 us off-CPU. Conservative interrupt/endpoint
charging leaves 79,708,457,446 byte-us, still above the pool bound. Conversely,
some B01/B02 failures contain substantial WAITING/READY time; do not classify
every failure as the same CPU mechanism. B02's earliest retained miss overlaps
a 1,808.7-us whole `DrainSocket` span, including receive syscalls and inline
dispatch, with waiting and ready delays. Its exact wait owner remains unknown.

A01 supplies a simple illustrative cost model: 13.102661 us per positive SNP
packet comprises raw 10.604298, encryption 0.813131 and residual 1.685232 us.
Raw is 80.9324% of whole-run SNP elapsed. Optimistically treating Q=1,248 B as
structural bytes per packet requires at least 53,774 packets/s for 64 MiB/s;
charging that sample mean consumes 0.704571 elapsed seconds per second before
receive/control/selection and other actor work. Its measured structural packet
basis is about 1,133.972 B, implying roughly 59,180 packets/s and a 0.775420
fraction **if** the all-packet cost mix applies. That assumption is unproven.
These are sample means, not CPU costs, WCET, wire/structural equality or a
deadline-feasibility proof. The actual B04 short interval is more informative
than either whole-run average.

Instrumentation consumes actor time. Component scopes, pool scans, timing
recorders and full ETW are identical across arms but their placement interaction
does not cancel. A01's 2.858529-s "other SNP" residual includes productive work
and pool observation; it cannot bound observer cost in B04. Critical raw versus
encryption versus observer cost is **NOT MEASURED** separately. Thus the covered
result indicates insufficient serial execution capacity in the tested observed
profile; it does not establish an uninstrumented hardware limit or prove that
parallel native I/O supplies the missing capacity.

### Receive blocking is an additional design obligation

Pinned `CConnectionTransportUDP::PacketReceived` takes a blocking
`ConnectionScopeLock` while the global actor lock is already held, before
packet-type dispatch. Its `ScopeLock` delegates to
`std::recursive_timed_mutex::lock()`. Main API lookup can retain the connection
lock after releasing the table lookup lock. One blocked connection can
therefore stall unrelated service. Other receive paths can take the shared
message-queue lock or emit a synchronous statistics reply. The measured
30-byte receive does not identify ACK/control, connection, lock or owner.
Source anchors are `steamnetworkingsockets_udp.cpp::PacketReceived/RecvStats`,
`steamnetworkingsockets_socketthread.cpp::DrainSocket`,
`steamnetworkingsockets_lowlevel.h::ScopeLock`,
`steamnetworkingsockets_connections.h::ConnectionScopeLock`, and
`csteamnetworkingsockets.cpp::GetConnectionByHandleForAPI`.
Egress workers do not automatically fix this blocking path.

### Verification, cleanup and next decision

The private build passes all three selected CTest targets (GameSession and the
two existing pooled/native-observer source contracts). The supplied-API thread
fixture passes nine cases, the pool fixture passes, and the existing trace
layout/cap/priority ownership tests pass. These qualify the necessary study
controls, not a corrected production transport. A minimal reader fix recognizes
the exact existing `[Runtime:Luau]` prefix; original reader/A01 output are
preserved and only A01's protected section is refreshed without another CSV
pass. Earlier setup failures and the pre-run rejected Main pool-hook race are
retained and are not counted as matrix runs.

Every run has matching source/package identities, 32 generation-matched peer
summaries, loss-free complete ETW, valid native thread lifecycle/cost/pool
coverage, and an owned process tree reaped. All twelve harness exits are 1;
failed production/recovery results are preserved, not capture rejection or
replacement samples. The final direct worker census at
`2026-10-10T03:53:39.1920992Z` shows native processes 0, UDP 39450/39451 owners 0
and the task kernel logger absent; local operators/readers are also absent.
No Packet Monitor capture, physical probe, tunnel, credential ticket or
installed capture-service update was used in this study.

The full raw copies comprise **1,429 files / 24,195,641,016 bytes**, verified
by byte count and SHA-256. Only 22,694,442,650 bytes of matching worker ETL/CSV
duplicates were removed after local retention; no unique evidence was deleted.
The final worker retains 18.884 GiB available RAM and 22.608 GiB free disk, and
package hashes remain unchanged. Builds/caches and original failed receipts
remain preserved.

Local evidence root:
`C:\Users\aiden\.codex\artifacts\farm32-execution-environment-study-v1`.
`declared-matrix-measurements.json` contains all run UUIDs, source references,
raw-copy/analysis hashes, cost/resource data and exact recovery-barrier rows;
its SHA-256 is
`d41a1abb97eb5dbf42a85cb763279a100d6a113e55c936f9c76c3f022dca56b9`.
The full per-CPU `whole-trace-interrupt-totals.json` pin is
`630a8e29f88ee0a917371237dfcd5f34467c680e5eb08e66ca7e6dff2b5803da`.
Actual unchanged harness and its three dependencies are retained under
`OriginalHarness`; `original-harness-provenance.json` pin is
`e879275d8a7ae8394ea3ff7820a7b380d7637aba6dcbddc17a8219fb94dbc5d8`.
Worker bytes match canonical C57 working bytes; raw Git differences are solely
CRLF normalization, explicitly recorded rather than called byte-identical.

Starting C57's six current hosted checks are green. The distinct original
b56/968 F1 CI failures recorded in section 10 remain failures; counterpart or
later documentation-head success does not erase them. The private overlay has
not received hosted production qualification. This update adopts no runtime
candidate and does not authorize a physical retry.

Ranked next steps:

1. Review Option 1's bounded **prepare → native send → commit** transaction
   before implementation. Require pending-ACK handling, FIFO/connection order,
   socket leases, true native-success timestamps, feedback evidence frontier,
   retry/failure ownership, protected reserve funding and shutdown/join proofs.
   Include the receive/global-to-connection lock interaction above. Demonstrate
   actual capacity benefit; adding workers alone proves none of these facts.
2. Keep an explicitly funded stronger deployment envelope as an alternative,
   with its product/support restriction reviewed. This experiment does not
   exclude faster hardware or genuinely reserved execution; another core or
   priority change without an identified mechanism is not a qualified option.
3. Retain transport replacement as the highest-cost alternative requiring the
   complete wire/security/ordering/attribution/provider proof described above.

The smallest next discriminating experiment, after review, is one bounded
native32 capture of the critical sender interval with the **already collected
per-callback raw/encryption components exported** and observer contribution
distinguished. Join it to the same exact grant/pool witness; do not create a new
qualification framework or retry until it passes. For the separate B02 receive
delay, a bounded lock-acquisition/owner interval or switch-out stack is the
missing fact. Neither experiment was performed beyond the declared matrix.
No larger redesign is implemented. KI-006 remains OPEN, Foundation 3L B —
PARTIALLY READY; Local32 remains failed and Node32 unrun. No 3M or merge.
