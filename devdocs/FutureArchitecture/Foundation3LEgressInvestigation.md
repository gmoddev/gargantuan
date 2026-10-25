---
status: review-required-non-normative
owner: runtime-networking
evidence_date: 2026-10-10
---

# Foundation 3L — bounded GNS egress investigation

**Primary verdict: EGRESS OWNERSHIP DESIGN NOT QUALIFIED.** The new failed-interval
trace supports investigating removal of synchronous UDP calls from the serialized
GNS service path. The proposed actor/worker transaction is **not** qualified for
the existing product contract: process-wide funding, protected traffic, pending
ACK/result ordering, feedback chronology, and socket lifetime lack a bounded
source-backed solution in the present architecture. Stage 3 was not attempted.
No prototype, physical Local32/Node32 run, F1 amendment, production change, 3M
promotion, or PR merge is claimed.

This report is a decision record, not an accepted ADR or a replacement for
[D01](../../docs/adr/D01-pooled-service-curve.md), [KI-006](../../KNOWN_ISSUES.md),
or the [earlier capacity review](Foundation3LNetworkingCapacityReview.md).
The supplied independent assessment was a static assessment of the earlier
evidence; its offload direction was a research hypothesis, not qualification.

## 1. Provenance and scope

The draft [PR #6](https://github.com/gmoddev/gargantuan/pull/6) started this
investigation at `287fbb60ff6aea7c5c1d80dd235de2d7e32f3ae6`. Its six
reported CI jobs had succeeded at review time (two Windows Release, two Ubuntu
headless ASan/UBSan, two GNS sanitizer jobs). Those jobs belong to that head,
not to the isolated diagnostics or this report. The diagnostic source was
`c57e7cc0b32c3ef1c9fb9efffae93b3944366982` plus the prior diagnostic
overlay `1a951af184386c04d48f68bb54a6e25b2feba9eb00f42fcb782f1f424223019f`
and a local export of already captured per-callback raw/encryption QPC counters.
GNS remained pinned to `2cb93a06350bb065db53abdb0d87cf297e0bfd34`;
the unchanged F1 implementation is
[`FiniteGrantServiceCurve.hpp`](../../src/network/FiniteGrantServiceCurve.hpp).
The diagnostic source was built in an isolated remote workspace. Neither
diagnostic modification nor a candidate egress implementation was promoted.

The [predeclared plan](evidence/Foundation3LEgressStage1/Stage1Plan.md) fixed
two native32 loopback runs and no replacement. Both used 32 separate GNS
Players, 18,000 frames, a five-phase scale/recovery workload, Normal process
priority, service-thread pin19, the same server executable and unchanged F1
and first-failure recorders. B04 enabled detailed timing; B05 disabled its
timing sink while retaining F1, pool, lifecycle and ETW observers. B05 therefore
was a timing-sink control, not an instrumentation-free production binary.
Server package SHA-256 was
`9bc52c085da7d95c3081da40332762fe2b3c34292d71028c9b5e0707f3ab0232`;
the unchanged Player executable SHA-256 was
`16edd167237e34afc5042f77c10966da60bccd645dd8f8dbc053906f9d5b3700`.
The task-local diagnostic header SHA-256 was
`f6b8ee7cf7878d99d489829698094c9243aa0f0f9e39443508a1ff4bcae1e85a`.
This source hash is not a substitute for the packaged server hash.

The complete native logs, loss-free ETW traces, offline analysis and scripts
are retained in the task artifact at
`C:\Users\aiden\.codex\artifacts\foundation3l-egress-stage1` and the
remote task artifact at `C:\Sandbox\Codex\Artifacts\foundation3l-egress-stage1`.
The compact [first failed pool interval](evidence/Foundation3LEgressStage1/critical-B04.json)
is committed beside this report. It records the native-log SHA-256
`8615311783b60d905ad1d7bfeab84393b50b6b8017e721322f7b6c0d04642d77`
and offline B04 analysis SHA-256
`75603f77e0235e64d14f9a5d266f50a30aecffa04463a4e5a1183b67c6afa379`.

## 2. Stage 1: actual failure, not a whole-run average

| Diagnostic | B04 detailed timing | B05 timing-sink control |
| --- | ---: | ---: |
| Native run ID | `d7af967d-c680-434b-8b72-c7ca245ab8f5` | `4eaa47fb-6d21-4a02-ad04-29c16371d476` |
| Completed grants / strict peer failures | 6,701 / 28 | 6,023 / 5 |
| Closed common-four cohorts / strict failures | 1,085 / 3 | 610 / 0 |
| Maximum pool running deficit, byte-µs | 132,883,252,096 | 34,847,128,576 |
| Finite completion shortfall | 0 | 0 |
| Unique accepted, first-sent, ACKed, retired | exact equality | exact equality |
| ETW loss / lifecycle | zero loss; child job reaped | zero loss; child job reaped |

Both runs exited with real F1 failures. Neither is a protected-work or physical
qualification; protected ACK/control latency, FIFO, and reserves were not
measured. The analyzer reports `ProtectedComplete=false` and
`ProtectedBoundsPass=false` for both. B05's offline analysis SHA-256 is
`3840050aae6918f9041c40e358159054cf80e0cc9ad685f06963730318f50f98`.

The **first B04 failed common-four interval** is 4,657 µs, from native
steady-clock 1,150,337,269,116 to 1,150,337,273,773 µs. Its four
generation-qualified members are tokens 2182–2185, with native handles and
grant sizes in the compact evidence. A reconstructed 239,907 **unique
structural** bytes were first sent inside it. The observed deficit is
72,618,979,648 byte-µs against the unchanged 72,100,864,000 byte-µs bound.
Only 7.72 µs of perfect service-time recovery at the 64 MiB/s pool rate would
mathematically remove this particular overrun; that small distance to the
bound makes instrumentation effects important. Token 2182's first failed peer
grant was activated at 1,150,337,250,461 µs, first sent at
1,150,337,250,598 µs, completed at 1,150,337,278,890 µs and observed at
1,150,337,285,823 µs; its maximum running deficit was
35,991,819,008 byte-µs. The four pool tokens join the 52 complete SNP
callbacks in the interval. Positive native packet bytes below are wire-side
packet bytes; they are not interchangeable with unique structural bytes.

| Failed-interval component | Direct observation | Interpretation |
| --- | ---: | --- |
| Service thread on CPU / off CPU | 4,657 / 0 µs | Scheduling absence does not explain this interval. |
| SNP sender overlap | 4,482.7 µs | Dominant serialized work class; nested in thinker/callback spans. |
| 52 complete token-bearing SNP callbacks | 4,398.2 µs | 208 positive native packets, 244,192 native packet bytes. |
| Synchronous raw-send wrapper/call spans inside those callbacks | **3,551.1 µs**, 208 calls | At least 76.3% of the failed interval lies in measured raw spans; includes syscall and measurement overhead, not proven pure kernel time. |
| Encryption inside the same complete callbacks | 295.8 µs | Remains actor work in a send-only offload. |
| Other SNP work inside the same complete callbacks | 551.3 µs | Includes packet selection/preparation and instrumentation; its internal split is not measured. |
| Receive drain overlap | 36.9 µs | Not the dominant observed interval cost. |
| Global lock wait overlap | 3.8 µs | Exact connection-lock owner is unmeasured. |

Two boundary SNP callbacks overlap only partly (63.2 and 21.3 µs); their
whole-callback raw spans are 75.3 and 71.9 µs and are **excluded** from the
3,551.1-µs lower bound. Thinker (4,577.7 µs), callback (4,551.5 µs), SNP,
raw and encryption spans are nested and must not be added. The QPC/steady
boundary calibration reports at most six QPC ticks, or 0.6 µs, bracket error;
it does not calibrate each native call's observer cost or prove a cross-clock
ordering finer than that bracket. ETW and native records identify the same
service thread and the trace reports no lost events/buffers.

This identifies a plausible removable serial span: four tokens each consume
roughly 0.87–0.90 ms of measured raw time across 13 callbacks. It does not
measure worker dispatch, OS scheduling on workers, actor result commit,
protected-work interference, or any candidate's tail latency. Thinker
eligibility/reason selection, competing protected ACK/control work, SNP
subcomponents other than encryption/raw, raw-call observer overhead and the
receive connection-lock owner remain unresolved. The worst B04 pool failure
occurred later than the frozen timing window, so this cost budget is for the
first failure, not every failure.

**Observer check limitation.** B05 had fewer peer failures and no pool
failure, but its workload produced fewer grants/cohorts and is one stochastic
run. An offline B04 analyzer was mistakenly launched on the same worker during
approximately the first minute of B05 and then stopped; B05's first reported
F1 failure occurred later, but the run is confounded for a numerical
timing-sink overhead estimate. The overlap was disclosed during execution and
the predeclared no-replacement rule was honored. No causal failure-count ratio
or observer-overhead subtraction is claimed. B05 still independently failed
strict peer F1 without the detailed timing sink. B04's large raw-call span,
continuous CPU residence and exact token join support a **conditional
architecture investigation**, not a predicted F1 pass.

At the optimistic 1,248 B/packet F1 quantum, four active 16 MiB/s grants need
about 53,773 packets/s, one packet per 18.6 µs across the process. The B04
raw-call spans average 17.1 µs for each of 208 positive packets, equivalent to
about 58,600 calls/s if serialized with no other cost. This is a capacity
sanity check, not a measured sustainable worker rate. A lone worker would have
little optimistic headroom; multiple workers could remove actor blocking, but
the prepare/commit and protected budgets are unknown. Thus Stage 1 supports
examining offload and does not support a prototype by itself.

## 3. Stage 2: proposed transaction and unresolved ownership

The smallest conceivable split leaves packet selection, reliable references,
sequence/nonce, encryption, pacing, ACK processing and F1 state with the GNS
actor. A worker would own only a copied complete datagram, destination and
generation-qualified socket lease, perform one synchronous OS send, and return
the actual result and timestamp. It must not call the present raw helper:
that helper requires GNS global lock and uses mutable simulation, tracing and
socket state ([pinned socketthread source](https://github.com/ValveSoftware/GameNetworkingSockets/blob/2cb93a06350bb065db53abdb0d87cf297e0bfd34/src/steamnetworkingsockets/clientlib/steamnetworkingsockets_socketthread.cpp#L1216-L1218)).

```text
Actor: Prepared --reserve pending record/lease--> Pending/Submitted
Worker: Pending/Submitted --actual positive OS result--> NativeSucceeded
Worker: Pending/Submitted --actual zero/error result--> NativeFailed
Actor: NativeSucceeded --chronological result commit--> Committed
Actor: NativeFailed --rollback or retained retry ownership--> Reconciled
Actor: Committed --real ACK--> ACKed --release references/debt--> Retired
Close: unresolved --stop assignment, cancel unstarted, join started,
       reconcile every result, release lease--> terminal purge/retirement
```

This is a **candidate state machine only**. A queue insertion and Windows
`WSA_IO_PENDING` would not count as a positive native send. Pinned Windows
`WSASendMsg` is currently synchronous with null overlapped/completion arguments
([socketthread.cpp](https://github.com/ValveSoftware/GameNetworkingSockets/blob/2cb93a06350bb065db53abdb0d87cf297e0bfd34/src/steamnetworkingsockets/clientlib/steamnetworkingsockets_socketthread.cpp#L525-L534)); an OS-only synchronous worker is the smallest plausible Windows boundary. POSIX `sendmsg` could follow the same contract, but Linux worker scheduling, completion, FIFO, and F1 are unmeasured. No IOCP, `sendmmsg` or `io_uring` rewrite is justified by these data.

| Race or resource | Current source-backed behavior | Required rule; qualification state |
| --- | --- | --- |
| ACK before actor commit | SNP inserts packet into its in-flight map only after positive send; ACK lookup uses that map and can free reliable references ([SNP send](https://github.com/ValveSoftware/GameNetworkingSockets/blob/2cb93a06350bb065db53abdb0d87cf297e0bfd34/src/steamnetworkingsockets/clientlib/steamnetworkingsockets_snp.cpp#L1915-L1979), [ACK](https://github.com/ValveSoftware/GameNetworkingSockets/blob/2cb93a06350bb065db53abdb0d87cf297e0bfd34/src/steamnetworkingsockets/clientlib/steamnetworkingsockets_snp.cpp#L1174-L1179)). | Generation/packet-number keyed provisional ACK and bounded replay only after real positive commit; no implementation or bound exists. |
| Native failure and retry | First-send credit follows positive native result in [`ApplyReliableServiceFeedback.cmake`](../../cmake/gns/ApplyReliableServiceFeedback.cmake); SNP failure retains/retries reliable data. | Negative result receives zero credit; later first positive send credits each segment exactly once. Pending references and retry queue must remain owned; unproven. |
| FIFO, nonce, mixed control | UDP consumes sequence before the OS call; encryption uses it. SNP packs structural data, retries, ACK/STOP_WAITING and receive-triggered control into sequenced packets ([UDP sequence](https://github.com/ValveSoftware/GameNetworkingSockets/blob/2cb93a06350bb065db53abdb0d87cf297e0bfd34/src/steamnetworkingsockets/clientlib/steamnetworkingsockets_udp.cpp#L537-L557), [SNP](https://github.com/ValveSoftware/GameNetworkingSockets/blob/2cb93a06350bb065db53abdb0d87cf297e0bfd34/src/steamnetworkingsockets/clientlib/steamnetworkingsockets_snp.cpp#L2523-L2536)). | At most one unresolved packet **of any class** per connection, with commit before the next preparation, would preserve preparation order; its effect on prompt ACK/control latency is unmeasured. No packet number/nonce reuse on retry. |
| Feedback frontier | Snapshot observes active F1 at current time ([bridge](../../cmake/gns/ReliableServiceFeedbackBridge.cpp)); F1 rejects a later replay whose event time precedes its last observation ([curve](../../src/network/FiniteGrantServiceCurve.hpp)). | A resolved-time watermark must defer publication and replay actual result times before observing the entire elapsed interval. Deferral must not fabricate health, pause F1 indefinitely, or break the 50-ms freshness rule. No bounded turnaround proof exists. |
| Socket generation and close | Adapter closes/purges immediately under its global owner lock ([transport](../../src/network/GameNetworkingSocketsTransport.cpp)); native socket destruction closes the OS handle. | Stop new assignment, cancel unstarted packets, join begun OS calls, commit/terminally reconcile results, then release generation-qualified leases outside a lock needed by workers. No lease/close protocol exists. |
| Protected traffic | ACK/control sends and receive callbacks share the actor and sequenced packet path. | Priority and reserved slots/worker time must bound control latency even when structural work fills the queue. Current prompt ACK estimate excludes loss/retry ([budget](../../cmake/gns/PromptAckWireBudget.hpp)); no worst-case bound exists. |

The independent source challenges agree that a per-connection single unresolved
packet is the smallest credible FIFO rule. It does **not** close the ACK race:
an ACK can reach the actor between worker success and result commit. It also
does not close the feedback race: a snapshot at T1 between native success T0
and actor commit T2 can permanently latch a false F1 breach, while replaying
T0 after T1 violates F1 timestamp monotonicity. A frozen but apparently
healthy snapshot would hide elapsed service and is unacceptable. The pending
ACK record and resolved-time watermark are design obligations, not existing
GNS features or proven bounded fixes.

## 4. Process-wide capacity is the stop gate

[`GameSession`](../../src/network/GameSession.cpp) constructs its own
`ReliableByteAdmission`, whose `MaximumDrainGrants=4` check is local
([admission](../../src/network/ReliableByteAdmission.hpp)). In contrast,
[`GameNetworkingSocketsTransport`](../../src/network/GameNetworkingSocketsTransport.cpp)
holds a process-wide GNS interface and connection-owner map. No current
process-wide authority prevents two GameSessions from accepting four grants
each. Four proposed packet/result slots, even at one per connection, do not
fund those eight accepted obligations. The earlier review's `4 × 1,300 = 5,200`
byte copied-datagram example omits packet references, result records, provisional
ACKs, socket leases, retry metadata and their terminal lifetime.

For `K` simultaneous accepted grants across **all** sessions, the structural
execution requirement is at least `16 MiB/s × K` while incomplete. Four grants
require 64 MiB/s. Eight require 128 MiB/s before gameplay, control and
transport reserves. A single pooled profile configures a 96 MiB/s backend
ceiling with 64 structural plus 8 gameplay, 4 control and 8 transport MiB/s
reserves ([profile](../../include/gargantuan/network/ReliableServiceProfile.hpp));
that is a configured per-profile funding model, **not** proof of a process-wide
hardware ceiling or an enforced cross-session budget. The actual shared
service capacity, worker utilization/tails and all-sessions reserve floor are
unmeasured. The [ordinary pacer](../../cmake/gns/OrdinaryWirePacer.cpp)
sets an upper bound for ordinary traffic, not the missing structural or
protected lower bound. A process-wide model would need a declared session
limit or cross-session admission/funding authority, bounded pending
packets/bytes/ACKs/results/leases, protected slot and scheduling reservations,
and proof that no accepted grant can be stranded. Neither restriction nor
authority exists in the accepted 3L design, and adding one changes admission
and lifecycle architecture beyond this bounded egress transaction.

The three independent Stage 2 challenges were: (1) ACK/result/feedback
ordering; (2) FIFO, nonce, socket generation and shutdown; and (3) multi-session
capacity and protected work. The first two found a plausible *shape* of a
transaction but no implemented bounded proof; the third supplied the decisive
counterexample of two concurrently funded sessions. Reconciliation here is to
stop at the ownership gate, not assume one session or silently cap admission.
No sub-agent produced or committed code.

## 5. Decision, tests and next options

Stage 3's paired baseline/candidate pins, correctness/race tests, actor
occupancy comparison and strict zero-failure native32 campaign were **not
attempted** because Stage 2 did not pass. Thus worker/commit cost, candidate
peer/pool F1, ACK/FIFO, protected reserves and prototype shutdown outcomes are
**NOT MEASURED**. The Stage 1 jobs themselves ended with 0 child processes and
0 port owners; their finite shortfall was zero and exact accepted/unique
first-sent/ACKed/retired conservation held. That is diagnostic cleanup and
baseline evidence only, not an egress correctness result.

Ranked next decisions:

1. **Architecture review of global funding and pending-result semantics.**
   Decide whether a product-supported one-session-per-process envelope is
   acceptable or specify cross-session admission and protected capacity. Then
   design and test the pending ACK/feedback/lease transaction before asking for
   another prototype authorization. This is a new architecture decision, not
   an automatic continuation of this bounded campaign.
2. **A restricted deployment envelope** retaining GNS and unchanged F1, if
   supported hardware and operating restrictions are acceptable. Ordinary
   pin19 already failed; another affinity-only retry is not evidence of a fix.
3. **Transport replacement** only after a candidate demonstrates the needed
   capacity and a full reliability, security, ordering, first-send and F1
   migration plan. No cited engine has an equivalent established F1 promise.

Go/no-go: **no-go for the isolated egress prototype under this authorization**.
Keep draft PR #6 open for review. KI-006 remains **OPEN**, Foundation 3L B is
**PARTIALLY READY**, Local32 strict F1 remains failed, Node32 remains unrun,
and the unchanged F1 and acceptance gates remain authoritative.
