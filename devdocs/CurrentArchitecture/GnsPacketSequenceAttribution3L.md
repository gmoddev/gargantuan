---
status: corrected-with-scoped-validation
owner: runtime-networking
last_verified: 2026-09-13
---

# KI-008 packet sequence attribution

Work starts at `46da288a3f3ce56ffbd88b42c45f26645825d096` on
`foundation/3l-content-availability`. The accepted gameplay contract, byte
admission, reliable ordering and packet validation remain the constraints.

## Demonstrated cause and dependency correction assessment

Ownership **E: pinned GNS service-loop defect**. The receive-side packet-number
guard is a consequence, not the initiating defect. `Thinker_ProcessThinkers`
refreshes the current time before selecting each timer. Under concurrent send
pressure, connections reschedule themselves into the same continuously growing
pass. The service thread does not return to `PollRawUDPSockets` until the queue
ceases to be due or its 10,000-iteration fallback fires. It sends/retransmits
while starving its own receive/ACK processing. UDP socket queues fill and the
kernel drops packets; packet numbers continue advancing on actual sends.

The unchanged canonical Linux fixture fails 3/3, without sanitizer diagnostics.
Kernel `RcvbufErrors` deltas are 3,136,032, 3,194,983 and 1,702,682; observed socket
receive queues reach 524,288 bytes. GNS's supported debug callback confirms a
5,372.8-ms service-thread lock hold, 10,000 connection timer invocations and
618,678 packet sends before receiving again. The first recorded close is client
backend handle 3504858724: gap 17,659, `0bfe->50f9`, local reason 5001. A later
pass holds the lock for 8,027.1 ms and sends 984,564 data packets. These are
backend service-thread operations, not application callback polling.

A direct supported-GNS-API control links no Gargantuan Engine, adapter,
GameSession or admission implementation. One finite 524,288-byte reliable
message to each of 32 peers triggers the same 10,000-timer starvation, with
1,217,930 packet sends in an 8,765.4-ms hold, followed by a second such pass and
timeout. Its final guard differs from the canonical packet-gap close; it proves
the initiating service defect independently, not an identical terminal trace.
The same initial control passes at 1/2/4/8/16 peers. These are timing-dependent
observations, not a universal peer threshold or a proposed workload reduction.

The pinned revision is `2cb93a06350bb065db53abdb0d87cf297e0bfd34`. Current upstream
`a424b7db649438acafb60c99cae6667587c42732` retains the timer-loop behavior;
[the source](https://github.com/ValveSoftware/GameNetworkingSockets/blob/a424b7db649438acafb60c99cae6667587c42732/src/steamnetworkingsockets/steamnetworkingsockets_thinker.cpp)
and exact file history were inspected. May 2026 changes concern TSan annotations
and moving a method out of line, not receive fairness. Issue 307 concerns general
performance warnings and issue 315 concerns P2P setup; neither establishes this
root cause or supplies an exact fix.

The narrowly justified correction keeps the pin and existing scheduler, but
captures one eligibility cutoff at entry to `Thinker_ProcessThinkers`. Timers
scheduled after that cutoff wait for the next socket-service pass. Each callback
still receives a fresh timestamp: freezing callback time would risk reversing
time relative to packet processing. No buffer, rate, packet number, ACK encoding,
wire format, ordering domain, API, ABI or connection architecture changes. The
existing 10,000-iteration emergency guard remains. This is a local compatibility
patch, not an upstream backport or a broad dependency upgrade.

## Reproduction and first invalid state

The canonical command is
`gargantuan_game_session_real_transport_tests --reliable-workload-32-structural`.
It creates 32 real peers, first services eight qualified RPC producers, then
offers six 24,576-byte structural property changes per frame for 480 frames,
with 16 concurrent 3,072-byte RPC payloads per eligible producer at the existing
40-frame cadence (and a final overload burst). Frames wait at least 16.667 ms;
actual elapsed time, not an assumed 60 Hz, determines offered throughput.
The original deployment profile uses R = 8,388,608 B/s, A = 268,435,456 B/s,
N = 32, server backend min=max 16,777,216 B/s, structural share 75%, peer/global
credit caps 524,288 B, peer backlog limit 1,048,640 B and global backlog limit
17,303,552 B. Clients retain their inherited backend rate. Gameplay admission
headroom is 262,176 B, distinct from the accepted qualified 20-KiB workload burst.
The original backend buffer settings are used.
There is no injected packet loss, reconnect, or fixed random seed.

Three unchanged MSVC Release runs exit 1 after 42.445, 42.922 and 42.685 seconds.
The middle run exposes packet gaps including 32,357 (`04f8->835d`). The first
and third instead first report a structural frame exceeding its limit: those
failures must not be relabeled packet-sequence evidence. Three unchanged Linux
Clang 19 ASan/UBSan/LSan runs exit 1 after 48.743, 49.114 and 37.153 seconds,
with packet-gap closes and no sanitizer report. The failure is load- and
timing-sensitive, occurs on both platforms, and does not require sanitizers.
The exact terminal reason and time are not deterministic.

Pinned `steamnetworkingsockets_connections.cpp` expands a received 16-bit packet
number relative to the last full received number, authenticates the packet,
then rejects a forward gap greater than 16,384 with local reason 5001. The
rejection is receive-side, before normal reliable reassembly processing.
`SendEncryptedDataChunk` consumes a packet number on actual sends. The debug
trace shows sender progression to `50f9` while the last received ACK is `050d`,
then the receiver's `0bfe->50f9` rejection. This is actual progression with
receive loss, not evidence of a fabricated number or application message ID.
The diagnostic alone does not prove wraparound, stale packets or handle reuse.

The first incorrect scheduling decision is admitting a timer rescheduled after
the current dispatch pass began back into that same pass merely because wall
time advanced. Repetition prevents the next UDP/ACK read pass. In the canonical
debug trace, backend service thread hash `2373736102593145204` reports the
5,372.8-ms hold at steady-clock count `510247881356054`, then the first close
at `510247931135755`. GNS reports its existing 10,001-iteration emergency stop.
This is a starvation defect in the existing timer dispatcher, rather than a
missing application `RunCallbacks` call: the application cannot service the
backend's sockets while that backend holds its global lock.

## Isolation, pressure and lifecycle

| Control, original GNS | Observed result |
| --- | --- |
| 16-peer combined GameSession fixture | Pass, 187.704 s |
| 32-peer gameplay-only fixture | Pass, 41.221 s |
| 32-peer structural-only fixture | Fail, 51.016 s; gap 25,345 (`0e99->719a`) |
| Direct GNS, paced 147,456-byte messages, 1/2/4/8/16/32 peers | All pass |
| Direct GNS, one 524,288-byte message each, 1/2/4/8/16/18/20 peers | All pass |
| Direct GNS, one 524,288-byte message each, 24/32 peers | Timer starvation and failure |
| Direct GNS, 32 peers, one 262,144/294,912-byte message each | Pass |
| Direct GNS, 32 peers, one 393,216-byte message each | Timer starvation and failure |

The smallest observed failing direct controls total 12 MiB: 24 peers × 512 KiB
or 32 × 384 KiB. This is an observed boundary, not a proven mathematical minimum.
The direct control proves the same initiating starvation; its eventual timeout
or authentication desynchronization is distinct from the exact canonical guard.
Use `gargantuan_gns_service_reproducer 24 524288 1` or `32 393216 1` to retain
those finite controls. `--ki008-attribution` additionally permits bounded
`KI008_PEERS`, `KI008_GROUPS`, `KI008_BYTES` and `KI008_GAMEPLAY` environment
controls; none changes ordinary qualification defaults.

Linux OS samples every 250 ms observe first receive-buffer drops at 20.115,
20.476 and 19.827 seconds. Outgoing UDP totals are 3,239,463 / 3,286,356 /
1,786,399; received totals are 103,431 / 91,373 / 83,717. The socket queues reach
524,288 bytes (Linux includes kernel accounting), consistent with GNS's
256-KiB raw UDP buffer request, distinct from its 16-MiB message queue setting.
Maximum observed per-socket drop counts are 101,215 / 130,081 / 81,137.
Process RSS maxima are 1,089,679,360 / 1,120,059,392 / 1,059,569,664 bytes.
MSVC RSS maxima are 269,836,288 / 265,424,896 / 247,951,360 bytes. These sampled
process figures are not complete allocation or instantaneous recovery peaks.

The first failing backend handle is created and reaches Connected before its
local problem close. Engine peers retain generation 1; no reconnect or churn
occurs before failure. Backend failure precedes adapter teardown. A complete
cross-indexed backend-handle-to-engine-peer lifetime timeline is **not measured**.
No stale callback, send-after-close or handle reuse is demonstrated as the
initiator. The direct GNS control excludes Gargantuan connection/admission
ownership entirely. Existing lifecycle regression suites remain necessary;
absence of a lifecycle trigger here is not universal lifecycle proof.

Application byte credit bounds accepted message work, but cannot cap autonomous
backend retransmission and packet-number advancement while ACK reads starve.
Matching GNS SendRateMin/Max is supported by the pinned configuration contract;
the configured rate is not itself evidence of profile misuse. Exact per-peer
credit, backend pending bytes, queue residence and send-return history at the
first failed packet are **not measured** without a timing-changing observer.
A heavier status-sampling variant materially changed execution and was stopped;
it is not passing qualification evidence. Socket pressure and backend dispatch
starvation are directly measured; first-packet loss attribution to a particular
socket and packet capture are **not measured**.

## Correction validation

The build applies the correction only to the verified pinned thinker source,
using its full normalized SHA-256 and an idempotent replacement. Unexpected
source changes fail configuration. A private dependency regression uses two
timers whose callbacks each reschedule themselves and take longer than their
rescheduling delay. Every poll must dispatch each once and use fresh monotonic
callback timestamps. The identical sanitizer-instrumented test object fails
against the preserved original GNS library and passes against the corrected
library. No new sanitizer exclusion is added.

The first corrected, otherwise unchanged Linux canonical run exits 0 in
242.246 seconds, completes every accepted RPC without error, converges structural
state for every peer, and reaches ordinary recovery. Direct previously failing
controls pass at 32 × 512 KiB, 32 × 384 KiB and 24 × 512 KiB, with all messages
received and drain times 0.252 / 0.161 / 0.208 seconds respectively. The direct
fixture checks message sequence counters; full payload comparison belongs to
the canonical GameSession RPC fixture.

The cutoff addresses timers with future deadlines that become due while the
pass runs. It does not remove the emergency guard for pathological callbacks
that repeatedly reschedule an already-past deadline or ASAP sentinel.

The final canonical fixture adds bounded, in-memory observations of actual
journal readers and admission metrics without per-message backend polling. It
keeps all original offered frames, payloads, peer counts and deadlines. Both
platforms complete qualified → overload → qualified recovery with exit 0:

| Measurement | MSVC Release | Clang 19 ASan/UBSan/LSan |
| --- | ---: | ---: |
| Entire process duration | 197.637 s | 266.437 s |
| Actual 480-frame overload duration | 166.477 s | 231.183 s |
| Overload accepted/completed RPCs | 6,656 / 6,656 | 6,656 / 6,656 |
| RPC errors | 0 | 0 |
| Worst per-peer overload p95 / p99 / max | 3,320.62 / 3,320.71 / 3,320.74 ms | 3,792.46 / 3,828.36 / 3,828.41 ms |
| Drain after offered load ends | 1,726.10 ms | 1,158.79 ms |
| Last peer structural convergence | 1,674.74 ms | 1,062.09 ms |
| Recovery worst peer p95 / p99 / max | 75.07 / 77.86 / 77.86 ms | 80.04 / 83.37 / 83.37 ms |
| Structural reserved / accepted bytes | 2,268,777,992 / 2,268,777,992 | 2,269,096,300 / 2,269,096,300 |
| Credit / backlog deferrals | 404 / 9,995 | 9 / 937 |
| Peer / global credit high-water | 524,288 / 524,288 B | 524,288 / 524,288 B |
| Peer / global reliable backlog high-water | 682,598 / 8,913,449 B | 590,848 / 8,909,859 B |
| Maximum admission wait | 2,250.187 ms | 1,751.037 ms |
| Maximum application Step interval | 936.67 ms | 1,702.14 ms |
| Observed pending Enter/Leave high-water | 0 | 0 |
| Maximum raw journal entries required | 58 | 18 |
| Minimum journal margin / 16,384 | 16,326 | 16,366 |
| Owner / sequence at minimum margin | peer 18:1 / 1901 | peer 6:1 / 1179 |
| Maximum observed required-entry age | 4,448.81 ms | 1,654.28 ms |
| Journal recovery backlog | 0 | 0 |
| Peer journal owners / connections after disconnect | 0 / 0 | 0 / 0 |
| Maximum sampled process RSS | 268,525,568 B | 1,118,711,808 B |

The minimum margin exceeds the earlier single-peer 16,210-entry result, while
neither run establishes a universal production margin. Required-entry age is
sampled after Step and can underestimate retention by one service interval.
The Step interval is application cadence, not a direct measurement of the
corrected GNS UDP poll gap. Backlog and credit high-waters are cumulative and
must not be interpreted as residual backlog during recovery. Zero pending
Enter/Leave does not mean zero property backlog: this load mostly mutates
already-known objects. Planning and complete-group selection checks retain
65,536 and 8,192 limits. No Known mutation or admission implementation changed.

The corrected Linux run still records 80,533 kernel receive-buffer drops out
of 2,798,923 UDP sends, versus millions in the original runs. The maximum sampled
per-socket drop count is 3,724. Reliable retransmission now completes despite
loss. The correction does not promise a loss-free UDP socket or qualify the
seconds-long overload latency against ordinary RPC targets. All 32 peers receive
at least 70,891,260 B (Windows) / 70,896,588 B (Linux) of structural data and
converge. The eight active ordinary recovery peers each complete 48 RPCs.

Explicit shutdown can report `Game session stopped`, `Gargantuan local shutdown`
or rejection of a scheduler submission while the remote has already closed.
These occur after the passing recovery measurements, during deliberate cleanup;
the fixture verifies zero live connections and peer journal owners afterward.
No such close occurs during the measured workload. A destruction injection in
the middle of this exact aggregate pressure case and instantaneous recovery
allocation peak are **not measured**.

## Validation receipt and remaining gates

Both complete canonical suites exit 0: MSVC Release passes 12 affected CTests,
and Linux Clang 19 ASan/UBSan/LSan passes 10 initial affected CTests, followed by
the explicit KI-007 replication-relevance suite. GameSession's default suite
includes late-produced Character/Remote handoff, byte-admission rollback and
lifecycle tests. Both full eight-phase single-peer workloads and RPC-only
32-peer cases pass. Both production-admission matrices pass all 12 cases, and
`AnalyzeProductionByteAdmission.ps1` confirms complete delivery and unchanged
credit/profile/probe bounds. No new sanitizer suppression is used.

Official Windows Local and Node, using rebuilt Player/Server distributions,
pass the existing near-max 512-object, 1,048,197-byte, eight-cycle content fixture
with 100 small RPCs each, then 100 upper qualified 16-KiB RPCs each. Small cases
complete in 29.730 s total and upper cases in 292.005 s total. No sibling source
is modified. The build uses four jobs and persistent worker caches; latency
cases run after compilation. The 19-page Astro documentation build passes with
the existing missing-404-entry warning.

| Official case | RPC p95 / p99 / max | Errors / timeouts | Event maximum ACK gap |
| --- | ---: | ---: | ---: |
| Small Local | 35.775 / 73.230 / 101.332 ms | 0 / 0 | 101.512 ms |
| Small Node | 48.250 / 72.878 / 75.141 ms | 0 / 0 | 104.202 ms |
| 16-KiB Local | 98.236 / 99.298 / 100.032 ms | 0 / 0 | 127.423 ms |
| 16-KiB Node | 83.950 / 84.846 / 85.317 ms | 0 / 0 | 101.901 ms |

The final source receipt compares 561 native/runtime/test/CMake files. Final
help text adds the attribution option to usage without changing workload logic.
Windows sources match the final manifest; the Linux fixture is rebuilt for
that final usage-only change. The patch is checked for repeat-application
idempotence and rejection of unexpected source changes in an isolated copy.
Raw measurements remain in `build-3l3-worker/evidence/ki008` and
`build-3l3-worker/evidence/ki008-20260913`; worker originals are under
`C:\Sandbox\Codex\Artifacts\reliable-workload-20260913\ki008` and
`C:\Sandbox\Codex\Artifacts\ki008-20260913`. The task Linux container and
incremental Windows/Linux build/dependency caches are preserved.

Starting source `46da288a3f3ce56ffbd88b42c45f26645825d096` has terminal success
for [Native CI](https://github.com/gmoddev/gargantuan/actions/runs/34741243563)
and [GNS sanitizer CI](https://github.com/gmoddev/gargantuan/actions/runs/34741243527).
Publication CI for this correction is a separate current-source check. The GNS
workflow now requires the unchanged combined 32-peer overload/recovery fixture,
alongside the timer regression and existing qualification modes.

KI-008 is corrected in this scoped evidence; the old size-limit terminal seen
in two original MSVC runs is not independently attributed or claimed fixed.
It does not recur in corrected validation. Broader real-client 200/500-peer
service, aggregate Event/action fanout, physical-link capacity, rendered client
qualification and independent current-source security remain open. Those
unexecuted workloads are **not measured**; deterministic 200/500-peer byte
accountant evidence is not a substitute. Next closure work is current-source
security and full client/scale gameplay qualification under the accepted
contract. **B — FOUNDATION 3L PARTIALLY READY; no 3M.**
