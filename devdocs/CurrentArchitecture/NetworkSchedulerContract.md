---
status: current
owner: networking
last_verified: 2026-08-15
related_code:
  - include/gargantuan/network/Scheduler.hpp
  - src/network/Scheduler.cpp
  - tests/SchedulerContractTests.cpp
  - tests/SimulatedTransportTests.cpp
related_adrs:
  - docs/src/content/docs/developing/networking-architecture.mdx
---

# Network scheduler contract

## Validation checkpoint

The deterministic simulator and pure contracts now prove the transport and
scheduler-facing portion of the accepted networking architecture. The simulator
owns delivery mechanics, connection lifecycle, in-flight transport limits, and
transport observations. A future production `NetworkScheduler` owns bounded work
before transport submission. Neither layer owns application or DataModel
authority.

The validation matrix is deliberately explicit about later layers:

| Accepted requirement | Classification | Evidence or remaining owner |
| --- | --- | --- |
| Reliable healthy-connection delivery | `PROVEN` | Simulator eventual-delivery and stress tests |
| Reliable ordering | `PROVEN` | Per-direction ordered simulator delivery tests |
| Unreliable loss | `PROVEN` | Seeded forced-loss test and drop statistics |
| Unreliable duplication | `PROVEN` | Seeded forced-duplication test and duplicate statistics |
| Unreliable reordering | `PROVEN` | Seeded bounded-reorder test |
| Unreliable sequenced stale rejection | `PROVEN` | Reordered older sequence is discarded per channel |
| Connection lifecycle | `PROVEN` | Connect, authenticate, connected, close, timeout, and replacement tests |
| Structured disconnect | `PROVEN` | Local/remote, timeout, resource, and transport failure tests |
| Reliable and event-queue boundedness | `PROVEN` | Hard ceilings, overflow closure, and bounded stress tests |
| Datagram boundedness/no unreliable fragmentation | `PROVEN` | Prequeue datagram rejection test |
| Bandwidth throttling | `PROVEN` | Deterministic serialization/drain test |
| Explicit deterministic time | `PROVEN` | `Advance`/`Pump`, monotonic exhaustion, and clock-chunk tests |
| Deterministic seeded faults | `PROVEN` | Repeated equal-seed traces and different-seed divergence |
| Stale connection identity | `PROVEN` | Slot reuse advances generation and stale operations fail |
| Transport statistics | `PROVEN` | Queue, delivery, receive, drop, duplication, and unavailable-value tests |
| Scheduler traffic precedence | `PROVEN` | Deterministic policy proof orders control through background |
| Scheduler reliable admission/backpressure | `PROVEN` | Bounded queue, temporary `WouldBlock`, and terminal exhaustion tests |
| Scheduler unreliable drop/supersession | `PROVEN` | Pre-transport drop and same-channel newest-wins tests |
| Scheduler budgets and flush semantics | `PROVEN` | Per-flush limits, multi-tick drain, and simulator integration tests |
| `ChangeJournal.Sequence` type separation | `PROVEN` | No scheduler/transport contract accepts journal sequence |
| Irrelevant journal skipping and state coalescing | `DEFERRED TO REPLICATION` | Requires `ReplicationCoordinator` behavior |
| Publish/unpublish execution | `DEFERRED TO REPLICATION` | Intent types are proven; execution is not implemented |
| Remote request termination and handler budgets | `DEFERRED TO REMOTES` | Terminal value vocabulary exists; runtime does not |
| Reference visibility/dependency policy | `DEFERRED TO REPLICATION` | Per-peer planning is intentionally above scheduler |
| Backend retransmission, congestion control, and path behavior | `DEFERRED TO REAL TRANSPORT` | Simulator proves semantics, not backend mechanics |
| Ticket validation and authenticated peer establishment | `DEFERRED TO AUTHENTICATION` | Opaque handshake data carries no authority |
| Transport and scheduler authority separation | `PROVEN` | No DataModel, mutation, capability, schema, or Luau dependency |

This checkpoint corrected validation sequencing, not an accepted invariant:
transport and scheduler semantics are the gate before a real adapter; remote and
replication behavior must be proven against the simulator when those layers are
implemented. Requiring nonexistent later layers to pass before the real adapter
would contradict the accepted implementation order.

## Scheduler ownership

`INetworkScheduler` defines the narrow future production boundary:

```text
trusted engine subsystem
    -> validated NetworkMessageIntent
    -> RegisterConnection / Submit
    -> explicit Flush for one connection and tick budget
    -> IGameTransport::Send
```

The scheduler owns:

- generation-safe registered connection state and validated negotiated limits;
- per-connection semantic queues;
- admission, byte/message accounting, and observable rejection;
- traffic-class selection at explicit tick/flush boundaries;
- reliable backlog policy and transport-backpressure retention;
- unreliable congestion drop and sequenced unsent-state supersession;
- per-connection bytes/messages submitted per tick;
- opportunities for semantics-preserving future batching; and
- scheduler statistics distinct from transport statistics.

It does not traverse or mutate a DataModel, invoke `MutationGateway`, decide
replication relevance, own schema identity, execute remote handlers or Luau,
assign capabilities, authenticate peers, expose backend handles, retransmit,
encrypt, or implement transport congestion control.

## Register, submit, flush, and cancellation

`RegisterConnection` supplies one connected generation and its already validated
session limits from host-owned connection state. Invalid identities, invalid
limits, duplicate registrations, and a live conflicting generation fail closed.
Registration is lifecycle bookkeeping, not authentication metadata.

`Submit` validates the intent against the registered session even though
`NetworkMessageIntent` construction already performs structural validation. It
queues accepted work; it does not call the transport, create a packet, or imply
delivery. Invalid/stale destinations and wider-than-session payloads are
rejected.

`Flush(Connection, Budget)` is the explicit per-connection scheduler tick
boundary. It makes currently eligible work available to `IGameTransport::Send`
within the supplied validated bytes/messages budget. `Drained` describes the
scheduler queue, not the remote peer. `BudgetLimited` retains bounded work for a
later tick. `TransportBackpressured` retains accepted reliable work after
`WouldBlock`. A terminal transport outcome clears that connection's queued state.
There is no acknowledgement implication.

`CancelConnection` removes only the exact generation. It is called for terminal
connection lifecycle and cannot cancel a newer generation that reused the slot.

## Traffic policy

The semantic precedence is:

1. control;
2. structural replication;
3. reliable application;
4. realtime sequenced state;
5. ephemeral application; and
6. background.

This is selection order, not a lane, stream, OS priority, or fixed bandwidth
percentage. Background work cannot delay already eligible control work. A
budget-limited tick may leave lower-priority work queued; the contract does not
promise that every class sends every tick. More elaborate fairness requires
production measurements and remains a scheduler implementation choice provided
control/structural precedence and boundedness remain intact.

## Reliable and unreliable congestion

Accepted reliable work is counted against
`NetworkLimits.MaximumQueuedReliableBytes` and is never silently discarded while
the connection is healthy. Transport `WouldBlock` is temporary backpressure:
the scheduler retains the work and reports it. Crossing the hard scheduler
backlog ceiling is terminal `ResourceExhaustion`; accepted work cannot be
promised after that connection failure.

Unreliable work never gains an unbounded scheduler backlog. The initial contract
retains at most one negotiated tick's byte/message budget before transport;
excess ordinary unreliable intent is dropped observably before submission.
For `UnreliableSequenced`, existing `StateChannelId` and strong sequence metadata
are sufficient: a newer unsent value replaces the older value in the same
domain/channel, while an older or equal value is dropped. No gameplay channel
vocabulary is introduced.

## Batching and budgets

Batching is an allowed optimization after a game codec/framing contract exists.
It may combine eligible small messages only when delivery mode, logical ordering
domain, and application boundaries remain recoverable. It cannot merge reliable
and unreliable semantics, fragment one oversized unreliable message, change
application meaning, or carry authority. This milestone defines no packet frame
or codec and produces no batches.

`SchedulerTickBudget` is valid only against validated negotiated
`NetworkLimits`. It is nonzero, fits the session's per-tick bytes/messages
ceilings, and can submit any one session-valid message. The scheduler does not
invent an independent unbounded budget.

## Scheduler statistics

`SchedulerStatistics` reports intent admission/rejection, reliable backlog
exhaustion, unreliable drops before transport, sequenced supersession, transport
submissions, batching, budget-limited flushes, transport backpressure, and exact
queued bytes/messages. These counters never masquerade as `NetworkStatistics`.
Queue validity applies byte ceilings to byte totals. Message-count consistency is
derived separately from exact queue accounting and the non-empty payload invariant;
a byte limit is never interpreted as a message-count limit. In particular, a
scheduler drop does not increment a transport drop counter.

## Deterministic policy proof and deferred implementation

`SchedulerContractTests.cpp` contains a deliberately test-only deterministic
policy harness implementing the interface. It proves selection, admission,
supersession, flush, backpressure, cancellation, statistics separation, a bounded
5,000-message burst, and integration with `SimulatedTransport`. Identical state,
intent order, limits, and ticks produce identical submission traces.

There is no production `NetworkScheduler` execution, thread, timer, codec,
`ReplicationCoordinator`, `RemoteManager`, multiplayer replication, Luau remote,
real transport, authentication/ticket, Player, Node, or Studio play-mode
implementation.
