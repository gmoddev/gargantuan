---
status: partial-qualification
owner: runtime-networking
last_verified: 2026-09-13
---

# Foundation 3L client and scale qualification

**B — FOUNDATION 3L PARTIALLY READY.** The qualified 32- and 200-protocol-peer
Local/Node workload passes recipient service, gameplay budgets and lifecycle
checks. This narrows KI-006; it does not qualify physical 200/500 clients or
close all real-client scale evidence. No production correction is retained.
The historical 200/50 diagnostic remains unqualified and failing. No 3M.

## Recipient service qualification (2026-09-13)

### Source, workload and metric

Start: `049095905528cf1c8abcbcd2507da4f162ecf3b8`, branch
`foundation/3l-content-availability`. Production source is unchanged by this
task. Diagnostic commit `8e7000b30` and workload commit
`566ecac7d16da5f067d867c1f9fc5709337821b5` contain the verified test source.
[The canonical workload](RecipientServiceWorkload3L.md) records the
pre-run arithmetic, exact unchanged limits, commands and deployment assumptions.
Eight root-motion Characters each have eight recipients; one real gameplay
client supplies <=15 Event/ACK exchanges/s, <=10 sequential RPCs/s and <=0.5
actions/s. Shared 512-object content loads, remains resident, evicts and reloads.
The real client alternates movement direction so the longer trial stays on its
resident floor. Other recipients validate protocol/hierarchy/state; they are
not additional Player engines.

[DueServiceFixture.hpp](../../tests/DueServiceFixture.hpp) analyzes bounded
records after the timed phases. The origin is FrameBegin of the authoritative
desired due tick, not the preceding observation. Full ObjectId, recipient
generation, control epoch, authoritative tick and state sequence join snapshot,
production, scheduler acceptance and observation. ClientHandled is reported
separately for the one live client. Forced publications have separate
built-to-observation latency and acceptance/observation conservation. No pointer
identity, new queue, lane, priority, rate, wire change or production logging is
introduced.

Future cadence forecasts, pre-discovery cadence reschedules, unchanged
suppression, explicit recipient retirement and unresolved work are separate
counters. A reschedule cannot satisfy confirmed due work. Overflow, decoding,
missing origin/identity, missing accepted observation, unfinished RPCs or an
over-limit workload fail qualification. The record cap is 4,194,304 (320 MiB
reserved); analysis maps are bounded by that finite record set. The original
load-only detailed trace remains available with its original 1,048,576 cap.

The old raw gap/tick `phaseHealthy` guard remains intact. Qualified mode also
reports `serviceHealthy`, using the accepted RPC/Event/action targets and zero
Character scheduler rejection. It additionally requires complete due-service
and all-interval workload accounting. Raw-gap failures are visible diagnostics,
not silently turned into passes or universal Character latency promises.

### Qualified matrix

The authoritative matrix receipt is worker artifact
`C:\Sandbox\Codex\Artifacts\due-service-3l-20260913-v4` on `dockerbox`.
`source.json` verifies 564 source files with zero mismatches. MSVC Release uses
the existing incremental Node-linked benchmark and native test builds, bounded
to four compiler jobs, with compilation separate from timed runs. Node remains
read-only at `c1f60b179c1aafccc233babbebde18925ea4f32e`. Its existing real-TLS
provider harness drives the same primary-repository workload.

Every row exits 0 with five passing normative service phases and
`[Content:ServiceQualification] complete=1`:

| Provider / peers | Ordinary due samples | Due→observation p50 / p95 / p99 / max, ms | Forced recipient states | Raw gap max, ms |
| --- | ---: | --- | ---: | ---: |
| Local / 32 | 46,721 | 0.8701 / 3.4937 / 4.3719 / 72.5193 | 880 | 202.422 |
| Node / 32 | 46,977 | 0.8455 / 3.4447 / 4.3100 / 72.2049 | 888 | 203.113 |
| Local / 200 | 47,253 | 2.3927 / 7.8542 / 19.6125 / 76.4958 | 880 | 340.775 |
| Node / 200 | 46,935 | 2.4339 / 8.1177 / 20.2065 / 65.6693 | 872 | 320.977 |

All eight Characters carry root-motion tracks, so root due distributions equal
the ordinary Character distributions in this workload. All rows have zero
missing produced states, missing accepted observations, unresolved due work,
identity/origin failures, scheduler rejections and selection lateness ticks.
No Character relationship retires during these qualified phases. The 64 future
relationships at each end are scheduled beyond the last measured tick, not
missing samples. Ordinary+forced accepted observations are respectively
47,601 / 47,865 / 48,133 / 47,807. Maximum observed fanout is exactly eight.
The 200-peer load/reload raw cadence guards still fail; bounded due service and
normative gameplay service pass.

All-window reliable demand includes the 32-B adapter allowance exactly once,
every forced Character recipient, actions and replies, with GRPL charged to
structural admission separately. Across the matrix the maximum required peer
burst is 4.67219 messages / 558.889 B versus 16 / 20,480 allowed; the maximum
global burst is 65.862 messages / 9,711.33 B versus the stricter N=32 limits
128 / 163,840. Every phase and the combined timeline pass both direction
buckets. The largest encoded Remote frame is 70 B, and RPC overlap is one,
with zero requests pending at the end. This qualifies the combined small-frame
mix; existing upper-size/concurrent GNS evidence remains a separate retained gate.

### Gameplay and structural service

There are 500 completed RPCs per provider/scale trial, 100 per phase, with zero
timeouts/errors. Values below are the **worst of the five phase percentiles**,
not a fabricated pooled distribution. Action phases have only about seven
samples each; the maximum is more informative than a tail-percentile claim.

| Provider / peers | RPC p50 / p95 / p99 / max, ms | Event ACK service-gap max, ms | Action result max, ms |
| --- | --- | ---: | ---: |
| Local / 32 | 33.454 / 36.520 / 61.512 / 71.281 | 144.590 | 58.143 |
| Node / 32 | 33.459 / 36.731 / 60.317 / 61.894 | 143.371 | 37.142 |
| Local / 200 | 33.394 / 39.549 / 91.219 / 95.346 | 224.882 | 39.240 |
| Node / 200 | 33.363 / 40.565 / 90.674 / 95.784 | 216.237 | 79.964 |

All are inside RPC 150/250/500 ms and Event/action 250 ms. There are no action
submission failures or rejections. The four trials submit 35/36/36/35 action
requests and receive the same number of results. Event submissions/ACK egress
are 1,050/1,050, 1,055/1,054, 1,052/1,051 and 1,048/1,048; up to one exchange
is at the final observation boundary, not claimed as a completed latency sample.
Per-phase matched client ACK counters and latency samples are retained in logs.
Requests/responses each use 47,600 complete-message bytes per trial. Actual
Event ingress bytes are 105,202 / 105,711 / 105,416 / 105,011, including adapter
allowance. No application broadcast fans these Remotes out beyond the caller.

Aggregate root request/commit counts are exactly 17,128/17,128,
17,232/17,232, 17,312/17,312 and 17,328/17,328. The animation runs at host
cadence; requests are emitted only for nonzero root deltas. Distinct transmitted
authoritative states number 29,387 / 29,502 / 29,233 / 29,155, with staggered
recipient cadence; these are not eight times a single globally shared 20-Hz
producer. The pre-run conservative 480-snapshot/s bound accounts for this.

All 512-object load/resident/evict/reload checks converge. Load / eviction /
reload maximum convergence is 253.587 / 85.513 / 238.881 ms Local-32,
240.338 / 89.729 / 235.116 Node-32, 1,321.070 / 688.850 / 1,302.610 Local-200,
and 1,300.990 / 675.331 / 1,326.330 Node-200. The steady resident phase retains
the same semantically validated object population. Reload reuses one provider
acquisition, makes a second admission, and assigns fresh full ObjectIds; old
root/part identities and spatial projections do not survive eviction.

Per-tick planning work high-water remains 65,536 and 3J selection 8,192. Reliable peer/global
backlog high-water is 154,883 / 2,478,128 B at 32 and 154,883 / 4,914,996 B at
200, inside unchanged Qp/Qg. Credit deferrals are visible; accepted structural
traffic is not treated as free gameplay. Live journal reader minimum margins
are 10,249 / 10,233 / 10,249 / 10,249 entries, with no lost live history or
journal failure. The probe validates the DataModel journal scope before
subtracting cursor sequences. Phase-end journal backlog returns to zero.
Retained planning-record high-water is 179,104 at 32 and 1,344,856 at 200;
resident-phase retained records are 18,304 and 146,488, respectively. These are
retained dependency/planning records, not the per-tick work budget or raw-journal
readers. Per-peer record high-water is 5,052 / 6,040. Credit deferral counts are
197/219/175/167 across the four trials; backlog deferrals are zero.

### Work ownership and retention

| Provider / peers | Server work per tick p50 / p95 / p99 / max, ms | Client work per tick p50 / p95 / p99 / max, ms |
| --- | --- | --- |
| Local / 32 | 0.6056 / 3.1778 / 3.7244 / 40.2800 | 0.1641 / 0.2724 / 0.3987 / 36.5480 |
| Node / 32 | 0.5898 / 3.2023 / 3.8320 / 41.7288 | 0.1613 / 0.2683 / 0.4250 / 35.6553 |
| Local / 200 | 2.0278 / 7.2815 / 14.2608 / 51.4290 | 0.1530 / 0.2704 / 0.4037 / 40.2428 |
| Node / 200 | 2.0425 / 7.4921 / 15.1034 / 57.9889 | 0.1514 / 0.2620 / 0.3983 / 37.0893 |

At 200 peers, total server/client/199-observer/pacing/other milliseconds over
the joined five-phase timeline are Local 14,265.1 / 806.112 / 1,278.78 /
53,945.8 / 914.408, and Node 14,340.2 / 785.485 / 1,268.18 / 53,723.6 /
901.835. Observer processing is explicit shared-fixture cost. These are not
200 parallel Player event loops, GPU presentation, or network latency claims.
The one live client's due→ClientHandled max is separately recorded (59.883 ms
for Local-200); observer timestamps precede live GameSession semantic handling.

An additional post-run-only summary adds actual callback-start intervals and
Character/Remote semantic-handler gaps. `due-service-3l-20260913-v5` repeats
Local/Node 200 at final diagnostic source; the timed workload, collection,
qualification predicates and production source are identical to v4. Both
repeats exit 0 with complete due/budget/shutdown accounting. Due p50/p95/p99/max
is Local 2.4700/7.7658/19.8310/83.0507 ms (47,082 samples) and Node
2.4490/7.8414/19.4072/69.0071 ms (46,946 samples), with zero missing, unresolved,
retired, late or rejected qualified service. These repeats supplement the v4
matrix; they do not replace its original measurements.

| Final 200-peer repeat | Client callback interval p50 / p95 / p99 / max, ms | Character handler gap max, ms | Remote handler gap max, ms |
| --- | --- | ---: | ---: |
| Local | 16.5790 / 22.0282 / 23.2615 / 85.0048 | 174.366 | 198.616 |
| Node | 16.5709 / 22.1206 / 23.5874 / 85.5387 | 165.877 | 195.180 |

These gaps are cadence/service diagnostics; mixed Remote handler gaps are not
RPC RTTs or substitutes for matched Event ACK measurements. In the v4 live
client, per-tick structural preflight maxima are 9.3426/8.2778/10.1577/11.1108
ms and live-apply maxima 10.5537/9.7469/12.7090/10.1740 ms across the four
provider/scale rows. These phase subcosts overlap encompassing client work and
must not be added to it as separate owners.

RSS high-water includes the trace and is 224,878,592 / 232,206,336 /
444,719,104 / 448,139,264 B. It is not a leak detector. Before final shutdown,
one resident provider unit/cache is intentional. After shutdown all four cases
report zero connections, live journal readers and admission peer owners;
reserved bytes equal accepted plus rolled-back bytes. The empty accountant's
336 logical bytes are its fixed object size, not a remaining reservation.
Content requested/acquiring/prepared/resident units, completion reservation,
completed/decoded/cached bytes, retained records and resident package objects
are all zero. Destruction of the replication coordinator ends its planner,
pending-group and retired-catalog ownership; individual private container
capacities after destruction are not separately measured.

### Historical comparison and remaining scope

Historical Local/Node load raw gaps remain **655.446 / 647.402 ms**, with the
unchanged guard failing. Yet load due→observation p50/p95/p99/max is
8.7686/24.1672/52.5131/86.3973 and 8.7404/25.2325/51.2124/87.3489 ms.
Each load phase has 41,651 on-time ordinary states and 3,640 forced recipient
states, all accepted and observed, with zero unresolved due records. The
extra post-phase consistency tick is included in this new trace; the older
41,480/45,120 counts describe the earlier load-only trace interval.

Required load message bursts are ingress peer 74.7083 / 74.4025 and egress
peer 88.9399 / 88.7773 versus 16, and global egress 445.564 / 460.455 versus
256. Whole-trial global egress needs 635.320 / 636.112 messages. Byte buckets
still pass. Historical load is deliberately unqualified; it cannot establish
a supported-scale starvation defect. Prior joined worst-interval server/client/
observer attribution remains preserved below.

The later historical reload retains **199 unresolved schedule forecasts** in
the new all-phase diagnostic, despite zero missing produced/accepted state keys
and no selection lateness. Those forecasts lack a terminal resolution in this
trace; GRPL retirement did not resolve them. Do not claim complete whole-trial
cadence closure or infer a production drop from them. Resolving that unqualified
stress-only diagnostic is separate from the four completely accounted qualified
trials. Historical `complete=0` and exit 1 are preserved.

Official one-Player Local/Node service and real 32-client GNS qualified/
overload/recovery evidence below are reused within their unchanged production
scope. The new fanout matrix has one actual client per case. Full fanout with
32 real clients, real 200/500 clients, a funded high-scale physical profile,
actual NIC/WAN service lower bounds and rendered/GPU latency remain **not
measured**. The simulator uses R=8 MiB/s, A=256 or 1,600 MiB/s, backend=2R,
S=0.75 and 25% gameplay reserve; these mathematical reservations are not proof
that the product selected or funded such a physical deployment.
Headless client semantic application is the current qualification boundary;
there is no additional numeric GPU-present gate to invent for Foundation 3L.

**KI-006 disposition B: narrow, keep OPEN for remaining physical/real-client
qualification.** The qualified simulator workload has no demonstrated
production recipient-service defect. Do not mark Foundation 3L Ready from it.
Final acceptance is not yet eligible to close those missing gates. Exact next
task: select the supported funded deployment/client profile, then run this
qualified Character/root/Remote/action/content mix with actual GameSession
clients on that profile, preserving existing service targets and ownership
checks. Do not assume physical 200/500 support, merge, or begin 3M.

### Validation receipt

Focused MSVC GameSession/diagnostic tests pass. Clang 19 ASan/UBSan/LSan passes
both `GameSessionTests` and `ContentScaleUnwindTests` (2/2; 61.83 and 43.11 s),
with no new suppression. The final header is covered by the Linux source
manifest `v4/linux-source.json`, identical to `v5/source.json`; MSVC rebuilds
the final benchmark and both 200-peer repeats pass. The v4 manifest remains
the immutable source receipt for the four-case original matrix. The only v4→v5
source difference adds the post-run callback/handler summary described above.

The existing remote incremental caches are preserved. Linux CMake regeneration
took 383 seconds; this was build preparation, outside timed qualification.
An earlier obsolete diagnostic build was stopped before final validation;
it is not counted as a pass. No worker process remains running for this task,
and the unrelated Docker workloads remain untouched.

The isolated documentation build passes 19 pages and search/sitemap generation
using Node 24.19.0. All 47 checked relative Markdown targets resolve; the old
KI-006 heading anchor is retained. The build uses the unchanged committed Astro
inputs, excluding the concurrent morphology edits. SEC-3L-001 and KI-008
production evidence is reused within its original scope. Published-source CI
status is reported with the final source-control receipt, not assumed green.

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
