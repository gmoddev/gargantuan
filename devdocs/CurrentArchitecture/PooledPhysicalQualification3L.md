---
status: f1-phase1-pass-later-gates-pending
owner: runtime-networking-and-runtime-host
last_verified: 2026-10-06
---

# Foundation 3L pooled physical qualification attempt

## Native execution-cost evidence; private UDP candidate not adopted (2026-10-09)

The loss-free `native-f1-etw-window-e51-01` sample binds the service thread to
PID 26016 / TID 5304. Its frozen 53,987.7-us history has 53,975.1 us scheduled
residency and 12.6 us off-CPU. The first measured failure observer is inside the
sample. This rules out a substantial dispatch blackout in that observed history;
it does not establish production WCET or the exact lost grant certificate.
The four-grant sender callbacks cover 700 successful native UDP packets /
821,978 B in 12,545.8 us, with no observed off-CPU interval. Separate bounded
interrupt analysis finds 32 us of DPC and no ISR on the CPU used by these
callbacks in the wider selected window. Even deliberately overcharging 272 us
for interrupts and reporting precision (32 us plus one us for each of all 240
retained DPC/ISR events across CPUs) leaves only about 63.87 MiB/s of native
bytes, before structural attribution and the rest of service-loop work. This
is diagnostic execution-cost evidence, not proof of hardware incapacity:
loopback, recording overhead and whole production worst-case cost differ.

A private component sample uses the existing sender spans to distinguish
encryption and the synchronous Windows UDP call. In one unchanged funded
four-grant test, 1,848 successful packets / 2,166,384 native B take 15,308.6 us
of sender elapsed time: 1,129.9 us encryption, 11,991.2 us raw send and 2,187.5 us
other work. Nested components are not summed twice. These QPC durations are
elapsed measurements, not independently measured CPU or upper bounds.

The narrowly scoped private candidate substitutes `WSASendTo` for `WSASendMsg`
only when the existing message has no ancillary control. Nonempty control keeps
the original call; datagram buffers, address, synchronous success/error result
and first-send ownership remain unchanged. Eight supplied-API cases, 13 timing
cases and the two existing applied-source checks pass. An initial fixture build
failure from a missing IPv6 header is preserved and corrected with that include.
One candidate funded-four test passes, transmitting exactly the same packet and
wire counts as the baseline. Raw-call elapsed time is 11,527.4 us, only about
3.87% lower in this single comparison. Peer/pool running maxima increase from
2,650,800,128 / 7,180,648,448 to 3,674,210,304 / 8,321,499,136 byte-us, within
unchanged bounds for this small fixture. Its tiny common-four sample is not a
representative saturated interval.

**The candidate is not adopted.** This comparison does not establish a useful
correction for the actual Local32 failure. The candidate's four private worker
source changes are restored to the component baseline after exact hash checks;
no production or installed artifact changed. The
candidate-linked development binary is stale against restored sources and is
not eligible to run or package until rebuilt. Both experiment jobs are reaped;
direct census finds no native process or UDP 39450/39451 listener. Comparison
SHA-256 is `610d0e0f5d64989ca03a58dd92f745d06b944a91c78e4ad0553def9b2f45c890`;
the retained private decision is
`675acd525e1fa50130c12f23e88fc64b201be592404d25e3362d2c2509364aa7`.

### Post-wake decision A: retain strict F1; measured profile unavailable

**A — KEEP F1 STRICT** is selected again after the independently challenged
execution-cost evidence. The actual Local32 path has not demonstrated the
declared finite-grant service and cannot be treated as a qualified production
profile. This applies the existing [profile refusal rule](PooledReliableServiceProof3L.md#physical-headroom),
not a new service constant, lower admission rate or blanket Windows rejection.
The two known wake fixes remain qualified; their correction does not erase
the subsequent actual failure. Passing conservation/recovery, isolated native
controls or configured 18-MiB/s pacing cannot substitute for this missing fact.

B's first-send-anchored alternative still fails 17/32 retained witnesses and
therefore cannot solve the measured capacity failure. C would need a coherent
packet-boundary scheduler that separates urgent ACK/control, protected ordinary
DATA and unsent finite-grant work while retaining FIFO, native first-send
ownership and reserves. Current pinned connection callbacks bundle these duties
under the serial global-lock actor. Prioritizing token-bearing callbacks or
assigning an arbitrary per-pass quota can delay delivery/control and cannot
create packet-processing capacity. D's batching/concurrent submission would
need explicit datagram-buffer/completion/ordering ownership and portability
qualification; no supported flag supplies that architecture automatically.

The next canonical engineering slice is **bounded aggregate native service
capacity**, with a CPU/work feasibility model and a challenged scheduler/IO
design before implementation. Its available execution budget must cover four
finite grants plus protected traffic; byte funding alone is insufficient. Even
optimistically allocating all Q=1,248 B to unique structural service requires
at least 67,108,864/1,248 =53,773.128205 packets/s; actual framing, underfilled
packets, protected traffic, receive/ACK, retransmission and maintenance add work.
The supported execution model must demonstrate that capacity and the unchanged
F1 dispatch/blocking requirement in the complete 32-peer workload/recovery,
rather than infer it from configured rates or isolated four-grant controls. The
existing observer, raw events and direct cleanup census are enough to establish
this refusal. No further evidence framework or timer/API micro-patch is added.
Fresh Local32 follows only a justified, deterministically/native qualified
correction with required execution-changing CI green. Real-TLS Node32 and later
3L gates remain required afterward; this decision does not skip them.

No F1 equation, eligible clock, running minimum, admission, reserve, resource,
recovery or acceptance gate changes. No further physical attempt, provider PASS,
3M work or merge is made. **KI-006 OPEN; Foundation 3L B — PARTIALLY READY.**

## Separate-process native witness: recurring capacity failure (2026-10-08)

Development diagnostic `native-f1-452aa71d-9953c` runs one Server and 32 separate
headless Players on worker loopback, with the original scale/recovery workload.
It is not a physical provider result or a whole-source qualified package. Native
diagnostic source is `452aa71dd`; GNS send/pacing/wake code is unchanged from
qualified C916. The development Server and original C916 Player execute in new
task-owned runtime copies; original packages and installed baselines are intact.
Two preceding setup invocations fail before server start because the diagnostic
descriptor first retained the old binary pin, then used sorted rather than
PackageBuilder's ordered content-table keys. Those failures are preserved;
the corrected diagnostic validates all 57 content members before starting.

The measured native workload fails: 6,905 completed grants, 6,788 qualified,
117 failed across all 32 peers, maximum 82,736,720,768 byte-us and zero finite
shortfall. All 1,928,005,711 accepted bytes first-send, ACK and retire. The owned
job is reaped; native processes and UDP 39450 are absent. All 107 indexed raw
files (142,551,009 B) verify locally and against the worker index, SHA-256
`382a4d57a490b73dd9d801d0895283e6a035e0d4e99060bfb7e3f0f4255a00c4`.

The first Main-observed failed grant is peer `3:1`, token `2373`, 295,256 B:
activation 1027058150896 us, first send 1027058151043, completion 1027058169635,
observation 1027058185979. Its 260 segments independently reproduce the exact
27,393,921,280 byte-us maximum. The first violating pre-send checkpoint spans
4,068 us from the running minimum after segment 102 to segment 147, with
50,028 B of intervening unique service:

```text
16,777,216 * 4,068 - 50,028 * 1,000,000
= 18,221,714,688 >18,025,216,000 byte-us
```

Its largest adjacent segment gap is only 589 us. This is accumulated recurring
under-rate service, not one >1.074-ms blackout. Peer `4:1` crosses arithmetically
68 us earlier; the first observer is not necessarily the earliest native failure.
These witnesses occur during structural offering, before cessation/recovery.
They do not establish the phase or cause of the earlier physical failure.

Frozen native history retains 8,192 contiguous records, sequence
1954664–1962855, with zero drops/counter overflow. Its history-only QPC extent
10270581189113–10270581867649 covers the failed grant at the measured 10-MHz
steady/QPC bridge. The earlier timer-creation metadata is separate. During the
4,068-us interval, thinker passes cover 4,033.9 us; nested SNP spans cover
3,960.9 us, including 3,653.4 structural and 307.5 marked nonstructural us.
Timer/poll totals 8.8 us, global-lock acquisition 3.5 us, receive drain zero.
Nested durations must not be added. A 641.8-us pass visits four structural
four-packet senders and 13 nonstructural single-packet senders. Nonstructural
DATA versus pure ACK/control, actual CPU time and preemption are NOT MEASURED.
Wall spans alone do not prove those causes or unavoidable platform jitter.

**Post-wake decision: retain strict qualification refusal and investigate C's
aggregate execution capacity.** Per-peer packet quanta and a byte pacer do not
bound aggregate callback work. An arbitrary ordinary-callback cap cannot be
derived from the byte reserves and may harm ACK/control. Alternative B's
first-send-anchored prefix curve is independently defensible as an explicitly
different finite-capacity contract, but counterfactual replay still rejects
17/32 retained failed witnesses (only 15 pass). It does not correct this failure;
no amendment or historical reclassification follows. Existing H_run, rates,
running minima, admission, reserves and all acceptance gates remain unchanged.
The next missing fact is execution versus ready/preempted time covering a failed
interval, using a bounded adaptation of the existing scheduler diagnostic.
No speculative quantum/timer patch or new physical attempt is made.

**KI-006 OPEN; Foundation 3L B — PARTIALLY READY; Node32 unrun; no 3M or merge.**

## Fresh corrected-wake Local32: F1 production service failure (2026-10-08)

Physical run `9953fd84-0d6f-444a-93eb-ff8efa1d96ce`, coordinator/lifecycle
`981fbecc-0596-4f90-88b6-47d0cbb2df1c`, executes the qualified native candidate
`c916b7d4f868177ae00b4b05dacfa7b8dbb1f563`. Thirty-two actual Player processes
connect; server exits 10 and the role, coordinator and outer lifecycle fail.
Of 7,133 completed Ready grants, 7,112 qualify; 21 fail across 16 peers.
All reported finite shortfalls are zero. Maximum running deficit is
**96,951,490,816 byte-us** (peer `2:1`), exceeding the unchanged
**18,025,216,000 byte-us** bound. This is **F1 PRODUCTION SERVICE FAILURE**,
not a provider PASS or an infrastructure-invalidated service measurement.

All 1,994,813,588 accepted bytes are uniquely first-sent, ACKed and retired;
terminal release, final pending tokens and active grants are zero. All five
scale phases pass. Gameplay, structural and mixed recovery prefixes converge
in 321,505, 38,775,827 and 315,804 us respectively, within their separate
canonical quotes. Successful recovery and conservation do not erase F1 failure.

The first violating token, interval, native segment/wake chronology, pool
historical running deficit and OS cause are **NOT MEASURED**. Farm final records
retain lifetime maxima and the last grant, which cannot reconstruct an earlier
failure. No attribution to jitter, Nagle, receive work or an induced phase is
made. The October 6 decision below is historical; this residual physical
failure reopens its explicit post-wake decision gate. F1 remains unchanged
while a bounded first-failure witness distinguishes an attributable scheduler
defect from a contract/platform limitation. No further physical attempt follows
this failure without a justified correction and required qualification.

Failed server role evidence and worker capture bytes are retained under
`farm32-9953fd84-failed-raw-v1`, with indexed sizes/SHA-256 verified. Worker
capture reports `FAILED_DIAGNOSTIC` because the role failed, with owned stop and
offline export confirmed. Full bidirectional acceptance is not inferred from
that status. The client capture later exceeds its 500-second deadline;
its failed diagnostic index reports owned stop/export false and no role-index
pin. Client role `result.json` and `evidence-sha256.json` are absent. Existing
client bytes are retained as unsealed diagnostics, never resealed as acceptance
evidence. This separate capture failure does not invalidate the sealed native
server service failure or supply a physical PASS. The retained public chronology
places server exit at 21:28:41.2802105Z, coordinator abort at 21:28:41.878Z and
client capture readiness at 21:23:49.702894Z. The abort occurs about 292 seconds
after readiness, before the capture deadline; waiting afterward for the absent
client role index causes the secondary timeout. All 32 unsealed native client
logs contain scale completion PASS, which does not replace missing role sealing.
No increase or restart of the 500-second capture clock is justified. Both child
trees are reaped.
Direct endpoint census verifies no
task process, UDP/control listener or run firewall rule in either policy store;
worker Packet Monitor is stopped, filters absent and capture service idle.
Exactly six run control secrets are retired; cleanup receipt SHA-256 is
`533b7f8f058373f9ce6319e4b418f175e9901279b0f5b481c3b1bf5d913c0dfa`.
Raw failure evidence and installed baselines remain intact.

Offline analyzer revision `4825df1bcc5f62a816fe35d68be4df9d83f9eb77` also has
all six original hosted jobs green: Windows 96 native +387 Python each, Linux
53 each, GNS 11 each, zero failures/errors/skips. Original archive index SHA-256
is `ca714f838fbd34953c0de82a18018169d414817f29b96206f402e4d6244440eb`;
source/merge tree reconciliation is `3c3eae3f1d7cb87314a0bdb83305ef95552c96d1`.
These jobs qualify the analyzer, not the failed provider service.

**KI-006 OPEN; Foundation 3L B — PARTIALLY READY. Node32 unrun; no 3M or merge.**

## Post-wake decision A — keep F1 strict (2026-10-06)

Native candidate `c916b7d4f868177ae00b4b05dacfa7b8dbb1f563` qualifies both
identified wake corrections without changing F1. The old-source regressions
fail and corrected applied Windows/Linux branches pass; native compatibility
passes 13/13. All six original push/PR jobs pass with no failures/errors/skips:
Windows **96 native +382 Python each**, Linux **53 each**, GNS **11 each**.
Original index SHA-256 is
`d954fa02e4d1fa853003585d178aeb7cdd4008eefb247d9e27d22f540487e062`;
all actual source/PR merge trees reconcile to `7d44512879f37988c8029ae3650788efa48e712c`.
Each Windows job's five required timing commands succeeds with normal-priority
children, reaping and zero recorded loss. The official package dispatch
`37539693115` separately passes Windows 96/Linux 53. Package artifact
`11450431614` has SHA-256
`f46f34192bb5316429d6385d4fd40732ad56cc5d8c07ddd127edcfe7c788102b`;
its Player/Server byte closures verify. Passing package jobs do not replace any
original job; all originals independently pass.

One predeclared clean, diagnostics-off batch passes **14/14** fresh normal-priority
children: four ACK-statistics, four funded four-grant, four mixed-traffic and two
structural 32-peer workloads. No retries, remaining processes or listeners;
all 58 retained files match their sizes/hashes. Matrix SHA-256 is
`49c179511e3e0a1764ad16d8bb6561c23558998324a3c5adb1509309cef094a4`.
ACK-statistics consumes 0.594–1.125 CPU seconds per approximately 64.3-s case;
the two 32-peer cases consume 31.172/32.438 CPU seconds over 40.031/40.125 s.
Default-off 32-peer and normal hosted outputs prove workload/conservation,
but their whole-peer F1 certificates are **NOT MEASURED**. Separately retained
instrumented development records qualify 2,408 grants/32 generations, with
285,525,458 B exactly first-sent/ACKed/retired, maximum 9,324,008,704 byte-us,
and common-four maximum 12,806,797,312 over 7,248 us. That worker cache remains
development scope; no provider result is inferred from it.

**Decision: A — KEEP F1 STRICT.** Independent challenge supports the existing
finite and recurring equations, including slow-grant/blackout negative tests.
Alternative B cannot derive a new constant from the old 1,109-us observation or
remove running verification merely because finite completion passes. Alternative
C needs measured remaining software monopolization; a dedicated thread alone
does not prevent OS preemption. An explicit soft real-time alternative D would
require a different, justified service/deployment contract and new negative
proofs; no residual measured failure here justifies that semantic change.
[Microsoft timer creation](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw)
and [thread scheduling](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleep)
do not establish a universal bounded ready-to-running delay. Retaining the
strict measured qualification requirement makes no such OS guarantee. A valid
future violation still fails and requires attribution/architecture review;
eligible clocks and failure history are never discounted. The original DA gap
remains **NOT MEASURED** causally and its historical failure is preserved.

Fresh Local32, real-TLS Node32 and the remaining resource/parity/final gates are
still required. KI-006 is OPEN, Foundation 3L B — PARTIALLY READY. No physical
PASS, 3M work or merge is claimed by this decision.

## Absolute sender wake correction: source regression qualified (2026-10-06)

The fixed FULL/ACK-statistics diagnostic on observation revision `b66ceb649`
is a **non-reproduction**. Run `37530178397`, Windows job `112497231286`,
passes both workloads and retains 48,896 control-phase records without overflow.
The timer creation succeeds; this run does not exercise the NULL-timer path.
Its diagnostic ZIP artifact `11445024420` has SHA-256
`3a304236d23f287822ed6cad80b1c214e1b477006e19e8036c47eb21ff09e302`.
It neither explains nor replaces the original `da16d5aba` failure below.

Two independent defects are established by the actual applied pinned sender
branches. Windows and Linux calculate remaining time with one clock reading,
then add it to a later clock reading, moving the absolute deadline by the
intervening pause. Windows also requires a timer handle for the existing
at-most-1,000-us spin even though that path never uses the handle. The correction
uses one clock sample for the target/clamp and requires the handle only for
the long timer wait. Manual polling, no-running-grant and Never bypasses,
long NULL-timer/error fallback, event wake, timer ownership and priorities remain
unchanged. No F1, admission, reserve or transport-send constant changes.

The existing native fairness target compiles test bodies extracted directly
from the applied socketthread source. Bounded OS/clock stubs show old Windows
and Linux deadline 210 us versus required 160 us, and old NULL-timer short wait
falling back to an integer millisecond. Corrected branches reach 160 us and
avoid that fallback; clamp, bypass, already-due, event, timer and error routes
also pass. The worker native build and fairness test pass with owned jobs reaped;
the final census finds no task process or UDP 39400/39450 listener. The worker
cache is development evidence only. All 13 existing selected compatibility tests
pass sequentially, including F1, mixed traffic/reserve, retry, funded ACK and
feedback refresh; the actual applied pinned-source checks also pass. Original
hosted CI and the exact-source physical package remain required before a fresh
provider run.
The original 1,109-us plateau's cause remains **NOT MEASURED**.

## Original hosted ACK statistics fails; exact wake attribution pending (2026-10-06)

Correction `da16d5aba83f2edffe789491aed8e8258a0ca967` is **not CI-qualified**.
Original push run `37518739890`, Windows job `112458311111`, passes 95/96
native tests but fails CTest 83, `gargantuan_gns_funded_ack_stats`. Its baseline
control arm (`prompt=0`) token 24 has 347 unique first-send segments conserving
393,652 B. The exact first running-bound crossing and maximum are the same
zero-service interval: post-segment 12 at 2,921,128,165 us to pre-segment 13 at
2,921,129,274 us, 1,109 us. Its maximum **18,605,932,544 byte-us** exceeds the
unchanged **18,025,216,000** bound. Finite shortfall is zero; activation to
completion is 21,818 us against 29,537.874 us allowed. All 1,969,723 cumulative
accepted bytes first-send, ACK and retire; pending/unacked/active grant are zero
and retransmission is zero. Those independent successes do not waive F1.

The original job log SHA-256 is
`251ba38201261b1a8f82669727e376486666d6217eb5e4a6fdd21d764c42a2d9`;
original diagnostic ZIP artifact `11441327474` is 8,590,822 B with SHA-256
`b692e1548c044424ba82653804591e359688db9903f7b8906adf86d615852eb3`.
Both retained segment lists reproduce the same exact curve. The actual reason
for the gap is **NOT MEASURED**: the original has no wake-phase or ETW record.
The grant's unsent structural work bypasses the ordinary DATA pacer. Both
original GNS sanitizer jobs pass 11 tests; successful counterpart/package jobs
cannot substitute for this failed original.

The existing exact ACK statistics diagnostic now reuses the ACK-cycle
65,536-record timer/create/wait/lock/receive/thinker buffer under its existing
opt-in environment. Detailed callbacks stay disabled. Phase recording stops
after the control pair is destroyed/joined; both full native arms still run,
and the borrowed buffer lives until their final destruction before rendering.
Normal CTest/runtime has no added clock reads or allocation. The existing
parser scopes phase records to the control arm and service thread and rejects
prompt/inter-arm rows, overflow, missing phases or uncleared ownership. An
initial worker probe passes both native arms but overflows when recording both;
that diagnostic refusal is retained and the same buffer is narrowed to control
without enlarging it. This bounded observation answers a demonstrated missing
causal fact; it changes no timer, F1 equation or acceptance gate. No fresh
physical run is made before attributable correction and required CI.

The control-only successor compiles on the worker and passes all 42 existing
scheduler/helper tests with no skips. Its exact native statistics probe runs
both complete arms and passes, retaining 42,184 control-phase records with no
overflow and a loss-free scheduler window. All seven raw files match their
recorded sizes/hashes. This validates the bounded observation and is a
**non-reproduction**, not an explanation or replacement of the hosted failure.

## Pooled ordinary DATA pacing: native correction checkpoint (2026-10-06)

Bounded worker development runs reproduce grant-scoped F1 failures while
ordinary callbacks compete with four structural drains. An original failing
gap overlaps 711 us of the remaining thinker batch and 329 us of 33-socket
receive processing, with no ETW switch-out in that interval. DPC/ISR time is
**NOT MEASURED**. Detailed native observations identify many ordinary sender
visits; a packet quantum alone does not bound their aggregate wire work.

The correction normalizes ASAP insertion to the current native timestamp and
bounds attributed pooled ordinary DATA while another structural grant is
partly first-sent. Its shared wire ceiling derives from existing reserves:
96 - (64 + 8) = 24 MiB/s, with at most one native datagram plus 48 B of credit.
Own unsent structural work and unmarked FULL/default clients retain their
paths. Independent ACK/NACK/stat deadlines, FIFO, admission, retirement and
every F1 constant remain unchanged. Failed/control-only sends refund credit;
successful DATA, including retransmission, consumes actual native bytes +48.

Corrected native run v8 retains **2,397/2,397** qualified grants across 32 peers,
zero F1 failures, maximum running deficit **9,652,408,000 byte-us**, and exact
common-four pool deficit **20,591,981,888 byte-us** over a **7,419-us** overlap.
Accepted = first-sent = ACKed = retired = **285,820,072 B**, terminal release
zero. Corrected active-eight ordinary control v9 retains **2,415/2,415**
qualified grants, 32 valid peers, maximum **10,410,404,352 byte-us**, and exact
retirement of **288,627,112 B**. Its native child exits zero; the old active-32
coverage parser rejects the control identity, so wrapper coverage is not PASS.
Both runs' seven raw files match their recorded lengths and SHA-256 hashes.

The corrected worker compatibility sweep passes all 13 selected native tests:
real transport, retirement/fairness, retry safety, funded ACK/compatibility/
four-grant/statistics, packet tails, four-grant/reserve and sanitizer-safety
fixtures, mixed traffic, observation source contract and pooled feedback
refresh. The updated pinned mechanism source check passes. The earlier
12/13 sweep's repeating-timer failure remains preserved; its initial poll
now fences the native microsecond tick so both normalized ASAP deadlines are
strictly due, without weakening its existing assertions.

These are development results on an explicitly mixed worker cache, not a
whole-checkout or provider qualification. The correction requires its own
original hosted CI, exact-source package and fresh physical acceptance.
Historical Local `2ca` remains F1 FAIL, Node32 is unrun, KI-006 OPEN and
Foundation 3L B — PARTIALLY READY. No 3M or merge is authorized.

## Fresh Local32 recovery passes; F1 running service fails (2026-10-06)

Three fresh Local attempts execute qualified A
`ed09e7126c158a1b8c0dae3e51081f54cb29339e`. Reviewed analyzer B
`8c5a22327a14af09d397ba8c3e647201143cb06a` is used only for offline
package verification; full strict provider replay is **NOT_REACHED**.
Their original failed sequence and terminal receipts remain unchanged:

| Run | Actual result |
| --- | --- |
| `d326a539-d817-4bfd-9015-b99ac3bef6aa` | Worker capture/export reserve volume is insufficient before Server native launch. Native recovery and F1 are **NOT MEASURED**. |
| `3a5d4860-9063-4043-8619-adf6a1098f3b` | Prepared runtime projection receipt mismatch from the `PackageRoot` case in the operational input, before native launch. Native recovery and F1 are **NOT MEASURED**. |
| `2ca63cf8-db11-4024-9ace-48725185bd74` | After the reserve and projection inputs are corrected, 32 actual native clients run. Server exits 10: causal recovery passes, but F1 running service fails. |

The first two corrections change operational inputs, not production, admission,
service limits or acceptance predicates. The third run uses coordinator
`35274d15-318a-478e-a793-b8992fcdc4a6`. Its retained causal TSVs and
`server.stderr.log` report all three cases **PASS** under their original bounds:

| Case | Accepted = first-sent = ACKed = retired B | Earliest prefix convergence us | Original bound us |
| --- | ---: | ---: | ---: |
| Gameplay | 5,344 | 335,246 | 20,470,580 |
| Structural | 355,274,682 | 13,079,486 | 26,536,577 |
| Mixed | 13,087 | 350,132 | 20,471,117 |

Later strict-barrier observations do not turn these converged cases into
deadline failures. Whole-run accepted, first-sent, ACKed and retired bytes
conserve at 1,486,393,002 B; terminal outstanding debt, active grants, pending
tokens and terminal release are zero. Conservation and recovery do not waive
F1. All 32 peers latch a running-service failure: their maximum running
deficits are 21,984,479,872–44,012,964,480 byte-us, above the unchanged
18,025,216,000-byte-us limit. Maximum finite-grant shortfall is zero.
The earliest failing historical token and its first-send gap remain
**NOT MEASURED**: retained summaries contain historical maxima and last-grant
timestamps, not the missing per-grant first-send history. ACK retirement wait
cannot substitute for first-send completion or identify that gap.

Failed raw evidence is preserved under
`C:\Users\aiden\.codex\artifacts\farm32-2ca63cf8-failed-raw-v1\`.
`preservation.json` SHA-256 is
`ba7e67adcfb3b38026e8bbe67d044c65d8dfa02e8f6f4c1f5b37a54f2859152b`:
29 role/capture files plus four public receipts, including the 3,116,367,872-B
ETL and 2,902,153,460-B pcapng, retain their verified hashes. Capture/controller
failure remains failure; preserved bytes do not establish capture acceptance.

Each attempt's exact six run-owned control secrets was retired through the
existing held-object checks after fresh full census. For the third run,
all twelve journals and the final receipt are under
`farm32-ed09e712-local32-v3\cleanup-raw-2ca63cf8\retirement\` in the same
artifact parent. `final-result.json` SHA-256 is
`3890bc06b64368143a53e37ff42619221cb4c2b616f4e3d7c3d1d66b77a09713`.
Final CLIENT/SERVER/native observations at 13:17:15–18 UTC show all six absent,
no owned/task/native/capture processes, task listeners or firewall rules,
and capture services running and idle with all eight service files matching.
Raw evidence, private configs and original failures remain preserved.

This is **Local32 FAIL**, not provider PASS. Node32 is not run; final acceptance,
3M and merge remain blocked. The retained four-client physical F1 PASS keeps
its historical source/run scope and does not qualify this later 32-client
recovery campaign. No architecture or runtime-source change follows from this
receipt. **KI-006 OPEN; 3L B — PARTIALLY READY**.

## Qualified execution source and offline archive bound correction (2026-10-06)

Execution candidate A is `ed09e7126c158a1b8c0dae3e51081f54cb29339e`.
All six original jobs pass: Windows 96 native plus 378 Python each, Linux
53 each and GNS 11 each, with zero failures/errors/skips. Original inventory
SHA-256 is `9920da45cf0b1b6992e49ed449b139fa644652edbf88590364381095b9675cf0`;
counts are `6a16e7539d3303a9a9a992cc07fdef453502185dbbb1c460b4a776fb8d393657`;
checkout reconciliation is
`9db5f8b30720e47f3b59645f2ddd9ee6aa06050a9ebc843a3571f7a01b87d754`.
Pushes execute A, PR jobs execute merge `442a48c`, all with tree
`a4074220e4d8baf86a5ad873750977fe1fa9954e`. Ten trace sets have original
successful exits, zero drops, exact arguments/Normal priority and cleanup.
PR late preparation validates and stops only System32 Appraiser PID 1080,
then records actual absence; no unrelated CPU or old blocked-wait cause is
inferred.

Official dispatch `37441120894` also passes Windows 96/378 and Linux 53.
Its Windows log SHA-256 is
`7eb9cd3019db9b7fbd71ac4354b77a808318443ebcff69166122cc46a00cf107`;
Linux log is `8e01c0de9079086c16335be45a959df31d936f3e523c3a4b4455adafae48523b`.
Package collection then refuses the original diagnostics ZIP's 286,761,989
aggregate expanded bytes against an obsolete 256-MiB reader bound. The ZIP
is 40,829,539 compressed bytes/270 members, SHA-256
`52d1acbff744b344b03aa7f29dca50f1020240541b6fe80aba6c75e0b5e92308`.
Only 64,374-byte JUnit and 42-byte source-marker members are needed; unused
trace/negative-fixture members are never extracted or decompressed by this
reader. This is an offline tooling refusal, not CI or production failure.

The minimal later analyzer B removes only that unused aggregate predicate.
Compressed/member-count/path/duplicate/required-member/digest/source/JUnit
checks remain. The complete offline acceptance unit suite passes 34 normal
and 34 optimized tests, zero skips, including unread-large-member and required
size/path/duplicate/count/source refusals. The existing A/B contract requires clean reviewed descendant
B and separate focused validation, while native packages, live qualifier,
CI and physical provenance remain exactly A. Partial collection and original
ZIPs are preserved; no archive is repacked. Exact-source package verification
still precedes versioned staging, startup checks and fresh physical preflight.
No Local32 or Node32 result is inferred. **KI-006 OPEN; 3L B — PARTIALLY READY;
no 3M or merge**.

## Active hosted telemetry without a registered task (2026-10-06)

Source `92e3c9b9627ee4ffb45fc086e1f285e5953ecf72` PR Windows job
`112181731697`, run `37437173945`, correctly refuses setup at
`2026-10-06T08:38:10.0921351Z`: successful enumeration finds no exact
registered task, while the actual `CompatTelRunner.exe` census is one.
Its raw log SHA-256 is
`2cde6b41565616af7f9d5a3d5084eef4fa802cfbf1f9c2c8e82df116f6a062be`;
diagnostics ZIP is `ff8c8ed0962ed4d46b56d3b6f9a001efe1512062a87cce231d861ba172e127e9`.
PR native build/tests/timing are **NOT RUN** and PR Linux is skipped.
No resource PASS, package or provider result is inferred. The new instance's
PID, path and birth were **NOT MEASURED** by that earlier step.

The image can run this competitor independently of the named task. Bounded
preparation now handles only the exact Windows System32 Appraiser executable
on the disposable GitHub-hosted VM. Every candidate must have the expected
name, normalized System32 path, positive PID and nonnull native birth. A
`System.Diagnostics.Process` object opens its handle before a second successful
CIM PID/birth/name/path check. Termination and bounded exit wait use that same
held object; identity change or API/handle error refuses termination, and
the object is disposed on every path. This avoids reopening an arbitrary PID.
Successful disappearance permits no kill and still requires a fresh empty
final census. Reappearance or a persistent process fails; there is no cleanup
retry loop, tree kill, arbitrary image or user-host operation.

This same closed-target quiescence immediately precedes the five timing
workloads. Its exit wait consumes only the remaining 15-second monotonic
budget, and the one-minute outer step still bounds synchronous APIs. Actual
PowerShell/runtime versions and checked fixed identities are retained in
host logs. Optional exact-task disabling/stopping and Disabled readback remain
unchanged. Only exact Appraiser absence is established; unrelated background
CPU and the old 119-ms blocked API remain **NOT MEASURED**. No latency is
discounted or production code, workload, priority, affinity, timer, service
contract or acceptance gate changed. The two focused mocked boundary tests
pass in normal and optimized modes with zero skips; their driver exits 0 and
owned test-process census is empty. No real task/process method or native
workload is exercised by those checks. All new original CI remain prerequisites to Local32. **KI-006 OPEN; 3L B —
PARTIALLY READY; Node32 not run; no 3M or merge**.

## Hosted task absence is a resource fact, not a setup defect (2026-10-06)

Source `0dd022697622307fa49f53171a4f25347651ed8c` fails before native
build or measurement. Push/PR Windows jobs `112174997839`/`112175015251`
and official dispatch Windows `112175377621` reject the initial exact
Appraiser task lookup: the hosted image reports no matching registered task.
Disable/Stop, native build, Windows tests, timing and packaging are **NOT RUN**.
The two downstream original Linux jobs and dispatch Linux are skipped.
No package, stage, run identity or physical attempt is created. Push/PR logs
have SHA-256 `a1138b8319bd777003cd517767d48d86aaf8ba6424b3a2e4c9a246c4cde7d9b6` /
`949b34b16fd532b35814a920ca4d87d501ca1d4801b56674fb74141cb7bbc13e`;
dispatch log is `0a7ac19acce47ac145daa4b783eb7451d2cc932600f7476aee80279247820a63`.

This is an unnecessary harness requirement: acceptance needs absence of the
measured CPU competitor, not proof that its scheduled task exists. The minimal
correction successfully enumerates tasks with terminating errors, then filters
the exact path/name. Zero matches permits no task mutation and requires an
actual zero process census, logged **NOT_REGISTERED**, not Disabled. An active
Appraiser without the registered target, duplicate target or provider error
fails setup. A present exact target retains Disable→Stop, strict Disabled
readback and bounded absence. A fresh read-only process census immediately
before the five timing workloads also requires zero. No errors are swallowed
as absence and no process-kill fallback is introduced.

The two focused mocked host-boundary tests pass in normal and optimized
modes with zero skips. Their driver exits 0 and owned test-process census is
empty; no real task action or native workload is executed. All other
source, helper, workload and acceptance logic is unchanged. The complete
hosted inventory remains 40 helper plus 338 other Python checks. Both failed
source revisions remain failed; fresh original CI and exact-source packaging
remain required before actual Local32. **KI-006 OPEN; 3L B — PARTIALLY READY;
Node32 not run; no 3M or merge**.

## Hosted telemetry contention and pooled observation gap (2026-10-06)

Candidate `be949192ca867b0c5dc07b364187120fcb4a0ea9` does not qualify:
its six original jobs contain four successes, PR Windows failure and PR Linux
skipped. Both Windows jobs pass 96 native and 374 Python checks; executed
Linux has 53 and both GNS jobs have 11, with zero JUnit failures/errors/skips.
Successful push results do not replace failed PR qualification. Original
index SHA-256 is `267a4f726d11a178bbbbe7e2d77688e196ce1360e495e6f614663ca08133c5bc`.

PR Windows job `112066581327`, run `37400561278`, fails Full/mixed RPC
p95: 232.4581 ms >150 ms. All 49 RPCs complete without error; the four
initial requests at steps 0–4 are the outliers. The loss-free trace retains
Normal child priority, original exit 1 and successful cleanup. Its p95
witness has 3.2167 ms scheduled and 229.2414 ms off CPU. Three completed
fixture sleeps account for 58.4899 ms. Separate UserRequest waits retain
119.2905 ms waiting and 51.0821 ms ready delay. The larger client-poll
maximum at step 18 occurs after these RPCs and cannot explain them.

Decoding the original ETL identifies competing PID 5596 as
`CompatTelRunner.exe`, executing `appraiser.dll/DoScheduledTelemetryRun`.
During ready delays of 28.8088, 22.2733 and 23.0174 ms, telemetry consumes
105.425, 66.0638 and 85.8189 logical-CPU-ms across four processors.
External CPU competition is **MEASURED**. The 119-ms blocking API, lock owner
and exclusive cause remain **NOT MEASURED**; no delay is subtracted. Original
ETL SHA-256 is `5bdfd1635b9504b3ddaf4b83734635297d9b08f1bd829ee4b40ddec66376d813`;
read-only `tracerpt` decode SHA-256 is
`89886860d459ea372ecb80d155aee6e3fd3082c1ad6d9a50aaa52ee50db7be14`.

Separate official package dispatch `37400664975`, Windows job
`112066902930`, fails the first direct pooled workload's mixed Event/action
maxima: 2186.4521/2188.0690 ms >250 ms. Same-batch RPCs complete within
73 ms; Event and action retain the same steps 120–126 as passing push,
which observes 92.2021/92.3351 ms. Five completed sleeps consume 533.7300 ms
versus 76.9633 ms requested, and client poll 126 consumes 1644.8528 ms.
No thread/process CPU advance is recorded across the Event interval; its
endpoint CPU-query brackets are only 1.6/2.9 µs. The later 2925.88-ms sleep
is after completion. This is waiting/descheduling evidence, not a proven
transport defect. Its exact lock/host cause and direct child priority are
**NOT MEASURED** because the first command has no scheduler trace. Raw log
SHA-256 is `b954696fe9277abea9ab8d00e97290c4643b69e628ffbcc7c328fe4327eece3b`;
diagnostics ZIP is `22a4a5ee167885a4d959f6c4db388f9efe724d608a913e87fe29f290a123b465`.
No official package is produced or staged.

The bounded correction removes this measured scheduled-telemetry competitor
only on the disposable GitHub-hosted Windows VM, using its exact registered
task, disabled readback and actual process absence. It changes no user host,
security service, production code, priority, affinity, timer or acceptance
clock. `PooledFull` routes the unchanged first pooled command through the
existing Normal launcher and trace; all five logical workloads still execute
once in order. This closes the demonstrated first-command observation gap,
without adding a verifier framework or declaring either failure repaired.
The standalone helper builds with MSVC `/W4 /WX`; 40 normal and 40 optimized
helper-enabled noncapture tests pass with zero skips. Hosted task actions in
those boundary tests are mocked; no user endpoint task is changed. Both
drivers exit 0 and final helper/compiler process censuses are empty.
All new original CI remain required before a
fresh package/preflight/Local32 measurement. Client RAM has reached 17.01 GiB,
but fresh preflight remains mandatory. **KI-006 OPEN; 3L B — PARTIALLY READY;
Node32 not run; no 3M or merge**.

## Pooled aggregate hosted timing failure before scheduler capture (2026-10-05)

Source `09aa075ad420c1ff1c647a8cf8a207cdb18e00e7` passes the four focused
native regressions, both original GNS jobs and the PR Windows checks. Its
original push `37393893089`, Windows job `112045140196`, nevertheless fails
the second direct command, `--pooled --reliable-workload-32-structural`.
At `2026-10-06T01:19:46.6824650Z`, aggregate-recovery peer 0's RPC p95 is
170.268 ms against the unchanged 150-ms bound. All 48 requests complete with
zero errors; p99/max are 178.411 ms. Native JUnit is 96/96 with zero
failures/errors/skips and Python is 372/372. Push Linux is skipped. Successful
PR or package-dispatch results do not replace this failed required original.

The retained raw log SHA-256 is
`4513c708b20a8cd1ae1002d322f5033f6401a11b85f6f1caac137510f8bc425c`;
original diagnostic ZIP SHA-256 is
`6cca2c70f10c086eebad8837ad717c7e13952f005a32a760ad9a3f8e2ecbc48b`.
RPC 24 and RPCs 25–28 exceed the p95 bound. Their wall/CPU observations
support an off-CPU or blocking contribution, but cannot distinguish native
lock waits from host descheduling. Larger intervening tick stalls do not
directly explain those request intervals. All scheduler wrappers were later
commands and never ran: failing-window scheduler evidence and the original
child's API priority are **NOT MEASURED**. No production cause is inferred.

Only that second command now uses the existing bounded scheduler controller
and verified Normal child launch. Its pooled flags, workload, logical order,
single execution, original wall clocks and acceptance gates are unchanged.
No new verifier or replay layer is introduced. The helper builds with
MSVC `/W4 /WX`; 36 normal and 36 optimized focused noncapture checks pass
with zero skips. The final helper/compiler process census is empty. The
driver's earlier nonzero status from matching its SSH ancestor is retained;
it does not replace the measured test exits or final process census.
New original hosted qualification precedes another actual Local32 measurement;
the failed job is preserved and not rerun. No fresh physical run is consumed
at this checkpoint. Retained F1 PASS remains valid; **KI-006 OPEN;
Foundation 3L B — PARTIALLY READY; Node32 not run; no 3M or merge**.

## Corrected Name candidate: actual Local32 still fails recovery (2026-10-05)

Candidate `b9f8ded29dcd3e9b1be805f5e76b03e901c9c162` passes all six
original hosted jobs: Windows 95 native plus 372 Python checks each, Linux
53 checks each, and GNS 10 checks each, with no JUnit failures/errors/skips.
Native push/PR runs are `37378058515`/`37378065805`; GNS push/PR runs are
`37378058572`/`37378065818`. Original index SHA-256 is
`e19a41b99648db325a81718ce7ec85f0d91a5a2bdd511d0a50e2318f2c61fcef`.
Official non-diagnostic dispatch `37378078144` also passes both jobs and
package validation. Artifact `11376237179` has original ZIP SHA-256
`84c8024f60711bb1657fb6b0aeda76c958741ce7a441cf1550bcff1e00079c0f`.
Both endpoint inventories and projected startup smokes pass before launch.

Fresh Local32 run `1072bd82-e3b6-4956-b919-778caa5dcaab`, coordinator
`fbf1cc09-82fc-492c-a1a5-211ea871b9d7`, starts at
`2026-10-05T23:19:53.181528Z` and finishes collection at
`23:28:44.083276Z`. Both actual staged imports/configurations, fresh role
preflights and the control barrier pass. Client preflight measures
10,064,433,152 available bytes. All 32 clients reach readiness. The first
production result is `recovery_work_did_not_converge_within_bound`.

Structural cessation is tick 9,646 at monotonic 772,020,032,573 us, with
fence 23,246. The frozen upper bound is 894,997,894 B across 2,341 frames;
the unchanged derived deadline is 35,112,557 us. Prepared/accepted causal
events reconstruct all 32 cursors below that fence at the deadline:
22,431–22,648, or 598–815 records behind. The failure is real at the
deadline. Reference completion only enables its report at 65,080,874 us;
19 peers have converged by that later terminal observation and 13 have not.
Post-cessation accepted/first-sent/ACKed/retired are
883,606,031 / 882,610,010 / 882,498,588 / 882,498,588 B. Outstanding debt
is 1,107,443 B and terminal release is zero; this is interrupted accounting,
not convergence.

The 1,174 completed recovery ticks average 55.28 ms of work. Sampled live
Session work averages 55.47 ms, with 42.64 ms of incremental preparation
and 11.38 ms of encoding. These are wall times, not thread CPU measurements.
Process counters observe approximately 2.08 CPU cores across Main, the
reference worker and transport, without an exclusive per-thread split.
The final frame-to-failure interval is 41.350 ms; measured Main phases
account for 39.303 ms, so final sealing cannot explain a 30-second stall.
The asynchronous reference competes during measurement, but its exclusive
effect is **NOT MEASURED**. Early allowance can also reject a stale native
snapshot before reaching the existing post-encoding refresh; the separate
feedback-deferral counter was not exported and its incidence is
**NOT MEASURED**. No deadline or service allowance follows from these timings.

The Name correction alone is insufficient. The next candidate preserves the
t0 reference and measures live prefix convergence before starting the same
bounded reference replay, then compares that original timestamp with the
unchanged derived bound. It also refreshes genuinely stale native evidence
before eligibility, retaining full ACK/retirement and funding validation.
These execution changes require focused regression and hosted qualification;
they are not a recovery PASS or authorization to skip Local32 acceptance.

Focused worker qualification passes the corrected real-32-peer feedback test,
GameSession suite, seven-case frozen-reference suite and admission-evidence
suite (four CTest entries total). The exact same feedback fixture against the
original `b9f8ded29` GameSession ages 31 real native snapshots, makes no
refreshes, accepts one grant and records 62 freshness deferrals; it fails the
required refresh assertion. Corrected production reads 31 fresh snapshots,
accepts four grants and records zero freshness deferrals. Its unavailable
refresh arm makes 62 actual queries but accepts only the one fresh peer's
grant and records 62 deferrals. Both corrected arms converge native
first-send, ACK and exact retirement without terminal release. The fixture
uses the existing legal one-tick relevance setting to isolate eligibility
from catalog-refresh cadence; production defaults are unchanged. Server
compilation, the existing recovery parser and whitespace checks pass.
Raw source/executable pins, original failing controls and corrected outputs
remain in `farm32-feedback-refresh-native-b9-v1`. Hosted checks and fresh
physical acceptance for this correction are still pending.

Failed diagnostic captures, role/coordinator results and initiating errors
remain preserved. Outer launch fails and collection refuses provider
acceptance. Fresh cleanup at `23:41:32.522081Z` observes all six controls
absent, no run-owned processes/listeners/rules, idle captures/services and
no Packet Monitor filters. Final cleanup SHA-256 is
`e5586c2f13a4d8a6c96e074870b1a4870415860a1b1b08f943f6dc7cd9a99ef6`.
The earlier five-control attempt stops on a transient PID census refusal;
its records remain, and only identity is retired in the fresh continuation.
No unrelated process is killed. Raw evidence is retained in
`farm32-b9f8ded29-local32-v1`, its private `collection/server-role`, and
the read-only `farm32-1072-recovery-readonly-v1` artifact directory.
Retained F1 PASS is unchanged. **KI-006 OPEN; Foundation 3L B — PARTIALLY
READY; Local32 FAIL; Node32 not run; no 3M or merge.**

## Actual Local32 recovery convergence failure (2026-10-05)

Tooling `999d3b0ffe03bcb1f71d8f76d96623907169c965` passes all six original
hosted jobs: both native Windows jobs pass 95 CTest entries and 372 Python
checks, both native Linux jobs pass 53 entries, and both GNS sanitizer jobs
pass 10 entries. Native push/PR runs are `37360882888`/`37360890152`;
GNS push/PR runs are `37360882751`/`37360891066`. No rerun substitutes for an
original result. The retained original-artifact index SHA-256 is
`961c4672a44f561cd42fe2ba45f6f5292cc5e114e245e554bf3e9a76978cee84`.

Fresh Local32 run `441c7f4c-870f-4e8f-92c9-66674adc86ff`, coordinator
`caf625f2-335c-4412-b0eb-ec921ccb952b`, uses unchanged native/package
`9ae8f68c2c8ed50589b5c54ed56657fa6fd02aad`. Fresh resource preflight reports
11.42 GiB client available RAM. Actual staging, installed pins, child imports,
capture configuration and one-use control preflight pass. The run reaches
production recovery. Its first native failure is
`recovery_work_did_not_converge_within_bound`; subsequent client connection
closures are consequences of server shutdown, not the first cause.

At cessation the journal fence is 23,240. The immutable reference totals
607,606,906 B across 1,604 frames, deriving the unchanged 31,088,518-us
convergence bound. All 32 source cursors remain below the fence at that
deadline. At the 37,494,756-us terminal observation only three have converged;
29 remain incomplete. All 1,466 accepted post-cessation frames match their
frozen per-peer prefix exactly in sequence, complete bytes, fingerprint and
cursor coverage. There is no measured live-reference divergence or cancellation
explaining this miss. Delayed quote sealing does not extend the deadline.

The terminal live ledger records 555,370,399 B accepted, 554,025,459 B uniquely
first-sent, and 553,795,791 B ACKed/retired, with zero terminal release. This
is interrupted-run accounting, not terminal convergence. The 644 completed
recovery ticks consume 37.457 s of measured work, with median 59.519 ms,
p95 78.351 ms and maximum 90.092 ms. Preparation dominates sampled Session
work. Source inspection finds Name preflight decisions that reject already
represented history or an unselected trailing record, forcing avoidable
oversized candidate copying/encoding. Their exclusive physical time contribution
is **NOT MEASURED**. A narrow correction requires exact-output regression and
new execution-changing CI; admission, freshness, recovery and F1 gates remain
unchanged. No blind physical retry is justified.

The narrow correction aligns both live and frozen Name byte proofs with the
existing producer: skip transactionally covered native Name history before
charging an operation, and stop at the selected operation limit before testing
an unselected trailing record. Selected-string validation remains unchanged.
On the cached MSVC/GNS worker, baseline `999d3b0f` with the new regression
performs five encoder calls against the reference's six and fails the intended
one-encode assertion. The corrected covered invalid-UTF8/embedded-NUL cases
perform one call against six; the unselected-barrier case performs one against
two. Deferral, rejected preparation and accepted preparation all retain exact
bytes, fingerprints, cursors, read charges and transaction results. The selected
declaring-class guard remains a fallback; its fixture now places that operation
inside every reduced candidate rather than outside the selected prefix.
Corrected `gargantuan_replication_relevance` passes at 21:44 UTC (8.31 s), and
`FrozenRecoveryQuoteTests` passes with the same production source (68.78 s).
Both CTest entries have zero failures. Initial missing-DLL test startup and the
intermediate fixture failure remain preserved alongside the final results in
`farm32-name-preflight-native-999d-v1/worker-logs-v1`. Hosted qualification and a
new official package are still required; this deterministic improvement does
not establish corrected Local32 recovery or provider PASS.

The current run's F1 terminal result is **NOT MEASURED**: the recovery exception
occurs before the normal terminal F1 writer, and the existing reader finds zero
observations. Fresh feedback observations and ongoing ACK/retirement progress
are not substitutes for that service-contract verdict. This absent result
neither invalidates the separate retained F1 Phase 1 PASS nor proves F1 passed
in this failed provider attempt.

Both capture indices retain the correct identities but are
`FAILED_DIAGNOSTIC`, so collection correctly refuses provider acceptance. The
worker owned stop and offline export succeed with zero recorded event/buffer
losses and 1,759,989 diagnostic frames. Client owned stop/export are unconfirmed;
its four unsealed files remain diagnostic. Native traces, role logs, resource
samples, captures and original errors remain preserved under the run's sealed
roots. Managed terminals report owned child trees reaped, and direct endpoint
census observes no task processes, owned listeners/firewall rules or active
capture. All six exact one-run control secrets are retired in order using the
existing held-object guards, and the final fresh endpoint census passes at
21:14 UTC. The retained direct cleanup result SHA-256 is
`24b5575d6ee012f9298c90acd26b6ed1c54a0a8b8052efe7f0535e4064e4c9fa`.
An earlier cleanup lease refused a temporarily present historical PID before
any disposition; that refusal remains preserved. Its replacement process
identity is **NOT MEASURED**. The later census finds every original owner PID
absent; no unrelated process is terminated or exempted.

**Local32 recovery FAIL; Node32 NOT RUN; KI-006 OPEN; Foundation 3L
B — PARTIALLY READY.** Retained F1 Phase 1 PASS is unchanged. No 3M or merge.

## Corrected sampler passes; separate aggregate RPC CI failure (2026-10-05)

Tooling `d66444f3eb2a283e37ab6fb5451d55cab2849f92` passes all 95 native
CTest entries, including the corrected sampler, and 371 Python tooling checks.
Original push `37275013641`, Windows job `111650013022`, subsequently fails
five unchanged RPC p95 gates in the fourth standalone command,
`--reliable-workload-32`, FULL_RESERVATION aggregate recovery. The failing
peer p95 values are 162.058, 165.997, 163.524, 165.514 and 166.892 ms against
150 ms. All eight active peers accept and complete 48 requests each with zero
errors; structural accepted bytes and the final pending/journal backlog are
zero. This is separate from the earlier sampler startup refusal.

The third command's Full scheduler trace completes successfully before this
failure; it does not cover the failing fourth command. The latter's raw
operation/CPU/sleep spans contain wall-time stalls, but exact blocking versus
host scheduling and its inherited process priority are **NOT MEASURED**.
The existing fixed-case diagnostic is extended to the exact fourth command,
once in original order, with the same bounded owned Job/ETW machinery and
verified ordinary Normal child scheduling. No argument, sample, latency gate,
production implementation or F1 constant changes. The original failure remains
failed; no timing discount or passing PR substitution is permitted.

Original PR Windows passes 95/95 and 371 Python checks, PR Linux passes 53/53,
and both GNS jobs pass 10/10; push Linux is skipped. The failed original log
SHA-256 is `8e2282ed193635911ffc03a516232bb95133b9ffe20cb0357216123b8315126d`.
Corrected-source CI must qualify before fresh Local32 preflight and execution.
The fixed-case helper compiles with the hosted `/O2 /W4 /WX` contract on the
worker and passes all 34 focused helper tests with zero skips. The checks
exercise case binding, original failure/crash propagation, coverage denials,
ownership and cleanup without starting a kernel session or native workload;
owned helper/test processes are absent afterward. This is tooling validation,
not an RPC timing or provider acceptance result.
Retained F1 Phase 1 PASS is unchanged; **KI-006 OPEN; Foundation 3L
B — PARTIALLY READY; no Local32/Node32 acceptance or 3M.**

## Original tooling CI sampler startup refusal (2026-10-05)

Execution-changing tooling `a4baec8370328d57244aaec58da7bc69ab84e147`
does not qualify for physical execution. Original native push `37267234328`,
Windows job `111626513015`, passes 94 of 95 native tests and fails the
resource-sampler fixture's unchanged five-second initialization gate. Its
dependent Linux job `111636804701` is skipped. A successful original PR
Windows job is separate evidence and cannot replace this failed push. The
completed original PR jobs pass Windows 95/95, Linux 53/53 and 371 separate
Windows Python checks; both original GNS jobs pass 10/10. These outcomes
preserve the failed original push and do not qualify `a4baec83`.

The reported 6,133 ms parent counter preparation occurred before the timed
startup and is not the failed child initialization duration. The first fixture
sampler starts before any of its 32 harmless child owners, and uses supplied
NIC values with actual kernel32 CPU/memory reads. Source inspection establishes
that the separate sampler child compiles the same fixed C# counter/pipe-reader
definitions again. Its exclusive compilation, PowerShell startup, source/hash
preparation and first-baseline times are **NOT MEASURED** in the failed receipt.
The original artifact upload omitted that retained temporary sampler directory.
Duplicate compilation is an implementation fact, not a proven exclusive cause
of the original timeout.

Source review also found a reachable evidence-layout defect: the sampler's
runtime subdirectory was inside the role-evidence root, while its seal indexed
only top-level files and canonical acceptance required complete flat coverage.
The correction keeps run-owned runtime directories in the registry and retains
their complete bounded raw archive and exact prepared assembly as indexed flat
role files. It does not exempt a directory, omit raw records or expand the
128-file/16-MiB acceptance limits.

The failed push JUnit SHA-256 is
`d351d32b00326fdf437ba54d4262b22dde870458a80ce52b89fa21c0c2ec4763`.
No physical retry or CI rerun reclassifies the failure. Corrected source must
retain the five-second startup/query/stop bounds and actual first baseline,
pass its deterministic checks and new original hosted jobs, and pass fresh
endpoint preflights before Local32 or real-TLS Node32 execution.
Retained F1 Phase 1 PASS is unchanged; **KI-006 OPEN; Foundation 3L
B — PARTIALLY READY; no 3M.**

The corrected focused worker endpoint fixture passes with 32 actual harmless
children, 64 process rows and two host rows. Counter preparation takes 527 ms
outside startup; cold sampler startup with verified assembly loading takes
584 ms. Native-path 18, projection 23 and source-AST nine checks pass, as does
the separate actual host-resource fixture. The two staging failures (missing
unchanged projection helper and Windows Python alias selection) are retained;
they are not production failures. Hosted CI for the corrected source remains
required. These fixture results do not supply Local32 or Node32 acceptance.

The preceding failed Local run `78b900ef-4784-4cc2-8704-ce5c37edf332`
has separately completed control retirement. Independent raw replay joins
12 ordered journals, 51 raw observations and nine original indexed inputs;
all six exact run-control secrets are absent and both endpoints are observed
idle. Raw closure SHA-256
`a019fdccb5ecef362e05bac24f614dde43980ffb9c63b5dddc2085ded59a90eb`;
independent actual review SHA-256
`47f4be989ccac144b0041f5e66b01257a115315b748a7d94ffd9ffffbd087e39`.
This closes that run's cleanup prerequisite and supersedes its earlier pending
cleanup statement below; it does not change its failed launch or supply a
provider acceptance result.

## Local attempt stopped before the coordinator listener (2026-10-05)

Fresh Local run `78b900ef-4784-4cc2-8704-ce5c37edf332`, coordinator
`fed720f9-703f-4cdd-ba8f-0517f0696092`, consumed execution tooling
`eaf956be2873855b69eb6a9672dc0beb23f7c10f` and unchanged native `9ae8f68c2`.
Both staged actual-import and configured checks passed. The worker host stdout
and stderr files are empty; no host terminal, listening marker, control barrier
or role launch was recorded. The original wrapper stderr was discarded and is
**NOT MEASURED**. The original launch summary reports cleanup unproven because
the host terminal is absent; it does not establish a live descendant or child
reaping. No provider workload or service metric was measured.

A separate harmless actual-worker `StartOwned` witness reproduced
`ModuleNotFoundError: dependency`. The fixed helper directory has only the
adapter and ACL module; the run stage already has the verified dependency,
lock and 13 pinned Coordinator exports. The bounded adapter correction selects
those exact stage-owned sources, retains a startup diagnostic without inventing
reap truth, and reports the original launch error alongside cleanup refusal.
New source and original hosted CI must qualify before another physical attempt.

The post-failure read-only installed observation passed all eight inventory
checks and reported no task processes or listeners, with native and Packet
Monitor capture idle. That observation is not secret-retirement evidence.
Current failed-run control retirement remains separate and pending. All original
failure receipts are preserved; Local24, real-TLS Node26 and final54 acceptance
remain unmeasured. **KI-006 OPEN; Foundation 3L B — PARTIALLY READY; no 3M.**

## Local32 correction qualified; client resource preflight blocked (2026-10-04)

Execution-changing tooling `eaf956be2873855b69eb6a9672dc0beb23f7c10f`
passes all six original attempt-1 hosted jobs. Native push `37253600366`
and PR `37253602633` each pass Windows 95 and Linux 53 native tests,
plus 364 separate Windows Python checks, with no failed or skipped tests.
GNS push `37253600339` and PR `37253602820` each pass 10 sanitizer tests.
Both original Windows Full and Aggregate32Structural traces replay exactly
against the pinned source. Original checkout head/merge/tree, logs, artifact
digests and test identities are reconciled without workflow retries.
The earlier `ce26e26a` failed/skipped push results remain unchanged.

Original raw CI independent review SHA-256:
`aded719f9b60e21b42ac30962a410f9239279a61611b6de3c9357a7c74bc53d2`.
The complete source-bound current-CI verifier also passes; its receipt is
`C:\Users\aiden\.codex\artifacts\farm32-v14-independent-finalization-eaf956be-v1\current-ci-independent-review.json`,
SHA-256 `ebb27c658ed409933ae579122cffa46b2bd247a0423e5aca622171c95507f759`.
Independent rejoin review SHA-256:
`8104a94bd5ee76a9c9af88d8f47d693e48b22172332572d361e4adb21c659a7c`.

Failed Local `68bfec9e` cleanup is independently complete. The original
four-control STOPPED result and eight journals are preserved. A separately
qualified, fresh two-control continuation supplies the remaining four journals,
final six-leaf absence and idle observations. The composite verifier checks
12 logical journals, 32 raw observations and all 31 original indexed inputs.
Raw closure SHA-256
`6d2058f99c23b8caf9243646dbc1be1eb592621c172f475939d08b3efa458fa5`;
independent actual audit SHA-256
`a3e3082f1adc2b5b324ead92aeff7b06300d4b1f7e7a7f406513178e4bdd913b`.
This cleanup evidence does not reclassify the original provider failure.

V14's applied source bindings pass 101 candidate tests and 162 framework tests
in each of normal and optimized Python, with zero skips. The independent
refreeze review verifies the six CI literal bindings, byte-exact current
cleanup reader, all 14 intended source changes and preserved predecessor
snapshots. Candidate adoption and physical launch remain pending.

Read-only installed observation at `2026-10-05T03:50:15Z` verifies both endpoint
package and source pins, no observed task processes/listeners, running idle
capture services, all eight worker service files and four Node/config pins.
Its resource gate fails: client `DESKTOP-B8V8NAN` has 3.357 GiB available RAM
against the unchanged 8-GiB floor; worker `HOSTPC` has 19.657 GiB available RAM
and 36.382 GiB free on C:. This is an installed observation, not a physical
attempt. Mapped receipt SHA-256:
`84a4453dbb5796e4ebcecb15ea91673c8dbfd3dadeb97b3f365d27f84f13585e`.
Fresh full source, installed, resource, projection, capture and lifecycle
preflights remain mandatory before launch. No unrelated applications are
stopped to satisfy the resource gate.

Retained F1 PASS is unchanged. Local32, real-TLS Node32 and final acceptance
remain unqualified; **KI-006 OPEN; Foundation 3L B — PARTIALLY READY**.
No new physical attempt, Foundation 3M work or merge is performed.

## Local32 bootstrap launch/sampling failure (2026-10-04)

Tooling `929da7d071029d945a461d9631a97d1b2398f701` passes all six
original push/PR Windows, Linux and GNS jobs. The preceding `1aeb7221` run is
separately closed with its six exact control leaves retired and fresh idle
evidence; its original failed terminals and captures remain unchanged.
Fresh Local run `68bfec9e-1335-4f45-b7ad-9ea502bd2592`, coordinator
`ed6d0a59-463f-4a6d-846c-c6b37e924432`, passes staged imports, strict capture
configuration and exact runtime projection checks. The run-owned projected
Server UDP permission allows real connectivity: 28 clients reach Ready.

The first causal failure is `clients_or_manifest_not_ready` at server tick
1,201. No workload clock records are produced. On the server's own monotonic
clock, ticks 1 through 1,200 span 19,987.143 ms; the 28th client reaches Ready
14,936.061 ms after tick 1. On the client host's independent clock, slot
19-to-20 starts are 6,356.758 ms apart and slot 27-to-28 starts are
5,613.952 ms apart. Slots 0 through 29 span 18,153.388 ms; slots 30/31 have
no native start record. These clocks are not subtracted across hosts.

Source places a synchronous resource sweep and adapter/CIM queries between
successive client starts. This is a launcher infrastructure defect: resource
observation can delay the workload it observes. The precise OS call responsible
for each retained gap is **NOT MEASURED**; the old supervisor did not timestamp
each call. The 1,200-tick bootstrap limit, 75-ms startup stagger, role deadlines,
resource evidence limits and all production service/acceptance gates remain
unchanged. Correcting the launch/sampling coupling requires independent bounded
sampling, preserving startup coverage and exact process identity.

The native server exits 7 and retains `Status=FAIL`. The original worker host
and role terminals remain `FAILED`, exit 1 and `ChildTreeReaped=false`; the
client terminal is `ABORTED`, exit 1 and `ChildTreeReaped=true`. The outer
endpoint forces a false reap bit for a naturally exited unsuccessful runner,
while its exited-PID shortcut cannot independently prove descendant absence.
A sound correction needs containment evidence separate from workload status.
The capture controller also rejects failed role evidence before offline export,
so the original worker `capture-sha256.json` is missing. Failed diagnostic
export must remain explicitly failed and cannot satisfy capture acceptance.

Original public input index:
`C:\Users\aiden\.codex\artifacts\farm32-68bf-root-public-evidence-v1\public-input-index.json`,
SHA-256 `21e14443e34d19c074fec23019d447b1e0dfbd7a619c3a1ce72a02d90ddf5c9e`.
Independent source/native chronology:
`C:\Users\aiden\.codex\artifacts\farm32-68bf-bootstrap-independent-attribution-v1\analysis.json`,
SHA-256 `a5620c986755c4277d25f3c7ee43b7b9dfcb061119bdf456435917fd0ba4e58e`.
Later read-only absence observations do not rewrite the original terminals.
Exact fresh failed-run cleanup and correction qualification remain required.
The source correction separates bounded resource sampling from client launch,
adopts upstream checked Windows Job containment at
`cf1628de587ee0ccfbd74742d62946dac1adf522`, and permits explicitly failed
diagnostic capture export without satisfying success binding. The full
production-pinned farm suite passes 258 tests without skips. The focused
ownership, outer endpoint and capture suites pass 24, 14 and 26 tests in both
normal and optimized Python; an independent review confirms checked release,
rollback and interruption behavior. A 32-harmless-child sampler regression
passes with a six-second supplied network query: all owner publications finish
in 477 ms, initialization in 1,567 ms, with 64 process rows, two host rows and
retained failure partials. This is source/infrastructure evidence only.

Independent lifecycle review SHA-256:
`7b57ee6659328fb9186bb498cd1002fcde9b30109d69be3afa8ab03403f6207c`.
Portable sampler qualification SHA-256:
`035178ec34d057b8732d4d6827c690ae151d2d880560c4e694f3e5b5199820ab`.
The original tooling `ce26e26a` push Windows job `111573334011` passes
94 of 95 native tests and fails the endpoint fixture's five-second sampler
initialization check. Its Linux job is skipped; these failed/skipped results
remain historical and cannot be replaced by the separately passing PR Windows
job. The fixture omitted production's parent-counter preparation before the
sampler clock. The correction restores that precondition only in the fixture,
retains both actual five-second checks and cold child initialization, and adds
six AST checks plus bounded timing diagnostics. The exclusive slow phase of
the original CI failure is **NOT MEASURED**.

The corrected standalone sampler and full endpoint fixtures pass locally with
32 harmless children each, 64 process rows, two host rows, and all 36 recorded
owned processes exited or absent in each suite. Their six initialization
measurements range from 1,081 to 1,528 ms. The full endpoint also passes its
18 native-path and 23 projection cases. Retained qualification SHA-256:
`dcc04b870be4c645ce67b135bbe444147eb5c87f72047d4e7f8969996625dc64`.
Production endpoint code, deadlines and acceptance limits are unchanged.
This fixture correction requires its own original hosted candidate checks;
local harmless tests do not qualify a physical farm.

Required hosted candidate CI, exact cleanup, fresh source closure and endpoint
preflight must pass before a subsequent provider run. No installed service,
native production code, admission, resource limit or acceptance gate changes.
Retained F1 physical PASS is unchanged. **KI-006 OPEN; Foundation 3L B —
PARTIALLY READY; Local32/real-TLS Node32 and final acceptance remain unqualified;
no 3M.**

## Local32 projected-server UDP permission omission (2026-10-04)

Fresh tooling `7a3e7ea8` run `1aeb7221-8c47-4ccd-98c1-389d486e1c4c`,
coordinator `3f9857cd-746d-44da-9750-19fb8307df19`, follows successful
original six-job hosted checks and independently closed `f0c18bed` cleanup.
Both staged imports and capture configurations pass. The unchanged native
Server starts in 14 ms from the compact exact runtime projection and publishes
its ready socket before client slot 0 starts. The client bootstrap times out
before Ready; the provider workload does not begin. All three outer terminals
remain FAILED with original `ChildTreeReaped=false`. Collection lacks the
worker evidence index; both original capture gates remain INCOMPLETE.

The retained client pcap contains 21 requests from `10.253.3.1:64428` to
`10.253.3.2:39450`, each UDP length 520. Offline export of the original worker
ETL through its exact pinned NDIS exporter yields 125,851 frames and the same
21 requests, matched by payload SHA-256 and tuple in order, with no response
in either capture. The export is diagnostic only and does not repair either
capture gate. Cross-host one-way latency and an original per-packet WFP drop
decision are **NOT MEASURED**.

Read-only effective firewall inspection finds Private inbound default Block
and no rule for the projected Server path. The apparently broad UWP exceptions
are package SID/family restricted in compiled WFP filters, including the
`ALE_PACKAGE_FAMILY_NAME` restriction omitted from the high-level Package
field. This supports the firewall explanation; it is not an observed original
drop verdict. Source inspection proves the independent infrastructure defect:
the launcher establishes coordinator TCP permission but does not establish or
verify game UDP permission for the newly projected executable. Historical
package/probe exceptions do not follow an executable into a new path.

The correction binds a temporary worker permission to the sealed run, exact
verified native projection, Private fiber interface, local UDP 39450 and the
single peer fiber address. Both persistent and effective scope must match;
stale ownership, changed hashes, ambiguous profiles/interfaces and explicit
matching Blocks deny launch. Every launch exit reconciles only its unchanged
owned rule and propagates cleanup failures. Unrelated rules, production
transport, admission, F1 constants and installed services remain unchanged.
Local deterministic qualification passes 240 farm tests and 47 optimized outer
tests without skips; ROOT separately replays the 12 focused game-rule cases.
Generated PowerShell runs against mocked network/security getters, including
exact scope, explicit Blocks, failed publication, stale ownership, altered
rules, AST-name duplication and the bounded hash-checked compressed transport.
No test publishes an operational rule or starts a physical workload. Receipt:
`C:\Users\aiden\.codex\artifacts\farm32-game-udp-lifecycle-source-qualification-v1\result.json`,
SHA-256 `6d7c0f85f4dac5651dd6b1c6e7943669984dc68bc757830328a4d8cd7c36e3b3`.
Current execution-changing CI, exact failed-run retirement and fresh endpoint
preflights remain separate required gates before another provider attempt.

Diagnostic raw/reconciliation files remain under
`C:\Users\aiden\.codex\artifacts\farm32-1aeb-failed-packet-attribution-v1`.
Its `packet-reconciliation.json` SHA-256 is
`fe208d2e5f6123e3e996359bd8a52a0863b62c960cfe1aa0aba028d6e8e4c131`;
compiled WFP XML SHA-256 is
`913048a629a411c7aa1a78e59da377b8b6551d4febf989f1ef53bdea03791a1f`.
Retained F1 Phase 1 remains PASS. **KI-006 OPEN; Foundation 3L B — PARTIALLY
READY; no 3M.** No completed Local32, real-TLS Node32 or final acceptance PASS
is inferred from this failed attempt or the correction.

## Local32 native path-length startup failure (2026-10-04)

Fresh run `f0c18bed-722e-4820-86f6-20be1e6f60c1`, coordinator
`8400bb71-e929-48b6-93c9-c443467dc0ce`, uses tooling `578edaa4` after
its six original hosted jobs pass and the earlier `cf89463c` run's six secrets
are independently confirmed retired. Both staged imports and strict capture
configuration pass. The worker Server exits 3 before readiness, reporting native
package integrity failure. Its failed host/role terminals also report child
reaping false; missing worker capture evidence cannot close the run. No Local
provider service, recovery, resource, or readiness PASS is claimed.

The runtime projection has exactly the same 58 file paths, sizes and hashes as
the passing NativeA startup diagnostic below. However, its root is 173 characters
and eight asset paths reach 270 characters, versus 132/229 in that diagnostic.
The unchanged Server executable has no `longPathAware` application manifest;
native package validation uses ordinary unprefixed Windows file APIs.

A separate startup-only regression preserves the package and executable hashes
and reproduces the exact integrity error/exit 3 at root/member lengths 173/270.
The same Server bytes at 134/231 and unchanged Player at 136/233 exit 0. All
three owned children are reaped; original envelopes and runtime inventories are
unchanged. No bind, connect, capture or farm workload occurs. Receipt:
`C:\Users\aiden\.codex\artifacts\farm32-f0-path-native-regression-v2\result.json`,
SHA-256 `c5097a7d850ea5e914d1e7289d8186e3fe9871b2c6c4197b80084f53915c9963`.
The physical failure's precise syscall/exception is **NOT MEASURED** because the
native catch reports only integrity failure. The controlled native comparison
establishes the path-length cause without changing native package validation.

Projection now checks every full member and directory-ancestor path against
the native package builder's existing 240-character limit before publishing any
copy/attempt/receipt and again on prepared validation. Eighteen new tests cover
both roles, the actual AST-extracted helper, 240 acceptance, 241/270 denial, and
absence of published state after denial. The existing 23 projection cases pass.
Use shorter fresh run/role registry names for the next candidate; required
current-source CI and this failed run's exact secret/idle cleanup remain
separate gates before a fresh provider launch. Historical receipts are preserved.

Retained F1 physical Phase 1 remains PASS. KI-006 remains OPEN; Foundation 3L
remains B — PARTIALLY READY. Local32, real-TLS Node32 and the final acceptance
sweep remain **NOT MEASURED** for a qualified completed provider campaign.

## Local32 package-envelope startup failure (2026-10-04)

The corrected source separates the deployment envelope from a fresh, exact
native runtime projection before preflight and ticket sealing. The final
projection suite passes 23 cases, including UTF8 path bounds/order, stale or
partial copies, redirection, missing/extra content, tampering and receipt
identity. The complete farm Python scope passes 228 tests, and acceptance
passes 32 tests normally and with optimization. These are distinct scopes.

Actual unchanged NativeA Server startup (12 ticks, no bind) and headless Player
startup (12 frames, no connect) both exit 0 from the projected 58-file roots.
Both child processes are reaped; runtime and original envelope inventories are
unchanged. The diagnostic receipt is
`C:\Users\aiden\.codex\artifacts\farm32-native-runtime-startup-v1\result.json`,
SHA-256 `bc103808f4a52581278cf047a5f85a8db274fae630b4621fa7c788a5e335c21e`.
This is package-startup qualification, not a physical provider attempt. Required
execution-changing CI and failed-run secret retirement remain separate gates.

Tooling head `2537f4517733a25990fd5f6456a59c962b39ff13` is qualified by all
six original hosted jobs: Native push `37165435339`, Native PR `37165437335`,
GNS push `37165435338` and GNS PR `37165437338`. Both Windows jobs pass 95
native tests, both Linux jobs pass 53, and both GNS jobs pass 10. Complete
original checkout/log/artifact reconciliation and the four independent raw
scheduler trace replays pass. Earlier CI failures remain historical failures.

The fresh Local32 attempt is physical run
`cf89463c-cd4a-4d18-849a-cb66b51ba97e`, coordinator run
`9e0aa139-56f9-436b-a7e9-87c5288f8f4a`. Both staged imports and strict capture
configuration pass. SERVER control acknowledges launch at 02:26:56.454 UTC;
the endpoint reports native exit **3** at 02:26:58.671107 UTC, before readiness.
CLIENT never receives its launch operation. No client probe evidence or F1
service measurement exists. Missing capture collection and conservative
false child-reap terminal bits are downstream failures, not alternate causes.

Both deployed and official Server envelopes contain 57 declared content files,
`game.package.json`, and the undeclared qualifier `deployment-sha256.json`.
Every declared size/hash matches, and the ordered content-table hash matches.
`PackageBuilder::ParseManifest` permits only declared content and the game
manifest; CI writes the qualifier inventory after the generator's native
validation. The exact thrown native exception text and emission timestamp are
**NOT MEASURED** because the native catch exposes only the generic integrity
diagnostic. The retained source and byte-closure evidence establish the layout
defect without inferring missing native events.

Attribution is retained in
`C:\Users\aiden\.codex\artifacts\role-startup-cf89463c-v1\attribution.json`,
SHA-256 `29f24058cecb6e350d8a8d1b4457267a993a37984ccd28d3d0159b605335b4ef`.
Fresh supplemental observation confirms idle endpoints; the original failed
terminals and sequence remain unchanged. Exact-secret cleanup and independently
qualified clean runtime staging precede any fresh attempt. Local32, real-TLS
Node32 and the final acceptance sweep remain unqualified. The retained F1 PASS
is unchanged and must not be rerun. **KI-006 OPEN; 3L B — PARTIALLY READY**.

## Full diagnostic decoder qualified locally; original CI required (2026-10-04)

The Full development native child passes, while its original diagnostic remains
failed at the obsolete five-million-row cutoff. The corrected SDK helper passes
31 tests in each Python mode without skips. A separate read-only replay of the
unchanged retained ETL decodes all 7,731,540 rows within the existing byte/time
bounds. Original evidence is preserved; this replay launches no workload or
capture and does not explain the earlier hosted timing failure. See the
[decoder validation receipt](ContentAvailabilityFoundation3L_3Validation.md#full-decoder-correction-and-retained-etl-reconstruction-2026-10-04).
New original push/PR CI and independent candidate review are still required
before fresh Local32/Node32 adoption. V5 remains unadopted with null pins.
Retained F1 PASS is unchanged; **KI-006 OPEN; 3L B — PARTIALLY READY**.

## c33 original CI failure blocks physical launch (2026-10-03)

Original Native PR `37158397178` fails FULL_RESERVATION's third command,
`--reliable-workload`, upper case: RPC p95 218.8903 ms exceeds 150 ms and
action 17's 556.5228 ms exceeds 250 ms. Windows CTest 95, helper 20 and hosted
tooling 295 pass; Linux is skipped. The fourth/fifth commands never start, so
their scheduler evidence cannot diagnose this failure. Exact request/phase
overlaps exist, but the scheduling or blocking mechanism remains **NOT MEASURED**.

The existing bounded Full helper will instrument this exact invocation once
after deterministic qualification. Workload, flags, clocks, gates, production,
admission and reserves remain unchanged. Dormant V5's independent static review
is complete, with 71 pure tests in each mode and unchanged 54-gate/20-field
assembly; both adoption pins remain null. No new physical attempt occurred.
Retained F1 PASS and the failed b52d supplemental cleanup remain unchanged.
**KI-006 OPEN; 3L B — PARTIALLY READY**. Earlier pending paragraphs are historical
and do not authorize launch.

## Aggregate diagnostic candidate awaits original CI (2026-10-03)

The fixed original aggregate command passes a bounded worker development trace
with all three phases and 7,424 paired request/resource records, loss-free native
scheduler evidence and verified cleanup. Source `8854feecf` additionally preserves
negative native failures in every standalone CI guard and retains the actual
helper/workload binaries for hash verification. It changes no production,
admission, transport, workload or latency gate. The earlier failed development
staging and 92/95 environment-affected native suite remain failed; corrected
runtime selection passes the three affected tests separately.

These are development results, not hosted CI or physical acceptance. New original
CI, exact-source review and a fresh dormant launcher precede Local32/Node32.
Retained F1 PASS is unchanged; **KI-006 OPEN; 3L B — PARTIALLY READY**. The
[development validation entry](ContentAvailabilityFoundation3L_3Validation.md#aggregate-diagnostic-development-qualified-hosted-ci-pending-2026-10-03)
records the immutable evidence and remaining gates.

## Corrected capture staging awaits aggregate timing qualification (2026-10-03)

Capture-root correction at tooling `3c8f65d39` passes 315 local tooling tests.
Its original Native PR run `37151679339` passes 95 CTest cases and the tooling
step, then fails three aggregate RPC p99 gates in FULL_RESERVATION's first
normal phase of `--reliable-workload-32-structural` (354.201/355.002/418.106 ms
against 250 ms). No pooled structural work is active in that phase. The exact
mechanism remains unmeasured because aggregate request chronology was absent.
Bounded request-linked scheduler instrumentation is the next correction;
no latency sample, threshold, admission or transport policy is adjusted.

Failed Local `b52d6e7b` has a separate completed cleanup chain proving all six
one-run secrets absent and both endpoints idle. Its original failed role
terminals and incomplete first cleanup receipt are preserved. Dormant V4 was
reviewed but not adopted or executed. No physical attempt followed this hosted
failure. Retained F1 PASS remains valid; fresh Local32/Node32 and final audit
remain outstanding. See the [current validation entry](ContentAvailabilityFoundation3L_3Validation.md#original-aggregate-timing-failure-after-capture-correction-2026-10-03).

## Local startup failure before service measurement (2026-10-03 20:30 UTC)

Local attempt `b52d6e7b-6ebb-44e2-b12c-a2b4fb88bbc0` and Coordinator
`067e0c39-c482-4fda-8e37-6a1571b4178b` failed before either capture became
ready. Both actual staged imports passed; both strict capture configurations
then rejected missing sealed parent directories. This is an attributed staging
defect, with no native farm or production service measurement. Original failed
cleanup terminals are preserved. The
[validation ledger](ContentAvailabilityFoundation3L_3Validation.md#local-capture-startup-attribution-2026-10-03-2030-utc)
records raw evidence and the reviewed provisioning correction. V3 is revoked;
new candidate qualification, exact-run cleanup and fresh preflights precede
another attempt. The client RAM gate passed at 19:52 UTC. Retained F1 physical
PASS remains valid; Local32, real-TLS Node32 and final acceptance remain pending.

## Provider continuation checkpoint (2026-10-03 15:10 UTC)

All four original required workflows for tooling source `1ccefb336` and the
separately predeclared controlled 95-case/five-workload run are complete and
successful. Original artifacts, checkout identities and raw controlled evidence
are independently verified in the
[validation ledger](ContentAvailabilityFoundation3L_3Validation.md#current-tooling-and-controlled-capacity-qualification-2026-10-03-1510-utc).
The frozen native candidate remains `9ae8f68c2`; exact-tooling offline replay
reproduces the F1 physical PASS below. No fresh physical attempt occurred during
this continuation. Local32 and real-TLS Node32 remain **NOT MEASURED**; their
reviewed launchers remain dormant while client RAM is below the existing 8-GiB
gate (7.781 GiB at 15:09:45 UTC). Final guarded adoption and fresh per-run
preflights precede any launch. Historical failed runs retain their verdicts.

## Funded-ACK candidate F1 physical Phase 1 passed (2026-10-03 UTC)

**F1 PHYSICAL PHASE 1 — PASS** for execution source
`9ae8f68c2c8ed50589b5c54ed56657fa6fd02aad`, with pin-only qualifier
`d4fa8b64f2645a87e798f7a0a858a7d4d4285771`. This fresh run qualifies the
reserve-funded ACK/retry correction; it does not replace the historical
October 1 result below. Before execution, exact-source Native CI `37091176350`
and GNS sanitizer CI `37091175492` passed; original JUnit verifies 95 Windows,
53 Linux and 10 GNS tests. Both endpoints passed all eight socket-free probe
self-tests, exact installed-file checks and fresh evidence/control preflights.

Physical run `454bf4f8-c8f2-4c18-9866-1b61218da514` and lifecycle
`7a48260b-6911-4394-bbb5-65a65e17bac0` completed four actual clients and
32 maximum-grant waves per peer through unchanged production admission.
The raw replay establishes 128 complete 524,288-byte grants and two subsequent
1,258-byte certificates. Four pre-ready bootstrap admissions (5,171, 5,171,
6,361 and 6,361 B) carry the explicit pre-ready timestamp sentinel; they are
conserved separately and are not invented qualified F1 certificates.

| Slot | Maximum grants | First-send completion min–max, µs | Maximum running deficit, byte-µs |
| --- | ---: | ---: | ---: |
| 1 | 32 | 28,025–28,940 | 7,230,980,096 |
| 2 | 32 | 28,012–28,846 | 7,012,876,288 |
| 3 | 32 | 27,980–28,984 | 6,392,119,296 |
| 4 | 32 | 27,880–28,857 | 5,653,921,792 |

All finite and native latched running certificates pass the unchanged F1
bounds. There are 32 common four-grant intervals totaling 857,089 µs. The pool
proof sums the four native all-subinterval certificates: the conservative
sum of peer maxima is 26,289,897,472 byte-µs, below 72,100,864,000. A directly
sampled pool maximum is **NOT MEASURED**. Each peer first-sent and ACKed
16,777,216 campaign bytes. Separately counted retry bytes are 18,328, 25,150,
29,698 and 44,479; none count as new service. Accepted = retired =
67,134,444 B, with terminal release, outstanding debt and active grants zero.
Pending high-water is 2,097,152 B. The designated gameplay producer completed
80 RPCs and 80 events within the unchanged latency gates.

Both sealed captures contain 68,209 complete frames with zero reported loss.
Ports 57170–57173 are bidirectional at both endpoints, with exact matching
opposite-direction counts. The worker ETL is 93,847,552 B, below the independent
four-client profile's 240-MiB threshold. This does not qualify the different
Farm32 16-GiB profile. Endpoint, physical coordinator and outer lifecycle
results all succeed; both endpoints finish IDLE.

The offline replayer initially rejected the canonical preassignment request
UUIDs because it required every journal envelope to use the assigned lifecycle
UUID. The correction pairs each request/reply with its endpoint, assigned run,
canonical workflow schema/hash and subsequent registration; other rows retain
their run checks. Twenty-one replay tests and 24 final-acceptance tests pass.
The original physical evidence is unchanged. Independent agent and root replay
both return `MEASURED_PASS`; no additional physical attempt was performed.

Evidence is retained under
`C:\Sandbox\Codex\Evidence\physical-qualifier\lifecycle-cfb2fec6b24c5d02`,
including the hash-verified worker copy. Native CSV SHA-256 is
`9BD15A23C9A8C408C17DA60B8ED511F37E72BCC86CC64F6289A433585FF5780C`;
admission CSV SHA-256 is
`668C2FC83E076C99B82128B9A67C8A367B7AE9C82C813B32FBA45F193614AAAB`.
The complete replay, input hashes and analyzer provenance are retained at
`C:\Users\aiden\.codex\artifacts\f1-replay-454bf4f8-v1`; `result.json`
SHA-256 is `EE58D7894B9122232B494579C4767BAFCE336F80E0C8F30AE3529B21CD90CA1B`.

Owner-verified cleanup restored both installed baselines and removed temporary
profiles, tickets, keys, scheduled task and daemons. Both rollback checks report
zero owned processes, lifecycle listeners and UDP 39450 bindings; tunnel cleanup
passes. The capture service is Running/idle, Packet Monitor and Windows trace
are stopped, and no filters remain. The restoration receipt is retained in
`prepared-cfb2fec6b24c5d02\lifecycle\endpoint-restoration.json` under the
`gargantuan-f1-funded-ack-final-stage-9ae8-d4fa-v2` artifact. The previous stage
`822b60a9-6d33-40c4-9559-f74dc19c0475` was rejected for an expired readiness
proof before assignment, capture or probe; it was fully rolled back and retired.

**KI-006 remains OPEN; Foundation 3L remains B — PARTIALLY READY.** Fresh
Local/real-TLS Node 32-client matrices, their resource/recovery/evidence gates
and final acceptance remain outstanding. No 3M work or merge is authorized by
this result.

## Corrected F1 physical Phase 1 passed (2026-10-01)

**F1 PHYSICAL PHASE 1 — PASS.** The scoped `ReliableNoNagle` structural
transport candidate and corrected capture hook were qualified before this run.
Source/qualifier commit `fbb235bdae5db8aa8946d329a18035d08996fe32`
passed 72 qualifier tests, the pinned-GNS and native suites, and all six
hosted Windows/Linux native and GNS-sanitizer jobs (push and PR runs). The
installed worker capture hook and its service pin both matched
`8FE61C01009BA27132D8094B95CD88EB5EB884A3AF5B7A4FBEFAF40A64630EBE`.
An installed-service synthetic full-volume capture had already completed with
zero lost events and idle cleanup. Neither that synthetic run nor the 100/100
worker-local deterministic campaigns is substituted for the physical result.

Fresh physical run `e120a1f0-47ae-45e0-869f-9d0450795bc4` and outer
lifecycle run `e29e2200-1538-480f-9969-3ca8b01458fb` used exactly four
real clients over direct fiber. A prior staging identity
`c8dcfb54-0ec9-4c03-984b-60d64c448acc` was rejected before any physical
launch because its evidence directory exceeded the ten-minute preflight age;
both idle stages were rolled back and a new identity was used. The accepted
run's endpoint configs, evidence-root checks, authenticated tunnel directions,
actual-child coordinator, four client connections and GameSession readiness
passed. Each client was assigned 32 exact 524,288-byte maximum structural
grants through production credit, fairness and ACK-gated admission. The
retained CSV yielded 128 maximum-grant completion records and three smaller
completion records (1,258, 1,258 and 663 B). The 25,648-byte difference
between total accepted bytes and the 128 maximum grants includes earlier
startup work; its full per-grant breakdown is **NOT MEASURED** in this CSV.
The workload did not invent a continuous 16 MiB/s cross-grant admission stream.

The retained native CSV is
`C:\Users\aiden\.codex\artifacts\gargantuan-f1-ce4733-stage-fbb235bda\prepared-b4aea576c5adabca\lifecycle\physical-gns-server.csv`,
SHA-256 `9E8717D3C478D65F709C01D6AAA7927CBCBCFA29DD6C3F9E0AA9071D3AC1CEC1`.
It was copied from the worker probe's work directory after lifecycle cleanup;
the separately sealed endpoint evidence manifests remain unchanged. Every
maximum grant completed unique first-send in **28,681–28,974 µs** after
activation, inside F1's approximately 37,324.387-µs finite envelope. The
per-peer maximum-grant observations were:

| Slot | Complete 512-KiB grants | First-send completion, min–max µs | Maximum within-grant deficit, byte-µs |
| --- | ---: | ---: | ---: |
| 1 | 32 | 28,681–28,974 | 5,737,807,872 |
| 2 | 32 | 28,681–28,961 | 6,794,772,480 |
| 3 | 32 | 28,683–28,937 | 8,925,478,912 |
| 4 | 32 | 28,682–28,877 | 8,237,613,056 |

All 131 grant records have `last_completed_failed=0`. The maximum observed
individual deficit was 8,925,478,912 byte-µs, below the unchanged
18,025,216,000-byte-µs F1 bound. The probe recorded **29 genuine common
four-grant first-send episodes** totaling **622,756 µs**. Its pool verdict is
derived from the four native grant curves: over every common interval, the
sum of their individual bounded deficits is at most 29,695,672,320 byte-µs,
below the canonical 72,100,864,000-byte-µs pool bound. The direct physical
pool high-water was **NOT MEASURED**; the stated number is a conservative
sum-of-peer-maxima bound, not a sampled pool maximum. The first-send/ACK and
convergence summary reported 16,777,216 B per peer across the 32 maximum
grants, zero retries, accepted = retired = 67,134,512 B, terminal release =
0, outstanding debt = 0, active grants = 0, and `floor_failure=0`.

Both capture manifests verified byte count and SHA-256 for every member:
13 client and 14 worker files. The client pcapng held **68,212** complete
packets with zero dumpcap/interface drops. Worker Packet Monitor reported
79,342 processed events and **zero lost events**; its complete converted
pcapng and ETL were below the fixed cap. Both captures agreed exactly on all
four UDP tuples (`52964`–`52967`): client outbound/worker inbound counts were
1,348, 1,303, 1,346, 1,310 and worker outbound/client inbound counts were
15,607, 15,826, 15,621, 15,851. Endpoint results, physical coordinator and
outer lifecycle each reported success with classification
`FOUR_CLIENT_PHASE1_ONLY`; the outer lifecycle finished with both endpoints
`IDLE`. Owner-verified rollback restored both installed baselines, found zero
owned processes, lifecycle listeners or UDP 39450 sockets, and released both
CodexLock claims. Packet Monitor was stopped with no filters; the capture
service remained Running and idle.

This passes the bounded **F1 four-client Phase 1** gate only. The separate
32-actual-client Local/Node and remaining Foundation 3L acceptance gates have
not been inferred from this run. **KI-006 remains OPEN; Foundation 3L remains
B — PARTIALLY READY; no Foundation 3M or PR merge.**

## Full-volume capture-infrastructure preflight (2026-10-01)

The prior worker hook requested a 64-MiB circular NDIS trace. A complete
32-wave/four-client maximum-grant workload carries 64 MiB of attributed
structural bytes before packet and ETL overhead, so circular capture could
overwrite early evidence without the existing pcap parser detecting it. The
corrected hook requests a bounded 256-MiB single-file trace, rejects ETL files
at or above 240 MiB, and fails closed on nonzero or unavailable ETW lost-event
count. It uses the same fiber-miniport filter and complete-frame pcapng format.

A task-owned synthetic fiber transfer of 60,000 UDP datagrams (72,000,000
payload bytes) yielded a 94,896,128-byte ETL. The trace was single-file with
the 256-MiB cap, 181 buffers, 71,643 events, and zero events lost. Replaying
that retained ETL through the final hook exported 62,300 complete miniport
frames to a 76,940,164-byte pcapng in 10.390 seconds including the loss check,
below the capture service's 30-second Stop lease. Every packet block matched
the initial export. A separate retained 41,418,752-byte trace exported 27,004
frames in 1.271 seconds. These are capture and conversion preflights; they do
not measure native F1 first-send service or establish physical Phase 1 PASS.
At this historical preflight checkpoint, the installed capture-service hook
and pin still remained the historical version; the corrected hook was later
installed and qualified before the passing physical run above.

The first installed-service synthetic Start with the expanded fast-export hook
failed **before capture** with Windows process error 206. Pinned service source
embeds the entire hook in UTF-16 Base64 `-EncodedCommand`; the 12,765-character
hook produced a 34,184-character argument, above the 32,767-character Windows
command-line limit. The service remained Running and left no active marker or
trace. The corrected hook embeds a compressed copy of the same exporter C#
source, keeping the readable source separately in the repository. The new
regression verifies exact decoded-source equality and that Start/Stop command
lines remain below the Windows limit even for a 512-character evidence path
and CRLF line endings. Replaying the same 94,896,128-byte ETL through this
shortened hook exported the same 62,300 frames and identical pcapng hash
`218226D1BC3A1F38067833706E48DD422A00531D03A066F171EAF0FB17C28D88`
in 10.348 seconds with zero lost events. A successful installed-service
synthetic capture was still required at that checkpoint; no physical F1
attempt was consumed by the failed service preflight.

## Corrected F1 candidate deterministic preflight (2026-10-01)

Source commit `ce4733de8d68086667bc8fe5138bcc743f21c54e` keeps the
grant-scoped F1 constants unchanged and avoids a redundant native status/config
read immediately before submitting each token-bearing POOLED_SERVICE structural
message. The scoped `ReliableNoNagle` tail correction and precise native
first-send attribution remain in place. This additional hot-path change is
supported by source inspection and repeatability evidence; the exact callsite
responsible for the earlier rare status-to-send stall was **NOT MEASURED**.

The pinned-GNS Windows fixture reproduces the historical ordinary-Reliable
1,135 + 123-byte split with a 4,979-µs tail gap and F1 failure. The corrected
1,258-byte fixture keeps that split but first-sends the tail 14 µs later and
passes the unchanged running bound. Packet-boundary, tiny and 512-KiB sizes,
mixed reliable traffic, four maximum grants, ACK and retirement, and source
checks passed locally. The qualifier suite passed 70/70. The candidate's
32-wave real-GameSession four-client loopback campaign passed **100/100**
independent runs, **12,800** maximum-grant peer obligations, and 400 peer
result lines. Every run had four-grant overlap and local cleanup; the minimum
reported common overlap was 444,048 µs. The largest observed individual
running deficit was 12,816,974,912 byte-µs, below the unchanged
18,025,216,000-byte-µs bound. Cumulative accepted and retired bytes both
equaled 6,713,450,520. Process exit codes were **NOT MEASURED** by this
campaign wrapper; probe-level PASS, conservation, and cleanup were checked
from retained logs. These worker-local results are deterministic preflight,
**not physical F1 evidence**.

The versioned candidate pins source archive SHA-256
`18AF89DAEEF3D5DD8E1AC6ED7089EDBEF278990AA211CBB4E54BCF3555D846FB`
and Windows probe SHA-256
`925DC0684787B1D629901D5047FF9ECC968AFC5F4E579DE576F701322E9BB99F`.
The archive verified against all 31 source paths in the worker build tree.
Historical failed runs below retain their original source and measured verdicts.
Hosted execution-changing CI, endpoint preflight, capture and a fresh physical
F1 attempt are separate required gates. **KI-006 OPEN; Foundation 3L B —
PARTIALLY READY; no 3M.**

## F1 Phase 1 reached native drain and failed its running bound (2026-09-30)

**F1 PHYSICAL PHASE 1 — FAIL; F1 PRODUCTION SERVICE FAILURE.** The second and
last fresh attempt authorized for this task used physical run
`374a128c-3fa9-47d5-9603-5a97c3f79139` and lifecycle run
`e5ecb1e4-df26-4586-a2e3-dbad4c4f92b2`. Source revision
`d725b3a9f28f291e432415bc6555af8bf3fbd59a` passed 70 qualifier tests
and all six hosted native CI jobs. The corrected qualifier package ZIP was
`5F6D245A39F3F2CCAEB5A62E768445D07C95520466EA3908A0D67BBD608C161D`
and its `qualifier.py` was
`7000D7FF17FD4CA384582EAB2296E0C7513A872C1F02B9189535863794C33731`.
The F1 native probe remained
`E564D3CDE19F099FB3F51237C1C5B83DE4FFF78A36C4691E9231AA3E9D1DB5AA`.
Both endpoints passed installed-file, fiber, LAN, capture-idle and evidence
preflights; the actual worker child connected in 16 ms, both runner-owned
tunnel directions passed, and PID 35980 owned UDP `10.253.3.2:39450` before
`SERVER_LIVE`. The earlier control and marker false negatives were resolved.

Four actual GNS clients connected and registered: slot 1 nonce `1783077198`,
slot 2 producer nonce `1783077197`, slot 3 nonce `1783077199`, and slot 4
nonce `1783077200`. Their first accepted structural grants were respectively
5,171, 5,171, 5,766 and 6,361 attributed bytes. Four grants were outstanding;
wave 1 began, but no client completed a workload wave. Before a common
four-grant first-send interval formed, slots 1 and 2 received another legal
1,258-byte grant each. At native trace step 144, slot 1's grant activated at
`303441892390` µs and first-sent 1,135 bytes at `303441892455` µs. Its
remaining 123 bytes had not first-sent by `303441896959` µs. Its running
deficit was **75,547,803,648 byte-µs**. Slot 2 activated at
`303441892374` µs, first-sent 1,135 bytes at `303441892425` µs, and still
had 123 bytes unsent at `303441896962` µs; its deficit was
**76,118,228,992 byte-µs**. Both exceed F1's unchanged
**18,025,216,000 byte-µs** within-grant bound. Feedback was present, valid,
available and current; `StructuralServiceFailed=1` and `Qualified=0` for
both peers. The native trace SHA-256 is
`288FCF0792FE7E1735B6591A81A7FCDBC811407F36AB71D69088728129743513`.

The two grants eventually first-sent completely at `303441898406` and
`303441898373` µs, 6,016 and 5,999 µs after activation. Both meet the
separate finite completion envelope of about 6,149.369 µs for 1,258 bytes,
but their completed-grant running-deficit high waters were
**99,841,212,416** and **99,790,880,768 byte-µs**, and both completed with
the service-failed flag. The finite startup allowance cannot excuse this
intra-grant gap. The common four-peer running interval was zero, so its
64-MiB/s pool curve and a maximum 512-KiB grant remain **NOT MEASURED** in
this attempt. The exact cause of the delayed 123-byte tails inside native
GNS packetization/scheduling is not yet attributed; changing transport policy
or F1 semantics requires a separate decision and validation, not a retry here.

Both captures independently contain all four bidirectional UDP tuples; their
direction counts are mirrored:

| Client UDP port | Worker outbound / inbound | Client outbound / inbound |
| --- | ---: | ---: |
| 64708 | 147 / 214 | 214 / 147 |
| 64709 | 189 / 208 | 208 / 189 |
| 64710 | 146 / 205 | 205 / 146 |
| 64711 | 188 / 204 | 204 / 188 |

Packet Monitor exported 1,752 complete fiber miniport frames; an independent
pcap read found 1,501 qualified frames, exactly matching the client capture's
1,501 frames. No drop/lost-event counter was retained, so that metric is
**NOT MEASURED**. Native unique first-send and eventual structural ACK each
totaled 25,648 bytes, with zero retransmission. Maximum observed aggregate
sent-unacked was 22,937 bytes; admission reported four-grant high water and
22,469-byte pending/debt high water. After abort, exact retirement was 25,648
bytes, terminal release and outstanding debt were zero, and native cleanup
reported `good=1`. This convergence applies only to the accepted bytes before
the failure; the full workload's delivery and terminal gates are not qualified.
Both physical coordinator and outer lifecycle returned failure as a consequence
of the native service failure. Their available evidence manifests verify; the
client abort did not produce a final manifest. Both daemons stopped, stage
bytes and the worker's prior probe were restored, UDP `39450` and control
listeners were unbound, Packet Monitor was idle, and no probe remained.

The physical retry budget is exhausted. **KI-006 remains OPEN; Foundation 3L
remains B — PARTIALLY READY; no Foundation 3M work is eligible.** The next
task is a bounded native attribution of the 1,135+123-byte first-send gap and
an explicit transport/architecture decision consistent with F1 and the
gameplay/transport reserves before any new physical authorization.

## F1 Phase 1 stopped at an impossible server-live marker (2026-09-30)

**F1 PHYSICAL SERVICE — NOT MEASURED; QUALIFICATION-INFRASTRUCTURE DEFECT.**
The first fresh attempt after the control-path correction used physical run
`18f17968-521e-431e-a402-7b549c214166` and lifecycle run
`e1087ede-b4a0-44ef-85f8-1fafd32a359d`. Both real-file checks, both
evidence-root/worker-LAN proofs, all six native CI jobs, and the runner's
forward/reverse one-use tunnel handshakes passed. The actual worker child
connected from `192.168.0.108:58009` to the client listener in 15 ms and
exchanged `STAGE_READY`. The client capture and worker Packet Monitor started.
The worker launched its exact pinned F1 probe, PID 33032, but the qualifier
declared `server listener did not become live` after six seconds. The client
probe group had not started. The server capture exported 540 fiber frames but
zero qualified UDP `39450` directions/tuples; the client pcap contained only
headers. No grant or F1 first-send service was measured. The empty-direction
cleanup errors follow from the premature barrier stop, not from an F1 result.
Both daemons stopped and task-owned stages rolled back with evidence retained.

The qualifier's `ServerLive` predicate required a GNS `event=listening` text
marker before checking the probe-owned UDP socket. The pinned native F1 source
and executable do not contain that marker. A bounded worker-local loopback
diagnostic ran the same F1 binary with no capture or clients: its live PID
owned UDP `127.0.0.1:39450` at 1.5, 3.5, and 6.5 seconds, yet stderr never
contained the marker. This independently reproduces the false-negative
readiness predicate. The correction uses the live probe PID and its exact
owned UDP `10.253.3.2:39450` socket as the server-ready proof, with an
explicit `SERVER_LISTENER_VERIFIED` evidence event. It does not alter F1,
admission, the native probe, or capture-direction acceptance. A second fresh
physical attempt is eligible only after this corrected qualifier passes
deterministic tests, source/package checks, CI, and full fresh preflights.
**KI-006 remains OPEN; Foundation 3L remains B — PARTIALLY READY.**

## F1 physical Phase 1 stopped at the worker control barrier (2026-09-30)

**F1 PHYSICAL PHASE 1 — FAIL; LIFECYCLE/COORDINATION DEFECT.** One fresh
attempt used physical run `4523839a-4d29-44b0-9246-9c7bc9053fed` and outer
lifecycle run `e61b4f94-7df4-4029-831c-416f3989c5be`. The starting
Gargantuan revision was `700d53a7937b0cebd0b3e0dfceb6ebddf3fb3aab`;
the staged qualifier revision was `8e7b36b8ec433295fec842ec301f41d602b3de55`
on draft PR #6. The installed F1 candidate ZIP SHA-256 was
`F85805FD0B1806D0B16831971C328786872AA0FF051E2342A692E4B9A2BD9803`;
the staged qualifier ZIP SHA-256 was
`D1B7641B631FB116F2E84862740A43021F4A1D3192058FB0C05E68E9BE3668E6`.
The F1 native probe remained
`E564D3CDE19F099FB3F51237C1C5B83DE4FFF78A36C4691E9231AA3E9D1DB5AA`
with source archive
`10D0CB47ED24D8A249735C49BC55DD52A600AA9DAB05480261435C6E3483775F`
and pinned GNS `2cb93a06350bb065db53abdb0d87cf297e0bfd34`.

Before assignment, installed-file checks and socket-free F1 self-tests passed
on both endpoints; the normal-LAN interactive worker challenge and both
one-use tunnel handshakes passed. The direct-fiber link, capture device,
idle capture service and Packet Monitor, endpoint evidence-root access, and
six current-head hosted CI checks also passed. These preflights did not
establish that the actual worker endpoint process could connect to the
client coordinator's TCP `192.168.0.68:39451` listener during execution.

The outer coordinator recorded client `LIVE` and started the server role.
The physical client sent `STAGE_READY`; the physical coordinator received it.
The worker physical endpoint wrote provenance, then its three-second control
connect timed out before it could send `STAGE_READY`. Its local result was
`ABORT: timed out`; the physical client then received a connection reset and
recorded `ABORT`. The outer server role reported failure and the lifecycle
coordinator aborted. The worker's first failure is in
`C:\GargantuanQualification\physical-qualifier-service-evidence\lifecycle-006fa4f5dc9a2d3d\result.json`;
the cross-role sequence is in
`C:\Users\aiden\.codex\artifacts\gargantuan-f1-phase1-stage-draft-20260930\prepared-006fa4f5dc9a2d3d\lifecycle\evidence\e61b4f94-7df4-4029-831c-416f3989c5be\host\control.jsonl`.

No physical endpoint received the command to arm capture or start a GNS
probe. There are no qualified client tuples, grant measurements, first-send,
ACK, retirement, or packet-capture results in this attempt. F1 service and
transport behavior are **NOT MEASURED**; no application-readiness or Phase 1
PASS is inferred. The runner's tunnel was stopped; both lifecycle daemons
were owner-verified and stopped, both endpoint staged workflows/policies and
the worker candidate payload were restored from byte-verified rollback
snapshots. UDP `39450`, lifecycle ports and tunnels are clear; no task-owned
probe or dumpcap remains; the worker capture service is running idle with
Packet Monitor stopped and no filters. The one permitted attempt was not
retried. Diagnose and deterministically qualify the actual worker endpoint's
control connection before seeking authorization for another physical Phase 1
run. **KI-006 remains OPEN; Foundation 3L remains B — PARTIALLY READY.**

### Control-barrier attribution and child-path qualification (2026-09-30)

The failed physical run's client coordinator was listening before the worker
child started. The fresh control-only reproduction recorded the actual worker
child PID, medium-integrity `HOSTPC\host` token, source
`192.168.0.108:65419`, destination `192.168.0.68:39451`, and an exact
three-second TCP connect timeout. The client coordinator had already written
`LISTENER_READY`, accepted the client role, and remained active. Windows
Firewall event 2097 and its resulting enabled inbound TCP **Block** rule
identify the defect: the rule was created for the newly versioned coordinator
Python executable as the listener started. The earlier physical runtime had
also received an automatic Block rule at its listener startup; its rule was
changed to Allow only after that run's worker timeout. The interactive LAN
check used a different Python executable with an Allow rule and could not
qualify this child path. There is no observed worker-side WFP denial.

The bounded correction selects the already installed client qualifier Python
runtime for the coordinator and endpoint children. Its executable SHA-256 is
`737A7E3B71E3578F8432ACC7DD88C452E593622C544BC13DA4789D69C63DA5AE`,
identical to the versioned package runtime, and its existing inbound TCP Allow
rule applies on the Private normal-LAN profile. The task-owned stage helper
rejects a missing Allow rule or any enabled Block rule for that exact runtime.
The qualifier source remains versioned and pinned separately; neither
capture-service binary/hook nor the native F1 probe was replaced by this fix.

The dedicated `CONTROL_PREFLIGHT_ONLY` workflow ran through the actual client
and worker Codex lifecycle child launcher with fresh physical/lifecycle IDs
and full owner-verified rollback between each pass. Three **consecutive**
passes used the real worker child to bind `192.168.0.108`, connect to the
pinned client listener, exchange authenticated `STAGE_READY` and completion,
and return both outer roles to `IDLE`. Their worker connect times were
16 ms, under 1 ms, and 16 ms. No pass started capture, GNS, or a probe.
The retained audit and evidence are under
`C:\Users\aiden\.codex\artifacts\gargantuan-f1-control-stage-20260930`.
These passes qualify the corrected control path only; F1 physical service
remains **NOT MEASURED** until a fresh physical attempt completes its gates.

**Decision update (2026-09-29):** The earlier attempts below were
evaluated under superseded candidate contracts. The [F1 amendment to D01](../../docs/adr/D01-pooled-service-curve.md)
now defines finite accepted-grant first-send drain capacity and a separate
intra-grant running check. None of those earlier receipts is a physical F1
pass. The 2026-09-30 F1 run above also failed before measurement; KI-006
remains open pending a separately authorized fresh physical run.

The next F1 candidate is packaged separately from installed tools. Its native
source revision is `e082e6b3ab4e5345c03daa1a9bf630d270cb95f0`, GNS pin is
`2cb93a06350bb065db53abdb0d87cf297e0bfd34`, source archive SHA-256 is
`10D0CB47ED24D8A249735C49BC55DD52A600AA9DAB05480261435C6E3483775F`,
and Windows probe SHA-256 is
`E564D3CDE19F099FB3F51237C1C5B83DE4FFF78A36C4691E9231AA3E9D1DB5AA`.
The source archive matched all 24 selected native files in the worker build
tree after accounting only for CRLF/LF differences. The final probe was
relinked after the updated `GameSession.cpp` object, then passed its socket-free
F1 self-test. Real-file manifest, archive, probe and runtime hash preflight
passed without starting capture or probe traffic. This is candidate
provenance, not physical qualification. The corrected candidate ZIP SHA-256 is
`2F6F9217CD09D276CB23119CC34367FDFD43397F3AF08621023711A0FDB326E8`.

## Capture runtime qualified; strengthened workload still blocked (2026-09-28)

The worker's `GargantuanPhysicalQualifierCapture` service now runs the pinned
30-second hook-bound helper. Its installed executable SHA-256 is
`463934849064F7DD51FEAB73B8471FD9DBB6CAF3D9C95EBB38A8D65B2EFCA664`.
The installed hook remains
`231FAE4B89155630138BDC9ABBB1BB526C3B322D0BE2667D2A2E8B5CFEC1375D`.
The source-matched helper suite passed, including a 16-second stop and the
90-second lease/security checks. An installed-service capture-only smoke
exported a 41,418,752-byte ETL in 26.656 seconds to a complete
32,233,120-byte pcapng with 27,006 blocks (SHA-256
`AAACD86F92FC62FD92210E87ECE7C0A3439A03734303ED08252F61A2720987EF`).
The service returned to running/idle; Packet Monitor and Windows trace were
stopped, and the physical probe pins were not changed. The rollback copy is
under `C:\Sandbox\Codex\Artifacts\gargantuan-3l-sustained-20260928\capture-service\installed-backup`.

Worker-only four-peer loopback diagnostics tested barriered, exact-512-KiB
structural groups with a distinct journal tail and an unchanged 16-MiB/s
per-peer floor. Socket-free analyzer regressions cover feedable below-floor,
unprimed-ACK, and drained-GNS-queue intervals. A two-group wave exceeded the
4–15-ms selected interval while the server processed structural work. A
32-wave one-group variant reached four active grants and valid first-send
rates above the floor, but did **not** form three batches of three consecutive
four-peer windows. One run ended after wave 5 with an ACK-empty selected
interval and a terminal client submission; a traced run reached wave 26 but
had only one two-window batch. A later native-result audit corrected the
initial attribution of that run: the producer exhausted the qualifier's
512-sample cap and shut down, then a non-producer attempted a 60-byte
unreliable realtime send after its connection had ended. Native result `3`
means `k_EResultNoConnection`, not `k_EResultLimitExceeded` (`25`). The
resulting terminal send was downstream of the qualifier abort, not evidence
of a GNS queue-limit violation. A 4-ms diagnostic stopped on an early service-feedback
failure. These are isolated diagnostics, not physical qualification, and do
not establish a production service-floor failure. Candidate source and traces
are retained under `C:\Sandbox\Codex\Artifacts\gargantuan-3l-sustained-20260928`.

The workload has not independently qualified its sustained four-peer backlog
window; later full-wave variants avoided terminal submissions but still failed
the canonical window gate. **No fresh physical Phase 1
attempt was launched in this task.** The installed physical probe is still
the prior SHA-256 `1E25676BDF1DA6ED2EA8A28AB183F519D18A4802BD00E7F6730CFA77D395BD5A`;
no endpoint or capture was taken over. KI-006 stays OPEN, Foundation 3L
remains B — PARTIALLY READY, and no 3M gate was started. The historical
15-second service statements below describe the earlier physical attempt.

## Local four-peer contract remains blocked after bounded demand correction (2026-09-28)

An isolated worker-only MSVC Release build tested 32 exact-512-KiB structural
waves across four real loopback GameSessions, with one gameplay producer. The
four-bank candidate waits for bootstrap journal and admission to drain, then
authors at most one group and a 64-byte journal tail per wave. It recycles a
bank only after four intervening waves, keeps active-wave journal lag at most
18 operations, and replenishes at a bounded GNS queue low-water mark. The
hard limits are 32 waves, 288 authored updates, 16,771,200 authored string
bytes, 70 seconds total, 40 seconds per active wave, 80 producer RPC/Event
pairs, and 65,536 trace rows. The canonical four-grant maximum, 512-KiB
per-peer pending cap, 16-MiB/s floor, ACK/retirement requirements, and
POOLED_SERVICE profile were unchanged. This is a diagnostic candidate, **not**
an adopted physical probe or a qualified workload.

The complete four-bank run accepted and verified retirement of 67,148,916
bytes, with zero terminal release and four grants at high water. It authored
all 32 waves, and all four clients applied the final wave. The server observed
542 four-grant attributed-backlog rows, but the raw CSV has only one isolated
all-peer eligible step. There were 50 individually qualified peer windows and
12 feedable below-floor intervals. A 6-ms sampling variant also accepted and
retired all 67,148,916 bytes; its raw CSV has only two isolated all-peer
eligible steps, plus below-floor intervals. Earlier
replenishment, a 16-bank journal reserve, and a 1.1-second quiet-bootstrap
barrier also failed the same unmodified three-batch/three-consecutive-window
gate. All completed runs cleaned up with no outstanding bytes or grants and
no new terminal scheduler/GNS rejection. These are loopback diagnostics, not
physical service measurements. Artifacts and candidate source are under
`C:\Sandbox\Codex\Artifacts\gargantuan-3l-end-to-end-20260928` on the
controller and worker.

The first summary inflated each batch's window count because `Analyze()` ran
once for gating and again for reporting without resetting its batch counters.
That did not change the gate's FAIL result. The candidate analyzer now resets
on every invocation and has an idempotency regression. A fresh 6-ms run with
that fix accepted and retired 67,148,916 bytes, reached four-grant high water,
and recorded 529 four-grant attributed-backlog rows and 58 individually
qualified peer windows, but **zero** all-peer eligible steps and zero batches;
one feedable interval was below floor. This latest corrected result is the
local gate verdict.

The per-peer trace explains the current limit: a four-grant period usually
lasted only 4–10 approximately 5-ms samples, and all four queues were
simultaneously feedable for about 4–5 samples. First-send often exceeded the
floor during those samples, but ACK deltas arrived in bursts with intervening
ACK-empty samples. For example, at steps 870–874 of the quiet-bootstrap run,
all four grants and structural journal demand persisted; ACKs were zero for
all peers at step 870, zero for three peers at step 871, positive for three at
step 872, and zero for all at steps 873–874. Those cannot form three
consecutive ACK-positive eligible windows. Other fully feedable windows had
unique first-send below the unchanged floor and are retained as failures.
The one-second requalification interval then separated most subsequent
four-grant periods. A longer journal reserve did not keep the 512-KiB GNS
queues feedable after grants became staggered. This evidence does **not**
establish that physical production throughput is below 16 MiB/s; no valid
physical four-peer floor measurement occurred.

The native-result audit also corrected the old apparent GNS queue-limit
finding: `backend_result=3` is `k_EResultNoConnection`, whereas
`k_EResultLimitExceeded=25`. The 60-byte non-producer unreliable send occurred
after the earlier qualifier exhausted its own 512-gameplay-sample cap and
closed the connection. An opt-in GNS send-failure trace now reports the named
native result, connection state, pending/unacked bytes, queue time, rate, and
native status. MSVC transport tests pass. Seventeen socket-free floor-analyzer
cases pass, including exact/above/below floor, edge interval, grant and
backlog transitions, delayed ACK, mid-window feedability loss, and repeat
analysis. The worker
capture service and hook hashes were rechecked against the qualified pins
above; the service remains running idle.

**Architecture decision required before another physical Phase 1 run.** The
bounded qualifier can provide the 64-MiB aggregate structural demand and
clean retirement, but repeated local four-peer runs cannot satisfy the
current per-window ACK-positive three-by-three gate with the current queue,
grant/requalification, and feedback cadence. Choosing whether to change the
canonical service/queue contract or the canonical measurement requirement is
outside this qualification task. No physical run or lifecycle run ID was
allocated, no endpoint probe pin changed, and later 3L gates remain NOT
MEASURED. KI-006 stays OPEN; Foundation 3L remains B — PARTIALLY READY.

## One diagnostic Phase 1 attempt: no sustained four-grant window (2026-09-28)

The single permitted diagnostic attempt used physical run
`69bfc09f-b04c-4a10-a545-3d01cca2dd97`, label `1f6ca175cd5b489f`,
and lifecycle run `f5482f2a-5f30-424d-9c0d-2524e8dbd2dd`. Both catalog,
source/probe pin, non-admin evidence-root and worker LAN preflights passed.
The runner alone performed the one-use forward/reverse tunnel proof. The
candidate probe SHA-256 was
`F130DC868A807FFA4EF10887079162C562230854AE17C013559452791993E969`;
the service and capture hook stayed on their installed pins. Four actual
GameSession clients reached Ready and registered: nonce `93731` was the sole
producer, and `93732`–`93734` were non-producers. Wave 1 began, the structural
updates were applied, and the worker reached four active grants.

The control order was: coordinator LISTENING at client Unix ms
`1790627895389`; both endpoints staged; client capture LIVE at `7895980`;
worker probe launch at worker Unix ms `7898335`; four client probe launches
at client Unix ms `7899227`–`7899246`; then the worker probe failed its
funding analyzer and closed the GNS server. The worker emitted its `FAILED`
control result at worker Unix ms `1790627910436`; the coordinator received
the client nonzero result and sent ABORT at client Unix ms `1790627910447`.
Client and worker clock offsets were not bounded, so the eleven-millisecond
wall-clock difference is not a cross-machine latency measurement. The
worker's explicit failure was `sustained four-way backlog/fresh-window proof
incomplete`; all clients then reported `Game server connection closed`.
There was **no terminal scheduler submission rejection** in this fresh run.
Its absence cannot timestamp or explain the old run's untimestamped rejection.

The worker trace SHA-256 is
`7B6FB68F4EA3261726040FAEA17BFA822FAD45A2A14813D0D3DEF17A73AA1A14`.
It contains 7,026 rows and 4,449 rows with journal lag, but only two
four-grant attributed-backlog rows and **zero class-2 or class-3 service-floor
intervals**. No qualified four-grant batch formed (three required). The
producer completed 83 RPC and 82 Event samples at its fixed minimum 100-ms
cadence, with no terminal send trace. Exactly one producer and bounded
distinct demand are established; a flooding contract violation is not.
The finite one-wave workload did not maintain the simultaneous, feedable
four-peer backlog required to measure the canonical floor. This attempt is
**INVALID / UNDERFED QUALIFICATION WINDOW**, not a measured POOLED_SERVICE
floor failure. The old 6.714-ms row remains **INDETERMINATE** because its
missing rejection order and continuous-eligibility evidence cannot be
reconstructed retroactively. The 16-MiB/s per-peer target is unchanged.

The worker reported 16,972,172 B accepted and exactly retired, zero terminal
release and outstanding bytes after cleanup, four grants at high water,
2,097,152 B maximum debt and pending admission, and no sampled retransmission.
Final per-peer unique-first/ACKed bytes were 4,259,972 / 4,259,972 for slot 1,
4,243,494 / 4,243,494 for slots 2 and 3, and 4,243,501 / 4,243,501 for
slot 4. Sampled per-peer GNS pending peaked at 522,019 B, unacked at
351,333 B, and queue age at 27.648 ms. These are finite-run totals and
maxima; canonical per-peer and aggregate sustained service, fairness and
full-window recovery are **not measured**.

The retained client pcapng is structurally complete: 3,640 outbound and
19,109 inbound qualified UDP frames across all four source ports. Its
client-result capability did not run after the host aborted, so no client
result file or as-run capture acceptance exists. Worker
capture `stop` did **not** acknowledge completion. The installed generic
service killed its hash-pinned export hook at 15 seconds on this 30 MiB ETL;
the endpoint recorded `CaptureStop hook failed` and no pcapng at result time.
Post-run, the unchanged installed hook converted a copy of that raw ETL in
19.262 seconds and exported 23,752 complete miniport frames. The recovered
pcap SHA-256 is
`04FB1AB7D01615FDF1685F2FA27F6EB8AF4376EA6AA2767C490D7ACECB26590E`;
it has 19,109 outbound and 3,640 inbound qualified frames across the same
four ports. This recovered pcap was copied back and the owned service stop
acknowledged, leaving the service idle. The recovery does **not** turn the
as-run worker capture gate into a pass. The client pcap SHA-256 is
`0931A9E57E1E28062ED6061E1EE524277EE6D7E006AD7C9D04CFA83A1A35C3BA`.

The fixed upstream helper now allows a bounded 30-second hook export, with a
16-second completion/idle regression, and the Gargantuan endpoint waits at
most 45 seconds for its explicit stop acknowledgement. Upstream draft PR #3
is stacked on the existing classification PR #2; its source commit is
`e02fad12ac53b7cb93535013357122172217ab30`. Python suites, the Windows
helper test, and the physical hook simulation passed. **The installed
capture service was not replaced or physically requalified.** The temporary
worker candidate probe was rolled back to its prior SHA-256
`1E25676BDF1DA6ED2EA8A28AB183F519D18A4802BD00E7F6730CFA77D395BD5A`;
both lifecycle daemons, temporary profiles, credentials and listeners were
removed. Packet Monitor and Windows trace are stopped, with no filters;
UDP 39450 is unbound and the service is running idle.

The worker result and physical coordinator are `ABORT/Success=false`; the
client aborted without a final result file. The outer lifecycle is
`Success=false`, host exit 1, with both agents
`FAILED/AGENT_FAILED`. Evidence, including the raw ETL, native CSV and
post-run recovery receipt, is under
`C:\Sandbox\Codex\Artifacts\gargantuan-3l-attribution\physical-phase1-1f6ca175cd5b489f`.
**STRENGTHENED FOUR-CLIENT PHASE 1 — FAIL / NO CANONICAL FLOOR MEASUREMENT;
KI-006 OPEN; Foundation 3L B — PARTIALLY READY.** This run was not retried.
Next, attribute why the bounded one-wave demand produces only two
four-grant backlog rows, and qualify a contract-correct sustained workload
and the updated capture service before any separately authorized physical
attempt. Phase 2 Local/Node, 32 actual clients, final acceptance, merge and
3M remain gated.

## Post-run attribution of the corrected Phase 1 stop (2026-09-28)

The retained run `da16daee-49d2-4812-a985-3159806d202e` is **INDETERMINATE**
as a service-floor test. On the worker's monotonic microsecond clock, the sole
class-3 interval for producer peer slot 1 was `[118738033926,
118738040640]`. At both endpoints its 524,288-byte debt token 23 and 320
structural journal-lag records persisted, with four active grants in those
two slot-1 samples. Unique first-send increased by 38,995 bytes and ACKed
stream bytes by 92,097. GNS pending reliable bytes fell from 38,995 to zero
over this interval; two other peers also ended step 1038 with zero pending,
and the global active-grant count had dropped to three by the fourth peer's
sample eight microseconds later. The row does not establish that all four
peers remained eligible, with continuously feedable scheduler and backend
queues, throughout the selected interval. There is no reload marker, but
the retained trace lacks queue depth and a probe-abort timestamp at this
resolution.

The exact native rule selected a same-token, same-size debt with active
retirement attribution, nonzero journal lag and four grants at both samples;
6,714 microseconds is inside its 4,000–15,000-microsecond window. The
unchanged 16-MiB/s peer floor is 16,777,216 B/s, so
`ceil(16,777,216 × 6,714 / 1,000,000) = 112,643 B`. The 38,995-B
observation is 5,808,013 B/s and correctly sets native `Qualified=0` and
`floor_failure=1`. The arithmetic is valid; the evidence does not prove
the sustained workload preconditions needed to attribute that row to
POOLED_SERVICE. A single short terminal-edge row is not a sustained
four-peer or aggregate service measurement.

The producer began after the four client starts (`1790589317438`–`7456`
Unix ms on the client), sent 36 sequential ordinary RPC/Event pairs at a
100-ms minimum cadence, and reported its terminal scheduler submission by
the client failure message at `1790589323304` client Unix ms. The worker
probe stopped on the native floor flag after the interval. The producer's
stderr has no timestamp on the rejection and no underlying transport status.
Client and worker wall clocks were not correlated to a common bounded
offset, so the rejection cannot be placed **BEFORE, DURING, or AFTER** the
6.714-ms worker interval. `NetworkScheduler::Flush` did accept an intent
into its queue before calling `IGameTransport::Send`; a `WouldBlock` would
retain it, while any other failed transport status clears the connection.
The generic `Transport rejected scheduler submission` fallback does not
identify whether GNS rejected a frame, reached its pending-reliable cap,
lost the connection, or failed another backend operation. Caller traffic,
request size, queue depth and GNS status at that event were not recorded.
The completed producer samples and fixed cadence do not support a producer
flood diagnosis. No production scheduler or POOLED_SERVICE defect is
established, and the 16-MiB/s floor remains unchanged.

Abort cleanup independently truncated the client pcapng because it waited
for dumpcap's duration only after a successful probe. It terminated dumpcap
while a block was being written on failure. The worker endpoint allowed ten
seconds for the capture-service `stop` command, but the service finished
the owned export about 11 seconds after the abort; the endpoint had
already tried to validate a missing pcap. The first adapter correction waits
for the client capture's fixed bounded duration on abort as on success, gave
service stop a 25-second bound, and required its completed-export
acknowledgement before capture validation. Regression tests cover abort-side
final-block completion and receipt validation. The later diagnostic attempt
above exposed the service's separate 15-second hook limit; source now raises
that bound to 30 seconds and the adapter wait to 45 seconds. The new service
binary is not installed.

Opt-in GNS and scheduler terminal-send diagnostics now record paired Unix and
monotonic clocks, status, rejection site, message bytes/type, and scheduler
queue state when the existing qualifier trace environment variable is set.
The endpoint also records a paired clock at probe launch. Those changes
were built as a separate candidate from base
`14644a369f9e7bfb9a81c21354adae62902d63d7`, qualification overlay
`2ED31AE67E0F99619940BBB130CD451DB37FEF3A5CEEDAB475E682C3FBEE7003`,
and GNS pin `2cb93a06350bb065db53abdb0d87cf297e0bfd34`.
The candidate probe SHA-256 is
`F130DC868A807FFA4EF10887079162C562230854AE17C013559452791993E969`;
its five-case socket-free analyzer self-test passed on both PCs. The worker
capture service independently acknowledged `start/running`, `stop/stopped`
after export, and `status/idle` for a smoke run with a complete 15,296-byte
pcapng. A short real client dumpcap abort smoke exited normally and closed a
complete 556-byte pcapng; zero matching packets were expected and did not
qualify any direction. The adapter's 64 local tests passed. The candidate
remained in task-owned staging at this attribution checkpoint. The later
single diagnostic attempt is recorded above; its temporary probe installation
was rolled back, and the installed service/hook pins stayed unchanged.
**STRENGTHENED FOUR-CLIENT PHASE 1 — FAIL / ATTRIBUTION INDETERMINATE;
KI-006 OPEN; Foundation 3L B — PARTIALLY READY.**

## Corrected strengthened Phase 1 stopped on service/transport failure (2026-09-28)

**STRENGTHENED FOUR-CLIENT PHASE 1 — FAIL; NO FUNDING PASS.** The previous
attempt's all-zero producer flags were a qualifier staging defect. Gargantuan
revision `dcab0993c` made the first client the sole producer without changing
the pinned probe, POOLED_SERVICE, GNS, GameSession or service constants. All 62
qualifier tests passed, including staged-config rejection of invalid producer
flags and launched arguments `1,0,0,0`; the unchanged probe's socket-free
five-case analyzer self-test passed on both endpoints.

One corrected fresh attempt used physical run
`da16daee-49d2-4812-a985-3159806d202e`, label `bb6aa500a8d84824`, and
lifecycle run `0e9effd2-3d1c-4afa-9003-c577c5683711`. The qualifier source
SHA-256 was
`CECDF79009D4463F2696DF9E934352AF675A203704112472EF04BA9DD69B744D`;
the fixed workflow SHA-256 remained
`61b8a001779a695363f7252cb82e08b9b0d7675fdb94fe865b6792d63f2f2320`,
and the unchanged probe SHA-256 remained
`1E25676BDF1DA6ED2EA8A28AB183F519D18A4802BD00E7F6730CFA77D395BD5A`.
Both endpoint catalog, evidence-root, fiber/NIC, capture-idle and source-pin
preflights passed without touching the one-use tunnel proof. The lifecycle
runner alone performed its built-in forward/reverse tunnel preflight, which
passed before assignment. No separate proof consumption or retry occurred.

Four actual GNS sessions connected, with unique nonces `92707`–`92710`.
Worker registration reported exactly one producer (`92707`) and three
non-producers. Client control evidence independently records probe arguments
`1,0,0,0`. Wave 1 began and all four clients moved, but none reported the
wave applied. The producer completed 36 RPC and 36 Event samples before its
session failed; its RPC p95/p99/max were 99.5973/121.841/121.841 ms and
Event maximum was 121.843 ms. These are short pre-abort samples, not a full
gameplay qualification.

The worker's pinned native feedback trace has 3,657 rows. At simulation step
1038, peer slot 1 retained a same-token 524,288-byte grant, journal lag 320,
and four active drain grants across a 6,714-microsecond interval. Its unique
first-send delta was 38,995 bytes (5,808,013 B/s), below the required 112,643
bytes for the unchanged 16-MiB/s peer floor; ACK delta was 92,097 and retry
delta zero. The analyzer marked that interval class 3 and
`floor_failure=1`, then the worker stopped with `observed service/feedback
failure`. Only ten sampled rows showed four-grant backlog and only one peer
had one qualified window; there were zero qualified four-grant batches. No
repeatable per-peer or aggregate structural-service floor was established.
The producer client also failed with `Transport rejected scheduler
submission`; the retained logs do not determine whether that transport
failure caused, followed, or independently accompanied the floor failure.
The control barrier aborted on the client's nonzero exit. This run supports a
bounded under-floor observation under four-grant demand, not a complete
attribution to POOLED_SERVICE, GNS, CPU, queueing or link loss.

The worker reported 6,486,412 B accepted, 4,913,548 B exactly retired and
1,572,864 B terminally released during abort, with zero outstanding debt or
grants after cleanup. Terminal release is not service. Four grants and
2,097,152 B debt were observed, with aggregate pending high-water
2,097,152 B; sampled per-peer pending/unacked maxima were 519,745/188,742 B,
maximum sampled GNS queue time 27.528 ms, and sampled retransmission zero.
These truncated-run observations do not prove fairness, bounded full-run
credit/debt, recovery or convergence. Host CPU/memory headroom, Packet Monitor
drop/lost events, fixed service recovery and full structural convergence were
**not measured**.

Capture integrity also failed the full gate. The client pcapng ended with a
malformed block when abort cleanup terminated its timed capture. Worker
capture `stop` exceeded the endpoint's ten-second hook timeout, so the
worker result could not validate its pcap at result time. The capture service
subsequently stopped and exported 11,147 complete miniport frames. Its
retained complete pcap contains all four UDP source ports (`57815`–`57818`)
in both directions, with 7,384 qualified outbound and 3,175 inbound packets;
the client pcap remains incomplete. Neither packet counts nor the later worker
export repair the failed capture gate. Client and worker results are
`ABORT/Success=false`, the physical coordinator is `ABORT/Success=false`, and
the outer lifecycle is `Success=false` with host exit 1 and both agents
`FAILED/AGENT_FAILED`.

The run artifact and copied worker evidence are retained under
`C:\Sandbox\Codex\Artifacts\gargantuan-3l-four-client-design\physical-phase1-bb6aa500a8d84824`.
The worker's exact native CSV SHA-256 is
`B4E7694781228D91D970B741A35BC90502D7E53A78B9BFCB2CC0DBD4D0BE84ED`;
the worker pcap SHA-256 is
`31F635E74E67ED66BB4EE9D115F87341C05FFBFAFC5DFFC3DFA8F7B5EE5E425E`,
verified against the worker original. Client, coordinator and lifecycle-host
evidence manifests have zero mismatches (13/2/2 files). The worker endpoint
did not produce a manifest because cleanup failed; a separate 18-file hash
inventory covers its copied evidence, native trace and client probe CSVs.

Task-owned daemons, scheduled task, temporary profiles, credentials, staged
configs and listeners were removed. Both installed qualifier copies returned
to the prior `BFD15390A40E02F94964E5ECC5C381780E3C48460AC17411E8820FDBC80B4136`
pin; the worker capture hook pin stayed
`231FAE4B89155630138BDC9ABBB1BB526C3B322D0BE2667D2A2E8B5CFEC1375D`.
The worker capture service is running idle, Packet Monitor stopped with no
filters, Windows trace stopped, and UDP 39450 and lifecycle listeners clear.
This was the single corrected fresh attempt after the attributable staging
defect; the Phase 1 automatic retry budget is exhausted.

**KI-006 OPEN; Foundation 3L B — PARTIALLY READY.** Phase 2 Local, Phase 2
Node, the 32-actual-client matrix and final acceptance sweep remain **not
measured**; 3M and merge remain gated. The next task is a bounded attribution
of the exact GNS scheduler-submission status and its timing relative to native
service feedback, plus the abort capture-finalization path. Establish whether
the below-floor row represents an implementation defect, transport/resource
failure or invalid qualification interval before choosing a correction or any
newly authorized physical attempt. Do not change the accepted service floor.

## Strengthened Phase 1 fresh attempt stopped at missing producer (2026-09-28)

**STRENGTHENED FOUR-CLIENT PHASE 1 — FAIL BEFORE SERVICE MEASUREMENT.** One
fresh physical run `68b5abfe-ed9c-44b3-963e-c1d6543fc7d8` (label
`35c0190e2b084f54`) created lifecycle run
`fa69fa22-bc5f-4236-8516-eaf6aacce018`. The lifecycle runner alone
performed the one-use forward/reverse tunnel handshake; it passed before
assignment. No independent tunnel or nonce check consumed the proof.
Non-consuming endpoint catalog, exact probe/workflow/source pins, writable
evidence roots, capture-idle state, 10-GbE/MTU/address and unbound-port checks
passed first. The installed qualifier SHA-256 was
`B3F39071E66BD8846C7DD5131E6CBBFF432B45013FC8AC60B5DCCA9F1C0D0A81`,
workflow SHA-256 was
`61b8a001779a695363f7252cb82e08b9b0d7675fdb94fe865b6792d63f2f2320`,
and the unchanged probe SHA-256 was
`1E25676BDF1DA6ED2EA8A28AB183F519D18A4802BD00E7F6730CFA77D395BD5A`.

The server accepted four real GNS sessions with client nonces `92707`,
`92708`, `92709` and `92710`. The capture identified four client source ports,
`54961`, `54962`, `54963` and `54964`, on `10.253.3.1` to worker
`10.253.3.2:39450`; the evidence does not map each nonce to a port. The first
causal failure was the worker probe's
`[Probe:Failure] exactly one gameplay producer required`. Its four peer rows
all say `producer=0`: Phase 1 staging gave every client a final probe argument
of `0`, although this canonical workload requires exactly one producer. The
server stopped before a qualified Ready interval or any workload wave
(`waves=0`, `applied=0`). Client `92707` exited unsuccessfully and the other
clients lost the server connection. This is a qualification-harness workload
construction defect; the run does not establish a production POOLED_SERVICE
service failure. It was not retried.

The probe's `four_grant_backlog_rows=8`, `grants_high_water=4` and
`pending_high_water=86352` are startup observations before any workload wave.
They are **not** a qualified four-grant backlog duration, debt/credit bound,
pending/unacked peak under load or service sample. The analyzer reported zero
qualified batches and `B-four-grant-overlap`; that secondary classification
must not replace the explicit missing-producer failure. Per-peer and aggregate
service rates, unique first-send/ACK/retired bytes under load, retransmission,
fairness, queue time, latency, recovery and convergence are **not measured**.

Both captures retained all four exact UDP tuples in both directions. The
complete client pcap has 996 outbound and 594 inbound qualified packets:
ports `54961` 253/148, `54962` 246/148, `54963` 251/149 and `54964`
246/149 (outbound/inbound). The worker pcap has 649 outbound and 1,065
inbound: ports `54961` 162/271, `54962` 163/265, `54963` 162/267 and
`54964` 162/262. The worker capture service exported 2,016 complete fiber
miniport frames and stopped successfully; no Packet Monitor drop/lost-event
count was established. The client and worker results both say `ABORT`,
`Success=false`, `client probe exited unsuccessfully: 92707`. The physical
coordinator also says `ABORT`, `Success=false`; the outer lifecycle says
`Success=false`, host exit 1 and both agents `FAILED/AGENT_FAILED`.

The retained run artifact is
`C:\Sandbox\Codex\Artifacts\gargantuan-3l-four-client-design\physical-phase1-35c0190e2b084f54`;
its lifecycle receipt and full copied worker evidence are under `evidence` and
`worker-evidence-full`. Client and worker evidence roots remain under
`C:\Sandbox\Codex\Evidence\physical-qualifier\lifecycle-35c0190e2b084f54`
and
`C:\GargantuanQualification\physical-qualifier-service-evidence\lifecycle-35c0190e2b084f54`.
The four evidence manifests have zero mismatches (client/coordinator/worker/host:
13/2/13/2 files). Client and worker temporary tasks, listeners, profiles,
credentials and configs were removed; prior qualifier pins were restored to
`BFD15390A40E02F94964E5ECC5C381780E3C48460AC17411E8820FDBC80B4136`.
The worker capture service is running idle, Packet Monitor stopped with no
filters, Windows trace stopped, and UDP 39450 unbound. The installed capture
hook remains at
`231FAE4B89155630138BDC9ABBB1BB526C3B322D0BE2667D2A2E8B5CFEC1375D`.

The next task is to correct Phase 1 qualifier staging to designate exactly one
client producer, add a regression that rejects a zero- or multiple-producer
four-client stage, and independently qualify the constructed workload before
a separately authorized fresh physical attempt. Preserve the canonical
strengthened workload and service constants. The prior four-client readiness
PASS remains historical evidence; this attempt did not repeat that gate.
**KI-006 OPEN; Foundation 3L B — PARTIALLY READY.** No 3M or merge.

## Strengthened Phase 1 staging stopped before assignment (2026-09-28)

The strengthened four-client Phase 1 adapter was staged with fixed non-smoke
probe arguments, a 70-second complete client capture, a distinct
`FOUR_CLIENT_PHASE1_ONLY` classification and a bounded lifecycle workflow.
Its pinned probe/source manifest matched on both PCs; the existing binary's
socket-free analyzer self-test passed on both. The qualifier suite passed 61
tests before the lifecycle launch, including Phase 1 result parsing and classification
rejection. Both interactive evidence-root preflights, worker capture-idle and
10 GbE/MTU/address checks, and an independent forward/reverse tunnel handshake
passed. No capture or probe started during those checks.

The staged physical run ID was `ffa573c0-91a8-418a-9ac6-4efb63b12728`,
label `2370720e43cb49e6`. The independent tunnel proof consumed the worker's
one-use reverse-proof file for this label. The subsequent lifecycle runner
repeated that preflight with the same label; the worker rejected the consumed
proof and the host timed out before assignment. The retained staging artifact
is `C:\Sandbox\Codex\Artifacts\gargantuan-3l-four-client-design\physical-phase1-2370720e43cb49e6`.
There is **no lifecycle run ID**, no physical coordinator result, no probe or
packet capture, and no Phase 1 funding measurement. The staged ID is not a
completed physical attempt. Under the preflight stop rule, it was not retried.

Task-owned daemons, tunnel, temporary profiles, credentials and staged configs
were cleaned. UDP 39450 and lifecycle listeners are clear; Packet Monitor is
stopped with no filters, and the capture service is idle. Both installed
qualifier sources were restored to their prior
`BFD15390A40E02F94964E5ECC5C381780E3C48460AC17411E8820FDBC80B4136`
pin; the worker hook remains
`231FAE4B89155630138BDC9ABBB1BB526C3B322D0BE2667D2A2E8B5CFEC1375D`.
The harness now reports a consumed worker proof promptly, with a regression
test. A separately authorized fresh stage must use the lifecycle runner's
single built-in tunnel preflight; it must not spend the same one-use proof in
an independent check. **STRENGTHENED FOUR-CLIENT PHASE 1 — NOT MEASURED;
KI-006 OPEN; Foundation 3L B — PARTIALLY READY.**

## Four-client physical readiness passed; historical lifecycle false negative corrected post-run (2026-09-28)

**FOUR-CLIENT PHYSICAL READINESS — PASS. OUTER LIFECYCLE
RECONCILIATION — HISTORICAL FALSE NEGATIVE, CORRECTED POST-RUN.** The original
outer lifecycle receipt remains FAIL; no physical rerun was made for its
classification. This interpretation follows independent qualification of Agent
Coordinator runtime correction `6a9824cca891ad4cea0e1e6a0d1e374c09b02f7e`
and test-only revision `2712a4bc26db0b5f6924631ebe5ab9a25c158475`:
87 local tests and hosted Ubuntu and Windows control checks passed. The
deterministic cases cover delayed clean completion, stale `MISSING_CAPABILITY`
with local `IDLE` and clean Codex exit, and rejection of actual missing
capability, failed bootstrap, nonzero exit, stale report without local `IDLE`,
stale report with failed process, and duplicate completion. Retained client,
coordinator, worker, and lifecycle-host evidence manifests have zero missing
or mismatched files (13, 2, 13, and 2 entries, respectively).

Before one fresh attempt, the three previous evidence defects were separately
qualified. The client Mellanox dumpcap closed a four-flow synthetic pcapng by
its own duration stop: 20 outbound and 20 inbound packets per flow, with a
valid complete file. The staged worker NDIS physical-interface hook exported
20 inbound and 20 outbound packets per flow on both diagnostic UDP 39452 and
readiness UDP 39450. The installed fixed-operation service repeated the
39450 proof under its endpoint-process lease. Generic coordinator tests and
Gargantuan staging tests established that a trusted four-client classification
must agree with both endpoint results. The new upstream pin is
`9c81cfb16640dc18e29b6253fc0f5463c4c4dd4b`; the installed worker hook
and service hash pin are
`231FAE4B89155630138BDC9ABBB1BB526C3B322D0BE2667D2A2E8B5CFEC1375D`.
The service binary and GNS probe were unchanged, and the prior hook/config
remain in the protected worker-local rollback backup.

Exactly one fresh four-client attempt followed. Physical run
`acd22294-75d5-4457-b69e-73bb6a00d8af`, label `dbeac03bebd84d1c`, used
lifecycle run `55d56c05-4dda-435b-a27b-d8578a084978`. Both evidence/LAN
preflights and the forward/reverse tunnel handshakes passed before assignment.
The qualifier source hash on both PCs was
`BFD15390A40E02F94964E5ECC5C381780E3C48460AC17411E8820FDBC80B4136`;
the four-client workflow hash was
`e075e8675028d620e9b86ca9b13cc25caf8752993657171a19e9735cbcbe7a03`.
The unchanged probe hash was
`1E25676BDF1DA6ED2EA8A28AB183F519D18A4802BD00E7F6730CFA77D395BD5A`.
Four distinct client nonces **92707–92710** launched once after `SERVER_LIVE`.
The server accepted four distinct GNS connections and reported
`ready=4 expected=4`; the verified probe requires Ready/active 4/4 to remain
simultaneous for its canonical one-second interval. Each client reported
`ready=1 expected=1 clean_remote_shutdown=1`; all five probe cleanup reports
had `good=1`.

Both complete raw captures passed the exact four-tuple direction gate:

| Client source port | Client outbound / inbound | Worker inbound / outbound |
| --- | ---: | ---: |
| 49241 | 151 / 100 | 151 / 100 |
| 49242 | 148 / 100 | 148 / 100 |
| 49243 | 156 / 101 | 156 / 101 |
| 49244 | 148 / 101 | 148 / 101 |

The client total is **603 outbound / 402 inbound**; the worker has **603
inbound / 402 outbound**. Client dumpcap closed its 1,005-packet pcapng after
40 seconds and reported zero drops. The worker service stopped its owned NDIS
trace and exported 1,323 complete fiber frames, of which 1,005 are the exact
qualified GNS tuple; the additional frames are outside the tuple gate. The
client pcap SHA-256 is
`89C4C53CC9D04EED89B85C2662373A6A20B4847E92E3C4B6991019EDB973FB4C`;
the worker pcap SHA-256 is
`6EC30209D8A6F3F26D2725DE72E57E0E6A8A22C26A9CF8BC58D2F3A359800361`.
Client, worker, coordinator and lifecycle-host evidence manifests verify
against their retained files.

Both physical endpoints exited 0 with `Success=true`; the physical coordinator
returned `Success=true`, and all three results were classified
`FOUR_CLIENT_READINESS_ONLY`. The lifecycle host also recorded both capability
results as successful and returned code 0. Its enclosing run adapter still
returned FAIL because the server Codex agent finished in
`NEEDS_USER/MISSING_CAPABILITY` instead of `IDLE` after the physical protocol
completed. That later agent status does not negate the recorded GameSession or
packet evidence, but it prevents a clean outer lifecycle PASS claim. The
physical readiness evidence is **PASS**; the historical outer lifecycle
receipt is **FAIL**, attributable to the post-run-corrected false negative.
There was no retry.

Retrospective inspection of the worker Codex session identified the false
`MISSING_CAPABILITY` report: its bootstrap command yielded after 30 seconds,
while the tool wrapper displayed `exit_code=undefined` without the live session
ID. The server control journal then completed its authorized capability and
cleanup successfully before the model emitted that report. Agent Coordinator
revision `6a9824cca891ad4cea0e1e6a0d1e374c09b02f7e` instructs the agent to
poll yielded tool sessions and reconciles this specific stale model reason only
when the locally validated bootstrap reached `IDLE` and the Codex process
completed cleanly. Genuine missing-capability and other approval failures are
not overridden.
The original outer FAIL receipt is preserved; this code correction has not been
physically rerun and does not retroactively change that receipt.

The retained bundle is under
`C:\Sandbox\Codex\Artifacts\gargantuan-3l-four-client-design\physical-four-dbeac03bebd84d1c`;
client raw evidence is under
`C:\Sandbox\Codex\Evidence\physical-qualifier\lifecycle-dbeac03bebd84d1c`,
and worker raw evidence is under
`C:\GargantuanQualification\physical-qualifier-service-evidence\lifecycle-dbeac03bebd84d1c`.
The one-run daemons, task, tunnel, configs, secrets and temporary profiles
were removed. UDP 39450 and lifecycle listeners are clear; the worker trace
and Packet Monitor are stopped with no Packet Monitor filters, and the capture
service is running idle on the new hook pin. **KI-006 remains OPEN; Foundation
3L remains B — PARTIALLY READY.** The strengthened Phase 1 workload, 32-client
matrix, 3M and merge were not started.

## Four-client application readiness passed; capture gate failed (2026-09-28)

The separately authorized four-client attempt used the one-client-qualified
interactive Codex profile format, evidence roots, lifecycle daemons, fixed SSH
forward/reverse topology, barrier, GNS probe binary and worker capture service.
Before staging, both Mellanox interfaces were Up at 10 GbE and MTU 1500, with
the existing `10.253.3.1/30` and `10.253.3.2/30` routes. The client dumpcap
device matched the current Mellanox interface GUID. Both logged-in non-admin
users passed harmless Codex startup and evidence-root write/read/rename/delete
checks. The exact probe hash on both PCs remained
`1E25676BDF1DA6ED2EA8A28AB183F519D18A4802BD00E7F6730CFA77D395BD5A`;
the installed worker hook and service pin remained
`2BC2E1430A29ACDE61DCCD35B943998FDB45D8E81E3E2A206D66158F334634D9`.
UDP 39450 and lifecycle ports were unbound, Packet Monitor stopped with no
filters, and the capture service running idle. The interactive path did not
require the restricted worker socket broker. Fresh evidence/LAN checks and
both run-bound SSH tunnel handshakes passed before agent assignment.

Physical run `4a0eedd7-a16a-4be2-a876-d618723db4ba`, label
`d67e466ebaf846d4`, used lifecycle run
`25e59048-71d5-4d3f-b716-fb8cd17b6f60`. The staged qualifier source SHA-256
was `E0BD1D6B947BA0F3BFE3300FA32C7CAC2D430541781B45B9FF5A042820B0C54A`;
the four-client workflow hash was
`e075e8675028d620e9b86ca9b13cc25caf8752993657171a19e9735cbcbe7a03`.
Four distinct client probe
identities, nonces **92707, 92708, 92709 and 92710**, launched once after
`SERVER_LIVE`. The captures identify four source-port tuples:
`10.253.3.1:{62242,62243,62244,62245} -> 10.253.3.2:39450`.
The evidence does not establish a nonce-to-port mapping. The worker logged
four incoming GNS callbacks, four successful accepts and four connected states
for distinct server connection IDs `1325778377`, `1863519865`, `931456497`
and `2580737725`;
each client logged connected and its canonical clean close. Each client probe
reported `ready=1`, `expected=1`, `clean_remote_shutdown=1` and cleanup
`good=1`. The verified probe source requires the server's Ready-peer and active
connection counts to be **4/4 simultaneously for at least one second** before
reporting success; its server output was `ready=4`, `expected=4`, cleanup
`good=1`. The strengthened structural-service workload was not run.

The **first causal failure** was the client capture gate at `CLIENT_DONE`:
dumpcap's raw pcapng ends 456 bytes into a declared 1,248-byte packet block,
so the complete-file parser correctly rejected it as malformed. The raw file
SHA-256 is
`861E02886C5D9132BD85BB9819608E505E5D4BEFB7431853601D1E867EF0351B`;
its manifest verifies the retained bytes. A separately saved valid-prefix
diagnostic contains 918 complete packets, **584 outbound / 334 inbound**, with
both directions on all four ports, but the invalid raw capture cannot satisfy
the gate. Hard termination of dumpcap during cleanup is consistent with this
partial final block; the exact write interruption is not independently proven.

The worker capture independently failed the four-tuple gate. Its Mellanox
miniport component 13, edge 1, produced 453 full-length, unique packet group
IDs; ETL and pcap each have 453 packets, with zero reported drops or lost
events. The qualified UDP counts by client source port were:

| Source port | Worker outbound | Worker inbound |
| --- | ---: | ---: |
| 62242 | 38 | 148 |
| 62243 | 40 | 150 |
| 62244 | 38 | 0 |
| 62245 | 39 | 0 |

Thus the worker total was **155 outbound / 298 inbound**, but two actual client
flows lacked worker ingress capture despite all four reaching GameSession Ready.
The worker pcap SHA-256 is
`60B015029453346C74334BF9AC88B9F9A6EC709FC9E088A38E7E9D3F0B0F8B53`;
the ETL SHA-256 is
`A38FDE4B5F3E625ED7EDA20A99696723FA2ACF6887C8E45DAEF77E09E253F364`.
These observations do not establish packet loss or the cause of the missing
miniport receive records.

Both physical endpoint probe processes exited 0 but sent unsuccessful results
because of their capture gates. The client reported its malformed capture
first; the worker reported two missing inbound tuples next. The physical
coordinator result and lifecycle host were unsuccessful. The delayed `FINALIZE`
was legal; `RUN_DONE Success=false` completed the physical protocol and both
lifecycle agents ended FAILED. The pinned generic coordinator still labels its
top-level result `ONE_CLIENT_READINESS_ONLY` even though both endpoint results
are `FOUR_CLIENT_READINESS_ONLY`; that classification mismatch also needs
reconciliation before any four-client acceptance claim.

The raw client and worker manifests verify. The retained lifecycle bundle and
derived valid-prefix diagnostic are under
`C:\Sandbox\Codex\Artifacts\gargantuan-3l-four-client-design\physical-four-d67e466ebaf846d4`;
the client raw evidence is under
`C:\Sandbox\Codex\Evidence\physical-qualifier\lifecycle-d67e466ebaf846d4`,
and the worker raw evidence is under
`C:\GargantuanQualification\physical-qualifier-service-evidence\lifecycle-d67e466ebaf846d4`.
The one-run probes, daemons, tunnel, tasks, configs, keys and temporary profiles
were removed; UDP 39450 and lifecycle listeners are clear. Packet Monitor is
stopped with no filters, and the installed capture service is running idle on
the same pin. This consumed run was **not retried**.
**FOUR-CLIENT SIMULTANEOUS READINESS — FAIL. KI-006 remains OPEN; Foundation 3L
remains B — PARTIALLY READY.** The next task is bounded attribution and
correction of client capture finalization, worker four-flow ingress visibility
and the four-client coordinator classification, followed by a separately
authorized fresh four-client attempt. The qualified one-client test need not
repeat unless that diagnosis directly invalidates it. Do not begin strengthened
Phase 1, 3M, final acceptance or merge.

## One-client physical readiness passed (2026-09-28)

The preceding agent-startup failure was reproduced without a probe: this
Codex CLI rejects the legacy `[profiles.physical-qualification-interactive]`
table in the main `config.toml` before `thread.started`. Temporary dedicated
`physical-qualification-interactive.config.toml` files with the same approved
interactive sandbox and approval settings passed harmless `codex exec` smoke
checks under the logged-in, non-admin user on each PC. The temporary files
were removed after this attempt. No global or restricted profile was changed.

One separately authorized fresh attempt used physical run
`61920077-ed08-4552-b4d6-0d314e431959`, stage label
`3f8637c529cc489f`, and lifecycle run
`2e195100-fcc5-47ae-9442-6443bcf97025`. Both evidence/LAN preflights and
the fixed SSH forward/reverse handshakes passed before assignment. Both Codex
agents started, registered and reached the barrier. The worker capture started
before its GNS listener reported ready at `10.253.3.2:39450`; the client
connected over the direct fiber, the worker accepted the callback, and both
GameSession readiness reports passed (`ready=1`, `expected=1`). The client
reported `clean_remote_shutdown=1` and a clean GNS close.

The same-run client capture contains **98 outbound / 43 inbound** qualified UDP
packets; the worker miniport export contains **86 outbound / 147 inbound** on
the exact two-peer tuple. Worker ETL and pcap each contain 233 packets, with
zero Packet Monitor drops and no lost events. The client pcap SHA-256 is
`DE2A6BF56E57DCA0C92DD9ED58BD8BE6609AE5BCCB7162F25AF537B39DB1A3AF`;
the worker pcap SHA-256 is
`68B104348C58EEF117B78D5862C0E9CA672ECD7FA8F09F16FE79A3BB23B7FC29`,
and its ETL SHA-256 is
`10D831A5DE402005A4A3E44A67F42963F431448CCD17395062C6B64C72EE2781`.
Both endpoint results and the coordinator result are successful, the delayed
`FINALIZE` was reconciled, `RUN_DONE Success=true` reached both endpoints,
and the lifecycle host returned PASS with both agents `IDLE`.
**ONE-CLIENT PHYSICAL READINESS — PASS.** This qualifies only the one-client
physical prerequisite; it does not establish four-client simultaneous
readiness or complete the older Phase 1 funding matrix.

The retained lifecycle bundle is
`C:\Sandbox\Codex\Artifacts\gargantuan-3l-interactive-diagnostic\physical-lifecycle-3f8637c529cc489f`;
the client physical evidence is under
`C:\Sandbox\Codex\Evidence\physical-qualifier\lifecycle-3f8637c529cc489f`,
and the worker evidence is under
`C:\GargantuanQualification\physical-qualifier-service-evidence\lifecycle-3f8637c529cc489f`.
The one-run daemons, tasks, tunnel, configs, secrets and temporary profiles
were removed. Both UDP 39450 listeners are gone. Packet Monitor is stopped
with no filters, and the installed capture service is running idle on the
verified hook pin. **KI-006 remains OPEN; Foundation 3L remains B — PARTIALLY
READY.** The next gated task is four-client simultaneous physical readiness,
not Phase 1, 3M, final acceptance or merge.

## Live-GNS capture corrected; fresh one-client lifecycle stopped at agent startup (2026-09-28)

The preserved worker ETL from run `a1e20f02-cb11-425d-99b4-b30119261eff`
contains 62 raw payload events, all outbound on Mellanox miniport component 13
(`mlx5.sys`, edge 1); its pcap export contains all 62. No inbound payload was
present in that ETL. The flow counters included receive activity but did not
identify the GNS tuple and were not used as packet evidence. A bounded real-GNS
component sweep showed the same outbound-only result at miniport 13, WFP Native
Filter 30 and TCPIP 75, including with the UDP port-only filter and with all
components selected. WFP component 30 records two edge snapshots of a packet
and is unsuitable for direct aggregate counts.

The causal observation defect is Packet Monitor's `-t UDP` predicate on these
live-GNS receive frames: a filter on the two fixed fiber MACs and IPv4 exposed
150 inbound/68 outbound miniport frames, of which 150 inbound/64 outbound were
the GNS tuple. Adding `-t UDP` to that filter suppressed every inbound payload
again. Keeping the MACs, IPv4 and port 39450 but omitting `-t UDP` recorded 127
inbound/61 outbound qualified GNS packets on miniport 13, all 188 exported with
zero Packet Monitor drops. Packet Monitor's own metadata identified the received
frames as UDP. The exact internal reason its UDP filter rejects these live-GNS
receive frames is not established; the earlier five/five synthetic exchange
did not exercise this behavior. No NIC offload setting was changed. The worker
hook now uses the fixed peer-MAC/IPv4/port filter and still exports only the
miniport. The existing pcap validator requires both directions on the exact
two-peer UDP tuple, so TCP on that port cannot qualify readiness.

The installed, hash-pinned hook `2BC2E1430A29ACDE61DCCD35B943998FDB45D8E81E3E2A206D66158F334634D9`
then passed a separate capture-only real-GNS proof `e60416771cd34df0`:
miniport-13 ETL and pcap each contain 167 full-length payloads, 135 inbound
and 32 outbound on the qualified UDP tuple. All 167 packet group IDs are unique;
there were no lost events or reported drops. The ETL SHA-256 is
`CF98E89C6860ED27FAAC392FE5B69BA84DEA815CD3A0D962E93113EC6D543C57`;
the pcap SHA-256 is
`407103DA0FC4945ACBFF61078BF79EA535C32D4E1073857329A088C7B91EA4FC`.
The fixed GNS probe binary remained
`1E25676BDF1DA6ED2EA8A28AB183F519D18A4802BD00E7F6730CFA77D395BD5A`;
the qualifier source remained
`93B71C5C4FFE4AB1412EE3E38581465525654D5D06E4799EB494F2ED8257768C`.
The capture service's binary and fixed operations were unchanged, and its hook
pin matches the installed hook. A protected worker-local backup retains the
prior hook and service config.

After that proof, exactly one fresh interactive-profile physical run was
staged: `429c38d7-26bd-474d-b5d1-e97d13b0e8bd`, label
`49a4802f85d34aea`, lifecycle run
`575f286e-2c91-438c-9e4a-1f63a96d4cc5`. Both evidence and worker LAN
preflights passed; the fixed SSH forward/reverse handshakes passed before
assignment. Both lifecycle endpoints then changed from `PREPARING` to
`AGENT_FAILED` before either Codex agent reported a session ID or registered.
The coordinator expired its registration deadline and aborted. The immediate
CLI startup error was not retained by this lifecycle adapter, so its underlying
cause is not measured. Physical capture, GNS/GameSession readiness, client
clean close, packet counts and `FINALIZE`/`RUN_DONE` were not reached in this
fresh attempt. **ONE-CLIENT PHYSICAL READINESS — FAIL.** The failed run was not
retried.

The retained stage and lifecycle evidence is under
`C:\Sandbox\Codex\Artifacts\gargantuan-3l-interactive-diagnostic\physical-lifecycle-49a4802f85d34aea`;
the capture-only proof is under
`C:\GargantuanQualification\physical-qualifier-service-evidence\proof-e60416771cd34df0`
on the worker. Cleanup removed both one-run profiles and secrets, the task-owned
daemons/tasks/tunnel, and the staged physical configs; no probe or UDP 39450
listener remains. Packet Monitor is stopped with no filters, and the capture
service is running idle on the corrected pin. **KI-006 remains OPEN; Foundation
3L remains B — PARTIALLY READY.** The next task is to retain/diagnose the
Codex CLI startup failure on both endpoints, then perform one separately
authorized fresh one-client lifecycle attempt. Four-client readiness, Phase 1,
3M, final acceptance and merge remain gated on a one-client PASS.

## Interactive one-client GNS and Ready passed; worker ingress capture failed (2026-09-28)

Operator-authorized `PHYSICAL_QUALIFICATION_INTERACTIVE` used temporary named
Codex profiles on both PCs. A read-only Codex smoke verified the normal logged-in
non-admin SIDs (`aiden` ending `-1001`, `host` ending `-1001`) and
`danger-full-access`; the global and restricted Foundation 2B profiles were
unchanged. The worker's fixed socket broker and restricted evidence SID path
were bypassed. Both users passed fresh evidence write/read/rename/delete checks,
the limited worker user reached the main normal-LAN control listener, and the
fixed SSH forward and reverse completed authenticated pre-assignment handshakes.
The Mellanox adapters were Up at 10 GbE, MTU 1500, with direct
`10.253.3.1/30` and `10.253.3.2/30` routes. A separate bounded normal-user UDP
exchange over the fiber had passed before staging. The worker had an enabled
Public-profile UDP allow rule for the exact installed GNS executable. The
capture service was running and idle, Packet Monitor stopped with no filters,
and UDP 39450 unbound. The installed capture hook matched its service pin
`73A840FCA570F676B06D76457FF719301BBB4C93F9865A25B19EE1671EE63E3E`;
its earlier synthetic bidirectional qualification remains a separate claim.
No interactive UAC prompt was required. The already privileged worker SSH
management context registered only bounded, limited-user scheduled tasks;
Packet Monitor ran through the existing capture service.

The single fresh physical run was `a1e20f02-cb11-425d-99b4-b30119261eff`,
label `c60e576203944bf4`, with lifecycle run
`b7fc58d9-4295-4e5c-99ef-23b253747a9d`. Both agents registered and the
barrier started worker capture before its probe reported a live listener at
`10.253.3.2:39450`. The client used `10.253.3.1:51824` to connect; the worker
reported the incoming callback, accepted the connection, and both GNS sides
entered connected state. Both GameSession readiness reports passed (`ready=1`,
`expected=1`), and the client reported `clean_remote_shutdown=1` and its
canonical clean close. Neither result is a claim of one-client physical PASS.

The client Ethernet capture counted **125 outbound / 51 inbound** qualified UDP
packets on that exact tuple. The worker Packet Monitor miniport export counted
**62 outbound / 0 inbound**; ETL and pcap each contained 62 packets, with zero
drop count and no lost events. The worker's own GNS callback and accepted
connection, plus the client's outbound capture, show ingress reached the
application, but the selected Mellanox miniport capture did not observe that
direction. The underlying observation-layer cause is not yet attributed. The
worker result failed on the preserved bidirectional capture gate; `SERVER_DONE`
was unsuccessful, the delayed `FINALIZE` was legal, and `RUN_DONE Success=false`
left both lifecycle agents FAILED. The client result and local protocol state
were successful, but coordinator and lifecycle host success were not.
**ONE-CLIENT PHYSICAL READINESS — FAIL.** This run was not retried and four-client
readiness was not attempted.

The retained local bundle is
`C:\Sandbox\Codex\Artifacts\gargantuan-3l-interactive-diagnostic\physical-lifecycle-c60e576203944bf4`,
with copied worker ETL/pcap/evidence and both manifest hashes verified against
their files. The client capture is under
`C:\Sandbox\Codex\Evidence\physical-qualifier\lifecycle-c60e576203944bf4\client-evidence`.
The worker pcap SHA-256 is
`B9DF7C840783C0CBAEF006C13326E4434B18CD6D37C5738D639C3726147FACEF`;
the client pcap SHA-256 is
`F0BFC1417D7FDC95B7139320C1FC6D50C8CA9BF2DFDB10FF3B74AE637F1A4CB8`.
Owned agents, daemons, tasks, tunnel, probes, UDP listener, tickets, temporary
profiles and one-run secrets are gone. Packet Monitor is stopped with no
filters; the pinned capture service remains running and idle. Baseline endpoint
workflow and policy files were restored. The exact next task is bounded
attribution and correction of the worker's live GNS ingress observation path,
then a separately authorized fresh one-client attempt. Foundation 2B's
restricted-profile qualification remains valid and distinct: this physical run
proves application connectivity under the interactive profile only. KI-006
remains **OPEN**; Foundation 3L remains **B — PARTIALLY READY**. No Phase 1
strengthening, 3M or merge followed.

## Both lifecycle tunnel directions passed; physical GNS connection timed out (2026-09-28)

Fresh physical run `116805d6-7777-4422-9c23-d11644358297` used label
`fde5f3dc62bd4649` and one lifecycle run
`ae30e49e-4a24-46e1-a77c-4d4d3b7de82b`. Both evidence roots passed under
their real non-admin Codex sandbox SIDs before daemon start. The restricted
worker's fixed broker request connected and closed the pinned LAN control
socket. Worker capture service, Packet Monitor, filters and UDP 39450 were
idle before wake. The main host then launched one SSH process (PID 35532)
with fixed `-L 127.0.0.1:49964:127.0.0.1:49963` to the worker lifecycle
daemon and `-R 127.0.0.1:49961:127.0.0.1:49961` back to the main coordinator.
The main host authenticated the worker daemon as `SERVER/OFFLINE` through the
forward. The restricted worker SID ending `-1006` completed a fresh, two-way
nonce exchange through the reverse and the worker listener was owned by
`sshd` PID 12508. All five hard prerequisites passed before the lifecycle
assignment existed. The prior run had launched only `-L`; its worker's
`WinError 10061` was the absent `-R` listener, not the earlier sandbox LAN
socket denial.

Both agents woke automatically, pulled the fresh assignment, registered and
reached the barrier. The client returned LIVE; the physical worker capture
started and its probe reported GNS `event=listening` at
`10.253.3.2:39450` before the client probe launched. The client then timed
out attempting to connect (`end_reason=5003`); no server incoming callback,
accept or connected state was observed. Both pcap parsers counted **outbound 0,
inbound 0** qualifying UDP packets. Packet Monitor reported zero total packets,
zero drops and no lost events. The client never reached GameSession Ready or
canonical clean close. The worker reported a failed `SERVER_DONE`; the legacy
coordinator sent legal FINALIZE and `RUN_DONE Success=false`, then the lifecycle
host aborted on the worker's failed result. Both agents ended FAILED; no
successful COMPLETE occurred. The first failing physical gate is the GNS
client-to-worker connection. The exact network/filter owner is **not
attributed**: a subsequent bounded ICMP check sent from `10.253.3.1` to
`10.253.3.2` lost 2/2, while the reverse direction received 2/2, but ICMP
policy alone does not prove the UDP drop location.

This single physical attempt was **not retried**. The owned SSH process exited
and both forwards cleared. Task-owned daemons, worker broker/task, physical
probe/helper, UDP listener and lifecycle tickets are gone; staged endpoint
policies/workflows were restored. Packet Monitor is stopped with no filters,
the capture service is running and idle, and UDP 39450 is unbound. The
retained local evidence is
`C:\Sandbox\Codex\Artifacts\gargantuan-3l-capture-diagnostic\physical-lifecycle-fde5f3dc62bd4649`;
worker capture evidence was copied there for reconciliation. **ONE-CLIENT
PHYSICAL READINESS — FAIL.** KI-006 remains **OPEN** and Foundation 3L remains
**B — PARTIALLY READY**. The next task is bounded attribution and correction
of the client-to-worker fiber GNS path, followed only then by a separately
authorized fresh one-client lifecycle attempt. Four-client readiness, Phase 1
continuation and 3M remain gated.

## Worker control socket correction passed preflight; lifecycle stopped before the barrier (2026-09-27)

The prior worker `WinError 10013` is attributable to the legacy endpoint's
`socket.connect(("192.168.0.68", 39451))`, after its IPv4/TCP socket creation,
`bind(("192.168.0.108", 0))` and three-second timeout all succeeded. The
actual worker runtime is
`C:\Sandbox\Codex\Tools\physical-qualifier\runtime\python.exe` under the
medium-integrity, non-admin `HOSTPC\CodexSandboxOffline` SID ending `-1006`.
A bounded reproduction captured the stack and `WinError 10013` at `connect`;
the same executable, working directory and target connected successfully from
the worker's limited interactive `HOSTPC\host` session. The worker's installed
`foundation-2-endpoint` profile has `network_access = false`, and the enabled
`codex_sandbox_offline_block_outbound` firewall rule blocks non-loopback
outbound traffic for that exact sandbox SID, independent of executable and
port. TCP 39451 and the assigned ephemeral port were not excluded or already
owned; the intended LAN address was assigned. No firewall or sandbox policy
was relaxed.

The project adapter now uses a one-run, hash-pinned local broker in the
existing limited worker host session. Restricted Codex can request only the
preflight and exact staged worker endpoint; the broker owns the LAN control
socket and later endpoint process. Arbitrary command, endpoint, port and
config changes are rejected. This changes qualification process ownership,
not GNS, GameSession, capture, POOLED_SERVICE or the generic coordinator.

Fresh physical run `722dfaa2-6ac6-4ae4-a67b-c226944c2528` passed the
client and worker restricted evidence preflights and the worker socket
preflight before wake. The non-admin worker sandbox wrote the fixed request;
the limited broker connected and closed
`192.168.0.108:51205 -> 192.168.0.68:39451`, with proof returned to and
verified by the sandbox. The capture service was idle, Packet Monitor stopped
with no filters, and UDP 39450 unbound. The retained preflight and diagnostic
evidence is under
`C:\Sandbox\Codex\Artifacts\gargantuan-3l-capture-diagnostic\physical-lifecycle-7c387c3bd55c4b9e`.

The one authorized lifecycle run `0cf08bfe-62f7-423e-a4b5-33636e0f2a1b`
then woke both agents. The client pulled a fresh assignment and registered;
the worker did not. Its lifecycle journal records `WinError 10061` connecting
to its loopback coordinator address. The run's SSH tunnel had forwarded the
client to the worker daemon but omitted the reverse worker
`127.0.0.1:49961` listener. The host aborted on registration deadline; both
agents ended FAILED. No physical endpoint, capture or GNS process started;
packet directions, GameSession Ready, canonical close, FINALIZE and COMPLETE
are **not measured**. The run was not retried. A pre-wake reverse-tunnel check
now rejects this omission before any agent starts. After the failed run, the
broker's eventual endpoint launch was further confined to a protected copy of
the pinned package; this last source-copy hardening has mock coverage but no
second physical execution. Daemons, tunnel, broker
task and staged policies were cleaned up; no UDP listener, Packet Monitor
session/filter or active capture-service run remains. The qualified hook and
its pin were unchanged. **ONE-CLIENT PHYSICAL READINESS — FAIL.** KI-006 stays
OPEN and Foundation 3L remains **B — PARTIALLY READY**. The next task is one
new one-client attempt with both fixed loopback tunnel directions and all three
preflights; four-client readiness, Phase 1 and 3M remain out of scope until it
passes.

## One-client dual-evidence preflight passed; worker socket denied (2026-09-27)

Physical run `c9130ef2-24a9-4d9f-a76f-96f56d5ae629` used lifecycle run
`ea43f42e-8d02-4068-b68c-29e3527ccad1`. Before any lifecycle daemon or
agent wake, the actual non-admin client and worker `CodexSandboxOffline`
identities each passed a fresh create/write/flush/read/rename/delete preflight
under their respective qualification evidence roots. The worker's distinct SID
ends `-1006`; the client's ends `-1004`. Worker ACL provisioning preserved the
Administrators-owned root, SYSTEM and Administrators full access, and
`HOSTPC\host` modify access required by the installed capture service, adding
only the exact worker sandbox SID with modify access. The worker preflight
deleted its run directory afterward, and the physical endpoint created it
normally. No UAC was required and neither Codex agent ran elevated.

Both agents woke automatically, pulled fresh assignments and registered. The
client returned LIVE. The worker wrote its provenance and result under the
newly writable root, then failed with `[WinError 10013] An attempt was made to
access a socket in a way forbidden by its access permissions` before sending
`STAGE_READY`. The source attempts a worker LAN bind and coordinator connect at
this point; the result does not distinguish which socket call was denied. The
host aborted on the worker's failed result. There was no capture, GNS listener,
GameSession Ready, canonical close, FINALIZE or COMPLETE. Client and worker
packet counts are **not measured**. The physical run was not retried.

**ONE-CLIENT PHYSICAL READINESS — FAIL.** The client and worker endpoint agents
ended FAILED; temporary daemons, tunnel and worker task were removed and
endpoint policy restored. No task probe or listener remained. Worker Packet
Monitor was stopped with no filters and its qualified service was idle; its
hook and hash pin were unchanged. The retained local run record is under
`C:\Sandbox\Codex\Artifacts\gargantuan-3l-capture-diagnostic\physical-lifecycle-44376ade81d84182`;
the worker `result.json` SHA-256 is
`0DB95099775F63AA74AAD3A4E43158E8D552A9E347CC506FA7418290866E0F0B`.
The next task is a bounded diagnosis of the worker sandbox's denied socket
operation and its applicable network policy before a newly authorized
one-client attempt. No broad network permission change follows from this
result. Four-client readiness, Phase 1 and 3M were not started. KI-006 OPEN;
Foundation 3L B — PARTIALLY READY.

## One-client lifecycle attempt stopped at worker evidence access (2026-09-27)

The fresh physical run `ff95b60e-8c5e-4125-bbc9-73e1e6844231` used Agent
Coordinator lifecycle run `960369f2-08a5-40d5-9253-9cfe86786220`. The client
evidence root was repaired with a fixed, task-owned ACL: SYSTEM,
Administrators and the owner have full access; only the exact installed
`CodexSandboxOffline` SID has modify access. The restricted, non-admin client
created, flushed, read, renamed and deleted fresh evidence before wake. Both
agents then pulled fresh assignments and registered. The client first capability
returned LIVE. The worker's first capability failed with `WinError 5` while
creating `C:\GargantuanQualification\physical-qualifier-service-evidence\lifecycle-407f77f9b2c84b35`.
The coordinator aborted coherently. The client reported transport ABORT after
the worker failure; that is a consequence, not a second root cause.

**ONE-CLIENT PHYSICAL READINESS — FAIL.** Client/worker packet counts,
GNS connection, GameSession Ready and canonical close are **not measured**.
No probe or capture started. The qualified worker capture hook and pin were not
changed. The run was not retried. Temporary lifecycle daemons, SSH tunnel and
worker scheduled task were removed; endpoint policy was restored; worker Packet
Monitor was stopped with no filters, and its capture service was idle. Retained
local evidence is under
`C:\Sandbox\Codex\Artifacts\gargantuan-3l-capture-diagnostic\physical-lifecycle-407f77f9b2c84b35`.
The next task must preflight and repair only the worker's separate evidence-root
access under its actual restricted identity, then request a new one-client
attempt. Four-client readiness is not authorized by this result. KI-006 remains
OPEN; Foundation 3L remains B — PARTIALLY READY; no 3M.

## Bounded real-GNS Phase 1 stop (2026-09-24)

**STOP / INCOMPLETE FOUR-GRANT PROOF.** The [accepted funding review](PhysicalFundingGateReview3L.md)
permitted actual GameSession/GNS testing without a zero-loss synthetic UDP or
further NDIS prerequisite. The unchanged `POOLED_SERVICE` profile was exercised
with four actual GameSession clients on the dedicated static fiber. The
evidence-complete attempt connected all four clients and applied eight repeated
512-KiB structural waves, but the predeclared analyzer found qualified
simultaneous four-grant overlap in only waves **1, 4 and 6**: respectively
16,607, 16,920 and 18,433 microseconds. The other five waves had no qualifying
common interval (wave 7 qualified only two individual peers). The executable
exited 1. There was **no observed below-floor interval** and no demonstrated
transport-budget failure; the short burst workload did not supply enough
qualified interval evidence to declare Phase 1 healthy. Do not promote three
successful windows to sustained 64-MiB/s service or Phase 1 PASS.

### Source and physical configuration

The base is published `4d27238553fdc27b9f6772b438c49393eac3a8eb` plus a
five-file uncommitted qualification-only source overlay, not a clean-HEAD
binary. Base archive SHA-256 is `252f67d5e76b18f026c1239e1f959e37e973a728dae4f9c2c5bd235fe8d53a18`;
overlay archive SHA-256 is `9228e18366227c0636a92f309179ab1749e5523f068776219650ccfb3e3790e4`.
The overlay adds the manual probe, private bounded diagnostics and CMake target;
the accepted profile values and production admission behavior are unchanged.
Worker build: MSVC 19.51.36260.0 x64 Release, Ninja, CMake 3.31.6-msvc6,
four compile jobs, GNS enabled, precompiled headers disabled to avoid a worker
path-map/PCH compiler error. Declared pinned GNS revision is
`2cb93a06350bb065db53abdb0d87cf297e0bfd34`; the 278-file prepared GNS
tree has SHA-256 manifest `d0e3de240f1a2d008f6e8458379745e62ae22f3cfb7559a1e10b2f1db514efb6`.
Both endpoints ran the same newly built probe executable, SHA-256
`dee724ebda36f869ec66c8807f6108ad7d86f582551549a15d3ea0637d9f88ea`.
The copied client package includes exact DLL/runtime hashes in the archive.
Both worker and local no-socket analyzer self-tests passed before traffic.

The directly connected Mellanox ports were **already** preferred static
`10.253.3.1/30` (workstation Ethernet 3, if50) and `10.253.3.2/30` (worker
Ethernet 4, if19) when this task began. Both negotiated 10 GbE with MTU 1500,
driver 2.53.23539.0; the on-link `/30` routes select the fiber, and each side's
neighbor cache identifies the opposite Mellanox MAC. A scoped single-datagram
reachability check arrived on the worker from `10.253.3.1`. Phase 1 bound the
server to `10.253.3.2:39450` and connected clients to that address. A temporary
worker Public-profile UDP rule was limited to local `10.253.3.2:39450` and
remote `10.253.3.1`; it was removed after each attempt. No Windows security
policy was disabled. No address, route, NIC binding, driver or profile setting
was changed in this task. Static addressing remains a **candidate deployment
requirement**, retained because it was pre-existing; persistence was not tested
since qualification did not pass.

### Measurements and stop reason

The first launch reached its client-ready timeout before demand, then cleanup
cleared all state. A scoped UDP check verified reachability. A prompt-launch
repeat applied all eight waves but the PowerShell wrapper truncated its server
output on nonzero exit. The same bounded workload was repeated once with
separate stdout/stderr and complete CSV preservation; results below are from
that evidence-complete attempt. The earlier failures remain in the archive.

| Property | Completed Phase 1 measurement |
| --- | ---: |
| Actual GameSession clients / applied structural waves | 4 / 8 each |
| Maximum simultaneous drain grants / debt | 4 / 2,097,152 B |
| Structural retirement per wave | 2,097,152 B |
| Accepted / exact attributed retired / terminal released | 16,802,660 / 16,802,660 / 0 B |
| Unique stream first sent / ACKed, sampled cumulative deltas | 16,833,199 / 16,833,199 B |
| Reliable stream retransmitted delta | 0 B |
| Maximum pending / sent-unacked per peer | 361,701 / 303,715 B |
| Maximum GNS queue time | 19.252 ms |
| Configured/effective per-grant GNS send rate | 18,874,368 B/s |
| Maximum sampled GNS wire outbound per peer | 337,388 B/s |
| Worker NIC sent / received during wrapper | 18,677,150 / 564,060 B |
| Worker NIC receive errors / discards delta | 0 / 0 |
| Server process peak RSS / maximum host CPU sample | 27,099,136 B / 16% |
| Largest client process RSS / maximum local host CPU sample | 22,233,088 B / 48% |

Each of the four peers ended with first-sent and ACKed deltas equal, and all
eight 2-MiB aggregate waves retired exactly. The feedback trace has 3,086 rows,
zero invalid, overflow or below-floor classifications, and an ACK-positive
qualified interval for all four peers in each of the three overlapping waves.
The peak sampled RTT is 1 ms. Pending/unacked bytes and debt drained to zero at
cleanup. Retransmission is physical cost, not logical drain; its observed zero
does not convert the short offered workload into a 64-MiB/s physical proof.
No path-loss consequence was demonstrated by this attempt.

The gameplay producer recorded 130 RPCs and 129 Event ACKs. RPC p95/p99/max
were 53.369/76.537/77.006 ms; Event ACK max 77.008 ms, within the existing
150/250/500-ms RPC and 250-ms Event targets during the measured work. Every
client reported eight applied waves and movement; the server terminated the
session after its incomplete-overlap verdict, so all clients exited nonzero.
Action result, due-to-observation Character/root latency and per-core CPU are
**not measured** in this Phase 1 harness. Fixed 20-second service recovery and
workload-derived complete structural convergence were not independently
qualified. The client generator process count peaked at four; worker process
count peaked at one. Host CPU/memory and NIC samples are in the archive; link
rate alone was not used as headroom proof.

All four client processes exited. The worker rule, UDP listener and probe process
are gone. Session cleanup reports empty connections/journal requirements,
zero outstanding debt/grants, and exact accepted=retired+terminal conservation;
client cleanup also reports `good=1`. A full canonical cache/resident lifecycle
gate remains **not measured** because Phase 2 did not run. The pre-existing
static address plan remains preferred; no diagnostic NIC tuning is retained.

**Phase 2 Local: not run. Phase 2 Node: not run. KI-006: OPEN. Foundation 3L:
B — PARTIALLY READY.** No final acceptance sweep is eligible and no 3M work
began. The next permitted physical task is to make the bounded actual-client
probe produce sustained/overlapping qualified backlog windows for all four
grants, with complete host measurements, without changing the accepted profile.
After a true Phase 1 PASS, build and run the exact 32-actual-client canonical
Local and Node matrix. A synthetic zero-loss rerun is not the next gate.

Local untracked evidence archive: `build/physical-gns-evidence-20260924.zip`,
102 files plus manifest,
6,017,209 B, SHA-256
`c567c7d8d5d23e4d36c072862b4b409f5937f20561469dd933e89690318911f1`.
Every entry was decompressed and SHA-256 checked. The same archive is preserved
on the worker under
`C:\Sandbox\Codex\Artifacts\gargantuan-3l-gns-physical-20260924` with
a matching download hash. It contains source/base and overlay snapshots,
compiler script, exact binaries, raw per-peer GNS feedback/host CSV and JSON,
failed-attempt logs, analyzer, and cleanup receipts.

## Current funding-gate clarification (2026-09-23)

The accepted [funding review](PhysicalFundingGateReview3L.md) supersedes the
historical prerequisite below: **raw capacity is established; proceed to bounded
production-GNS qualification with static dedicated-link addressing**. Synthetic
zero-loss UDP and additional Windows/vendor attribution are not prerequisites.
All pooled service, latency, freshness, fairness/debt and physical actual-client
gates remain unchanged. Nonzero retransmission is evaluated as physical cost,
never counted as unique drain and not an automatic failure.

KI-006 remains OPEN; no actual-client physical PASS or Foundation closure is
implied. Earlier diagnostic measurements and failures below remain historical.

## Historical installed direct fiber update (2026-09-23)

The approved [receive-handoff retry](FiberPhysicalPreflight3L.md) completed
expanded NBL/NDIS/TCPIP/WFP tracing. A 900.018899-Mbps generated /
899.799008-Mbps received trial loses 167 packets after the last observed
filter edge and before TCP/IP. Native drops and metadata do not identify an
owner or correction. Two clean short controls are insufficient repeatability.
The instrumentation stop applies: funding remains unqualified, KI-006 OPEN,
and native indication/return ownership plus queue telemetry is the next task.

The [new 10 GbE fiber preflight](FiberPhysicalPreflight3L.md) supersedes this
receipt's deferred-cable status. The operator confirms a direct Mellanox-to-
Mellanox cable. Explicitly bound TCP delivers 9.246–9.471 Gbit/s. Earlier
UDP failures and clean samples remain historical evidence. WFP now identifies
WSH Default Inbound Block filter 147332 on a local/raw IPv4 receive/accept path.
Matched worker DHCP/static/DHCP profiling gives 140.840/900.001/145.683 Mbit/s;
NETIO sampled share changes 64.646%/2.745%/66.549% and FindCacheMatch
44.356%/0%/46.308%. AFD lifecycle tracing directly identifies DHCP's raw UDP
endpoint. No WSH policy change is needed for the static diagnostic condition.
Elevated receiver traces independently locate 2,244 missing sequences after
the last filter upper edge and before TCP/IP capture; TCP/IP's sequence set
exactly equals application delivery. The receiver's largest gap is blocked
waiting, with only 6 microseconds runnable before scheduling. Npcap unbinding
does not eliminate loss and is reverted. The hidden handoff queue/drop reason
remains unobserved, so the requested stop applies. **Physical funding remains
unqualified** at the unchanged
805.306368-Mbit/s envelope; the final repeated funding matrix was not run.
The larger send buffer remains provisional, not a deployment requirement.
DHCP/address state is restored, NIC settings are unchanged, and task rules,
traces and processes are absent. The receipt records the pre-existing Public
worker profile's inbound management limitation. Next: identify receive-handoff
queue/indication ownership and drop reason, establish a supported correction,
then stable bidirectional funding with an accepted persistent address plan.
No actual clients, Local/Node application
runs or final acceptance sweep occurred. KI-006 remains OPEN and
Foundation 3L B — PARTIALLY READY; no merge or 3M. The September 15 LAN evidence
below remains historical within its original scope.

## Historical verdict and source (2026-09-15)

**INCONCLUSIVE / NOT PHYSICALLY QUALIFIED.** Independent TCP and UDP evidence is
now available in both directions. Clean trials exceed the accepted envelope,
but reverse UDP loss, pacing variation and a ctsTraffic playback-window failure
remain unattributed on the shared Windows hosts. The conservative preflight
gate is **not passed**. This is a qualification-tool / host-timing attribution
stop, not proof of a hard 1-GbE capacity ceiling or an Engine defect.

The operator corrected the topology: **no direct cable is installed**. The
planned **25 Gb fiber cable is deferred until delivery in a few days**. Current
LAN measurements do not qualify that future path, its NICs or negotiated speed.
Intermediate network hardware models remain unidentified.

No actual Gargantuan server or clients were launched; Local and Node physical
qualification are **not measured**. KI-006 remains **OPEN**, Foundation 3L
**B — PARTIALLY READY / NOT READY FOR FINAL ACCEPTANCE**, and 3M **BLOCKED / NOT
STARTED**. No merge was performed.

Production source remains `eec0c7123a39698761e49ed276ee37aae673e023`.
This documentation-only resumption starts from published evidence
`669949b0dac722dd896e9dcf36ebaeb677c6bd96` on
`foundation/3l-content-availability`, primary repository `gmoddev/gargantuan`.
An isolated detached worktree preserves the user's dirty checkout. No production,
test, profile, rates, limits, wire, ordering or recovery semantics changed.
No active Foundation 3L checkpoint exists at this source.

The [morning attempt receipt](https://github.com/gmoddev/gargantuan/blob/669949b0dac722dd896e9dcf36ebaeb677c6bd96/devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md)
retains its original measurements and local administrator-token blocker.
Client-initiated ctsTraffic pull connections allowed independent reverse testing
without adding a local inbound firewall rule. Local administrator access was
not acquired and was not necessary for that method.

## Profile, topology and placement

The accepted [Option C profile](PooledReliableServiceProof3L.md#selected-32-peer-candidate)
requires **96 MiB/s = 100,663,296 B/s = 805.306368 Mbit/s** of usable envelope.
Fixed commitments remain 84 MiB/s: structural 64, gameplay 8,
control/realtime 4 and transport reserve 8; another 12 MiB/s is inside the
envelope. Four concurrent grants and the 16-MiB/s active-grant peer drain floor
are unchanged. The extra 104-MiB/s UDP probes test physical headroom only;
they do not change Gargantuan's profile or create a new acceptance threshold.

| Resource | Workstation / intended client generator | Dockerbox / intended server |
| --- | --- | --- |
| Host | DESKTOP-B8V8NAN | HOSTPC |
| CPU | Ryzen 9 7950X3D, 16 cores / 32 logical | Ryzen 9 5900X, 12 cores / 24 logical |
| OS | Windows 11 Pro, build 26200 | Windows 11 Pro, build 26200 |
| OS-visible RAM bytes | 33,157,488,640 | 34,281,426,944 |
| Free RAM KiB at inventory | 9,748,956 | 17,111,844 |
| Inventory UTC, 2026-09-15 | 20:56:29 | 21:19:12 |
| NIC | Realtek Gaming 2.5GbE Family Controller | Realtek PCIe GbE Family Controller |
| Negotiated speed | 2.5 Gbit/s | 1 Gbit/s |
| Driver version / date | 10.54.1111.2021 / 2021-11-11 | 10.79.50.1003 / 2025-10-03 |
| IPv4 / interface | 192.168.0.68/24 / Ethernet | 192.168.0.108/24 / Ethernet 2 |
| MTU | 1500 | 1500 |

The path traverses the existing LAN, not a direct cable. Exact intermediate
switch/router and cable models are **not established**. Both routes are on-link
`192.168.0.0/24`; workstation traffic is explicitly bound to `192.168.0.68`.
Its active Wi-Fi and VPN are not the selected peer route. Worker Wi-Fi is
disconnected; WSL virtual interfaces are outside this path. No available
high-speed physical NIC was established. No driver, NIC, MTU or route was tuned.

Hosts are shared, not reserved. Existing worker containers `drycreek-bot` and
`directus-db` remained running. Benchmark placement was workstation sender /
worker receiver for forward traffic, worker sender / workstation receiver for
reverse traffic. Trials were sequential, without overlapping evidence transfers.
TCP used four connections in one tool process per host. Forward UDP used one
process per host; reverse UDP used one worker server and either one or four
workstation receivers. Four receiver wrappers also sampled host CPU, so
measurement overhead is part of this environment and is not isolated.

## Independent TCP capacity

[Microsoft ctsTraffic](https://github.com/microsoft/ctsTraffic) 2.0.3.9 x64 was
obtained from its official release directory. Both executable copies match
SHA-256 `0548089E59C872306CE2C98E7163E2A717119756010CF64D3CB3DA2854F632CF`;
Authenticode reports **NotSigned**. This is distinct from signed NTTTCP below.

Each trial transfers 536,870,912 application bytes per connection over four
connections, 2,147,483,648 bytes total, with `-Verify:data`.
Forward uses `-Pattern:push`; reverse uses `-Pattern:pull` on independently
established client connections. Common settings are TCP port 55301,
`-Connections:4 -Iterations:1` on the client and `-ServerExitLimit:4` on the
worker. All six trials record four successes, zero network/protocol errors,
exit zero and no wrapper timeout.

Received throughput below divides verified application bytes by the interval
from the receiver's first connection-established timestamp to its last
connection-success timestamp. It excludes worker listener startup wait and
tool control bytes. Timestamps have millisecond resolution. CPU is the mean
of sampled host aggregate CPU percentages over each wrapper's lifetime,
including startup; it is not dedicated Engine CPU.

| Trial | Received Mbit/s | Workstation / worker mean CPU % |
| --- | ---: | --- |
| tcp-forward-1 | 944.520 | 16.43 / 7.75 |
| tcp-forward-2 | 939.047 | 14.86 / 8.53 |
| tcp-forward-3 | 947.019 | 16.86 / 8.56 |
| tcp-reverse-1 | 941.982 | 11.60 / 8.09 |
| tcp-reverse-2 | 941.569 | 13.00 / 8.31 |
| tcp-reverse-3 | 941.466 | 20.43 / 7.94 |

TCP alone funds the throughput comparison in every trial. Across these samples,
maximum observed benchmark RSS is 13,455,360 bytes workstation and 12,353,536
bytes worker. Maximum sampled core utilization is 93% and 82%, respectively.
These are tool measurements, not a 32-client resource envelope.

## UDP packet accounting, throughput and jitter

Forward uses Microsoft-signed [NTTTCP 5.40](https://github.com/microsoft/ntttcp/releases/tag/v5.40),
verified SHA-256
`F66561D09AF91305412FD60CA4B28D57C7B650035D3C1EDCC00A57B079E2247E`.
One sender/receiver connection uses 1,400-byte datagrams, 15 seconds measurement,
two seconds warmup, one second cooldown, base port 55201 and sequence/QPC
logging with `-jm`. Sender `-thr 98304` or `106496` is KiB/s for the single
thread; NTTTCP pacing truncates to integer bytes/ms. Loss is counted from
unique sequence numbers in the observed interior after warmup, not inferred
from mismatched sender/receiver byte totals. Packets beyond the first/last
observed sequence are excluded; these are exact **bounded-interval** loss counts.

Reverse uses ctsTraffic UDP at nominal 96 or 104 MiB/s, 15 seconds configured
stream length, `-DatagramByteSize:1400`, default one-second playback buffer,
port 55201, and per-receiver jitter CSV. Aggregate frame rate is 72,000 or
78,000/s, divided equally across four flows when applicable. Integer frame
sizing produces **1,398 bytes per frame, one UDP datagram per frame**, including
the tool header. Expected sequence counts are 1,080,000 or 1,170,000.
Zero-error frame runs allow missing-sequence packet accounting.

For both tools, received payload rate uses observed receive timestamps, not
nominal duration. Four-flow rates use the common earliest-to-latest receiver
timestamp span, not the sum of separate per-flow rates. This includes flow
startup skew. Sender pacing sometimes exceeded the configured 15-second span.

| Direction / trial | Nominal MiB/s | Expected / received unique packets | Lost / loss % | Received payload Mbit/s |
| --- | ---: | --- | --- | ---: |
| Forward udp-forward-seq-1 | 96 | 1,078,564 / 1,078,429 | 135 / 0.012517% | 805.220 |
| Forward udp-forward-headroom-1 | 104 | 1,168,287 / 1,168,017 | 270 / 0.023111% | 872.311 |
| Reverse udp-reverse-2, one flow | 96 | 1,080,000 / 1,074,989 | 5,011 / 0.463981% | 778.113 |
| Reverse udp-reverse-group-1, four flows | 96 | 1,080,000 / 1,069,075 | 10,925 / 1.011574% | 795.020 |
| Reverse udp-reverse-headroom-1, four flows | 104 | exact unique receive count not recoverable | exact packet loss not measured | not attributable as packet goodput |
| Reverse udp-reverse-headroom-2, four flows | 104 | 1,170,000 / 1,170,000 | 0 / 0% | 859.196 |
| Reverse udp-reverse-headroom-3, four flows | 104 | 1,170,000 / 1,170,000 | 0 / 0% | 852.324 |

The first headroom trial received 1,617,364,374 bytes, equivalent to 1,156,913
datagram-sized receives, but recorded only 1,060,648 completed frames,
109,352 missing completed frames and 96,265 error frames. Error frames include
received packets outside the playback sequence window. **109,352 is not a
network packet-loss count.** The byte-counter deficit of 13,087 is not an exact
unique-loss count either, because those error frames lack complete identity
accounting. Completed-frame goodput was 743.835 Mbit/s; that metric cannot
stand in for raw datagram capacity. The retained analyzer returns null packet
loss for error-frame trials rather than silently treating missed playback as loss.

Jitter below is absolute change in relative transit time,
`abs(delta receive time - delta send time)`, and its RFC-3550-style 1/16 EWMA.
It needs no cross-host clock offset synchronization. NTTTCP CSV is in receive
order; ctsTraffic CSV is completed-sequence order, so the latter is explicitly
a **sequence-adjacent** statistic, not a proven arrival-order jitter measure.
Grouped values are the largest per-flow p99 / EWMA maximum, not pooled percentiles.

| Trial | Transit variation p99 ms | Maximum EWMA jitter ms |
| --- | ---: | ---: |
| udp-forward-seq-1 | 0.224 | 1.303 |
| udp-forward-headroom-1 | 0.226 | 1.221 |
| udp-reverse-2 | 0.548 | 1.799 |
| udp-reverse-group-1 | 0.599 | 2.356 |
| udp-reverse-headroom-1, completed frames only | 0.631 | 3.255 |
| udp-reverse-headroom-2 | 0.611 | 0.864 |
| udp-reverse-headroom-3 | 0.613 | 0.974 |

All retained measurable runs have zero logged duplicate frames/packets;
NTTTCP observed no reordered arrivals in the analyzed intervals. ctsTraffic
sequence ordering cannot independently establish absence of packet reordering.
An initial reverse UDP attempt on 55301 failed with Windows bind error 10013:
that port lies in an excluded UDP range. It is a setup failure, excluded from
capacity/loss evidence; the subsequent trials used available port 55201.

During the anomalous first reverse headroom trial, the worker had a sampled
core at 100%, while mean host CPU was 14.21%; clean repeats had maximum cores
47% / 56% and host means 10.23% / 9.29%. This is correlation, not attribution
to a process or proof of a server bottleneck. Workstation wrapper samples showed
mean host CPU 17.17–17.67% during that anomalous trial. Per-receiver RSS stayed
at or below 15,396,864 bytes across the four-flow probes; worker tool RSS was
at or below 11,591,680 bytes. Short sampling can miss scheduling stalls.
No driver trace, packet capture or isolated-host reproduction establishes
the cause. Clean repeats are retained alongside, not substituted for, failures.
Forward NTTTCP reports workstation / worker host CPU 16.461% / 8.445% at
nominal 96 MiB/s and 14.972% / 9.320% at 104 MiB/s. Forward UDP process RSS
was not sampled. None of these measurements qualifies application resource use.

The evidence therefore does not establish a conservative usable UDP minimum
under the canonical deployment contract. No arbitrary acceptable loss percentage
has been added. Neither a permanent physical capacity failure nor a qualified
1-GbE profile follows from these mixed results.

## Baseline ICMP and MTU

Fresh samples at 21:19 UTC use 100 sequential 1,200-byte echoes per direction,
500-ms timeout and 50-ms spacing. All succeeded. Percentiles are nearest rank;
integer-millisecond zero means below the API's resolution.

| Direction | Loss | Mean RTT ms | p50 / p95 / p99 / max ms | Mean / max absolute successive RTT delta ms |
| --- | --- | ---: | --- | --- |
| Workstation to worker | 0 / 100 | 0.11 | 0 / 0 / 1 / 10 | 0.222 / 10 |
| Worker to workstation | 0 / 100 | 0.04 | 0 / 0 / 1 / 2 | 0.081 / 2 |

DF probes at 1,472 payload bytes succeeded and 1,473 returned `PacketTooBig`
both ways, consistent with MTU 1500. These were baseline samples after throughput
testing. ICMP under saturation and one-way absolute latency are **not measured**.

## Actual-client and provider gate

| Required physical evidence | Local | Node |
| --- | --- | --- |
| Actual GameSession clients / server launched | 0 / 0 | 0 / 0 |
| Connection establishment and health of 32 clients | not measured | not measured |
| Server tick/service, CPU/RSS/network | not measured | not measured |
| Character/root cadence, latency and publication gaps | not measured | not measured |
| RPC RTT, handler/response queue, timeouts/errors | not measured | not measured |
| Event ACK/service and action latency/rejections | not measured | not measured |
| Structural load/evict/reload and convergence | not measured | not measured |
| Fixed 20-second service recovery | not measured | not measured |
| Exact retirement/debt, grants/journal, fairness/backlog | not measured | not measured |
| Client CPU/RSS, callback intervals and missed observations | not measured | not measured |
| Logical lifecycle/debt/readers/content cleanup | not measured | not measured |

The [canonical workload](RecipientServiceWorkload3L.md) remains eight moving/root
Characters with eight recipients each, **one** qualified gameplay producer,
and the shared 512-object / 273,032-byte provider unit through
baseline/load/resident/evict/reload and accepted overload/recovery cases.
Launching 32 independent producers would incorrectly multiply offered demand.

The existing [GameSession benchmark](../../tests/GameSessionBenchmark.cpp)
uses an actual content client at peer index zero and protocol observers for
other peers on SimulatedNetwork. [DueServiceFixture](../../tests/DueServiceFixture.hpp)
likewise has one actual gameplay client. They are not a ready physical client
farm. The task authorizes the smallest missing actual-client harness **after
preflight passes**; that condition has not been established, so no harness
implementation or application run was started.

The [recovery contract](PooledReliableServiceRecoveryContract3L.md) keeps
fixed 20-second service recovery separate from retained-work-derived structural
convergence. Unaffected Engine/loopback results retain their original scope;
the stricter 200-peer tick diagnostic remains separate performance debt.

## Cleanup, artifacts and validation

Four temporary inbound worker rules allowed only the relevant executable,
local `192.168.0.108` and remote `192.168.0.68`, on all profiles:

| Rule name | Executable | Protocol / configured ports |
| --- | --- | --- |
| Codex-Gargantuan-PhysicalResume-20260915-TCP | ctsTraffic 2.0.3.9 | TCP 55301 |
| Codex-Gargantuan-PhysicalResume-20260915-UDP | ctsTraffic 2.0.3.9 | UDP 55201, after excluded-port setup correction |
| Codex-Gargantuan-PhysicalResume-Ntttcp-20260915-TCP | NTTTCP 5.40 | TCP 55201,56201 |
| Codex-Gargantuan-PhysicalResume-Ntttcp-20260915-UDP | NTTTCP 5.40 | UDP 55201 |

Worker cleanup at **2026-09-15T21:20:09Z** removed those four rules and verified
none remained, with no ctsTraffic/NTTTCP processes. Client cleanup at
**21:20:10Z** also found no task rules or benchmark processes. No local rule was
added; the optional local receiver setup script was prepared but not executed.
Both protected worker containers remained up eight days. No global firewall
disable, interactive UAC prompt, persistent elevation, driver update or unrelated
service change occurred. Tool files and logs are retained intentionally.

The resumption archive `pooled-physical-resume-20260915.zip` contains **329**
manifest-verified files: inventories, sequence/timing CSV, paired tool receipts,
analysis scripts/results, topology clarification and cleanup. SHA-256:

`43573204bfb3a2986efe396d64948deb98bc673a21ddb3f5d144efd47808c1a0`

- Local evidence: `C:\Users\aiden\AppData\Local\Temp\gargantuan-3l-physical-20260915\build\physical-resume\`.
- Retained worker archive: `C:\Sandbox\Codex\Logs\gargantuan-3l-physical-resume-20260915\`.
- Executables: local `build/physical-resume/Tools/ctsTraffic.exe` and
  `build/physical/Tools/ntttcp.exe`; worker
  `C:\Sandbox\Codex\Tools\ctsTraffic-2.0.3.9\ctsTraffic.exe` and
  `C:\Sandbox\Codex\Tools\ntttcp-5.40\ntttcp.exe`.

The archive excludes third-party executable/source directories. It preserves
both successful and invalid/anomalous trials. Local Temp is not permanent
artifact hosting. The earlier morning archive and receipt remain separate.

Existing terminal-success [Native CI](https://github.com/gmoddev/gargantuan/actions/runs/34900780304)
and [GNS CI](https://github.com/gmoddev/gargantuan/actions/runs/34900779755) at exact
production source `eec0c7123` are reused. No native suites were rerun for this
documentation-only resumption. Validation passes 53 relative file targets,
six TCP and seven UDP receipt rows, both cleanup receipts, all 329 archived
files against their SHA-256 manifest, matching worker archive hash, and
`git diff --check`. These checks do not imply application qualification.

## Exact next task

When the planned fiber path is physically installed, identify both NICs,
drivers, negotiated speed, MTU, selected route and any intermediate hardware.
Do not assume a 25-Gb cable alone establishes a 25-Gbit/s path. Repeat the
bounded independent TCP and sequence-accounted UDP preflight, resolving pacing
and playback-window attribution before declaring a usable envelope. If work
resumes on the current LAN instead, those unresolved reverse UDP results remain
a prerequisite; a clean subset alone is not the retained verdict.

If preflight passes, continue in the same task with the smallest canonical
32-actual-GameSession-client harness and unchanged Local and Node workloads,
including every service, resource and cleanup metric above. Do not reduce the
profile to obtain a pass.

**Supported physical profile: none established. KI-006 OPEN. Foundation 3L
B — PARTIALLY READY; not ready for final acceptance. No merge; no 3M.**

## 2026-10-01 — 32-client qualification infrastructure in progress

The retained F1 physical Phase 1 PASS remains authoritative for its four-client
finite-grant scope. The Local and real-TLS Node 32-actual-client provider matrices
in the gate table above are still **not measured**. This section records the new
candidate infrastructure without treating parser or mock tests as physical proof.

The qualified-scale package builder creates one identical 512-object content
unit for a Server runtime and a Player runtime. A hosted Windows workflow
dispatch can package those runtime trees with complete per-file SHA-256 indexes
and one source-commit pin. The run-manifest creator verifies both complete
package trees, binds a canonical run UUID and 32 distinct run-scoped nonces to
the exact Server/Player binary, package, content and deployment hashes, and
writes byte-identical manifests for both physical endpoints. It carries the
Node address and trusted root certificate hash for Node runs, but no TLS token.

The role-local supervisor uses fixed executable names and fixed arguments. It
owns either one production Server process or 32 independent production Player
processes, each with its own GameSession and nonce. It bounds startup, aggregate
runtime, working set, threads, logs, evidence and owned-process cleanup. The
read-only preflight inventories CPU, RAM, disk, process baseline, direct-fiber
route/link/MTU, UDP listener ownership and Node certificate/token presence.
These controls are infrastructure checks, not provider acceptance.

The separate two-host coordinator workflow now uses fixed role-local operations:
launch Server, observe its ready marker, launch 32 Players, observe the producer
Player's typed ready record, poll both roles within bounded transitions, and
collect the two exact run results. The adapter pins its PowerShell executable,
supervisor, full run manifest and source commit, accepts no wire-supplied paths
or arguments, and kills only its owned supervisor tree on abort. Five local
tests include an actual pinned Host/Join lifecycle with PowerShell children.
These tests qualify the control path, not application or capture evidence.

The offline reconciler verifies both immutable evidence indexes and the same
pinned run manifest, joins all 32 server/client identities, checks the five
phase and content-observation records, one-producer typed gameplay results,
monotonic role-local resource samples, and the final native admission receipt.
The farm-only native receipt records exact accepted/attributed-retired bytes,
terminal release, outstanding debt, active/high-water grants, credit/fairness
deferrals, current pending enter/leave counts, materialization/journal backlog
and structural failures. Reconciliation deliberately reports **INCOMPLETE**
until the independent service, fairness, capture, real-TLS, recovery, resource
and cleanup gates have physical evidence.

The separate offline Local/Node acceptance-observation candidate compares two
already-reconciled provider runs. It checks source/workload/deployment pin parity,
reports bounded role-local process samples and indexed evidence sizes, and keeps
the final verdict `INCOMPLETE`. The current typed receipts do not establish
fixed 20-second service recovery, exact workload-derived structural convergence,
journal retention margin under overload, simultaneous resource/network headroom,
or full provider parity. Its socket-free mock tests do not constitute a provider
run. See `tests/PhysicalGameSessionFarmAcceptance.md` for input and output scope.

The installed four-client capture path is not silently extended to this
campaign. Its worker service has a 90-second hard lease, whereas five required
phases each exceed 13 seconds before startup, warmup and terminal propagation.
A separately versioned Farm32 source candidate is pinned at Agent Coordinator
`e5cd675e87ea007e064b4cafe7e6650310390456` and staged in draft upstream PR
`GantriaEngine/agent-coordinator#5`. It uses a different service, pipe, data
directory and request version, preserving the installed F1 service and hook.
Its 600-second lease, 60-second privileged Stop, 1024-MiB nonwrapping worker
ETL and separate client dumpcap profile have passed source/mock checks only.
Retention, ownership, pin and lifecycle checks on the actual endpoints must
qualify before either provider run.
The current worker hook already uses a 256-MiB nonwrapping ETL and rejects
traces at 240 MiB or with lost events; whether that limit suffices for the
32-client campaign remains **not measured**. No new capture or 32-client
physical run is claimed here.

## 2026-10-01 — installed Farm32 capture infrastructure preflight

The side-by-side Agent Coordinator Farm32 service at upstream revision
`5ee889fab571a9047cbc0f8724f2077c7bb9eb74` was installed on the worker
with EXE SHA-256
`0aab359ef88cabb11ca2d5542ccfc3e1ca42c94a88bdfbe2e7bc602888383440`
and pinned hook SHA-256
`b731aa04d27212f596ba6641a39d49dbc32600aac580b5ca4776398084470dfc`.
The original four-client service remained running and untouched. This is a
synthetic capture-infrastructure result, not a
GameSession, Local provider, Node provider, or Foundation 3L acceptance result.

Run `42d6a3c6-da13-47e8-b856-c51832d9ecd8` captured 16 marked UDP packets in
each direct-fiber direction. The installed service completed Start, Stop, and
offline Finalize; Packet Monitor reported zero lost events. The worker returned
to capture-idle after the run.

Capacity run `c24358ef-68e0-4912-83cb-4dd7f3aa14dd` produced a nonwrapping
964,689,920-byte (920-MiB) ETL, SHA-256
`7b340308c21e604a57ff80f74a5bfd2255021e9c20e7a7bc32400f1fd59d69da`.
The ETL final write followed the Stop marker by 3.608 seconds, within the
service's 60-second hook bound. A separate offline replay through the installed
hook exited 0 after 78.385 seconds, within the campaign's 180-second Finalize
bound. It exported 796,092 complete fiber miniport frames into an
885,137,460-byte pcapng, SHA-256
`fbb655864f11731ac6bb48ba6d7d251a0fc23abe3de598ace6f72d95fef8b4bd`.
The pcap matched the original export byte for byte. An independent stream parse
found zero truncated blocks and all 789,300 marked synthetic packets; Packet
Monitor reported 807,781 processed events and zero lost events.

The retained evidence is under
`C:\Sandbox\Codex\Evidence\Foundation3LFarm32` on the worker. Task-owned
emitters exited, UDP 39452 is unbound, Packet Monitor and `netsh trace` are
idle, no Packet Monitor filters remain, and the Farm32 service has no active
capture record. The exact 960-MiB completeness threshold and a 32-client
application capture remain **NOT MEASURED**. The Local and Node provider matrices,
resource, overload/recovery, and final acceptance gates remain open.

## 2026-10-02 — 32-actual-client loopback farm preflight

An isolated worker-local preflight used the task-owned diagnostic package built
from `2cb7379fe` and the bounded direct farm runner. The first two diagnostic
attempts (`farm-loopback-20261002-a` and `farm-loopback-20261002-b`) exposed
qualification-tool defects before client workload: live log reads conflicted
with open Windows writers, then a singleton hashtable's `.Count` obscured the
server start record until its bounded tick budget expired. Both attempts
cleaned up. The direct and two-host endpoint runners now read only complete
lines through bounded, shared-read snapshots; the direct runner counts startup
records as an array. Live-writer mock regressions cover both readers.

Fresh loopback run `farm-loopback-20261002-c` passed the minimal actual-client
farm gate: 32 independent Player/GameSession processes, 32 ready clients, 32
unique run nonces, 32 unique connections, and 32 unique Players. The typed
result reports 5,954 resource samples and is retained with 70 files at
`C:\Sandbox\Codex\Evidence\Farm32LoopbackPreflight\farm-loopback-20261002-c`.
Its evidence index SHA-256 is
`4B8C8C710C57ABA0C8072A1A9EB3D632E2CCE74BEB70167FAEF546B04B5515F8`;
the result SHA-256 is
`4AC4BC9CCF5500827B244E798C8492C5342C4517847C17BEBF1F47F1BF7CEFF3`.
After the run, no task-owned Gargantuan process or UDP 39450 listener remained.
This same-host diagnostic preflight is not a direct-fiber Local provider matrix,
real-TLS Node matrix, capture result, resource-headroom result, or Foundation
3L acceptance. KI-006 remains OPEN and Foundation 3L remains B — PARTIALLY READY.

## 2026-10-02 — recovery cessation-quote contract conflict

The worker-only 32-actual-client Local loopback diagnostic at revision
`1a8a9b704` reached the structural recovery case. Run
`0db602d1-5c1e-4431-b618-1dd307546818` is retained under
`C:\Sandbox\Codex\Evidence\Farm32_1a8a9bDiagnostic` on the worker. Its
`server.stderr.log` SHA-256 is
`f7ab8b26dff018216f2cd9eb2c00545641a99be84bd9dc0d9b8a2de15edb2943`;
the `admission-fairness.tsv` SHA-256 is
`24864d864ea1004fde8a806073e47f3ca3890d991acef70ffa1af66273a4b65f`.
This is diagnostic evidence, not a provider qualification result.

The first recorded quote mismatch was connection slot 6, generation 1, grant
token 3424: live accepted complete bytes **118**, frozen quoted complete bytes
**344,548**, quoted sequence 119. The diagnostic printed
`observed_fingerprint=0:0`; the quote fingerprint was
`9916901384590782373:9383686036810715964`. The source journal tail stayed
at 23,272 while live pending Leaves rose from zero to six before the mismatch.
The trace does not record the 118-byte frame's decoded operation, so its exact
opcode is **NOT MEASURED**. A deterministic regression at `da6a37dfc` proves
that a post-capture live relevance Leave can be accepted with no journal-tail
change while a frozen journal-only quote cannot replay it.

The [recovery contract](PooledReliableServiceRecoveryContract3L.md) defines
`W_i` from semantically necessary work at excess-demand cessation. The
[canonical physical workload](#actual-client-and-provider-gate) requires eight
moving/root-motion Characters through the accepted overload/recovery cases.
The current quote freezes relevance selection and journal state at cessation,
but normal live relevance planning continues after that point. Consequently
the strict frame-by-frame live/quote identity audit can encounter later
legitimate relevance work that was unknowable to the frozen quote. Stopping
motion would change the specified workload; counting future work in cessation
`W_i` or ignoring live frames would change the current evidence contract.
**ARCHITECTURE DECISION REQUIRED** on how recovery under continuing motion
defines and verifies cessation work while preserving the fixed 20-second
service-recovery gate, exact convergence, fairness and normal relevance.

The frozen quote's proven oversize-Name preflight at `da6a37dfc` is independent
of this conflict. Its native oracle matched 40 frames on sequence, complete
bytes, fingerprint and cursor; it reduced encoding retries from 195 to 150
without changing live production. A later worker-only instrumentation run at
`403592934`, ID `43af4af0-06de-49b6-917d-f0557816b9ab`, ended in the
baseline phase when client 00 reported seven action requests and zero action
resolutions. It did not measure recovery and is not a retry PASS.

The direct-fiber Local and real-TLS Node 32-client matrices remain **NOT
MEASURED**. KI-006 remains **OPEN**; Foundation 3L remains **B — PARTIALLY
READY**. No Foundation 3M work or PR merge is authorized by this diagnostic.

## 2026-10-02 — causal recovery implementation in progress

The [causal cessation fence amendment](PooledReliableServiceRecoveryContract3L.md#causal-cessation-fence-amendment--2026-10-02)
selects C3/C5 under the current task's engineering-decision authorization.
The preceding architecture-stop receipt remains historical evidence. It is not
a claim that the new verifier or either provider has passed.

The frozen quote remains an immutable exact reference under cessation state.
Actual post-cessation bytes are audited at preparation/acceptance with source
coverage and grant identity. Recovery closes a finite represented and retired
prefix while moving relevance continues. The existing fixed service-recovery
deadline, workload-derived reference deadline, F1, admission and reserve gates
remain unchanged. Provider execution requires source-observer regressions,
causal-ledger negative tests, farm parser tests and required CI first.

The separate action investigation established that all seven failed requests in
`43af4af0-06de-49b6-917d-f0557816b9ab` were refused locally: there were seven
submission failures and no server action rejections. The retained evidence did
not distinguish absent control, suspended prediction or scheduler refusal.
Revision `be2b81bc8` adds bounded failure counters so a later diagnostic can
attribute the exact branch. It does not retroactively establish that branch.

At starting revision `82ecbd871`, Native run `37000058588` failed its Windows
standalone reliable-workload small-case timing gates (RPC maximum 1107.5 ms),
after CTest and 165 tooling tests passed. Same-head Native run `37000054845`
passed; both sanitizer runs passed. Neither result erases the other. The added
per-case wall-time diagnostics preserve every timing gate. The worker's
`be2b81bc8` character-network suite and all eight real-GNS reliable-workload
cases pass, with small-case RPC maximum 37.0447 ms. Hosted qualification of the
eventual execution-changing recovery candidate is still required.

Worker-only diagnostic `6eea014e-3a9d-44b9-bdfa-685ba8b8d2cf` at
`1f4ba2cfb` exercised the integrated causal observer and independent verifier.
All five workload phases reached their 32-client acknowledgments. Gameplay
recovery recorded 165 causal events, 32 reference frames and exactly 14,875 B
accepted/first-sent/ACKed/retired. Independent replay of its immutable TSV agrees;
the earliest event-proven prefix convergence was 332,871 us, before the native
333,318 us observation and 20,471,118 us deadline. This is a diagnostic subset,
not a provider PASS.

The run failed during structural recovery: client 0 exhausted its explicitly
configured 18,000 frames and returned `scale_incomplete` (exit 15). The resulting
required-peer disconnect invalidated recovery after 943 reference frames.
Structural/mixed recovery and full-run acceptance remain NOT MEASURED.
The retained quote, causal trace and logs were not relabeled as passing.
Cleanup verified no Gargantuan client/server process and UDP 39450 unbound.
Evidence: worker `C:\Sandbox\Codex\Evidence\Farm32_1f4ba2Diagnostic\`
under that run ID. No capture or two-host physical attempt was performed.

The synchronous reference replay measured 72–94 ms advances during this run,
in addition to 62–85 ms live Session steps. The bounded oracle-worker correction
is separately described in the recovery contract; live production cost still
needs qualification. A later diagnostic must use a client frame lifetime that
covers its explicit run budget, without changing the recovery deadline.

At diagnostic revision `be2b81bc8`, hosted Native run `37054274530` passed build,
complete CTest and farm tooling, then failed the standalone qualified recovery
latency case: RPC p95 203.206 ms, maximum 817.236 ms; Event maximum 781.126 ms.
The maximum step interval was 1355.57 ms, server Session wall time 402.852 ms,
and sleep overshoot 147.483 ms. CPU work versus preemption/blocking was NOT
MEASURED. These observations do not waive the failure or qualify the candidate.
Both corresponding GNS sanitizer runs passed.

The subsequent worker-only `a26235496` diagnostic
`c90df074-8d40-4215-8814-3d616cb86e1c` used the qualified joined oracle worker
and a 36,000-frame client lifetime covering its 540-second outer budget.
Gameplay causal recovery converged in the native observation at 332,351 us
with 6,462 B exactly accepted/first-sent/ACKed/retired. Structural recovery
failed its unchanged deadline: immutable reference 553,184,194 B, 1,463 quoted
frames, bound 35,112,492 us. When the quote finished and the deadline check ran
at 80,340,963 us, the live prefix was still unfinished (`prefix_converged_us=0`):
537,780,315 B accepted, 537,130,549 B first-sent and 537,115,771 B ACKed/retired.
This is a valid recovery failure, not successful late convergence or a new
architecture constant. The missing mixed case remains NOT MEASURED.

Moving reference work off Main alone did not make live replication fast enough.
Source inspection identified repeated encoding of oversized Name candidates
before geometric byte-limit retries; correction must preserve exact frames,
source cursors, errors and work-budget accounting. A separate diagnostic fix
reports inclusive validation/encode time: the earlier exclusive wrapper value
omitted its nested serialization scope. No production decision used that counter.
Worker cleanup again found zero client/server processes and UDP 39450 unbound.
Evidence is retained under `C:\Sandbox\Codex\Evidence\Farm32_a2623549Diagnostic\`
and this run ID. No provider or physical PASS is inferred.

Local/real-TLS Node 32-client qualification remains NOT MEASURED; KI-006 OPEN;
Foundation 3L B — PARTIALLY READY. The established corrected F1 physical Phase 1
PASS is unchanged.

Worker-only diagnostic `91f08244-fbc2-4103-b7e8-46d4d92f375d` at
`fa5b91f9c` exercised the narrow Name preflight under the same continuing-motion
workload after exact native A/B tests passed. Gameplay recovery converged at
315,005 us with 15,674 B accepted/first-sent/ACKed/retired. Structural recovery
still failed: the immutable reference was 481,343,350 B in 1,289 frames, with
bound 36,461,766 us. At 60,943,667 us the prefix remained unfinished:
473,394,819 B accepted and 473,001,167 B first-sent/ACKed/retired.
The corrected inclusive counter measured 45–86 ms validation/encoding during
56–105 ms live Session samples. The optimization therefore does not establish
recovery qualification; further attributable encoder work remains.
Mixed recovery is NOT MEASURED. Cleanup verified no client/server processes and
UDP 39450 unbound. Logs and causal TSVs remain under the run ID in
`C:\Sandbox\Codex\Evidence\Farm32_fa5b91f9Diagnostic\`.
No physical/provider attempt or PASS is inferred from this diagnostic.

The subsequent execution candidate `1dce88d9b` retains exact UTF-8 semantics
while scanning complete ASCII words with bounded unaligned-safe reads and
transferring the finished GRPL allocation rather than copying it. Native
qualification passes 1,409,819 UTF-8 parity cases, the fixed GRPL wire/error
regression, the full replication/relevance suites and seven oracle cases.
The latter include more than 65,536 empty progress steps, exactly 65,536 real
frames followed by completion, and rejection of frame 65,537. Name A/B tests
retain exact outcomes with odd/exhausted budgets and atomic retry termination.
The Release real-GNS workload passes all eight cases with 38,668,582 B created
and retired, zero terminal release and zero outstanding debt. Its recovery RPC
maximum was 37.1119 ms, Event maximum 37.1062 ms and action maximum 37.1747 ms.
These are local native results. Hosted Native/sanitizer CI and official-package
workflow `37065664987` remain pending; provider qualification is not inferred.

Worker-only diagnostic `7b2333c6-bea0-43e7-9e0a-f1fd5fb29eba` exercised that
candidate with 32 real clients and continuing motion. Gameplay recovery passed
with 5,344 B conserved and prefix convergence at 332,047 us. Structural recovery
still failed: reference 608,787,930 B in 1,608 frames, bound 33,798,602 us;
at 46,220,886 us the prefix remained unfinished, with 565,137,624 B accepted,
564,423,976 B first-sent and 563,661,224 B ACKed/retired. Mixed recovery remains
NOT MEASURED. The narrower preflight and UTF-8 changes therefore remain
insufficient for complete recovery qualification. Cleanup found no owned
client/server process or UDP 39450 listener. Preserve the evidence under
`C:\Sandbox\Codex\Evidence\Farm32_1dce88d9Diagnostic\` and the run ID.

Source inspection next identified a separate avoidable cost: incremental
production serializes doomed attempts up to the global 8 MiB codec bound before
checking the negotiated complete-frame limit. A bounded-encoder correction must
still validate the entire candidate first, preserve invalid-value precedence,
and retain the exact existing geometric retry, journal charge and final frame.
It may cap encoding at the hard negotiated limit, never available credit.

The correction at `025577469` passes the complete replication and relevance
CTest targets on worker Release candidate `e62955e91` (2/2, 8.87 s). Its 68
focused codec cases preserve exact wire output, late-invalid error precedence,
zero/header/exact boundaries and the public global cap. A doomed candidate
writes 458,931 payload bytes instead of 8,326,011. Production A/B current and
historical Name fixtures preserve exact bytes, errors, cursors and budgets;
historical attempted payload falls from 386,919,616 to 40,712,792 bytes. These
are deterministic work counters, not a physical throughput claim. The same run
passes 1,409,819 UTF-8 parity cases. Evidence is retained as
`C:\Users\aiden\.codex\artifacts\bounded-encoder-e62955e9-tests.log`.
Fresh 32-client recovery and hosted qualification remain separate gates.

Worker-only run `3f153c08-544d-4894-8098-1e08ecc17665` on `e62955e91`
reached all three causal recovery cases. Independent PowerShell replay confirms:

| Case | Events / reference frames | Accepted = first-sent = ACKed = retired B | Earliest prefix convergence us | Original bound us |
| --- | ---: | ---: | ---: | ---: |
| Gameplay | 156 / 32 | 5,344 | 316,646 | 20,470,580 |
| Structural | 7,662 / 1,213 | 450,564,901 | 22,914,408 | 31,475,696 |
| Mixed | 164 / 32 | 14,779 | 320,709 | 20,471,273 |

The farm nevertheless **FAILS**, first reported as client 08 exit 16 after
`phase=complete`. Five content phases were observed, but the native completion
check read `ScaleOverloadCase` from CharacterControl instead of its actual
DataModel owner, so its recovery-name observations remained unset. Independently,
the client recovery probe log used server-only `PhaseTick`; the resulting nil
format argument aborted the coroutine before recording probe completion. These
are distinct harness defects. The causal subset above does not establish full
recovery gameplay, terminal farm, capture, or provider PASS. No owned client or
server process or UDP 39450 listener remained after cleanup.

Evidence remains under
`C:\Sandbox\Codex\Evidence\Farm32_e62955e9Diagnostic\` with this run ID;
the local copy is `C:\Users\aiden\.codex\artifacts\farm32-e62955e9\`.
`farm32-e62955e9-causal-replay.json` records the independent totals and TSV hashes.
A fresh run is required after the completion and probe fixes are qualified.

The completion fixes at `976fae8d5` pass 10 native DataModel owner/stage/type
cases and the actual embedded client callback regression (three cases, 30 probes,
three completion acknowledgments). Its negative control reproduces the exact
historical missing-tick format error. A fresh worker run,
`4da65aae-e7ab-4bfa-8d0c-4b2179080995`, records valid client probe summaries but
**FAILS structural recovery at the unchanged deadline**. Reference work is
783,570,166 B in 2,063 frames with a 36,344,307-us bound. At 36,348,017 us its
prefix is unfinished: accepted 751,166,585 B, first-sent 750,772,933 B, and
ACKed/retired 749,518,126 B. Mixed recovery is NOT MEASURED in this run.

The final structural samples show 3,322–6,072-us Session work and
935–1,063-us encoding, with four active grants. The previous serialization
cost is reduced, but that alone does not qualify service. Grant delivery,
retirement and slot reuse require causal attribution before another diagnostic;
the earlier converged subset is not a substitute for this failure. Evidence is
retained under `C:\Sandbox\Codex\Evidence\Farm32_976fae8d5Diagnostic\` and
`C:\Users\aiden\.codex\artifacts\farm32-976fae8d5\`, with the run ID.
Cleanup again found no owned farm processes or UDP 39450 listener.

Focused native ACK diagnostic `e8163d99b` separately reproduces the delayed
retirement mechanism using the production adapter and pinned GNS. Eight grants
(393,652 and 524,288 B, two successive ACK-gated submissions at each of 1-ms and
16.667-ms Main polling) conserve exact first-send, ACK and retirement with zero
retransmission. Native ACK receipt follows first-send completion by
28,978–30,234 us for 393,652 B and 21,584–22,941 us for 524,288 B; Main observation
adds only 50–1,435 us in this fixture. Receiver traces record the ordinary
50,000-us ACK deadline from the first reliable packet, completed message,
successful ACK transmission and sender receipt. This identifies native delayed
ACK behavior in the focused reproduction; exact native ACK timestamps in the
earlier farm remain NOT MEASURED. Four grant slots remain ACK-owned. A transport
correction still requires reserve, mode-isolation and fresh recovery qualification.
The log is `C:\Users\aiden\.codex\artifacts\ack-cycle-e8163d99b.log`.

The reviewed Farm32 capture-only hook update was applied on 2026-10-02 with
fixed-input rollback protection and five passing transaction simulations.
Installed hook SHA-256 is
`DB8BDD022F3BB30128EBC3E1D8A22596494F2440ABA93DCECDC3334FAB3A8DC9`;
configuration SHA-256 is
`D0CCDBFD994A3BAEF78C7AF2105D88BE7FE29DD2C48E196A59EC84C25FFACCDC`.
The executable, DLL, lease runtime and baseline F1 service are unchanged.
The adoption receipt explicitly records **IDLE_HOOK_ADOPTED_UNQUALIFIED**.
No capture was started by adoption. Near-capacity operational capture/export,
zero-loss acceptance and cleanup remain required before provider use.
Protected originals and the adoption receipt are retained under
`C:\ProgramData\GantriaEngine\AgentCoordinatorCaptureFarm32\hook-adoption-Farm32Capture16GiB-v2`.

Synthetic capacity exercise `9395c8bd-3d82-4159-acd4-8aa626d47fd9` used that
installed hook and the pinned client capture helper. It is **INCOMPLETE**, not a
provider or production run. The stopped worker ETL is 9,603,383,296 B, below the
required 14-GiB exercise threshold. The client sent 3,042,800 marked data packets;
the worker received 3,039,000. All 3,039,400 worker data packets reached the client.
Terminal sequence markers agree with the sender counts, so the worker's missing
3,800 data packets cannot be accepted as complete evidence. The retained receipts
do not record which sequence ranges were absent; their cause remains under
investigation. Neither zero NIC error deltas nor clean cleanup establish zero
capture/socket loss.

The first 80,000 client capture packets show median 100-packet batch spacings of
15,527.6 us and 15,545.8 us by direction. This was initially attributed to the
synthetic emitter's short blocking `select` wait. The later V3 reproduction below
disproves that as a sufficient explanation. An artifact-only pacing correction preserves the original packet count,
20,000-packet/s ceiling, burst, and 480-second traffic bound. The failed outer
controller also stopped its client capture child before the child's normal
600-second close, losing final dumpcap loss diagnostics; the correction preserves
the existing bounded close phase even on emitter failure. Both corrections remain
unqualified operationally. Worker capture/service and both endpoint process/socket
cleanup succeeded. Raw evidence is retained under the run ID in worker
`C:\Sandbox\Codex\Evidence\Foundation3LFarm32\` and client
`C:\Sandbox\Codex\Evidence\Farm32NearCap-v1\`. Offline diagnostic export of the
retained ETL is separate from near-capacity acceptance. No Local/Node qualification
was performed and KI-006 remains open.

Synthetic V3 run `845edec4-c1f7-4f6e-96c2-3747ae9caba1` remains **INCOMPLETE**.
The run-bound, per-flow HELLO exchange preceded DATA; endpoint ledgers account
for all 3,053,100 client DATA packets and all 3,052,400 worker DATA packets,
with zero missing sequences. This establishes endpoint delivery for this run,
not capture completeness. Worker ETL reached only 9,649,520,640 B. The client
captured 6,113,310 packets and reported 1,022 pcap-buffer drops (dumpcap queue
drops zero). The zero-loss gate therefore fails independently of capacity.
Both capture services returned idle, both endpoint process inventories were
empty, and task UDP sockets were released. Source/evidence is retained in the
V3 artifact directory and the same endpoint evidence parents with this fresh UUID.

Pinned Python 3.12.6 reports `monotonic()` as `GetTickCount64()` with 15.625-ms
resolution. V3's nonblocking receive polling still produced median 100-packet
spacings of 15,597.9/15,616.85 us. The 5-ms send deadlines used that coarse clock.
V4 preparation uses monotonic, nonadjustable `QueryPerformanceCounter()` through
`perf_counter()`, reported resolution 0.1 us, and rejects an inadequate pacing
clock before opening sockets. A two-second no-network pacing check observed
365 batches, median 5,475 us, minimum 5,007 us, maximum 6,375 us. Packet count,
rate/burst ceilings, traffic duration and capture acceptance are unchanged.
This local pacing check does not qualify live capture throughput or loss.

The earlier V1 retained ETL was independently exported in 626,366 ms: 6,254,166
complete frames, 6,255,341 events, zero reported ETW event loss, and a
9,098,531,060-B pcapng. Bounded inspection finds all initial 3,800 client DATA
packets in the worker NIC capture before the worker's first transmission. The
missing V1 socket receipt packets therefore lie after that capture observation;
the exact Windows filtering/drop mechanism was NOT MEASURED. Historical evidence
is preserved and no later result upgrades either failed synthetic run.

Synthetic V4 run `0309a027-a783-44e7-8465-584eb20babaf` reached the intended
near-capacity exercise: worker ETL 15,054,929,920 B, below the unchanged 15-GiB
completeness ceiling and above the 14-GiB exercise threshold. QueryPerformanceCounter
pacing and the fixed client `-B 64` request were used. All 4,803,800 client DATA
packets and 4,806,500 worker DATA packets reached the opposite application with
exact per-flow sequence conservation. The client pcap contains 9,619,900 marked
packets, zero reported drops, and passes its independent full-sequence verifier.

The worker export completed, but **capture qualification remains INCOMPLETE**.
Its 14,274,812,236-B pcap has 9,730,757 packet blocks, matching both the export
count and the raw NDIS packet-event count; tracerpt reports zero lost ETW events.
Independent offline inspection nevertheless finds 877 missing client-to-worker
DATA packets and 1,138 missing worker-to-client DATA packets. These are interior
sequence gaps, not missing terminal markers, duplicate sequences, or merely
out-of-order recording. The raw event count and zero ETW-loss report do not
establish packet completeness. The exact worker recording mechanism requires
further attribution; neither application delivery nor the successful client
capture waives the worker evidence gate.

Client and worker cleanup receipts report no owned processes or task UDP
sockets; both capture services are running and idle. Original evidence remains
under the existing endpoint evidence parents with this UUID. The failed outer
receipt remains unchanged. No production/provider run or Foundation 3L PASS is
claimed by this synthetic infrastructure exercise.
