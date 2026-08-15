---
status: current
owner: networking
last_verified: 2026-08-15
related_code:
  - include/gargantuan/network/
  - src/network/
  - include/gargantuan/runtime/ObjectId.hpp
  - include/gargantuan/runtime/WireValue.hpp
  - tests/NetworkingContractsTests.cpp
  - tests/SimulatedTransportTests.cpp
  - tests/SchedulerContractTests.cpp
related_adrs:
  - docs/src/content/docs/developing/networking-architecture.mdx
---

# Networking Foundations 1–4 validation

## Scope and result

This is the bounded adversarial checkpoint after protocol hardening, pure
contracts, deterministic simulated transport, and the scheduler policy contract.
It covers the reusable protocol boundary and the networking types, simulator,
and test-only scheduler proof. It introduces no listener, socket, codec,
authentication, production scheduler, replication execution, or remote runtime.

The reviewed foundation is internally consistent and ready to support selection
of a real transport adapter. The fixes below preserve the accepted ownership and
authority architecture; no accepted networking invariant changed.

## Invariant map

| Invariant | Implementation owner | Validation owner | Tests | Documentation |
| --- | --- | --- | --- | --- |
| Protocol input | `runtime/ProtocolInput`, wire/snapshot/journal loaders | Shared bounded validators plus each decoder | Foundation hostile-input corpus | `ProtocolInputHardening.md` |
| Connection identity | `ConnectionId`; simulator slot generations | `IsValid`, exact-generation maps, allocation retirement | Networking contracts; stale reuse simulator cases | `NetworkingContracts.md`, `SimulatedTransport.md` |
| Connection lifecycle | Connection state contract; simulator links | Legal-transition validator and terminal link teardown | Contract transitions, close/timeout/reuse tests | `NetworkingContracts.md`, `SimulatedTransport.md` |
| Delivery semantics | `DeliveryMode`, message intent, transport | Intent/event validation; simulator delivery policy | Contract and fault-model tests | Networking ADR and contracts |
| Sequence domains | Strong `MonotonicSequence<Domain>` specializations | Explicit construction, validity, `TryNext` | Compile-time separation and exhaustion tests | `NetworkingContracts.md` |
| Network limits | `NetworkLimits` | `IsValid`, downward negotiation, intent/session checks | Boundary and forged-limit tests | `NetworkingContracts.md` |
| Transport statistics | `NetworkStatistics`; simulator counters | Optional-value validator and bounded/saturating updates | Deterministic trace and independent counter assertions | `SimulatedTransport.md` |
| Scheduler statistics | `SchedulerStatistics` | Unit-consistent queue/accounting validation | Scheduler accounting and drop-separation tests | `NetworkSchedulerContract.md` |
| Queue accounting | Simulator connection state; scheduler policy harness | Checked admission, exact dequeue, teardown, underflow closure | Exact ceiling, overflow, backpressure, cancellation, stress | Simulator and scheduler documents |
| Reliable backlog | Scheduler admission; transport accepted queue | Byte ceilings and structured terminal exhaustion | Exact-full/overfull and temporary-backpressure tests | Networking ADR and scheduler contract |
| Unreliable drops | Scheduler pre-transport policy; transport fault policy | Separate counters and bounded retained work | Congestion, loss, and statistics-separation tests | Simulator and scheduler documents |
| Sequenced supersession | Scheduler unsent queue; simulator receive state | Domain/channel keys and monotonic comparison | Repeated supersession, reorder, receive-congestion tests | Simulator and scheduler documents |
| Disconnect behavior | Simulator link lifecycle | One close per exact generation; scheduled-link cancellation | Duplicate close, timeout, saturation, delayed stale work | `SimulatedTransport.md` |
| Transport/scheduler separation | `IGameTransport` and `INetworkScheduler` | Separate result, limit, queue, and statistics types | Policy harness plus simulator integration | Networking ADR and scheduler contract |
| Authority separation | Contract dependency boundary | No DataModel, mutation, capability, or Luau dependency | Compile/build dependency evidence | Protocol hardening and networking ADR |
| Determinism | Simulator clock, ordered topology/schedule, seeded RNG | Explicit `Advance`/`Pump`, monotonic insertion key | Equal-time, clock-chunk, repeated-seed, stress traces | `SimulatedTransport.md` |

No invariant has two conflicting owners. Replication chooses peer knowledge,
the scheduler chooses eligible semantic work, and the transport owns accepted
message delivery mechanics.

## Issues found and fixes

### Correctness

- **Medium:** pending connection activation transitions were checked against
  current event capacity but not cumulatively reserved. Several accepted starts
  could therefore overrun the configured pending-event ceiling, after which a
  close could omit terminal events. Admission now reserves both activation and
  terminal events per connection; terminal emission is an asserted invariant.
- **Low:** delivery-time calculation committed `NextSendAvailable` before later
  latency/jitter/reorder additions could overflow. Prospective time, ordering,
  RNG, attempt, and statistics state now rolls back when scheduling fails.
- **Low:** valid limits could advertise an unreliable message larger than the
  per-tick send budget, contradicting scheduler-budget and available-datagram
  semantics. A valid send budget must now hold either maximum message class.
- **Low:** `ReplicationView` tracked one realtime sequence per object although
  delivery ordering is state-channel scoped. Replica state knowledge is now
  keyed by `(ObjectId, StateChannelId)` and forgetting a replica clears every
  channel entry.

### Safety and maintainability

- Cumulative simulator diagnostic counters now saturate instead of wrapping.
- Impossible reliable-byte or unreliable-copy dequeue underflow closes the link
  as `TransportFailure` instead of wrapping or delivering unaccounted work.
- Slot/generation hashes now combine into `uint64_t` before hashing, avoiding a
  width-dependent shift in `ConnectionId`, `ObjectId`, and `WireObjectId`.
- The scheduler count-versus-byte validation bug was fixed separately in
  published commit `24fdeae786884973bf661d272029a2e5a689389a` and revalidated
  here. Counts derive from exact queue accounting; byte ceilings constrain bytes.

### Documentation clarification

The simulator defines newest accepted sequenced state at destination-transport
observation. If consumer event congestion drops that unreliable value, an older
sequence remains stale. A regression now makes this intentional behavior explicit.

## Known-issue triage

### Fix before Foundation 5

None remain after this pass.

### Safe to defer

- backend congestion-control behavior, diagnostics, certificate/key handling,
  and handshake integration require real-transport evidence;
- production scheduler fairness and tuning require measured workloads;
- final wire sequence width/wrap and acknowledgement formats remain codec work;
  local strong sequences already fail closed on exhaustion; and
- `ValidateProtocolJsonDocument` is intentionally a pre-parse byte/depth guard;
  the actual JSON parser and tree validator own syntax and node correctness.

### Obsolete or already fixed

- scheduler message-count versus byte-ceiling comparison: fixed by `24fdeae`;
- simulator activation reservation and terminal-notification risk: fixed here;
- partial bandwidth/statistics mutation on delivery-time overflow: fixed here;
- per-object-only replica sequence knowledge: corrected before coordinator use;
- width-dependent identity hashing: corrected across the shared identity forms.

`KNOWN_ISSUES.md` contains no networking-foundation entry. Its current rendering,
physics, project, and Luau items do not affect this real-transport checkpoint.

## Adversarial evidence

The deterministic suites cover malformed values and enums, invalid identities,
stale generations, illegal lifecycle transitions, sequence exhaustion, downward
limit negotiation, exact reliable ceilings, one-over-limit failure, unreliable
congestion, repeated sequenced supersession across a budget-limited flush,
transport-versus-scheduler drop statistics, pending activation saturation,
terminal notification retention, near-maximum timestamp rejection, cancelled
old-generation delivery, equal-time ordering, repeated seeded traces, and bounded
5,000-message scheduler and simulator workloads.

The supported Debug build and complete CTest suite remain the release gate for
this checkpoint. A focused security diff review covers the changed identity,
bounds, queue, and lifecycle surfaces. Foundation 5 remains unimplemented.
