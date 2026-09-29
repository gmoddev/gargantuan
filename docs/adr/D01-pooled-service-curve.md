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

## F1 amendment — finite active-grant drain capacity (2026-09-29)

**F1 is the governing service interpretation.** The preceding D01 text is
preserved as decision history. F1 supersedes its generation-persistent
elapsed-active-time deficit, including the rule that grant turnover never
resets that deficit. The later R3 persistent-deficit, S1 semantic-busy-period,
and T2 sustained post-credit-offer interpretations are also superseded where
they conflict with this amendment. No physical Phase 1 result has qualified
F1. KI-006 remains open and Foundation 3L remains B — PARTIALLY READY.

### Admission and drain are separate rate domains

Peer structural credit refills at **2 MiB/s** with a **512 KiB** burst cap;
global credit refills at **64 MiB/s** with a **2 MiB** burst cap. Admission
retains one ACK-gated accepted grant per peer and at most four global grants.
Credit/fairness eligibility, accepted debt, pending limits and grant release
are unchanged. A peer waiting for credit, fairness, ACK or retirement does
not accrue a 16 MiB/s drain obligation. The 2 MiB/s peer rate controls its
sustainable share across grants. The **16 MiB/s** peer rate controls how
quickly a finite accepted grant drains once its unique bytes await first
native transmission. **64 MiB/s** is the simultaneous drain capacity of four
such grants; it is also the aggregate refill rate of 32 peers at 2 MiB/s.
Neither rate requires the same four peers to admit work continuously.

For each generation-safe accepted grant `g`, let `W_g` be its exact complete
attributed structural service bytes, using the same post-coalescing byte
basis as pooled admission and native structural attribution. Require
`0 < W_g <= 524,288 B`. Let `t_g` be its qualified activation timestamp,
`D_g(t)` its cumulative **unique** structural bytes at first native
transmission, and `f_g` the timestamp of its first positive first-send event.
Always require `0 <= D_g(t) <= W_g`. First-send drain ends at `D_g = W_g`;
ownership, ACK and debt retirement can continue afterward. Retransmission,
scheduler acceptance, GNS enqueue, authored bytes and ordinary reliable
gameplay/control bytes do not increase `D_g`.

### Phase A — finite rate-latency envelope

Ordinary reliable transport retains the pinned 5,000 µs GNS startup/Nagle
term. The sender/service scheduling term is 1,000 µs. With
`R_i = 16,777,216 B/s` and native quantum `Q_i = 1,248 B`, the grant latency
is derived, not measured or rounded into a new independent allowance:

```text
H_start = 5,000 µs
H_run   = 1,000 µs
L_g     = H_start + H_run + Q_i/R_i
        = 6.0743865966796875 ms

D_g(t) >= min(W_g, R_i × max(0, t - t_g - L_g))
T_complete(g) <= t_g + L_g + W_g/R_i
```

The equation applies to every legal grant size. A 77 B grant has an
approximately 6.079 ms completion envelope; it does not have a 77 B divided
by observed-microseconds throughput requirement. For a 512 KiB grant the
completion envelope is approximately 37.3243865967 ms. Grant completion
and the exact inequality are checked against native first-send evidence,
without a short ACK-positive or arbitrary 6 ms throughput window.

### Phase B — recurring drain within one grant

When bytes remain after `f_g`, let `τ_g(t)` be qualified running time from
`f_g` while `D_g(t) < W_g`. Define the within-grant deficit and bound:

```text
Y_g(t) = R_i × τ_g(t) - [D_g(t) - D_g(f_g)]
B_run  = Q_i + R_i × H_run = 18,025.216 B
Y_g(t) - min(previous Y_g, including initial zero) <= B_run
```

Evaluate at first-send progress and still-backlogged observation boundaries.
The running minimum prevents earlier excess service within the grant from
funding a later blackout. This check resets only when that finite grant has
completed first-send; it does not run through ACK wait, credit refill or
fairness rotation into another grant. It is vacuous for a grant completed by
its first native send. A slow sustained sender within a large grant must
fail even if its total completion appears to fit the startup allowance.

### Four simultaneous grants

Every qualified grant independently satisfies both phases. For four grants
active together, with individual activation times and sizes, also require:

```text
R_pool = 4 × R_i = 67,108,864 B/s
D_pool(t) = Σ_g D_g(t)
D_pool(t) >= Σ_g min(W_g, R_i × max(0, t - t_g - L_g))
```

For four synchronized 512 KiB grants, `W_pool = 2 MiB` and the finite
completion envelope is `L_g + W_pool/R_pool`, approximately
37.3243865967 ms. During the common interval after all four have begun
first-send and before any exhausts its unique bytes, define common
qualified running time `τ_pool`, structural first-send `S_pool`, and:

```text
Z_pool = R_pool × τ_pool - S_pool
B_run_pool = 4 × B_run = 72,100.864 B
Z_pool - min(previous Z_pool, including initial zero) <= B_run_pool
```

This common running test supplements the four individual tests. When one
grant exhausts its bytes, no nonexistent 16 MiB/s demand is charged to it.
Four-peer physical capacity qualification therefore needs overlapping
reachable finite grants, preferably maximum grants to expose their
rate-dominated drain interval, not an indefinite 64 MiB/s stream from the
same four peers. That physical run is a separate task after deterministic
qualification.

### Independent health and superseded readings

Cumulative `A_i = Σ_g W_g` for actually admitted grants and cumulative
first-send `D_i = Σ_g D_g` remain conservation and attribution diagnostics;
`D_i <= A_i` holds. They are not a cross-grant 16 MiB/s service clock.
Semantic source busy time does not manufacture offered bytes or drain time.
T2's generic min-plus curve may test synthetic verifier arithmetic, but
arbitrary continuous arrivals are not evidence that the current ACK-gated,
credit-limited production path can admit them. Production capacity evidence
must use reachable accepted finite grants.

Feedback freshness remains at most 50 ms; one-second requalification does
not erase accepted debt or delivery evidence. A later qualification grant
starts its own finite service contract and can be evaluated ex post. ACK
monotonicity, bounded sent-unacked bytes, eventual ACK convergence, exact
retirement, debt conservation, pending and active-grant convergence, and
20-second service recovery remain independent of first-send drain health.
The transport/gameplay reserves, FULL_RESERVATION, wire format and physical
funding gates are unchanged. Historical D01/R3/S1/T2 evidence retains its
original meaning; none is retroactively an F1 pass.
