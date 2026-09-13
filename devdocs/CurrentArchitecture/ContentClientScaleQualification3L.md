---
status: partial-qualification
owner: runtime-networking
last_verified: 2026-09-13
---

# Foundation 3L client and scale qualification

**B — FOUNDATION 3L PARTIALLY READY.** Current-source official Player Local and
Node cases pass, including the accepted RPC latency envelope. The retained
200-connected / 50-Character diagnostic still fails its tick and recipient-gap
health checks with streaming. No production correction is conclusively
attributed by this checkpoint. Do not close KI-006 or begin 3M.

## Joined recipient attribution (2026-09-13)

**Classification: mixed cadence/shared-fixture wall-clock failure, not a joined
650-ms production-state delivery delay.** The canonical diagnostic is also
outside the accepted reliable message-count envelope. The historical failures
are real measurements; neither their guard nor the workload is weakened here.

The documentation checkpoint is published as `1dccd89a6`, whose runtime/test
trees equal measured production `543cc0de0`. The bounded diagnostic commit is
`7b299ab61`. There is no production behavior correction. Six tracked source/test
files differ from the 562-file worker baseline manifest, all intentional; the
six replacements and one new header match local SHA-256 hashes exactly. The
other 556 manifest files are unchanged. Sibling and morphology work is preserved.

### Timestamp model and scope

The join key is full recipient ConnectionId, full Character ObjectId,
authoritative tick and state sequence; decoded GCHR records also preserve the
control epoch and materialization epoch. No pointer identity or new semantic
identity is introduced. Root request/commit joins use full Character identity
and the Engine simulation tick. Engine and server publication tick counters
are distinct: in this fixture the measured Engine tick is publication tick
minus 44. Shared steady-clock timestamps provide the mapping; they are not
interchangeable counters or cross-process clocks.

`GARGANTUAN_JOINED_LATENCY=1` enables `JoinedCharacterFixture` for the load phase
only. It reserves 1,048,576 fixed-shape records (80 MiB), with static stage names,
no retained payloads and an explicit failure on overflow/decode failure.
Control/Local/Node retain 770,689/772,151/772,151 records, with zero drops and
zero decode failures. Output is deferred until all measured phases finish.
Existing inactive runtime taps remain no-ops without a scoped test sink.

| Stage | Measured meaning |
| --- | --- |
| T0 | Start of the existing authoritative due tick, joined from `CharacterNextDue` and `FrameBegin`; not an invented continuous-wall deadline |
| Root request / commit | Existing Animator request and successful Main-thread admission, with matching Engine tick |
| T1 | `StateBuilt`: generation-safe authoritative snapshot constructed |
| T2 | `CharacterSnapshot` / `CharacterProduced`: cached snapshot and selected recipient frame ready |
| T3 | New structural dependency **not applicable** to these already-materialized Character endpoints; both frames use materialization epoch 11 |
| T4 | Existing scheduler accepts the GCHR frame; this is not a new 3J structural acceptance or reliable-credit charge |
| T5 | Simulated transport successfully submits the frame |
| T6 | Simulated transport queues the matching recipient event |
| T7/T8 | Protocol observer begins decoding, records the state at the exact timestamp used by the gap guard, then completes its checks |

The worst recipient is a protocol observer, not a Player. It records protocol
facts, not a live replicated Character transform. The separate real client's
`ClientCallback`/`ClientHandled` taps retain that distinction. For these
unreliable sequenced Character frames, reliable byte admission is **not
applicable**. An observer does not acquire a live-application timestamp merely
because it decoded a message.

### Worst Local and Node state joins

Both worst streaming gaps concern recipient **200:1**, root Character **63:1**,
control epoch **2**. Last observation: authoritative tick **504**, sequence
**478**. Next: tick **516**, sequence **490**. Values below are milliseconds
relative to the last observed state; they are an ordered state join, not sums
of unrelated stage maxima.

| Next-state milestone | Local | Node |
| --- | ---: | ---: |
| T0: tick 516 begins | 604.0847 | 599.6827 |
| Root request (Engine tick 472) | 604.3952 | 599.9798 |
| Root commit (same Engine tick) | 604.4025 | 599.9870 |
| Due wheel processes relationship | 632.5068 | 627.4749 |
| T1: authoritative snapshot built | 632.5303 | 627.5042 |
| T2: recipient frame ready | 632.6141 | 627.5854 |
| T4: scheduler accepted | 632.6155 | 627.5867 |
| T5: transport submitted | 633.1328 | 628.0622 |
| T6: recipient event queued | 633.6023 | 628.5026 |
| Observer decode begins | 656.3040 | 649.8658 |
| T8: guard observes state | **656.3057** | **649.8674** |
| Observer checks finish | 656.3062 | 649.8679 |

Thus the next due state takes **52.2210/50.1847 ms from T0 to observation**.
Snapshot construction to recipient-frame readiness is 0.0838/0.0812 ms;
frame readiness to scheduler acceptance 0.0014/0.0013 ms; accepted-to-submit
0.5173/0.4755 ms; submit-to-event-queue 0.4695/0.4404 ms; event-queue-to-observer
entry 22.7017/21.3632 ms; observer entry-to-state observation 0.0017/0.0016 ms.
The earlier state was built 1.4249/1.5920 ms before its observation. No joined
state spends approximately 650 ms in scheduler, transport or client application.

At tick 504 the relationship schedules its next due tick as 516, on the
LowRate tier (12 ticks at nominal 60 Hz). The next recorded schedule after
516 is 528. There are **no due publications to recipient 200 for ticks
505–515**. The Character nevertheless produces snapshots for other recipients
on every tick 505–516, with sequences 479–490. All 41,480 ordinary selected
recipient states in each run are produced on their desired due tick: zero
late selections and maximum tick lateness zero. The 3/6/12-tick tiers and
unchanged-state suppression remain authoritative; owner input's five-tick
cadence is a separate mechanism.

Inside the worst streaming interval, the affected Animator makes seven nonzero
root requests, Engine ticks 466–472 (publication ticks 510–516). Every request
commits in the same tick, taking approximately 7–8 microseconds. Earlier steps
in the interval produce no root request: animation requests require a valid
nonzero motion delta, not an unconditional motion command every wall interval.
Across load, requests equal commits: 1,270 control and 1,400 per streaming run.
No root request is lost or deferred. A root-request sequence does not require
one network publication per request to every low-rate observer.

All **45,120** full recipient/state keys match exactly across scheduler
acceptance, transport submission, event delivery and observation: zero missing
or extra keys. This includes 41,480 ordinary deliveries and 3,640 reliable
forced-semantic deliveries, which the earlier ordinary-publication counter did
not include. No intermediate state due to the worst recipient is missing.

### Exact interval ownership and observer isolation

The table clips paired timing spans to each run's actual worst observation
interval. These mutually exclusive owners sum to the measured gap. The control
worst pair is recipient 197:1 / Character 63:1, ticks 545–557, sequences 519–531;
it also spans 12 publication ticks. The values are corresponding worst intervals,
not an assertion that the same tick window is worst in every run.

| Wall-clock owner, ms | Control | Local | Node |
| --- | ---: | ---: | ---: |
| Server simulation/replication | 39.3439 | 340.0189 | 334.7485 |
| One gameplay client Engine | 3.1084 | 42.3026 | 47.4721 |
| 199 protocol observer drains | 4.6285 | 250.5823 | 245.2356 |
| Pacing | 151.7160 | 3.2578 | 3.7045 |
| Other fixture work / boundaries | 5.2560 | 20.1441 | 18.7067 |
| Total | **204.0528** | **656.3057** | **649.8674** |

Observer isolation uses separately timed `ServerDone`, `DrainBegin/DrainDone`
and decoded-packet scopes, without reducing peers or deferring canonical work.
GRPL decoding and hierarchy/property verification inside the protocol observers
accounts for **0 / 245.8639 / 240.5820 ms** of the table's observer time.
Character packet processing accounts for 2.1548/2.1609/2.1110 ms. Remaining
observer time is polling, loop and diagnostic overhead. In streaming, that
observer-only GRPL work blocks the next server tick because all roles share one
serial harness. The official Player does not run 199 other clients' validators.

The server component also increases substantially; this is **not purely an
observer artifact**. Streaming ticks include real structural preparation,
encoding and publication. No single production operation owns the whole gap.
Direct `Content::Step` work in the interval is approximately
0.014/0.017/0.019 ms, already included in server time; acquisition/admission
precedes the examined publication burst. Provider worker/TLS timing is not
invented from these Main-thread counters. Removing observer time arithmetically
is not a rerun: Engine animation uses elapsed time, so an accelerated harness
could change motion intervals. No weakened qualification variant is retained.

### Complete load producer and recipient accounting

Trace windows last 5.027959 / 5.5535743 / 5.5512429 seconds for
control/Local/Node. Unless stated otherwise, counts and bytes below are identical
in all three runs. Divide by the recorded wall duration, not simulated ticks,
for rates. Payload bytes include protocol headers, not the GNS adapter or TLS.

| Traffic | Producer count | Recipient/network count | Payload bytes |
| --- | ---: | ---: | ---: |
| Authoritative Character snapshots | 13,610 unique states across 50 Characters | 45,120 state deliveries in 36,507 GCHR frames | 5,110,758 |
| Ordinary Character publication subset | 13,584 unique snapshots | 41,480 state deliveries | Included above |
| Forced reliable Character subset | 26 unique states | 3,640 deliveries, each in its own frame; mean fanout 140 | 522,720 |
| Root-Character subset of all states | 2,273 unique states | 25,059 state deliveries | 2,385,006 compact-state bytes, excluding shared frame headers |
| Owner input | 3,241 commands from 50 owners | 3,241 server deliveries | 194,460 |
| Reliable Event offered / echoed ACK | 301 / 301 | Fanout one in each direction | 19,866 / 19,874 |
| RPC request / response | 100 / 100 | Fanout one; maximum outstanding one, final zero | 6,100 / 6,100 |
| Action request / accepted result | 8 / 8 | Fanout one for result; forced state fanout counted separately | 384 / 928 |
| Streaming GRPL | Recipient-specific prepared frames | 400 messages, 104,840 operations, 200 recipients | 31,145,760 |
| Control GRPL | Recipient-specific prepared frames | 200 messages, 2,440 operations, 200 recipients | 175,560 |
| Content acquisition | Zero control; one acquisition and one initial admission per streaming provider | One immutable 512-object package | 273,032 uncompressed provider payload bytes |

For example, Local produces approximately 2,450.7 unique Character snapshots/s,
delivers 8,124.5 states/s in 6,573.6 GCHR frames/s, and submits approximately
920,264 Character payload bytes/s. Its Event/RPC/action offer rates are
54.20/18.01/1.44 per second. Root-Character state delivery is a subset, not
additional traffic to add to the totals. The fixture's earlier phase counters
use slightly different boundaries; the trace counts every packet in its window,
including an echo crossing a phase boundary. Network/TLS framing, physical ACKs
and retransmissions are **not measured** by the simulated transport.

There is one ordinary reliable gameplay producer, not 50. Reliable accounting
includes every forced semantic recipient, action result and echoed Event, and
excludes GRPL from the ordinary gameplay bucket because it has separate
structural credit. No unknown/control packet is omitted in this load window.
All 200 recipients are included. Add the documented 32-byte adapter envelope
once per packet for the following service-budget calculations; this is budget
accounting, not measured GNS wire traffic.

For each peer/direction, evaluate every packet-bounded interval against
`messages <= 16 + 64*T` and `bytes <= 20480 + 32768*T`. Aggregate 200-peer
limits are `messages <= 256 + 1024*T` and `bytes <= 327680 + 524288*T`.
The minimum burst needed at the fixed accepted rate is
`max_interval(count - rate * duration)`; a phase average cannot establish this.

| Required message burst | Control | Local | Node | Accepted burst |
| --- | ---: | ---: | ---: | ---: |
| Peer 1 ingress | 89.0954 | 74.7063 | 74.4057 | 16 |
| Peer 1 egress, including semantic fanout | 106.6678 | 88.9821 | 88.7181 | 16 |
| Aggregate server egress | 533.0325 | 467.6004 | 463.8676 | 256 |

All three violate those message-count assumptions. The other peers' individual
message buckets pass, as do measured byte buckets. Aggregate reliable egress
is 4,049 messages and 679,190 bytes including adapters; its required byte burst
is below 75,673 bytes, within 327,680. Peer 1 ingress/egress is 409/427 messages
and 39,438/43,158 bytes including adapters. RPC concurrency and action count
remain bounded. This does not retroactively relabel a qualified failure: the
handoff explicitly classified the 200/50 case as an investigation fixture with
unproved complete envelope accounting. That accounting is now measured and
fails. No workload or accepted limit is changed to make the case pass.

### Guard verdict, validation and next task

The observation-gap metric is valid as **elapsed shared-fixture recipient
cadence**, but invalid as a standalone measure of production due-state latency.
It includes time before the next semantically due update and serial work for
199 other observers. Server streaming cost is also real. Classification is
therefore **mixed**, with an exact fixture/measurement owner in
`RunContentScale` and `ContentScalePeer::Observe`, not a single production
publication, transport or application defect. The 250-ms guard and its failures
remain intact. No caching, threading, queue, rate, wire or transaction change is
retained. Only bounded instrumentation and its focused tests are added.

Before/after instrumentation reproduces the result: earlier 662.822/642.989 ms;
first joined run 664.6061/653.1715 ms; final joined run 656.3057/649.8674 ms.
Control passes, streaming load/evict/reload fail unchanged health checks.
Final load tick p95/p99/max is 4.3591/5.4551/10.6414 ms control,
22.7121/29.9545/53.4288 Local and 22.1833/30.6537/50.5497 Node.
These are instrumentation comparisons, not optimization gains.

MSVC `gargantuan_game_session_tests` passes, including full-generation state
correlation, one packet byte charge, stale-peer rejection, bounded storage and
scoped-sink cleanup. Both joined comparisons run the unchanged three-provider
matrix; traces have no omissions or decode failures. The documentation build
passes with 19 pages in an isolated copy. Initial copied dependency and system
Node 20 setup failures were repaired using isolated dependencies and Node 24;
the active docs workspace and repository dependency files were not changed.

Prior security/KI-007/KI-008, sanitizer, overload and official Player evidence
remains evidence for the unchanged production behavior at `543cc0de0`, not an
exact-commit CI claim for the added instrumentation. No production behavior
correction or GNS path change requires those expensive matrices again. Existing
official Player application/Poll gaps are consistent with this attribution:
the Player has no serial 199-observer drain. It lacks this 200-peer server-side
join, so it does not prove a physical 200-client deployment; no automatic rerun
was performed.

Raw v1/v2 receipts, hashes and exit codes are on `dockerbox` under
`C:\Sandbox\Codex\Artifacts\joined-3l-20260913` and
`C:\Sandbox\Codex\Artifacts\joined-3l-20260913-v2`. Local analysis and full
per-state joins are under `C:\Users\aiden\AppData\Local\Temp\3l-joined-evidence-v2`;
temporary artifacts are not checked in. Diagnostic source is committed
separately from these documentation conclusions.

**Stop at the attribution boundary:** no single production owner violating an
accepted service property is established after the complete join. KI-006 stays
open; Foundation 3L remains **B — PARTIALLY READY**. Real-client 200/500 scale,
qualified aggregate fanout, physical deployment capacity, high-scale shutdown
retention and GPU-present visibility remain **not measured**.

**Exact next task:** define the companion diagnostic around per-recipient
due-tick-to-observation latency, retain raw observation cadence and server tick
health separately, and construct a 200-peer gameplay mix that demonstrably fits
all existing peer/global message and byte inequalities including forced semantic
fanout. Preserve this unqualified stress case as a control. Select/fund a physical
deployment profile before claiming real-client support. Do not optimize
production merely to satisfy the serial-harness gap or begin 3M.

## Earlier client/scale checkpoint

The remaining sections preserve the measurements and limitations preceding the
joined attribution above. Their pending-attribution statements are historical;
the new join supersedes that uncertainty without erasing the failed runs.

## Source and evidence boundary

Branch: `foundation/3l-content-availability`. Starting and measured production
HEAD: `543cc0de098627570a434e4002fcbb8f41c2b03a`. This checkpoint changes
documentation only. No sibling repository, public-portal branch, morphology
work, protocol, semantic authority, workload limit or runtime source is changed.

The worker source verifier reports **562 files, zero mismatches** against the
current native/configuration source manifest. Builds are incremental MSVC
Release on `dockerbox`, limited to four build jobs, with benchmarks run serially.
The read-only Node harness revision is
`c1f60b179c1aafccc233babbebde18925ea4f32e`. Node supplies TLS content acquisition;
it does not turn the 200-peer in-memory gameplay fixture into physical GNS peers.

Raw receipts are retained on `dockerbox` under
`C:\Sandbox\Codex\Artifacts\client-scale-543cc0de0-20260913`, including source,
build, preflight, control, Local, Node, repeated Local and official-run receipts.
Official per-process logs and server-memory CSVs are under
`C:\Sandbox\Codex\Logs\gargantuan-3l3\client-scale-543cc0de0-official`.
The controlling-machine analysis copy is
`C:\Users\aiden\AppData\Local\Temp\3l-client-scale-evidence`, including
`summary.json` and `official/summary.json`. These paths are evidence locations,
not dependencies of the repository or portable artifact URLs.

Binary SHA-256 receipts before the runs:

| Executable | SHA-256 |
| --- | --- |
| `gargantuan_node_content_scale.exe` | `B6A493B3C16DD970C8DBF6C2AFC1E29B9D00BE4FA8ECB23D3F0A558AB80BD351` |
| `gargantuan_replication_benchmark.exe` | `751BBBB5E45E98272ED410F8E7406B0F402135646F769A4A081C522476D59190` |

The initial orchestration attempts encountered PowerShell native-warning
handling and a benchmark target absent from the Node build. They produced no
qualification result. The corrected driver builds the existing native benchmark
target separately. Individual fixture exits, not the orchestration exit, decide
health: control 0; Local 1; Node 1; repeated Local 1; official Local/Node 0.
The repeat has work tracing disabled; the first comparison has tracing enabled.
Both reproduce the same Local health failure. Failed receipts are preserved.

## Gate inventory and authority

The [workload contract](ReliableGameplayWorkloadContract3L.md),
[3L.3 architecture](ContentAvailabilityFoundation3L_3.md),
[validation ledger](ContentAvailabilityFoundation3L_3Validation.md),
[reliable qualification](ReliableGameplayQualification3L.md), and
[KI-006](../../KNOWN_ISSUES.md#ki-006-content-coupled-gameplay-latency-exceeds-the-3l2-readiness-envelope)
define the scope. The inventory was made before new benchmarks.

| Gate | Existing evidence | Current status and exact missing evidence |
| --- | --- | --- |
| Generation-safe ownership and affected tests | SEC-3L-001 exact-source receipt | **PASS**, reused; no affected source change |
| Single-peer ordinary small/upper/mixed RPC, Event and action | KI-008 corrected real GNS matrices | **PASS** for measured cases; not all possible legal traffic |
| 32-peer structural/RPC overload, recovery, journal margin, peer cleanup | Corrected KI-008 MSVC and sanitizer runs | **PASS**, reused; overload latency is not ordinary service latency |
| 200/500 admission accounting and fairness | Deterministic admission fixture | **PASS** for accountant scope; actual client/path qualification remains **NOT MEASURED** |
| Changed-property client preflight | Earlier whole-candidate cost | Earlier timing **STALE** as a current measurement; remeasured below. No standalone numeric gate; optimization is **NOT APPLICABLE** as an automatic requirement |
| 200/50 canonical streaming differential | Historical failures | **FAIL** retained investigation guards, reproduced at current source for both providers |
| Official Player single-client pipeline and RPC | Earlier official Local/Node results | **PASS** measured current cases; separate no-content Player control and high-scale real Player matrix **NOT MEASURED** |
| Aggregate ordinary Event/action fanout at 32/200/500 | RPC-only accounting is incomplete | **NOT MEASURED** complete arrival/burst accounting and recipient service at qualified mix |
| 200/500 real-client scale and physical capacity | Dense stress/candidate reservation arithmetic | **NOT MEASURED** actual multi-client CPU/NIC path with funded deployment assumptions |
| Retention after lifecycle/recovery | Security churn and GNS disconnect tests | **PASS** tested logical cleanup; full high-scale client shutdown/resource accounting **NOT MEASURED** |
| GPU-present visibility | Headless semantic observations | **NOT MEASURED**; numeric renderer-visible target **NOT APPLICABLE** because none is accepted here |

The retained differential requires tick p95/p99/max <=16.667/33.334/100 ms,
recipient Character/root gaps <=250 ms and <=12 authoritative ticks, plus the
existing Remote/Event/action checks. The architecture explicitly calls these
**investigation guards, not a universal all-workload product contract**.
Their failure remains visible; it does not authorize a speculative optimization
or prove that every supported qualified deployment violates its contract.

The ordinary reliable contract remains 16 KiB frames, 32 KiB/s and 20 KiB burst
per peer/direction, four outstanding RPCs, and the separate aggregate producer,
message and fanout limits. RPC p95/p99/max are 150/250/500 ms; Event ACK service
gap and action result maxima are 250 ms. No acceptance criterion is relaxed.

## Client pipeline and preflight

Current ownership is transport receive through `GameSession::Poll`, structural
decode, `ReplicaApplier` candidate copy/index and semantic validation,
`LoadSnapshot` preflight, live application, then client semantic observation.
Player `Runtime::Step` separately extracts and publishes render state. Headless
render-state publication is not GPU presentation. This retains DataModel truth,
3E relevance, 3J materialization/Known, and 3L lifecycle authority.

`src/network/ReplicaApplier.cpp` still copies `SemanticState`, indexes the whole
candidate and loads a validation world for a changed property. Its identical
native-property shortcut does not remove that changed-property cost. The
affected path was not changed by SEC-3L-001 or KI-008.

Command: `gargantuan_replication_benchmark --incremental-scaling`. Ten changed
property trials and one removal trial run per size. Values below are milliseconds;
p50 is nearest rank, max is across the ten changed-property trials. These are
small diagnostic samples, not tail-distribution qualification.

| Replicas including root | Total p50 / max | Copy max | Semantic max | Preflight p50 / max | Live apply max |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 65 | 0.188 / 0.245 | 0.006 | 0.007 | 0.149 / 0.201 | 0.007 |
| 513 | 2.021 / 2.209 | 0.147 | 0.068 | 1.560 / 1.754 | 0.077 |
| 2,049 | 12.797 / 16.786 | 0.612 | 0.307 | 9.700 / 11.498 | 0.520 |
| 8,193 | 95.827 / 112.409 | 2.834 | 1.329 | 76.202 / 87.364 | 1.667 |

Stage maxima occur on potentially different trials and are not additive. Total
also includes index/other work. The 8,193-replica removal took 91.330 ms total,
73.520 ms preflight and 8,193 removal visits. These reproduce scaling cost, not
an 89–101 ms current preflight claim: current total and preflight are distinct.

**Preflight verdict:** a real whole-world cost remains, but no accepted
standalone preflight limit or supported 8,193-replica arrival cadence is violated
by this benchmark alone. It is non-blocking as an isolated optimization request.
The official client measurements below pass their observed service checks. A
larger-world application guarantee remains unqualified; do not infer one from
this benchmark or introduce a new transaction architecture here.

## Canonical 200-peer differential

The unchanged invocation is `--content-differential <package> <provider> 50 5 200`
for `none`, `local` and TLS `node`, using the existing 512-object, 273,032-byte
package. It has 200 connected protocol peers, 50 active Characters, five per
spatial neighborhood, 12 Hz staggered input and ten Animator root-motion owners.
One peer is a full gameplay client Engine; the other 199 are protocol observers.
Server, client and observers share a serial fixture and steady-clock domain.
Fifty active Characters do not mean fifty ordinary reliable producers; this
fixture does not account for the complete qualified aggregate fanout envelope.

All values below are ms except throughput. Each phase has 301 ticks.

| Provider / phase | Server tick p50 / p95 / p99 / max | Character/root max observed gap | Accepted Character states/wall-second | Health |
| --- | --- | ---: | ---: | --- |
| Control / load | 3.113 / 4.100 / 5.195 / 11.838 | 202.476 | 8,240.20 | PASS |
| Local / load | 7.552 / 20.841 / 32.605 / 55.219 | 662.822 | 7,462.56 | FAIL |
| Node / load | 7.764 / 21.455 / 29.266 / 51.539 | 642.989 | 7,496.88 | FAIL |
| Local / evict | 3.209 / 14.183 / 20.152 / 23.222 | 255.534 | 8,068.46 | FAIL |
| Node / evict | 3.189 / 14.685 / 19.988 / 23.587 | 252.050 | 8,070.46 | FAIL |
| Local / reload | 7.819 / 23.569 / 35.921 / 54.395 | 349.913 | 5,412.78 | FAIL |
| Node / reload | 7.852 / 25.116 / 35.485 / 55.748 | 359.281 | 5,374.15 | FAIL |

All control phases and both streaming baselines pass. The untraced Local repeat
also passes baseline and fails load/evict/reload: tick p95 22.535/14.740/24.927 ms
and recipient gaps 659.854/252.362/341.993 ms. This is not just trace overhead.

Character scheduler rejections are zero in every phase. Root requests equal
authoritative commits throughout; load counts are 1,270 control, 1,410 Local and
1,400 Node. Local/Node load delivers 25,059 root GCHR states, with maximum
authoritative-tick observation gap 12. Root minimum Y remains 2.495; load has 11
cell crossings. Root commit interval percentiles and recipient gap percentiles
are **not measured**; counters and maxima must not be described as distributions.

Local/Node converge on load in at most 1,400.07/1,359.61 ms, eviction in
741.20/740.02 ms, and reload in 2,191.15/2,227.04 ms. Each performs exactly one
acquisition and initial admission, and a second admission on fresh reload.
Correctness converges despite the diagnostic timing failures.

### Gameplay service in the same fixture

| Load phase | RPC p50 / p95 / p99 / max, ms | Event ACK round trip max, ms | Event ACK max service gap, ms | Action total p50 / p95 / p99 / max, ms |
| --- | --- | ---: | ---: | --- |
| Control | 33.793 / 35.500 / 35.848 / 35.927 | 37.405 | 20.172 | 33.681 / 35.750 / 35.750 / 37.340 |
| Local | 38.031 / 42.696 / 112.270 / 138.466 | 150.203 | 88.763 | 33.620 / 40.012 / 40.012 / 40.064 |
| Node | 37.516 / 61.028 / 100.097 / 102.714 | 140.962 | 88.702 | 33.173 / 40.124 / 40.124 / 40.672 |

Every phase completes 100 RPCs with zero timeouts/errors; no process crashes.
There are 297 measured Event ACK round trips and seven or eight measured action
round trips per phase. Local/Node load counts 302 Events and 299 ACKs over
5.558/5.533 wall seconds, approximately 54.33/54.58 Events/s and 53.79/54.04
ACKs/s. Phase counter boundaries differ from the 297 timed samples.
All phases pass the RPC, ACK-service and action-total checks. Across all phases,
Local/Node worst RPC is 138.466/119.563 ms, Event round trip 150.203/140.962 ms,
and measured action round trip 56.096/53.089 ms. Submission failures, rejections,
unexpected action endings and detected stalls are zero. Separate action
request-to-validation and validation-to-result durations are **not measured**.
Small action sample percentiles retain the fixture's estimator; do not infer a
well-characterized p99 from seven calls.

### Attribution limit

In Local's 662.822 ms observer-gap window, traced work is distributed across
server structural encoding (60.706 ms exclusive), frame building (46.415),
Known commit (37.733), structural selection (34.260), Remote materialization
(27.395), planning resume (24.848), client apply (23.794) and relevance query
(23.653). This is a window association, not a disjoint end-to-end latency budget
or proof that deleting one stage fixes the gap. The serial fixture also includes
observer draining, pacing, other work and multiple frames.

Across the Local load phase, server relevance query consumes 521.282 ms
exclusive, hysteresis 387.109 ms and graph synchronization 264.024 ms. Its
maximum combined frame work is 86.260 ms, while the official separate Player
does not reproduce a comparable long client stall. The evidence isolates a
streaming differential and distributes its owners; it does not identify one
conclusively defective production operation. No optimization is retained.

Per-message authoritative birth-to-preparation, acceptance-to-receive and
receive-to-semantic-observation joins are **not measured**. Convergence, phase
stage timings and observer gaps are different quantities. In-memory transport
has no physical-link capacity result.

## Official Player: Local versus Node

The existing `TestOfficialContentMemorySoak/near-max` runs the packaged official
Windows Player and Server, with physical GNS loopback and either Local or TLS
Node content. The content fixture has 512 objects and 1,048,197 payload bytes;
it is deliberately different from the scale package above. Both providers pass
eight churn cycles and the full client load/evict/reload/RPC sequence. Timings
below are Player-process durations, not cross-process timestamp subtraction.

| Metric, ms | Local | Node |
| --- | ---: | ---: |
| Frame interval p50 / p95 / p99 / max | 16.748 / 17.803 / 18.832 / 41.973 | 16.707 / 17.771 / 18.711 / 42.699 |
| Event-loop gap p50 / p95 / p99 / max | 16.668 / 19.069 / 34.076 / 54.165 | 16.635 / 19.053 / 33.299 / 56.302 |
| Poll max (includes receive/application work) | 38.555 | 39.969 |
| Structural decode max | 3.020 | 3.369 |
| Candidate copy max | 1.052 | 2.388 |
| Candidate semantic validation max | 1.625 | 2.264 |
| Preflight total max | 18.692 | 18.514 |
| Preflight validation / construction / parenting / properties max | 3.238 / 2.984 / 7.511 / 4.955 | 3.514 / 3.092 / 6.698 / 5.207 |
| Live application max | 5.859 | 6.085 |
| Structural application total max | 34.481 | 36.588 |
| Engine step / Session step max | 6.463 / 0.263 | 6.755 / 0.256 |
| CPU render extraction / publication max | 0.211 / 0.117 | 0.213 / 0.169 |
| Character handler max service gap | 89.747 | 90.198 |
| Remote handler max service gap | 89.736 | 88.694 |
| RPC p50 / p95 / p99 / max | 33.312 / 35.939 / 59.013 / 91.111 | 33.308 / 36.440 / 61.020 / 92.122 |

The trace contains 327/326 frames; frame percentiles use nearest rank and include
frames with zero structural work. Stage values are per-frame totals, may contain
multiple messages and may nest; independent maxima cannot be summed. The
Character/Remote gap fields are cumulative maxima, so no percentile distribution
of individual service gaps is claimed. Client handler counts are 113/110
Character messages and 181/181 Remote messages. Each provider completes 100
RPCs with zero errors/timeouts and no crash.

Client-relative semantic first visibility is 368.670/369.486 ms from script
start, eviction 950.467/948.550 ms and reload visibility
1,789.735/1,789.464 ms. These are lifecycle milestones, **not** durations from
the corresponding authoritative change. GPU-rendered visibility and trustworthy
cross-process per-message stage joins are **not measured**. CPU extraction and
publication do not supply a renderer latency guarantee.

A separate no-content official Player control is **not measured** here; the
passing no-streaming scale control is a client Engine, not a Player process.
These runs show bounded measured single-client service, not absence of starvation
under every legal workload or at 200/500 real clients. No provider-specific
semantic failure is demonstrated, and no Node change is justified.

## Scale and retained state

| Peer count | Current applicable result | Boundary |
| ---: | --- | --- |
| 1 | Official Local/Node above; reuse exact-source small/upper/mixed real GNS evidence | Headless single-client cases |
| 32 | Reuse corrected KI-008 combined overload/recovery and ordinary RPC matrices | Aggregate Event/action fanout still not measured |
| 200 | New control/Local/Node differential; reuse deterministic accountant | One real client, 199 observers; health FAIL under streaming |
| 500 | Reuse deterministic accountant and historical structural bounds | Healthy qualified real-client gameplay/fanout not measured; prior dense baseline was already unhealthy |

Do not blindly rerun the historical 500-active diagnostic as an accepted mix.
The workload contract explicitly says its 500-peer candidate dimensions need
4,000 MiB/s aggregate application service and 8,000 MiB/s summed backend
ceilings; those are reservation calculations, not measured hardware capacity.
The existing 200/50 fixture cannot substitute for 200/500 real clients or complete
ordinary-producer accounting. Choosing a supported physical deployment profile
is a platform boundary, not permission to weaken rates, change wire ordering,
add reliable lanes or claim commodity-network support.

For Local/Node 200-peer load, maximum private planning work remains 65,536 per
tick, maximum peer slice 2,048, maximum planner service gap seven ticks and the
3J selected cap exactly 8,192. Pending high-water is 68,096; planner records
high-water 1,417,005; relevance staging is 6,464 bytes and candidate storage
9,600 bytes. These logical records are not an allocator-byte measurement.
Journal failures are zero; journal lag high-water 6,135, staging 4,800 bytes,
and final phase backlog zero. Aggregate per-peer backlog high-water 1,227,000
is not occupancy of the raw mutation journal ring. Retired catalog records
return to zero, and the final resident reload intentionally retains content.

Differential RSS peaks are 110,542,848 bytes control, 200,347,648 Local and
200,400,896 Node. The streaming decoded high-water is 1,623,360 bytes. A short
process RSS increase is neither a leak proof nor proof of bounded long-term RSS.
Full post-shutdown client/cache/request/continuation/reservation accounting at
200/500 actual clients is **not measured**.

Official eight-cycle server RSS samples peak at 69,242,880/73,261,056 bytes,
private bytes at 58,626,048/60,887,040, with 108/111 samples. These CSVs sample
the **Server**, not combined Server/Player/Node memory. Each cycle reaches zero
decoded bytes; decoded high-water is 2,359,192, completion high-water 1,048,197
and detached-object high-water 512. A 1,048,197-byte content cache and 512
resident objects at the recorded reload checkpoint are intentional. Both runs
emit `FINAL_DRAIN` and `CONTENT_CHURN_OK`; they do not expose every allocator or
post-destruction counter.

Reuse KI-008's zero peer owners/connections and zero recovery backlog, and the
security receipt's sanitizer leak checks and failure-unwind coverage. Do not
generalize those bounded tests into an unmeasured 500-client retention result.

## Validation, verdict and next task

No production source changes; retain the
[SEC-3L-001 receipt](ContentAvailabilitySecurityClosure3L.md)'s MSVC Release
13/13 and Clang 19 ASan/UBSan/LSan 13/13, plus exact production HEAD terminal
[Native CI success](https://github.com/gmoddev/gargantuan/actions/runs/34751438089)
and [GNS sanitizer success](https://github.com/gmoddev/gargantuan/actions/runs/34751438088).
KI-008 source-equivalent evidence remains established: 6,656 overload RPCs per
platform, recovery <=1.727 s, and clean disconnect. No unrelated native or GNS
rerun is required by this documentation-only checkpoint. No new publication,
merge or claim of CI covering a later source revision is made.

Documentation verification: the new receipt's relative file links resolve and
the scoped diff passes `git diff --check`. Astro site source is unchanged; a new
site build is not run for this devdocs/known-issues-only checkpoint. The three
documentation changes were initially left uncommitted for review, then published
as `1dccd89a6` before the joined attribution above. Unrelated morphology edits
are retained.

Non-starvation is demonstrated for the measured official single-client and
retained reliable matrices. The canonical 200-peer diagnostic fails its
recipient/tick health guards despite successful RPC/Event/action service and
structural convergence. Full supported-scale non-starvation is **not established**.
The preflight benchmark is an optimization opportunity in isolation, not an
authorization for transaction redesign. No newly isolated security/correctness
defect or production correction is retained.

**Exact next task:** qualify the 200-peer streaming recipient-service failure
with joined due/prepare/accept/receive/observe timestamps and complete ordinary
producer/fanout accounting under the existing workload envelope. Keep the
200/50 control and failing receipts, distinguish fixture serial-observer work
from production client service, and identify one owner before proposing a fix.
For real 200/500-client capacity, first select and fund a physical deployment
profile consistent with the existing reservation contract; do not present
stress arithmetic as support. Then measure the required multi-client matrix,
shutdown retention and aggregate Event/action fanout. Renderer-present latency
remains separately not measured, with no invented numeric gate.

This is a partial qualification result, not eligibility for the final Foundation
3L closure/acceptance sweep. No 3M begins.
