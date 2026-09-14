---
status: production-checkpoint-stopped-recovery-implementation-required
owner: runtime-networking
last_verified: 2026-09-14
related_code:
  - src/network/ReliableByteAdmission.hpp
  - src/network/PooledReliableServiceFeedback.hpp
  - src/network/GameSession.cpp
  - tests/PooledReliableServiceProductionFixture.hpp
  - cmake/gns/ApplyReliableServiceFeedback.cmake
  - tests/ReliableServiceFeedbackFixture.hpp
related_adrs:
  - ../FutureArchitecture/Foundation3LServiceCoverageDecision.md
---

# Foundation 3L pooled production integration stop receipt

## Current disposition

**STOPPED / NOT QUALIFIED.** The isolated production integration based on
`222c5beb318552a9cc07ec043c5b34581d64980f` reaches exact attributed retirement,
bounded admission and real-GNS gameplay, but fails the former structural overload
recovery fixture. This checkpoint is not a production deployment recommendation.
No merge, physical 32-client run or Foundation 3M work occurred.

### Recovery-contract reconciliation (2026-09-14)

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
