---
status: accepted
owner: runtime-networking
last_verified: 2026-09-28
supersedes: short-window pooled service health and Phase 1 three-by-three window acceptance
---

# D01 — POOLED_SERVICE is a rate-plus-bounded-deficit contract

This decision adopts the final Foundation 3L service contract. It governs
production health and physical qualification. Historical receipts remain
evidence of the rule under which they were evaluated; they do not establish
compliance with D01.

## Rates and the native quantum

Peer credit refills at **2 MiB/s** between grants. Credit is admission
eligibility, not delivered service. While a peer owns an accepted active
structural grant with unique bytes awaiting first native send, its required
service rate is **R_i = 16 MiB/s = 16,777,216 B/s**. Four simultaneously
qualified active grants require **R_pool = 64 MiB/s = 67,108,864 B/s**.
The configured GNS backend rate is 18 MiB/s per connection: (64 MiB/s
structural pool + 8 MiB/s transport reserve) / four grants. A configured
send rate is feasibility funding, not proof of delivered service.

The pinned GNS revision `2cb93a06350bb065db53abdb0d87cf297e0bfd34`
sets its maximum encrypted first-send payload and send-rate burst overage to
**Q_i = 1,248 B**. This is the conservative native packetization quantum.
The separate **512 KiB = 524,288 B** maximum remains the complete structural
group, peer burst, peer pending, and one active peer obligation. It is not Q_i.

## Authorized handoff and exact bounds

The ordinary reliable send flag uses pinned GNS's 5,000 µs Nagle policy.
Its service thread rounds a scheduled thinker wake to an integer millisecond
and requests at least one millisecond. Only that requested quantization gets
another 1,000 µs. Thus **H_i = 6,000 µs**, without a measured tolerance:

```text
G_i = Q_i / R_i + H_i
    = 6,074.3865966796875 µs

B_i = Q_i + R_i × H_i
    = 101,911.296 B = 12,738,912 / 125 B

B_pool = 4 × B_i
       = 407,645.184 B = 50,955,648 / 125 B
```

Integer comparisons may use ceiling bounds of 101,912 B per peer and
407,646 B directly for the pool, or sum four individually rounded peer
bounds. The exact equations above remain authoritative. The implementation
compares byte-microsecond integers without introducing a new tolerance.

Same-step GameSession planning/flush CPU, the nominal 60 Hz frame cadence,
per-step budget deferral, credit and grant fairness, GNS token-bucket pacing,
feedback age, ACK delay, one-second requalification, and host/OS lateness
beyond GNS's requested wake receive **zero** additional H_i. Startup,
reload, teardown and terminal failure are outside active eligibility.
External pressure is not a baseline clock pause; a separate induced-recovery
test may classify bounded, independently observed pressure.

## Continuous campaign and service curve

For a generation-scoped campaign, τ_i is cumulative qualified active-grant
time and S_i is cumulative **unique structural payload bytes at first native
transmission**. Time advances only while a connected, generation-valid peer
owns an accepted first-send-backlogged structural grant, feedback is valid and
fresh, and no excluded lifecycle transition is active. It pauses after all
grant bytes are first sent, during ACK retirement, credit refill and fair
grant wait. Grant turnover never resets deficit history.

For every qualified-time subinterval [a,b], require:

```text
S_i(b) - S_i(a) >= R_i × (τ_i(b) - τ_i(a)) - B_i
X_i = R_i × τ_i - S_i
X_i - min(previous X_i, including initial zero) <= B_i
```

The running-minimum check prevents earlier excess service from funding a
later blackout. Native first-send events and the still-backlogged feedback
boundary are checkpoints; checking only average throughput or periodic ACK
deltas is insufficient. Retransmission, authored bytes, scheduler acceptance,
ordinary reliable gameplay and GNS enqueue acceptance never increase S_i.

Pool-qualified time is the intersection of four designated peers' qualified
active intervals. On every pool-qualified subinterval, the sum of their
structural first-send bytes must meet R_pool with B_pool. This supplements,
and never replaces, each peer's curve. The four peer curves also imply this
pool bound over their common active interval.

## Delivery, retirement and recovery

Structural unique ACK bytes A_i are monotonic and never exceed S_i. The
one-grant sent-unacked payload U_i = S_i - A_i remains between zero and
524,288 B. A fresh observation with zero ACK delta may still have healthy
first-send service; later ACK convergence remains mandatory. Native
retransmission is accounted separately and cannot inflate S_i or A_i.
After production stops, every accepted structural byte must eventually be
ACKed and retired exactly once; sent-unacked, structural pending, pooled debt,
grants and pending admission reach zero with no terminal release in the
healthy baseline. Conservation remains exact. The fixed 20-second bound is
for service recovery; complete structural convergence uses the workload-
derived bound, not a universal 20-second deadline.

Feedback must remain valid and at most **50 ms** old. Staleness is a service
health/qualification failure, not service slack. The **1-second** bounded
requalification mechanism remains for a previously unhealthy peer. It never
resets the generation's deficit history or grants a one-second active-service
blackout.

Existing FULL_RESERVATION, wire formats, complete-group and pending bounds,
fairness, gameplay/control reservations, and physical funding gates remain.
This decision supersedes production's short first-send-plus-positive-ACK
predicate and Phase 1's three batches of three consecutive ACK-positive
~6 ms throughput windows. A ~6 ms sampler may remain for telemetry only.
Physical Phase 1 must be requalified under this decision before any 3L
acceptance claim; KI-006 remains open and 3M remains blocked.
