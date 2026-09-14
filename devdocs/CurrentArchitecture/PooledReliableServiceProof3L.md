---
status: selected-candidate-executable-proof
owner: runtime-networking-and-runtime-host
last_verified: 2026-09-14
related_code:
  - tests/PooledReliableServiceModelFixture.hpp
  - tests/NetworkingContractsTests.cpp
  - src/network/ReliableByteAdmission.hpp
  - src/network/GameSession.cpp
  - src/network/Scheduler.cpp
related_adrs:
  - ../FutureArchitecture/Foundation3LServiceCoverageDecision.md
---

# Foundation 3L pooled reliable service executable proof

## Scope and authority

This document is the numeric/executable proof companion to
[Foundation3LServiceCoverageDecision.md](../FutureArchitecture/Foundation3LServiceCoverageDecision.md).
That decision selects Option C conceptually while retaining the existing full-reservation
service class. This document does not replace its conceptual analysis and does not describe
implemented production behavior.

The proof is intentionally in `CurrentArchitecture` rather than a second conceptual ADR:
it records an executable test specification and validation evidence. Production
`ReliableServiceProfile`, `ReliableByteAdmission`, `GameSession`, scheduler, GNS, wire and
ordering semantics remain unchanged by this proof. `FULL_RESERVATION` remains the only
implemented production service profile at this checkpoint; `POOLED_SERVICE` below is a
candidate for a later scoped implementation task.

No Foundation 3M work is included. KI-006 remains open for production implementation and
funded 32-actual-client physical qualification.

## Selected 32-peer candidate

Exactly one pooled candidate is selected for this proof.

| Field | Candidate value | Meaning |
| --- | ---: | --- |
| Coverage mode | `POOLED_SERVICE` | Explicitly distinct from existing `FULL_RESERVATION`; no reinterpretation of R/A |
| N | 32 connected peers | Maximum connected peers for this candidate |
| G | 524,288 B (512 KiB) | Existing maximum complete reliable group including adapter envelope |
| NominalLink | 1,000,000,000 bit/s | Initial physical class; not a qualification result |
| BackendCap / usable service envelope | 100,663,296 B/s (96 MiB/s, 805.306368 Mbit/s) | Maximum modeled aggregate backend drain commitment |
| StructuralPool | 67,108,864 B/s (64 MiB/s) | Sustainable aggregate structural pool |
| GameplayReserve | 8,388,608 B/s (8 MiB/s) | Physically reserved ordinary gameplay service |
| ControlRealtimeReserve | 4,194,304 B/s (4 MiB/s) | Separate bounded control/realtime service reserve |
| RequiredTransportReserve | 8,388,608 B/s (8 MiB/s) | Packet/retransmission/backend headroom in the modeled envelope |
| Unallocated backend slack | 12,582,912 B/s (12 MiB/s) | Additional slack inside BackendCap |
| PeerDrainFloor | 16,777,216 B/s (16 MiB/s) | Required verified drain floor while one complete structural grant is active |
| MaxConcurrentDrainGrants | 4 | At most four peers may own admitted structural drain obligations concurrently |
| PeerCreditRate | 2,097,152 B/s (2 MiB/s) | Sustained eligibility credit per connected peer |
| PeerBurstCap | 524,288 B | One G; idle credit cannot grow beyond one maximum group |
| GlobalCreditRate | 67,108,864 B/s (64 MiB/s) | Aggregate structural eligibility refill |
| GlobalBurstCap | 2,097,152 B (4G, 2 MiB) | At most one full four-grant wave of retained global credit |
| PerPeerPendingCap | 524,288 B | One pending complete group per peer in the executable model |
| GlobalPendingCap | 16,777,216 B (32G, 16 MiB) | One pending maximum group per connected peer |
| FeedbackStalenessLimit | 50 ms | Older/missing/error service feedback blocks new structural debt |
| Model service quantum | 500 us | Deterministic proof granularity; not a proposed production tick |

The accepted ordinary reliable gameplay envelope remains unchanged: per-peer burst 20 KiB,
32 KiB/s steady per direction, and for N=32 the moderate aggregate envelope is 160 KiB burst
plus 256 KiB/s steady. The existing RPC p95/p99/max targets remain 150/250/500 ms and
Event/action maxima remain 250 ms. The accepted nonqueue allowance remains 100 ms.

### Physical headroom

A decimal 1 GbE link exposes an ideal 125,000,000 B/s before Ethernet/IP/UDP/encryption,
loss, retransmission and host effects. The candidate uses only 100,663,296 B/s as the
funded model envelope, leaving 24,336,704 B/s (194.693632 Mbit/s, 19.47% of nominal line)
outside the modeled usable envelope.

Inside BackendCap:

```text
StructuralPool            64 MiB/s
GameplayReserve             8 MiB/s
ControlRealtimeReserve      4 MiB/s
RequiredTransportReserve    8 MiB/s
                         ------------
fixed modeled commitments  84 MiB/s
BackendCap                 96 MiB/s
unallocated slack          12 MiB/s
```

This arithmetic is a candidate design assumption, not evidence that the inspected workstation,
VPS WireGuard path or any arbitrary 1 GbE path supplies the service floor. A production profile
must be rejected until its actual host/path demonstrates the declared drain floors.

## Formal invariants

### Rate funding

At all times the named rate pools obey:

```text
StructuralPool
+ GameplayReserve
+ ControlRealtimeReserve
+ RequiredTransportReserve
<= BackendCap
```

For this candidate, `64 + 8 + 4 + 8 = 84 MiB/s <= 96 MiB/s`.

Active structural grants additionally obey:

```text
ActiveDrainGrants <= 4
ActiveDrainGrants * PeerDrainFloor <= StructuralPool
32 * PeerCreditRate <= StructuralPool
```

The candidate is exact on the last two limits: four active grants require 64 MiB/s and
32 fair sustained shares require 64 MiB/s. Credit is therefore an eligibility budget, not an
additional physical reservation.

### Debt funding at admission

The strictest accepted ordinary latency target is RPC p95 150 ms. With 100 ms reserved for
nonqueue work, structural admission uses a 50 ms funded queue window. Before every new
structural reservation, the executable model requires:

```text
CommittedStructuralDrainDebt
+ ReservedStructuralDrainDebt
+ ProspectiveStructuralDebt
+ max(QualifiedGameplayDebt, 160 KiB)
+ max(ControlDebt, 64 KiB)
+ RequiredTransportReserve * 50 ms
<= BackendCap * 50 ms
```

At the worst four-G structural wave this is conservatively:

```text
2,097,152 B structural
+ 163,840 B fresh gameplay headroom
+ 65,536 B control headroom
+ 419,430 B transport reserve over 50 ms
= 2,745,958 B
```

The 96 MiB/s funded envelope provides 5,033,164 B over the same 50 ms interval, leaving
2,287,206 B model margin before a fifth structural grant. The separate four-grant cap is
therefore the controlling structural concurrency bound.

Debt is not released because wall-clock time passes. It is released only by explicit verified
backend service, failed pre-acceptance rollback, or terminal connection teardown reconciliation.
Accepted structural debt is never reclassified as a failed reservation merely because later
backend service fails.

### Credit is not service

A connected peer begins with zero structural credit. Credit accrues with monotonic elapsed time
to the 512 KiB peer cap. Global credit accrues to the 2 MiB cap. A peer that owns G credit can
still be denied a grant because the four physical obligations are occupied, the aggregate
funded-debt invariant would fail, or service feedback is stale/error/missing.

No borrowed credit, negative debt or unlimited catch-up exists. Failed scheduler/acceptance
reservation refunds the exact eligibility debit up to the fixed caps and releases the grant;
Known remains unchanged. After matching acceptance, Known may advance according to the existing
3J/GameSession acceptance contract and the structural debt remains charged until service or
terminal teardown.

## FIFO gameplay proof

Reliable structural and gameplay traffic retain the existing ordering semantics. No lane,
fragmentation, semantic split or reliable-order bypass is assumed.

The worst protected peer follower used by this proof is a 20 KiB qualified peer gameplay burst
arriving immediately after one admitted G group. With the required 16 MiB/s verified per-grant
drain floor:

```text
(G + gameplay burst) / PeerDrainFloor
= (524,288 + 20,480) / 16,777,216 s
= 32.470703125 ms
```

The deterministic 500-us executable model observes 32.5 ms. Adding the accepted 100 ms
nonqueue allowance gives a 132.5 ms measured/model bound (132.471 ms continuous arithmetic),
inside RPC p95 150 ms and therefore also inside RPC p99/max and Event/action maximums.

This does not promise 150 ms under a peer/path that violates the required 16 MiB/s drain floor.
Such feedback makes the peer ineligible for new structural debt; existing accepted debt remains
charged. Healthy peers retain their separately funded gameplay service. A future production
implementation must define the terminal policy for a peer whose service floor cannot be restored.

The 32-peer aggregate gameplay burst also fits independently. With 8 MiB/s reserved and
256 KiB/s continuing qualified arrival, a 160 KiB aggregate burst has a fluid drain bound of
approximately 20.16 ms, below the same 50 ms queue budget.

## Fairness and finite structural progress

The executable specification uses one rotating active-peer cursor plus one large-waiter earmark.
It is intentionally not a pluggable scheduler framework.

- inactive peers consume no turn;
- at most one structural grant is active per peer;
- accepted grants drain at their verified floor and release their slot only after accounted debt
  is serviced or terminally reconciled;
- a credit-eligible G waiter that is blocked only on global credit retains the refill earmark so
  later tiny groups cannot consume that refill indefinitely;
- the rotation resumes after the last granted peer;
- generation teardown removes stale pending/credit/grant/fairness ownership;
- a slow or non-draining peer can occupy at most one of four structural grants and cannot obtain
  another while its obligation remains outstanding.

For one G group, one 16 MiB/s grant drains continuously in 31.25 ms. At the model's 500-us
service quantum it occupies 31.5 ms. With 32 simultaneously eligible peers and four grants,
there are eight waves. A peer in the last wave waits behind seven finite waves:

```text
L_i <= 7 * 31.5 ms = 220.5 ms
```

The model observes maximum first-grant wait from credit eligibility of exactly 220.5 ms and
maximum first verified service of 221.0 ms.

The fair sustained structural share is 2 MiB/s per continuously eligible peer. A conservative
service-curve completion statement is therefore:

```text
Completion_i <= L_i + Work_i / 2 MiB/s + bounded model step
```

For G, `Work_i / r_i = 250 ms`, so the conservative bound from credit eligibility is about
470.5 ms plus at most one 500-us model step. A fresh connection also needs at most 250 ms to
earn G credit, giving a conservative approximately 721 ms bound from zero-credit demand.
The actual deterministic one-G-per-peer wave completes in 502 ms from zero-credit demand.
These are qualified-but-degraded structural progress bounds, not gameplay SLAs.

## Slow peer, feedback and lifecycle rules

Fresh service feedback must be no more than 50 ms old and must establish at least the declared
per-grant drain floor for new structural debt. Missing, stale, contradictory/error or below-floor
feedback is conservative: no new structural grant is created for that peer. Existing accepted
debt is never inferred drained from elapsed time.

Disconnect behavior in the executable spec is:

1. before admission: pending metadata and peer credit are cleared;
2. after grant reservation but before acceptance: reservation/grant is rolled back exactly and
   Known does not advance;
3. after accepted debt exists: the old generation remains occupied until terminal teardown
   reconciles the transport debt; a replacement generation cannot bind to that slot early;
4. reconnect starts with zero credit, zero pending work, zero grant, and no inherited cursor state.

This mirrors the existing acceptance boundary without inventing semantic rollback of work that
was already accepted.

## Bounded overload and memory

The executable model retains one scalar pending group requirement per peer and no serialized
payload queue. New work offered while that peer already owns pending/reserved/accepted structural
work is counted as bounded overload rather than appended. Therefore:

```text
Peer pending bytes <= G
Global pending bytes <= 32G = 16 MiB
Peer credit <= G
Global credit <= 4G = 2 MiB
Committed structural debt <= 4G = 2 MiB
Active structural grants <= 4
```

The model uses a fixed 32-entry peer array. On the local x86-64 Clang/GCC build its complete
`Model` object is 8,920 bytes; this is a test-object layout observation, not an allocator or
future production ABI guarantee. Its logical state is O(N) with N fixed to 32 and does not grow
with test duration or offered overload history.

## Startup validation

The executable profile validator rejects the candidate before model construction when any of
the following is true:

- service mode is not explicitly `POOLED_SERVICE`;
- N or G differs from this selected candidate's scope;
- a required service rate or grant count is zero;
- fixed service pools exceed BackendCap;
- concurrent grant drain floors exceed StructuralPool;
- 32 guaranteed structural shares exceed StructuralPool;
- peer/global burst or pending bounds cannot hold the required complete groups;
- feedback may remain admissible longer than the strict 50-ms queue budget;
- one G plus a qualified peer gameplay burst cannot serialize inside that queue budget;
- qualified aggregate gameplay cannot drain inside the queue budget;
- worst admitted debt plus fixed transport reserve exceeds the funded queue window;
- any checked multiplication/addition/rate-duration calculation overflows.

The separate `FULL_RESERVATION` enum value is deliberately rejected by the pooled validator.
That is proof that the executable spec does not silently reinterpret existing production R/A
semantics. No wire distinction is required.

## Executable model matrix

`tests/PooledReliableServiceModelFixture.hpp` is a deterministic, network-free executable
specification invoked by the existing networking-contract CTest. It owns only abstract peer,
credit, grant, debt, feedback, demand, generation and time state. It does not include or clone
production admission/scheduler code.

Required and supporting cases:

1. `SmallGroupSinglePeer`
2. `MaximumGroupSinglePeer`
3. `MaximumGroupPlusOneRejected`
4. `CreditInsufficientDefers`
5. `AggregateCapacityInsufficientDefers`
6. `GameplayBehindMaximumGroup`
7. `ThirtyTwoMaximumGroups`
8. `TinyDoesNotStarveLarge`
9. `LargeDoesNotStarveTiny`
10. `GameplayBurstDuringStructuralWave`
11. `SlowPeerDoesNotMonopolize`
12. `NonDrainingPeerContainsDebt`
13. `MissingFeedbackFailsConservatively`
14. `FailedAcceptanceRollsBackGrant`
15. `DisconnectBeforeAdmission`
16. `DisconnectWithCommittedDebt`
17. `ReconnectStartsZeroCredit`
18. `ReconnectChurnNoAdvantage`
19. `SustainedOverloadBounded`
20. `RecoveryConverges`
21. `InvalidProfileRejects`
22. `ArithmeticOverflowRejects`
23. `GameplayBurstBeforeStructuralGrant`
24. `AlternatingTinyAndLargeFair`
25. `GameplayWhileSlowPeerOutstanding`
26. `ContradictoryFeedbackFailsConservatively`
27. `DisconnectAfterGrantBeforeAcceptance`
28. `AcceptedBackendFailureRetainsDebtUntilTeardown`
29. `IdleCreditCaps`
30. `CreditIsNotService`
31. `TimeAloneDoesNotReleaseDebt`
32. `FullReservationModeDistinct`
33. `FeedbackStalenessLimit`

## Pre-publication local model evidence

The standalone form of the same test-only model was compiled as C++23 with local GCC and
Clang 17 using `-O2 -Wall -Wextra -pedantic`. All 33 cases passed. Twenty repeated Clang runs
produced one identical stdout SHA-256, establishing deterministic model output on that local
compiler. This is development evidence only; repository CI is reported separately after the
normal branch push.

Selected local results:

| Scenario | Result |
| --- | --- |
| G+1 | rejected, zero Known progress |
| Gameplay immediately behind G | 32.5 ms modeled service delay; 132.5 ms including nonqueue allowance |
| 32xG finite wave | p50 376 ms; p95/p99/max 502/502/502 ms from zero-credit demand |
| 32xG max first grant from eligibility | 220.5 ms; first verified service 221.0 ms |
| 32xG committed structural high-water | 2,097,152 B = 4G |
| 32xG pending high-water | 16,777,216 B = 32G |
| tiny competitors vs one G | G complete at 281.5 ms |
| 31 G competitors vs one tiny | tiny complete at 471.0 ms |
| alternating tiny/G repeated | 8/8 completions; measured maximum same-peer grant gap 280 ms |
| structural wave + gameplay | gameplay delay 32.5 ms; control-debt high-water 65,536 B |
| slow 1 MiB/s peer | other 31 peers complete; healthy maximum 596.5 ms; slow debt remains charged |
| non-draining peer | exactly G remains committed; exactly one grant retained |
| healthy gameplay while slow debt exists | 2.0 ms service delay; slow debt remains charged |
| reconnect churn | stable peer admitted 5,242,880 B; churner admitted 0 B in the same scripted interval |
| 10-s sustained overload | pending high 32G; committed high 4G; gameplay delay high 13.5 ms; fixed model object 8,920 B |
| recovery after overload stops | converged in 226.5 ms |

No failed candidate case required numeric retuning after selecting the 16 MiB/s grant floor and
four-grant concurrency. The earlier exploratory 8 MiB/s grant floor was intentionally discarded
before publication because its G+20-KiB FIFO bound plus 100 ms nonqueue allowance exceeded the
150 ms RPC p95 target even though it remained below the 250 ms Event/action maximum. The selected
candidate therefore uses the stricter value rather than weakening a gameplay target.

## Implementation gate

The executable proof is necessary but not production qualification. If repository CI accepts the
model, this candidate is eligible for a later **scoped production implementation task**. That
future task must still implement an explicit pooled service mode without changing existing
full-reservation semantics and must prove the same invariants against actual GameSession/GNS
feedback and failure behavior.

No production change is authorized by this document. Before any pooled profile can be called
production-qualified, affected Windows Release and Linux sanitizer tests, actual same-FIFO GNS
service, loss/retransmission behavior, startup rejection, overload/recovery, and the funded
32-actual-client Local/Node physical matrix must pass. The current workstation/VPS evidence does
not establish the 96 MiB/s usable service envelope or 16 MiB/s active-peer floor.

KI-006 therefore remains **OPEN** and Foundation 3L remains **B — PARTIALLY READY**. No 3M.
