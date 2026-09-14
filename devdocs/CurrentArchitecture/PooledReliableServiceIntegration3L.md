---
status: production-qualification-receipt
owner: runtime-networking
last_verified: 2026-09-14
related_code:
  - src/network/ReliableByteAdmission.hpp
  - src/network/PooledReliableServiceFeedback.hpp
  - src/network/GameSession.cpp
  - tests/PooledReliableServiceProductionFixture.hpp
  - src/network/ReplicationCoordinator.cpp
  - tests/NameCoalescingFixture.hpp
  - tests/ReliableGameplayWorkloadFixture.hpp
  - cmake/gns/ApplyReliableServiceFeedback.cmake
  - tests/ReliableServiceFeedbackFixture.hpp
related_adrs:
  - ../FutureArchitecture/Foundation3LServiceCoverageDecision.md
---

# Foundation 3L pooled production integration receipt

## Current disposition

**IMPLEMENTATION AND WORKER QUALIFICATION COMPLETE.** The Name correction at
`555354bb5031f0e64b6594ce6606558651805314`, with recovery measurement corrected at
`ab3f0d61dfceac5e3e4935049b7333e4cebb7a8a`, is based on published
`8c41dd2ec5388ddd73648ae0b44613071da7df22`. It retains the reconciled service
recovery/convergence distinction and qualifies explicit production
`POOLED_SERVICE` within the measured Engine/loopback scope **only when both
required hosted workflows on the consuming published source are terminal green**.
The hosted gates and worker evidence are recorded below; running CI is not a pass.

After that publication gate, the exact next task is the **separate funded
32-actual-client Local/Node physical qualification** against the accepted Option C
profile. That physical evidence is **not measured** here. KI-006 remains **OPEN**,
Foundation 3L **B — PARTIALLY READY**, and 3M **BLOCKED / NOT STARTED**. No merge,
physical client run or 3M work was performed.

### Known-object Name correction (2026-09-14)

The [3J journal contract](ReplicationFoundation3J.md#journal-mutation-and-cancellation)
now coalesces native Name only in the current catalog's suffix after every
non-Name barrier. It reads `Publication.Name`, not the generic property map or
a separately cached value. Existing per-Known-object accepted metadata captures
the exclusive source boundary; only scheduler acceptance installs coverage and
commits source progress. Failed/deferred/discarded preparation commits nothing.
Closed segments, lifecycle, hierarchy, references/nil, Attributes/tags and
resnapshot semantics retain their existing ordering. The conservative global
barrier may forgo otherwise safe optimization under unrelated structural churn.

The first MSVC Release run passes all four affected suites (networking contracts,
scheduler, relevance including the focused Name fixture, and legacy GNS session;
10.88 seconds). The first canonical pooled run passes the existing fixture:

| Case | Previous journal remainder at 20 s | New final convergence after demand | New final journal remainder | New accepted structural bytes |
| --- | ---: | ---: | ---: | ---: |
| Structural overload | 7,248 | 1,629.12 ms | 0 | 4,342,072 |
| Mixed overload | 7,248 | 1,668.88 ms | 0 | 4,342,072 |

The expanded MSVC run passes the four suites again (9.28 seconds) and all eight
pooled workload cases. Structural/mixed overload respectively recover ordinary
service in 113.403/1,120.29 ms and converge in 1,635.78/1,709.31 ms against a
20,847.1-ms retained-work bound. Each retains 2,016 raw records at cessation,
conservatively represented by 32 current Names / 789,772 complete-message bytes
(singleton framing upper bound), and has zero journal records at 20 seconds.
Each emits 208 total transitions (32 materializations plus 176 Name updates) in
12 frames and coalesces 7,504 Name records. Recovery RPC/Event/action probes are
39.147/39.134/39.143 ms and 37.174/37.166/37.167 ms. The full run conserves
8,727,506 accepted/retired structural bytes, terminal release and outstanding
debt zero, with one grant and 393,866-byte debt high-water.

The general cessation inventory is explicitly an upper bound: closed Name
segments keep historical values, suffix Names use current authoritative state,
singleton framing overestimates batching, and unrelated records are charged G.
It is measurement-only scratch bounded by the existing journal window. The
20-second observation keeps the fixture running after convergence; `drain_ms`
therefore measures observation duration, while `convergence_ms` is the first
actual convergence. Maximum Remote interarrival gaps include intentional idle
time and are not the eligible-grant fairness metric. Qualified workload targets
are unchanged; deliberate mixed overload can exceed ordinary gameplay limits,
but its subsequent recovery probes must satisfy them.

These initial runs are retained for provenance. The final worker measurements
and publication gates below supersede them for the corrected executable source.

#### Recovery-probe correction

At executable checkpoint `555354bb5031f0e64b6594ce6606558651805314`, Linux
ASan/UBSan/LSan passes all eight focused CTests (43.41 seconds), the 42 pooled
model/hardening cases, 19 feedback-model cases, eight production pooled groups
and ten native-feedback fixtures. Canonical structural/mixed cases also recover
service in 120.589/1,113.61 ms and converge in 1,985.73/1,932.72 ms with zero
journal remainder at 20 seconds.

The complete first Linux workload nevertheless exits 1: the new one-shot probe
in gameplay-only overload receives a local `RequestAction` refusal, so the
fixture never observes a successful action even though its remaining 20-second
recovery window is unused. `PredictedCharacterNetwork::RequestAction` may refuse
admission while prediction is suspended after history overflow; reconciliation
clears that state. An early unaccepted probe is not evidence that service cannot
recover by the deadline. The failed log is retained as `linux-attempt1/pooled-workload.log`.

The corrected measurement sends at most one RPC/Event/action probe group per
second while recovery is unproven, records every attempt and local action
refusal, and still fails if no qualified group completes by 20 seconds. All
accepted RPC/Event/action errors and ordinary demand-phase action refusals remain
failures. A successful recovery group must meet 150-ms RPC and 250-ms Event/action
bounds with cleared debt/grants/reliable queues. No production admission,
prediction behavior, workload rate, or service-recovery deadline changes.
Final-source worker validation of this measurement correction passes below.

#### Executable-source closure at `ab3f0d61d`

The corrected MSVC workload passes all eight cases. Structural/mixed service
recovery is 113.472/1,112.43 ms; structural convergence is 1,637.63/1,681.98 ms
against the unchanged 20,847.1-ms bound. Both have 2,016 raw records at cessation,
32 retained Names, 7,504 coalesced records, 208 transitions / 12 frames and zero
journal records at 20 seconds. Each recovery needs one probe with no local
action refusal. Structural/mixed recovery RPC/Event/action times are
37.405/37.397/37.405 ms and 38.109/38.099/38.100 ms.

The current per-case client decode/apply CPU is 31.878/42.814 ms for structural
overload and 30.714/42.806 ms for mixed overload. The old structural run consumed
82.556/114.733 ms of decode/apply CPU while still incomplete at 20 seconds; it
does not provide CPU to full convergence. These are client decode/application
counters, not server tick CPU. Whole server tick CPU for this Name fixture is
**not measured**; its legacy materialization CPU counter reports zero.

The existing Local/Node consumer target is rebuilt against the same production
Engine source. All four qualified differential cases pass with a 512-object
package, eight active peers and eight-object neighborhoods:

| Simulated peers | Local | TLS Node |
| --- | --- | --- |
| 32 | PASS, 75.26 s | PASS, 75.05 s |
| 200 | PASS, 76.95 s | PASS, 76.78 s |

Every baseline/load/resident/evict/reload phase reports `serviceHealthy=1` and
every run reports qualification completion. At 200 peers, load/reload retains
the existing stricter diagnostic `phaseHealthy=0`; this is not silently promoted
to a tick/raw-gap pass. Both providers finish with zero connections, journal
readers, admission owners, requested/acquiring/prepared/resident content,
completion reservations, completed/decoded/cached bytes and resident objects.
The remaining 544 admission bytes are the fixed empty admission object, not a
transient owner. These are simulated-peer regression cases, not the separate
physical 32-client qualification. No sibling source was modified.
The raw workload remains 480 opportunities times 16 writes, alternating over
**32** Parts: 7,680 Name records / 180 MiB of value history. Its 384 creation
records bring the reported journal production to 8,064 records per case.

#### Final sanitizer, resource and service evidence

Clang 19 ASan/UBSan/LSan on the trusted worker passes **8/8 focused CTests in
43.01 seconds**, **42/42 pooled model/hardening cases**, **19/19 feedback-model
cases**, **8/8 production pooled groups**, the recovery-contract proof and
**10/10 native-feedback fixtures**. The production differential preserves the
healthy 32-peer wave, 31 completions with one non-draining peer retaining exactly
G, fixed profile rejection, exact retirement, generation cleanup and conservation.
Its 32-peer admission object is 5,440 logical bytes on Clang (5,408 on MSVC),
excluding allocator overhead and existing journal/scheduler/feedback storage.

The full established Linux GNS workflow exits **0**: both profile lifecycles,
the production-admission capacity matrix, all eight pooled gameplay cases,
pooled 32-peer structural overload, and full-reservation single-peer, 32-peer
gameplay and 32-peer structural compatibility fixtures pass. No sanitizer finding
is reported. These 32-peer cases use existing in-process/loopback fixtures.

| Final canonical case | MSVC service recovery | Clang sanitizer service recovery | MSVC convergence | Clang sanitizer convergence | Convergence bound |
| --- | ---: | ---: | ---: | ---: | ---: |
| Structural overload | 113.472 ms | 132.579 ms | 1,637.63 ms | 1,979.54 ms | 20,847.1 ms |
| Mixed overload | 1,112.43 ms | 1,112.92 ms | 1,681.98 ms | 1,985.87 ms | 20,847.1 ms |

Service recovery passes the unchanged **20-second** deadline independently of
convergence. Each case has **zero raw journal remainder at 20 seconds**. Clang
has 1,504 raw records at cessation (MSVC 2,016); each platform's conservative
remaining semantic inventory is 32 Names, zero other records, and 789,772 complete
message bytes. Both platforms emit 208 transitions / 12 frames, including 32
materializations and 176 Name updates, coalescing 7,504 source records. Previous
emitted transition/frame counts to full convergence are **not measured**: that
source still had 7,248 records after 20 seconds. The old case accepted 10,643,384
structural bytes while incomplete; the corrected case accepts 4,342,072 and
converges. These are measured case totals, not identical-duration throughput.

MSVC journal retained high-water is 9,884 / 16,384 records for structural/mixed;
the corresponding **required-reader** high-water is 2,592 / 2,640, leaving
13,792 / 13,744 records of safety margin. Retained history reaching its fixed
storage cap is distinct from a lagging required reader exhausting that window.
The final MSVC workload conserves **8,727,506 created = 8,727,506 retired +
0 terminal + 0 outstanding**; Clang conserves **8,727,438 = 8,727,438 + 0 + 0**.
Both reach one grant and 393,866 debt bytes high-water.

Clang structural/mixed recovery RPC/Event/action is 37.258/37.202/37.190 ms and
38.342/38.281/38.273 ms. Both need one recovery group with zero local action
refusals. Gameplay-only overload also recovers in 1,108.14 ms. Client decode/apply
CPU is 39.391/137.057 ms and 39.586/133.379 ms under sanitizers; these timings
must not be compared as an optimized-build speed ratio against MSVC.

The pooled 32-peer structural fixture drains in 1,455.64 ms after overload and
conserves **130,813,628 created = retired**, with zero terminal release and
outstanding debt, four grants and 590,984 debt bytes high-water. Measured service
gaps are 54.246 ms before overload, 168.676 ms during overload and 42.514 ms in
the subsequent ordinary phase. Overload required-journal high-water is 132,
margin 16,252, and final recovery backlog zero. Maximum admission wait is
1,497.178 ms, including cold/ineligible intervals; it is not the conditional
eligible-grant model bound. Canonical single-peer planning gap remains seven
ticks. No physical path fairness, whole-server tick CPU or rendered-client
behavior is inferred from these measurements.

The production change adds one 64-bit source-coverage marker to existing
accepted-object metadata and one 64-bit coordinator barrier. The candidate-only
set is bounded by the existing journal read/operation limits. There is no new
cached Name value, semantic truth set, global compaction or retained payload
queue. Generic property derivation and all Option C numeric limits are unchanged.

#### Publication gates and reproducibility

The exact executable-source hosted gates are [Native engine CI at ab3f0d61d](https://github.com/gmoddev/gargantuan/actions/runs/34897744683)
and [GNS sanitizer CI at ab3f0d61d](https://github.com/gmoddev/gargantuan/actions/runs/34897744791).
Both must finish successfully; the consuming documentation-only publication must
also have terminal-green required current-source checks before final handoff.
The superseded `555354bb5` runs were cancelled after the probe correction and
are not qualification evidence. A docs-only closure does not change the measured
executable source. Closure docs build **19 pages**, **100 relative links/anchors**
pass, and `git diff --check` passes.

Worker evidence is retained under
`C:\Sandbox\Codex\Logs\gargantuan-3l-name-coalescing`: `workload3.log` is final
MSVC, `name-coalescing-*.log` is the final Linux workflow, and
`local-node-32.log` / `local-node-200.log` are the consumer regressions. The
isolated local checkout retains copies under `build/name-coalescing/`.
The full 27-file C++ overlay through `555354bb5` has SHA-256
`a3385f9b124fc07022e7a0dafab1753a63014430b3dcdcd4ef02c49496955568`;
the final one-file `ab3f0d61d` probe overlay has SHA-256
`f8edecd7f3552fdf752f57a394e2f555f7b1bb309045a4dee57ce80ab7bdd945`.
Builds use four jobs and persistent caches. No sibling source was changed.

### Historical production stop

The isolated production integration based on
`222c5beb318552a9cc07ec043c5b34581d64980f` reaches exact attributed retirement,
bounded admission and real-GNS gameplay, but fails the former structural overload
recovery fixture. This checkpoint is not a production deployment recommendation.
No merge, physical 32-client run or Foundation 3M work occurred.

### Historical recovery-contract reconciliation (2026-09-14)

The architecture conflict is now resolved by
[`PooledReliableServiceRecoveryContract3L.md`](PooledReliableServiceRecoveryContract3L.md).
The selected decision is **B — separate service recovery from structural
convergence**:

- the existing 20-second gate remains a fixed **service-recovery** requirement
  for ordinary scheduler/admission/gameplay behavior after excess offered demand
  stops;
- complete **structural convergence** uses a bounded workload-derived service
  curve over semantically necessary retained work and the accepted 2 MiB/s
  per-peer / 64 MiB/s aggregate service floors;
- no rates, journal bounds, wire format, ordering, Known semantics or gameplay
  guarantees are relaxed.

The canonical 180-MiB fixture additionally exposes **C — existing safe
coalescing is not being used for the special `Name` field**. `Name` is stored
outside the generic snapshot/property map, so the current known-object journal
path replays each historical Name value. GRPL and 3J are state-projection
contracts, however: ChangeJournal sequence is not a wire sequence, complete
materialization carries only the current Name, and ordinary property history may
coalesce where no lifecycle/hierarchy/reference barrier requires intermediate
states. The 7,680 canonical Name records are therefore valid bounded journal
input but not 180 MiB of semantically necessary delivery state.

The reconciliation does **not** qualify or resume the stopped production
implementation. The next production task is a narrow, semantics-preserving
journal-to-frame correction: coalesce repeated coalescible scalar property
records, including `Name`, to current authoritative state within existing
ordered barriers, with acceptance-only cursor/Known commit. Then re-run the
canonical overload cases with the unchanged 20-second service-recovery gate and
the workload-derived structural-convergence bound. Physical qualification remains
blocked until that source is green.

### Production recovery conflict (2026-09-14)

The canonical command is the existing real-GNS fixture with the explicit mode:

```text
gargantuan_game_session_real_transport_tests --pooled --reliable-workload
```

The historical [workload contract](ReliableGameplayWorkloadContract3L.md#selected-engine-default)
combined convergence with a fixed **20 seconds** after 480 service opportunities.
Its structural overload mutates 16 already materialized Part names by 24 KiB
per opportunity. The production journal preserves independent Name property
operations in order. That offers **180 MiB of name values**, before protocol
overhead. The accepted pooled peer credit permits **2 MiB/s**, capped at G.
Even granting the full G credit at the start, the measured structural case's
11.3634 seconds of demand (including setup) plus 20 seconds can fund at most
about **63.23 MiB**. Full-rate feedback and zero transport delay cannot bridge
this difference. The reconciliation above supersedes the interpretation that
all 180 MiB must converge within the fixed service-recovery window.

The reference model's overload case retains one pending scalar group and
counts additional offers as overload without appending them. Production must
still account for authoritative journal history behind the next group.
Passing the model's pending/debt bounds does not establish bounded recovery
for this retained history. Do not discard authoritative state, raise rates,
or weaken freshness to obtain a pass; only semantics-preserving coalescing
already permitted by the replication contract is in scope for the next task.

MSVC Release source archive `source8.tar` has SHA-256
`e1a49478e2a02aed971072fdd76a981e92e80eec82345379d2e491cbd9dde622`.
It contains the 23 changed C++ headers/source/test files over the published
base. Worker logs are under
`C:\Sandbox\Codex\Logs\gargantuan-3l-pooled-resume`; the exact failing run is
`msvc-pooled-workload8.log`. The isolated local checkout retains the archive
and copied evidence under `build/pooled/`.

| Measured case | Demand duration | Recovery wait | Remaining journal records | Structural bytes accepted during case | Qualification grants |
| --- | ---: | ---: | ---: | ---: | ---: |
| Structural overload | 11.3634 s | 20.0124 s, FAIL | 7,248 | 10,643,384 | 28 |
| Mixed overload | 14.5459 s | 20.0180 s, FAIL | 7,248 | 10,643,384 | 28 |

Both cases remain connected and report zero RPC corruption/terminal errors.
Final accounting is exact: **21,330,130 created = 21,330,130 attributed
retirement + 0 terminal release + 0 outstanding**, with zero grants after
shutdown. This does not imply that unaccepted journal history converged.
The later small recovery case follows fixture destruction of the pressure
objects; its zero backlog does not erase the prior convergence failures.

Small, upper, concurrent-burst and mixed ordinary cases satisfy unchanged
gameplay gates: worst measured RPC maximum 91.9395 ms, Event maximum
92.0340 ms, action maximum 92.1925 ms, with no errors or rejected calls.
The overload case itself is not an ordinary-latency qualification. The
28 qualification probes also show that actual fresh-sample eligibility is
more restrictive than the reference differential's supplied healthy feedback.
That distinction remains covered by the service contract.

### Implemented checkpoint ownership

- `ReliableServiceProfile` selects explicit `POOLED_SERVICE`, freezes accepted
  Option C fields and preserves default `FULL_RESERVATION`. The startup CLI
  rejects mixed full-reservation overrides. GNS's physical per-connection
  ceiling is derived as (64 MiB/s structural + 8 MiB/s transport) / four grants
  = 18 MiB/s; this is configured headroom, never observed service evidence.
- `ReliableByteAdmission` owns finite peer/global credits, the existing fair
  turn and large-waiter earmark, one synchronous reservation, four grant slots,
  exact committed byte/token debt, rollback and explicit terminal release.
  A grant remains occupied while ordinary FIFO followers remain outstanding.
- `GameSession` owns generation-scoped feedback consumption. Only the exact
  native token/message/full-byte receipt retires structural debt. Scheduler
  acceptance totals minus aggregate payload retirement account for combined
  ordinary debt. Unknown gameplay/control retirement split is retained as a
  bounded feasible interval; the funded expression uses its conservative
  maximum, never a guessed class assignment.
- A private transport capability carries existing native snapshots and leases
  a closed generation's final receipt until session reconciliation releases
  its slot. One sender-local intent token follows the existing scheduler
  message into native send. Pin, packet encoding, reliable lane and order stay
  unchanged. Invalid cumulative counters can prove purge only, never drain.
- 3J retains complete-group preparation and acceptance-only Known. An expired
  admission quote is refreshed before acceptance; unavailable eligibility
  discards only proposed acceptance metadata, charging the work already done.
  Discard leaves sequence, Known, journal and complete plan unchanged.

No new semantic payload queue or per-message ACK history was added. The
production admission accountant measures **5,408 logical bytes at 32 peers**
on MSVC, excluding allocator overhead, the peer feedback ledger and existing
scheduler/journal storage. Credit caps are G / 4G; committed debt is <=4G;
the measured single-peer workload high-water is 393,850 bytes and one grant.
The existing journal reaches its 16,384-record retention cap; this must not
be described as the model's <=G pending-byte proof.

Two integration fixes are covered by focused regression: accumulated
independent journal operations now reach the existing bounded smaller-prefix
retry when the encoder's absolute ceiling is exceeded, and expired resource
quotes can discard an unaccepted preparation. Complete semantic groups are
not split. The lifecycle fixture now supplies a floor and releases its tested
movement key so an NPC remains in range while admission legitimately waits.

### Validation scope and next gate

The production differential passes a 32-peer G wave and a first-peer
non-draining wave (31 other completions, exactly G left with the stalled peer),
matching reference grant order, credit and counts. Separate production cases
cover fixed profile rejection, rollback/conservation, stale/missing feedback,
probe cooldown, generation reset, ordinary followers, funded deferral/recovery,
unrelated gameplay ACKs, retransmission and ordinary-class bounds. This is not
a differential proof of the corrected retained-journal overload path.

Final-source MSVC Release passes **4/4** focused CTests (networking contracts,
scheduler contracts, replication relevance and legacy real-GNS GameSession)
in 10.60 seconds. Networking contracts include **42/42** reference,
**19/19** feedback-model and **8/8** production groups. Native feedback passes
**10/10** fixtures, including **12/12** mixed-attribution cases and a complete
G message followed by 16-KiB gameplay with byte-exact FIFO delivery. Exact
attribution excludes those following gameplay bytes. Default full-reservation
and explicit pooled lifecycle commands both pass; the final pooled lifecycle
conserves **5,354 = 4,807 retired + 547 terminal + 0 outstanding**.
The adapter snapshot is 128 bytes, its optional terminal slot 136 bytes, and
10,000 snapshot reads average 235 ns on this worker. These are local layout
and timing observations, not portable ABI/performance guarantees.

The incremental MSVC build uses four jobs and persistent dependencies at
`C:\Sandbox\Codex\Builds\gargantuan\runtime-host-f1-1-baseline2-msvc-gns-vs`.
The existing LNK4098 runtime-library warning remains present. Documentation
builds **19 pages**, **69** relative links/anchors pass, and `git diff --check`
passes. Full engine-suite qualification is **not measured** on this source.
Linux ASan/UBSan/LSan, full GNS workflow, canonical Local/Node regression and
hosted Native/GNS workflows for this integration source were **not measured**
at the stop checkpoint: the explicit design stop was reached before those
qualification stages. Published prior terminal-green CI remains separate
evidence. Physical 32-actual-client pooled qualification is **not measured**,
as requested.

**Exact next task:** implement the existing semantics-preserving known-object
scalar-property coalescing in the journal-to-frame derivation path, including
`Name` via the current publication Name field. Preserve lifecycle/hierarchy/
reference barriers, complete-group atomicity, scheduler acceptance-only
journal/Known commit, exact attributed retirement and all Option C admission
bounds. Then re-run structural and mixed canonical overload with separate
service-recovery and structural-convergence gates. The next task is not physical
qualification. KI-006 remains **OPEN**, Foundation 3L **B — PARTIALLY READY**,
and 3M **BLOCKED / NOT STARTED**.

## Prior attribution stop resolution

The original aggregate-feedback insufficiency remains a valid finding. Decision B
now adds exact sender-local native message attribution in `62c663335cfd36498869fde3d8e85406f102e281`,
with compilation corrections in `a5a182ff94ae7373e0cdcdcc6232b434696926fe`.
The [qualification receipt](ReliableTransportFeedbackProof3LValidation.md#retirement-attribution-qualification-2026-09-14)
owns the current gate. Local mixed-retirement and real-GNS attribution pass,
and required Native engine and GNS sanitizer CI are terminal green on the corrected
source. **The retirement-attribution stop is resolved: READY FOR POOLED-SERVICE
INTEGRATION.** This was the published state before the stopped production
checkpoint above; it does not qualify the new integration.

## Preserved historical stop

The local stop checkpoint `ec4c103dd12410664e5d4d99fb18134630c27cc4` was based on
published aggregate feedback `c4c46345242a595e550f352cde04751bf81d3031`. It was not
published to the working branch. This document carries its finding forward;
the historical test/source and measurements below retain that exact scope.

Its `ReliableRetirementAttribution` fixture in `GnsServiceFairnessTests.cpp`
constructed two ordered reliable messages, each with 129 payload bytes and three
private GNS header bytes. Two legal histories retired opposite messages through
the actual pinned `SSNPSenderState::RemoveRefCountReliableSegment` implementation:

| After one retirement | Structural first | Gameplay first |
| --- | ---: | ---: |
| Unique stream first-sent | 264 | 264 |
| Unique stream ACKed | 132 | 132 |
| Aggregate payload retired | 129 | 129 |
| Retransmitted / pending stream bytes | 0 / 0 | 0 / 0 |
| Sent-unacked stream bytes | 132 | 132 |
| Actual structural payload outstanding | **0** | **129** |

Both histories have the same aggregate accounting and Connected state. Neither
send order nor timestamps recover the missing identity. Receiver delivery order
does not require FIFO sender retirement; retry references can delay retirement
independently. Subtracting every aggregate payload ACK delta from structural debt
therefore credits unrelated traffic as structural service.

Historical MSVC Release and Clang 19 ASan/UBSan/LSan probes both reproduced
`IndistinguishableClassRetirement=PASS`; the seven aggregate-feedback fixtures,
19-case feedback model and 42-case pooled model remained green. These measurements
proved insufficiency, not successful production integration. The old stop
checkpoint's own hosted CI was not measured. Its logs remain under
`C:\Sandbox\Codex\Logs\gargantuan-3l-pooled-integration`.

## Historical attribution resolution and then-remaining work

The [native contract](ReliableTransportFeedbackProof3L.md#exact-retirement-attribution)
matches final native message retirement to its sender-local token. Gameplay or
control retirement cannot clear another message's active obligation. No wire,
pin, reliable lane or ordering change is needed. The aggregate counter remains
available for aggregate accounting; it is not a substitute for the exact receipt.

The exact-attribution integration has since been carried into the stopped
production checkpoint described above. The remaining blocker is no longer native
retirement identity; it is the semantics-preserving journal-to-frame coalescing
correction and requalification under the reconciled recovery contract.

KI-006 remains **OPEN**. Foundation 3L remains **B — PARTIALLY READY**.
Physical 32-actual-client pooled qualification is **NOT MEASURED**.
Foundation 3M remains **BLOCKED / NOT STARTED**.
