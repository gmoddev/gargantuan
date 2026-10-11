---
status: capacity-review-required-not-adopted
owner: runtime-networking-and-product
reviewed_source: 9551ae974869b56ed12aa27aee2f4e7996fb23c9
reviewed: 2026-10-11
supersedes: none
---

# D02 — Reassess pooled service against product outcomes

## Verdict and authority

**Follow-up qualification verdict (2026-10-11): HYBRID F1 — CAPACITY/ARCHITECTURE
DECISION REQUIRED.** The deterministic production-admission schedule in section 9
fails the retained capacity and fairness targets even with optimistic native
service and zero ACK delay. Do not adopt or implement this proposal as written.
The original recommendation and its rationale below remain decision history.

**HYBRID F1.** Recommend replacing the packet-scale hard service curves with
hard completed-grant, protected-gameplay, capacity, fairness and recovery gates.
The repository justifies fast finite structural delivery and FIFO protection;
it does not establish an independent gameplay need for a native packet opportunity
every approximately 1 ms. This recommendation changes a timing promise explicitly,
not because the implementation has failed to meet it.

**This is a reviewed proposal, not an adopted contract.** D01/F1 continues to govern
production health and qualification. No evaluator, constant, admission predicate,
workload or historical verdict changes here. KI-006 remains OPEN; Foundation 3L
remains B — PARTIALLY READY; PR #6 remains draft. No new physical run is authorized
by this report. Implementation and fresh evidence require the next task.

Three independent reviews examined product provenance, contract counterexamples
and branch cost. The review rejected a universal 2-MiB/s delivered-byte curve for
all legal grant sizes and rejected merely disabling the farm's F1 verdict while
leaving production eligibility coupled to it. Neither is the recommendation.

## 1. Original intent and numerical provenance

The original [3L architecture](../../devdocs/CurrentArchitecture/ContentAvailabilityFoundation3L.md)
separates verified immutable content bytes, authoritative residency and peer
materialization. Engine owns validation, detached preparation, dependency ordering,
commit, eviction and lifetime. It is not a new Character scheduler or creator-facing
streaming API. Its invariants require bounded provider work, acceptance-only peer
progress and preserving Character/Remote service during content load.

The later [Option C decision](../../devdocs/FutureArchitecture/Foundation3LServiceCoverageDecision.md)
shares peak structural capacity among four active grants instead of permanently
funding peak capacity for all 32 connections. Its
[executable proof](../../devdocs/CurrentArchitecture/PooledReliableServiceProof3L.md)
deliberately chooses 16 MiB/s over 8 MiB/s to protect gameplay behind a maximum
reliable structural message. [D01/F1](D01-pooled-service-curve.md) then converts
transport assumptions into native timing assertions.

| Quantity | Derivation and purpose | Classification |
| --- | --- | --- |
| 2 MiB/s peer credit; 512 KiB cap | Fair sustained admission eligibility, finite burst, zero new-generation credit | Product/resource policy; credit is not measured service |
| Four grants; 64 MiB/s structural funding | Four times 16 MiB/s; also 32 times 2 MiB/s credit | Funded capacity and concurrency policy |
| 16 MiB/s active drain | `(512 KiB + 20 KiB)/16 MiB/s = 32.470703125 ms`; plus 100 ms nonqueue fits RPC p95 150 ms. At 8 MiB/s the sum is 164.94140625 ms | Gameplay FIFO protection and capacity design target |
| 50 ms queue window | 150 ms RPC p95 less the existing 100 ms combined nonqueue allowance | Product budget used by admission; still requires measured gameplay |
| 6.0743865967 ms initial latency | Pinned GNS 5 ms Nagle + 1 ms requested wake quantization + `1,248 B/16 MiB/s` | Implementation-derived bound, not a player response requirement |
| 1.0743865967 ms recurring no-progress bound | GNS packet quantum plus 1 ms requested wake, enforced through a running minimum | Implementation-derived intra-grant assertion |
| 20 seconds | Return to ordinary service after excess demand ends | Product recovery maximum; not universal complete convergence |

D01 expressly gives OS lateness, lock contention and Main work no extra time.
Its requested wake is not a supported Windows dispatch guarantee. Later strict-F1
decisions correctly refused to waive failures under the accepted rule; they did
not establish that a 1-ms native gap has necessary player-visible meaning.

## 2. What the measured failure does and does not mean

The original 1,057-B grant at source `5de6135c8` took **7.070 ms** from activation
to its sole native-positive send. Its size-dependent completion envelope was
**6.1373887062 ms**: a **0.9326112938-ms** miss. This is a real F1 failure.
It occurred about 66 seconds before recovery cessation. Its cause remains
unattributed. It is not evidence of 7-ms end-to-end gameplay latency.

| Boundary | Meaning and possible consequence of a later first send |
| --- | --- |
| Content Available | Verified immutable provider bytes; this network grant does not measure acquisition |
| Content Resident | Engine authoritative admission; not client materialization |
| Grant accepted/activated | Exact scheduler-owned structural obligation; Known/cursor commit is local accepted intent, not proof of remote application |
| First native-positive send | OS accepted a UDP datagram; unique attributed bytes advance only here |
| Complete native drain | Every unique structural byte has first been sent; packet gaps can differ even with equal completion time |
| Remote receipt and ACK | Reliable packet delivery/ACK; loss and retries can add delay; neither enqueue nor native completion proves this |
| Client application/readiness | GRPL applied with dependencies and identity checks; can gate newly materialized Character state and dependent Remotes |
| Exact retirement | Matching ACK-owned receipt releases debt/slot once; not a rendered observation |
| Gameplay/rendered effect | Requires joined request, due-state, client application or presentation evidence; no such join exists for the historical token |

For a newly relevant object, later structural receipt can delay readiness and its
dependent use. For an already materialized Character, recurring GCHR is a distinct,
replaceable path: 3G does not change simulation or materialization when it defers
publication. Shared CPU/socket work can still interfere with it and must be tested.
Ordinary reliable gameplay/control already behind structural bytes shares FIFO;
reserves do not permit accepted reliable order to be bypassed.

RPC p95/p99/max remain **150/250/500 ms**; Event ACK and action-result maxima remain
**250 ms**. Invocation timers must not restart after dependencies become ready.
Unmaterialized prerequisites are outside the currently qualified ordinary latency
promise, but their full wait remains measured. Character due-service accounting,
accepted-observation conservation and client application remain separate gates.
There is no basis to claim that players can or cannot notice this 7-ms interval,
or that the historical token violated another product gate.

## 3. Compare the contracts

| Option | Value | Cost/risk | Decision |
| --- | --- | --- | --- |
| A. Keep strict F1 | Strong detection of any native service blackout; mathematical per-subinterval guarantee | Requires a demonstrated bounded actor/lock/dispatch environment, or deployment constraints; precise wake/spin and native ownership complexity. Current supported environment lacks that proof. No independent gameplay need for the micro-bound was found | Do not retain as the default product hard gate. Appropriate only for a separately justified timing class |
| B. Outcome-only | Directly measures gameplay, completion, capacity and resources | Completion percentiles or average aggregate throughput alone can conceal one starved peer, stalled last bytes and ACK-owned slots. A 48.779296875-ms structural ceiling could exhaust the queue budget | Reject the weak form; outcome gates alone need explicit hard starvation/capacity bounds |
| C. Hybrid | Keeps hard finite completion, protected service, fair progress, sustained capacity, exact delivery and recovery; preserves packet curves as diagnostics | Deliberately allows some temporary packet gaps and slower tiny grants. Requires fresh proof of real slot turnover and source-shaped capacity | Recommend the bounded design below |

The outcome-only ceiling is `50 ms - 20 KiB/16 MiB/s = 48.779296875 ms`.
Do not spend all that headroom. The hybrid preserves the **existing maximum legal
grant's completion budget**, rather than selecting a tolerance from measured tails.
No comparison with another engine is needed to establish this conclusion.

## 4. Proposed enforceable hybrid

### H1 — One hard completed-native-grant maximum

For every generation-safe accepted grant with `0 < W <= G = 524,288 B`, preserve
the existing activation boundary and require complete unique native first-send by:

```text
R = 16,777,216 B/s; Q = 1,248 B; H = 6,000 us
T_native_max = H + (G + Q)/R
             = 37,324.3865966796875 us
R * (t_complete - t_activate) <= R * 6,000 + (G + Q) * 1,000,000
```

Use the exact integer inequality, not a rounded 38-ms deadline. At integer-us
resolution 37,324 passes and 37,325 fails. This is a **uniform maximum** for tiny,
medium and maximum grants. It is explicitly a replacement for Phase A's pointwise
curve and size-dependent deadline. A 1,057-B grant may now use the maximum-group
budget; its old 6.137-ms promise is not retained. This is not a 16-MiB/s continuous
service guarantee for every tiny grant.

Missing final bytes or no first send after the deadline fails, even if a later
burst or ACK completes the obligation. Observe active deadlines as well as completed
results; do not require completion before noticing starvation. Native timestamps
are authoritative: a real worker completion before the deadline remains timely
even if actor reconciliation is later. Pending/incomplete feedback cannot certify
PASS or an exact timing failure; reconcile its actual result and fail closed on
missing evidence. Publication cannot retimestamp a genuinely late completion to
repair it. Existing positive-result ownership, immutable feedback frontier and
counter validation stay hard.

### H2 — Hard protected FIFO and player service

Retain the 50-ms funded queue window and actual prospective debt/exposure checks.
Qualify the **complete exposed FIFO prefix**, including already accepted ordinary
work, the structural grant and a fresh qualified gameplay/control follower, within
one shared queue budget. A newly submitted qualified follower must complete native
first-send within 50 ms of its own submission under the declared workload. No
serial stage receives a separate 50-ms allowance.

The inherited arithmetic leaves useful headroom:

```text
37.3243865967 ms maximum structural completion
+ 1.220703125 ms serialization for a 20-KiB follower at 16 MiB/s
+ 100 ms combined nonqueue allowance
= 138.5450897217 ms < 150 ms
```

This is a feasibility calculation, **not** a proof that ordinary reliable followers
actually receive that service. Their Nagle, ACK/path, client processing and handler
time must be included in direct tests. Keep all existing RPC/Event/action gates,
traffic/fanout envelopes, due-state joins, whole-group dependencies and maxima.

### H3 — Capacity, fair admission and starvation are independent hard gates

Retain 2-MiB/s peer credit, 64-MiB/s global credit/funding, four ACK-owned slots,
the 96-MiB/s funded backend envelope and 8/4/8-MiB/s gameplay/control/transport
pools. Retain **16/64 MiB/s as delivered capacity qualification targets** for the
specified large-grant capacity workload. Replace the instantaneous active floor
with whole-workload capacity qualification; do not rename a configured send rate
or credit refill as delivered service.

Define the 16-MiB/s subcase separately: four precredited, concurrently activated
**maximum G grants** on distinct generation-valid peers. Each must satisfy H1
and `R * (t_complete - t_first_positive) <= G * 1,000,000`, i.e. finish within
**31.25 ms after its first positive native packet**. The complete G includes
the first packet at that boundary; that single packet's at-most-Q advance is
explicit, not an extra wake allowance. Keep all intervening native work, packet
gaps, locks and worker delays in this interval. It is a whole-grant burst-capacity
gate for this specified workload, not a subinterval rate assertion or a new
deadline for a tiny second packet. A 14-MiB/s large-grant sender fails it.

There is no extra warmup after first send. The existing credit warmup happens
before activation; H1 includes activation-to-first-send. ACK and retirement do
not extend this unique-send interval, but must complete exactly and their slot
occupancy remains inside the repeated 32-peer capacity test below. Four separately
passing burst intervals do not prove 64 MiB/s on a common wall-clock interval;
the aggregate/repeated test is independently required.

The new completion maximum alone permits only
`2 MiB/37.3243865967 ms = 53.5842697594 MiB/s` before ACK wait. Four maximum-grant
slots sustaining 64 MiB/s need average whole-slot cycles **at most 31.25 ms**.
Seven worst-case completion waves take 261.270706 ms, not the old model's 220.5 ms.
Thus H1 proves neither capacity nor the old fair-service arithmetic. Even old F1's
maximum envelope does not prove the model's 31.5-ms ACK-owned slot cycle.

Use the existing explicit maximum-grant capacity fixture and repeated finite
waves: 32 actual identities, continuously available complete G offers, four active
grants, real native-positive bytes, ACKs, retirement and Main release/regrant.
A proposed deterministic capacity target for that **specified** workload is:

```text
L_capacity = 250 ms credit warmup + 220.5 ms fair wait + T_native_max
           = 507.8243865967 ms
S_i(b) - S_i(a) >= 2 MiB/s * max(0, (b-a) - L_capacity)
```

Evaluate every continuously offered interval in that finite scripted workload,
clamped to its actual offered work; use exact integer comparisons. Sum the 32 peer
requirements for the 64-MiB/s pool. No pause for credit, ACK, occupied slots,
admission health or fair turns; no reset at grant turnover and no later overservice
can fund a later long blackout. Record ACK/retirement/slot occupancy to attribute a
failure. The sum of old budgets defines a **candidate acceptance budget**, not proof
that current production can supply it. Reprove the retained **220.5-ms first-grant
fair wait from credit eligibility** using actual slot occupancy; its historical
500-us model is not a physical actor proof.

Do not generalize this byte curve to arbitrarily tiny grants. At nominal 60 Hz,
77-B grants with one grant per peer per step permit at most 4,620 B/s per peer;
four admissions per step permit at most 18,480 B/s globally. More generally, four
admissions per frame require average groups of at least **279,620.267 B** to reach
64 MiB/s at 60 Hz, even before ACK/OS costs. Medium 64-KiB groups at that admission
ceiling supply at most 15 MiB/s. These are source-derived ceilings, not measured
capacity or a new minimum legal size. Batching, actual source production, encoded
group sizes and grant count must accompany every capacity claim.

For tiny/mixed workloads use hard per-grant completion, per-peer eligible admission
wait, maximum-waiter earmark/rotation, and exact workload-specific recovery. Under
conforming sustained demand, backlog growth or a deadline miss is a failure, not
an excuse to classify the peer unhealthy after the fact. This proposal does not
promise delivered 2 MiB/s for every legal unsplittable tiny-group stream. The
original credit policy never made refill itself evidence of that capability.

**If actual ACK-gated capacity or the retained fairness/recovery targets cannot
pass, 3L remains blocked.** Do not lower 64 MiB/s, grant sizes, peer count or the
recovery formula, and do not automatically start a pipeline rewrite. Return the
byte/grant/slot feasibility evidence for a separate capacity decision.

### H4 — Recovery, delivery and resources remain hard

Keep the 20-second service-recovery maximum and the existing C3/C5 immutable
cessation reference, exact quote, semantic coverage, generation-scoped finite cut
and causal convergence deadline. Keep moving relevance and exact causal replay.
The existing formula uses 2-MiB/s peer service, 64-MiB/s pool and 220.5-ms fairness;
its use under real produced group shapes must be requalified, not assumed from
H1. The native32 recovery correction remains qualified in its recorded scope.

Keep FULL_RESERVATION, reliable ordering, retransmission/ACK history, encryption,
nonces, wire formats, message limits, protected reserves and every hard lifecycle
and memory bound. In particular: G/32G pending caps, 4G accepted debt, four slots,
one grant per peer, 50-ms feedback freshness, 16,384 journal records and existing
3J transition/planning limits. Invalid/missing evidence never produces PASS.
Elapsed time, a failed health check or disconnect never fabricates retirement.

### Diagnostic and runtime-health behavior

Retain old finite/running/pool curves, exact witnesses and stage timings as optional
nonfatal diagnostics. An isolated diagnostic timing breach records token/generation,
grant size, native chronology and curve maximum. It does not alone change admission
eligibility, release debt, reset history, disconnect a peer or extend H1/H2.
Keep diagnostic histories bounded and avoid per-packet logging in ordinary runtime.

Hard H1/H2 violations, invalid/stale feedback or counter/lifetime defects stop new
structural obligations under the existing conservative policy. Preserve the current
bounded requalification mechanism; new successful evidence cannot erase a failed
campaign. Existing accepted packets remain owned through ACK or explicit terminal
reconciliation. Capacity/fairness/convergence qualification failure leaves the
deployment unqualified; no automated tuning or product scope fallback.

This requires changing the actual health predicate as well as qualification.
`PooledReliableServiceFeedback.hpp` currently combines `StructuralServiceFailed`,
`QueuedOfferFailed`, current running deficit and last-completed F1 failure into
`Qualified`. Merely ignoring the farm's verdict would leave production peers
ineligible. Keep hard integrity separate from the old diagnostic flag in the next
implementation; do not change this coupling in this report.

## 5. Requirement-to-gate matrix and discriminating tests

These are **test designs**, not executed new production regressions. Use existing
fixtures and bounded evidence; no new general tracing/verifier framework.

| Requirement | Hard gate / focused positive and negative design |
| --- | --- |
| Content authority/security | Existing hash/namespace/schema/dependency validation, detached commit and stale-generation denial; provider bytes cannot mutate the world |
| Tiny/medium/max completion | 77 B, 1,057 B, 64 KiB and 512 KiB: exact 37,324/37,325-us boundary; no send and withheld final byte fail; malformed/overflow/reversed times invalidate |
| Jitter versus starvation | A 7,070-us completed tiny grant may violate old telemetry yet pass H1. A prompt large first send followed by a stall must fail H1, capacity or protected FIFO when those budgets are crossed. A short recovered stall with all hard outcomes intact is intentionally diagnostic; intent cannot be inferred from timestamps |
| Four active / 32 peers | Repeated reachable G waves through submission, native send, ACK, retirement and slot release; keep capacity clock through waits. Every peer checked individually; sum cannot hide one excluded peer |
| Active burst capacity | Four overlapping G grants, per-peer post-first completion at most 31.25 ms plus H1; 14-MiB/s control fails even if H1 and its later 2-MiB/s credit share pass |
| Under-capacity masked by completion | Four G grants each taking 37.324 ms pass H1 but deliver at most 53.584 MiB/s before ACK. In repeated capacity waves their service deficit grows and must fail H3; do not reset at each wave |
| Tiny versus large fairness | Tiny competitors cannot consume a large waiter's earmark forever; 31 large peers cannot starve a tiny peer. Explicit 220.5-ms eligible wait and H1 checks; repeat with churn and ordinary debt |
| Packet starvation | One accepted packet/last byte withheld past H1 fails even with high service elsewhere. Enqueue and unrelated ordinary packets supply no structural progress |
| Protected gameplay/control | Complete G FIFO prefix followed by qualified burst, continuing arrivals and aggregate fanout. Native follower queue budget plus unchanged RPC p95/p99/max and Event/action maxima; include both directions, retransmission and bunched replies |
| Moving Characters/dependencies | Retain due-state/client joins and accepted-observation conservation. Delayed new dependency readiness stays measured from demand/invocation; no timer reset or cadence suppression |
| False conservation | Positive OS send only; zero/failure result supplies no unique bytes; retries cannot inflate first-send; ACK above unique service, duplicate retirement, changed token/message/bytes and hindsight repair fail |
| Backpressure/overload | No progress on rejection; no extra payload queue; all fixed caps hold. Declared feasible workload cannot grow backlog indefinitely; excessive offered load remains bounded and service returns within 20 s |
| ACK and lifetime | Hold ACK after timely native completion: slot/debt stay charged; repeated capacity/recovery fails if not restored. Disconnect/slot reuse/late completion cannot transfer credit, coverage or retirement to a new generation |
| Causal convergence | Existing gameplay/structural/mixed cessation cases, exact quote and replay. Withheld baseline, invented supersession, future bytes enlarging old reference and duplicate retirement fail |

Nine static integer/budget checks passed during this review: uniform H1
passes 1,057 B at 7,070 us, 77 B at 37,324 us and G at 37,324 us; it rejects
37,325 us for any size. Old size-dependent completion rejects the first two tiny
examples; the remaining checks cover the withheld-byte deadline and the capacity
and tiny/medium admission ceilings above. These are arithmetic checks only.
No current runtime test, workload or
historical receipt was reevaluated under this proposal.

The repeated capacity negative must last beyond its derived intercept, not just
one wave. Here `64 MiB/s * L_capacity = 32.5007607422 MiB`; an idealized sender at
53.5842697594 MiB/s accumulates deficit at 10.4157302406 MiB/s and exhausts that
budget after approximately 3.12035 s. The discrete model must include exact packet,
wave and ACK boundaries and continue until the negative fails. This test horizon
comes from the candidate equations, not an observed benchmark or added tolerance.

## 6. PR #6 scope and maintenance cost

Audited diff: base `eb2ce26ab1455670f4e7b53426cc3521403110c5`
(`foundation/3l-content-availability`, also merge-base) through reviewed head
`9551ae974869b56ed12aa27aee2f4e7996fb23c9`. Much original content implementation
is already in the base; this is the qualification branch delta, not all of 3L.

| Mutually exclusive file bucket | Files | Added | Deleted |
| --- | ---: | ---: | ---: |
| Engine/header/mixed host source | 42 | 3,869 | 207 |
| GNS patch scripts/support | 14 | 2,919 | 50 |
| Embedded farm/quote qualification | 12 | 3,111 | 0 |
| Tests and qualification scripts | 115 | 27,511 | 109 |
| Tools | 101 | 21,328 | 0 |
| Documentation/evidence | 44 | 12,826 | 16 |
| CI | 2 | 349 | 38 |
| Build integration | 2 | 210 | 0 |
| Total | 332 | 72,123 | 420 |

These are path/file buckets, not semantic attribution of every line. Mixed source
files contain diagnostics. Engine/header/mixed-host plus GNS support comprise
**9.4%** of additions; tests/tools **67.7%**. There are 383 commits, 119 touching
runtime/header/GNS paths, 25 touching GNS and 63 documentation-only; counts overlap
where stated. They do not establish 70 engine optimizations or measured staff-time
cost. The applied pinned GNS cache changes nine upstream files, +1,115/-29;
that generated patch overlaps support scripts and must not be added twice.

| Classification | Examples and retention recommendation |
| --- | --- |
| 1. Essential content/correctness/security | Keep Node response digest verification, dependency-complete acceptance, Character lifetime through speculative Leaves/rebind, failed-send reference release, positive-native ownership, ACK/retirement/debt/generation validation and incomplete-feedback rejection. Their investigation origin does not make them disposable |
| 2. Useful independent networking work | Keep immutable pinned journal reads, bounded encoding/error precedence, Name size preflight, qualified mixed-Attribute recovery fix `5de6135c8`, UTF-8 parity fast path/output move, and demonstrated GNS receive/timer fairness correction. Preserve exact-source evidence; no universal cross-profile performance claim |
| 3. Strict-F1 execution complexity | Review precise sender spin/wake, packet quantum/global running state, ordinary wire pacer, and opt-in process domain/four OS workers/64 tickets/pending actor path. Funded final-packet ACK may independently improve delivery and needs separate assessment. Worker improvement was measured but did not qualify strict F1 |
| 4. Diagnostics/one-off qualification | Farm rings, causal quote/replay scaffolding, capture orchestration and investigation adapters dominate added surface. Keep historical evidence/readers; isolate optional execution and use only tools needed to establish the new acceptance facts |

While the worker path remains, leases, pending receive journal, ACK/result
ordering, cancellation, reconciliation and bounded shutdown/fail-stop are essential
safety, not removal candidates. Do not selectively delete them to simplify the diff.
The path is currently opt-in/default-off. A later coherent experiment may compare
existing synchronous and worker modes against the new outcomes, after proofs;
this report neither chooses a removal nor authorizes a new campaign.

Costs still include upstream patch maintenance, actor/worker race review, platform
timing flakiness, evidence overhead and the qualification branch's large tooling
surface. Demoting packet timing reduces a requirement, not this existing code
automatically. Simplification must be justified by equivalent outcome evidence.

## 7. Evidence and exact source/CI state

Reviewed source **9551ae974869b56ed12aa27aee2f4e7996fb23c9**; PR #6 OPEN/draft.
This report is documentation only. It builds no binaries and executes no physical
or native32 workloads. Existing tests/receipts are reused only for their original
contracts and revisions.

- Recovery source `5de6135c877a65b26303662a64910d8c88f321c0`, run
  `1f48c4dd-2a7b-4f3d-a578-63720bc16602`: gameplay 0.314908/20.470926 s,
  structural 28.178837/40.767221 s, mixed 0.368538/20.471184 s; exact structural
  causal bytes 1,239,317,970, quotes and independent replay PASS. Whole run FAIL
  for the historical 7.070-ms initial grant. All 107 indexed files verified.
- Diagnostic source `76f03b7c59bdc61f88f4b910ae20ce42ca06dd48`, run
  `3d955b61-ae2a-4844-a4f2-53e6ddae1433`: 7,190 observed F1 grants pass,
  no delayed-initial trigger retained. Sealed run FAIL for structural offer cadence;
  total observer overhead unmeasured. This is neither native32 qualification nor
  evidence that the historical cause disappeared. All 107 indexed files verified;
  cleanup recorded no task runtimes/test-port sockets.
- Local32 current profile remains unqualified/failed; real-TLS Node32 and final
  acceptance are outstanding. Historical smaller F1 passes retain original scope.
- At the 2026-10-11 21:42:49 UTC CI inspection, diagnostic source 76f03b7c5's two GNS
  sanitizer runs **38174042331/38174045062** pass; two native workflows
  **38174042342/38174045029** remain in progress. Reviewed head 9551ae974's GNS
  PR/push **38175407178/38175405105** pass; native push/PR
  **38175405078/38175407204** remain in progress. No all-current-source CI PASS.

Primary receipts remain in the
[native ownership decision, sections 14–16](../../devdocs/FutureArchitecture/Foundation3LNativeEgressOwnershipDecision.md)
and [validation ledger](../../devdocs/CurrentArchitecture/ContentAvailabilityFoundation3L_3Validation.md).
CI snapshots describe inspection time, not promised eventual conclusions. Any
documentation publication SHA is distinct from these measured source SHAs.

## 8. Implementation sequence, dependencies and stopping point

1. **Next task: accept and qualify the hybrid contract design.** Start with small
   deterministic extensions to existing fixtures: H1 integer boundaries, isolated
   diagnostic misses, FIFO budgets and repeated ACK-owned slot waves. Establish
   whether retained 16/64 capacity, fair wait and existing convergence formula are
   achievable for the production group distribution and nominal Main cadence.
   Include byte/grant counts; do not create a universal busy-time framework.
2. Only after that model passes, adopt a separate D01 amendment naming H1–H4 and
   implement one consistent native/adapter/GameSession hard-health predicate.
   Keep old diagnostic flags separate. Update profile validation, feedback frontier,
   completed/active deadline evidence and failed-campaign retention together.
3. Requalify affected native tests, Windows Release, Linux ASan/UBSan/LSan, dedicated
   GNS sanitizer and required final-source CI. Update D01, profile/scheduler/real
   transport docs, feedback proof, pooled service proof/validation, farm F1 parser,
   physical qualification, recipient service and recovery contract dependencies.
   Preserve versioned readers and all original failures.
4. After separate execution authorization, one bounded fresh native32 campaign
   evaluates H1–H4, protected gameplay, byte/grant capacity, fairness, resources,
   exact ACK/retirement and causal recovery. A passing new contract needs fresh
   provenance; old samples cannot be relabeled.
5. Only after it passes, canonical Local32, real-TLS Node32, remaining resource/
   fairness/backpressure/recovery gates and independent final acceptance. Consider
   isolation/removal of F1-only execution machinery only through a coherent later
   A/B and safety requalification; no mass revert or speculative pipeline.

**Stop here.** The recommendation is GO for the reviewed hybrid design, not a
claim that the existing backend supplies its retained capacity or that 3L can
close. If the first deterministic slot/capacity proof fails, return a focused
capacity decision instead of starting another actor optimization campaign.
No contract implementation, physical run, 3M, release, merge or historical rewrite
is part of this task.

## 9. Deterministic capacity qualification — blocked (2026-10-11)

**HYBRID F1 — CAPACITY/ARCHITECTURE DECISION REQUIRED.** This follow-up executes
the previously requested preimplementation proof at base
`1710bc8d08cf2ae4049f3c58f5aed988f0969904`. It extends existing fixtures only.
No production health predicate, rate, wire, resource, recovery or admission source
changes. D01/F1 remains governing; the independently qualified native32 recovery
receipt remains valid. No native32 or physical campaign runs.

### First causal contradiction: ACK-owned slot reuse at nominal Main cadence

The source path is connection configuration at
`ReliableServiceProfile::BackendSendRate()` and adapter min/max rate settings;
Main's 16,667-us deadline; GameSession native receipt observation; exact
`ReliableByteAdmission::Retire`; `RefreshService` releasing a slot only after
structural **and ordinary** debt is zero; then rotating whole-group admission.
The current immediate flush improves initial handoff, but does not transfer
admission or slot release out of Main. Fresh feedback is reused during ordinary
within-step preparation. A just-submitted peer cannot regrant in that step.

The fixture uses the **real production admission accountant**, 32 generations,
the unchanged peer/global credit and funding, four slots, rotation, explicit
native-byte progress, ACK-ready time, retirement and regrant. Fixed-phase Main
steps have no CPU cost. Feedback is always valid/fresh/qualified to maximize
opportunity; no original F1 failure can reduce the modeled capacity.

The native side is deliberately an **optimistic fluid budget**, not GNS execution:
all configured 18-MiB/s wire service is unique structural payload, with free
1,248-B idle credit plus a 1,300-B final-packet overdraw. Encryption, framing,
worker queues, locks, retries, ACK delay and ordinary debt cost zero in the first
case. The synthetic 2,548-B advance is not a real first native-positive packet
and does not qualify packet ownership or native chronology.

```text
Earliest G drain in that favorable budget:
ceil((524,288 - 1,248 - 1,300) * 1,000,000 / 18 MiB/s)
  = 27,643 us > 16,667 us

Earliest nominal slot reuse = 2 * 16,667 = 33,334 us
Four-slot ceiling = 2 MiB / 0.033334 s = 59.998800024 MiB/s < 64 MiB/s
Last of eight waves waits 7 * 33,334 = 233,338 us > 220,500 us
```

This is failure of an ordinary **fixed-phase nominal schedule** allowed by the
current composition, not proof of universal hardware/platform incapacity.
Variable within-frame phases and catch-up steps can change slot timing; incidental
favorable jitter is not a credible bounded execution plan. The old scalar proof's
500-us readmit cadence and immediate drain/retirement/release are not the actual
Main/ACK-owned execution model.

At modeled time **6,000,115 us**, the repeated G case has **361,978,652 unique
bytes**, below the proposed aggregate prefix requirement **368,581,384 bytes**.
That one exact prefix is a sufficient failure witness, not an all-interval
capacity verifier. All 32 peers receive service and grants meet H1 and the G burst
gate. The finite run conserves **536,870,912 B accepted = sent = ACKed = retired**,
with no terminal release, at most 4G debt and four slots. Thus neither completion
nor exact conservation excuses the capacity/fairness miss.

### H1–H4 disposition and negative coverage

| Requirement | Deterministic result / limitation |
| --- | --- |
| H1 uniform completion | Candidate arithmetic PASS: 77 B, 1,057 B, 64 KiB and G at exact 37,324/37,325-us boundary; missing first/final bytes, invalid chronology and incomplete feedback distinguish correctly. Timely native timestamps remain timely despite later reconciliation. Original eight F1 cases remain unchanged |
| H2 shared FIFO | Shared-budget arithmetic PASS: one 50-ms window, optimistic G +20-KiB feasibility, serial 30+30-ms rejection and follower/prefix-delay negative. Event/action delivery maxima remain separate. This does not establish real ordinary follower service from structural completion or reserves |
| H3 burst capability | 18-MiB/s favorable model meets the G post-first 31.25-ms gate. A 14-MiB/s sender meets H1 but fails that burst gate; no configured-rate inference |
| H3 sustained capacity/fairness | FAIL in the nominal repeated maximum-grant schedule, before worker/OS/ACK cost. Every peer is checked; deliberately omitting one ready peer fails fairness independently of aggregate progress |
| ACK/ordinary debt | At modeled 16-MiB/s native service, G completes in 31,099 us. A 3,000-us ACK delay or ordinary-debt hold moves release to the third Main step and eligible wait to 350,007 us. A native completion cannot free an ACK-owned slot |
| Tiny/medium/mixed shape | 77-B and 64-KiB groups, plus alternating tiny/medium/G groups, conserve exactly and serve every peer within the tested first-wave fair bound. They are not mislabeled the continuously offered G capacity fixture. Four admissions/frame bound medium/tiny byte throughput independently of native rates |
| Source below offered rate | One G per peer per second completes and conserves, but yields no capacity qualification. Neither credit nor low offered load is evidence of 64 MiB/s |
| Backpressure/resources | Failed preaccept reservation rolls back exactly; source work is retained. Duplicate retirement rejects. Debt/credit/grant bounds and accepted/send/ACK/retired ordering hold at every step |
| H4 causal semantics | Existing 32 causal-evidence cases and existing recovery-contract regressions PASS unchanged. Cessation reference, semantic coverage and exact retirement are not altered |
| H4 rate-derived convergence | NOT established under the proposal: its 2/64-MiB/s and 220.5-ms prerequisites already fail H3. A finite admission-schedule negative shows H1/burst deadlines alone do not imply the old byte-only formula |

The finite H4 schedule uses 1,280 G offers per peer (640 MiB each): completion is
**341.590160 s**, versus the unchanged **340.470500-s** formula. It already fails
H3 capacity/fairness; it is not a jointly healthy H1–H3 counterexample. A real
GRPL codec test encodes/decodes one exact G group with eight bounded Name updates
and one Attribute barrier. `1,280 * 9 = 11,520 < 16,384` is hypothetical record-count
arithmetic only. There is no full source/journal/planning replay or 640-MiB cessation
quote here. Repeating identical values need not create new records. Do not
relabel the canonical qualified recovery campaign based on this synthetic case.

### Executed tests and reproducibility

- `FiniteGrantServiceCurveFixture.hpp`: **13/13** (eight original plus five
  H1/H2 candidate cases); existing registration and `--service-curve` entry.
- `PooledReliableServiceProductionFixture.hpp`: existing **18/18** plus
  **10/10** candidate schedule/codec cases. Expected contradictions assert
  `candidate_qualified=0`; a green regression executable is not a hybrid PASS.
- `gargantuan_networking_contracts`: **1/1 CTest PASS**, with the existing
  scalar model/hardening, feedback, recovery and **32 causal-evidence** cases.
- Existing pure `--service-curve` also retains **9/9** pooled curve cases.
  This branch returns before GNS transport creation; no socket workload runs.

Compilation uses the trusted `dockerbox` incremental MSVC Release build, bounded
parallelism four, and only the two affected test targets. Logs/JUnit and transferred
test-source hashes are retained under
`C:\Sandbox\Codex\Artifacts\foundation3l-hybrid-capacity`; local copies are at
`C:\Users\aiden\.codex\artifacts\foundation3l-hybrid-capacity`.
The first passing CTest JUnit truncated stdout to 1,024 B; the same focused test
was rerun with existing verbose/output-size options to preserve the actual
counterexample rows. No new verifier or tracing infrastructure was added.

Final focused CTest takes 0.12 s; the build and both deterministic commands exit
zero. The three tested source-file SHA-256 values match between local and worker:

| Test source | SHA-256 |
| --- | --- |
| `FiniteGrantServiceCurveFixture.hpp` | `66d78281fcdbe129c97f76e9e906f8e7a68ace8b1fde42dfef67596918ac934e` |
| `PooledReliableServiceProductionFixture.hpp` | `38d664ebd183b2fc494171de4890f717c2ca23e929e0240f55167aace9555c2f` |
| `NetworkingContractsTests.cpp` | `544bcdd7e1db3f79b875582ec69eeab2d132faf23e73d5840d2bf6e3605c2b4c` |

The proof is source-backed deterministic arithmetic/resource behavior. It proves
neither physical capacity, WCET, full source throughput nor a supported alternate
actor schedule. No new hosted/native32/physical evidence is inferred. The PR remains
draft and the proposed production predicate remains unimplemented.

At 2026-10-11 22:13:46 UTC, remote PR head is still `1710bc8d0`: its GNS push/PR
workflows **38177701544/38177705974** pass; native push/PR workflows
**38177701541/38177705969** remain in progress. They do not test these new fixtures.
This deterministic follow-up is retained as a local commit; no push/hosted
workflow is initiated because the ordinary push CI includes socket/native
execution outside this task's deterministic-only scope. CI policy is not changed
or bypassed, and no final-source hosted PASS is claimed.

### Ranked alternatives and exact next task

| Rank | Alternative | Required evidence and cost |
| --- | --- | --- |
| 1 | Review bounded additional **Main-owned** retirement/reconciliation/admission opportunities | Addresses the demonstrated slot quantization while retaining exact ownership and simulation authority. Define actual opportunity spacing, work and ACK bounds, then prove repeated G waves, fairness, protected service and unchanged recovery. Another timer alone is not proof; no implementation is authorized here |
| 2 | Review separation of first-send capacity permits from ACK-owned debt | Changes four-slot/debt ownership and funded resource exposure. Four unretired G grants already fill 4G: decoupling alone cannot admit a fifth G under current bounds. Requires an explicit capacity/ownership decision and exact ACK/retry/generation/shutdown/protected-traffic proofs; do not implement a pipeline automatically |
| 3 | Review a different physically funded burst model | G must finish inside one nominal frame to use that slot every frame. Even this favorable budget needs about 29.854 MiB/s per connection; four such rates exceed the current 72-MiB/s structural-plus-transport allocation. Raising configuration alone is not valid funding |
| 4 | Explicit product decision on delivered capacity/fairness and byte-plus-grant recovery semantics | Preserve legal sizes and existing gameplay/security/resource requirements. Any changed service promise needs a derived model and fresh evidence; do not silently turn 64-MiB/s funding into a lower delivered guarantee |

**Stop for the capacity/ownership decision.** Rank 1 is the smallest next
discriminating architecture review. Do not adopt D02, implement its health
predicate, tune timers, launch another native campaign or redesign GNS based
on a passing diagnostic sample. The product rationale for demoting unsupported
packet-scale timing remains, but the proposed replacement is not qualified.
