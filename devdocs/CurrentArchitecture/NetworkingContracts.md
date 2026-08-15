---
status: current
owner: networking
last_verified: 2026-08-15
related_code:
  - include/gargantuan/network/
  - src/network/
  - tests/NetworkingContractsTests.cpp
  - tests/SchedulerContractTests.cpp
related_adrs:
  - docs/src/content/docs/developing/networking-architecture.mdx
---

# Networking contracts

## Implemented contract boundary

Gargantuan now has transport-independent native value contracts for future game
networking. They contain no socket, packet, backend handle, runtime pointer,
thread, authentication ticket, or DataModel mutation authority.

```text
authoritative DataModel -> ChangeJournal
    -> future ReplicationCoordinator
    -> ReplicationOperation values
    -> future NetworkScheduler implementation
    -> NetworkMessageIntent
    -> IGameTransport

future Luau remotes -> future RemoteManager
    -> remote request/event values
    -> future NetworkScheduler implementation
    -> NetworkMessageIntent
    -> IGameTransport
```

The types are internal foundation contracts, not a public API or wire-format
compatibility promise.

## Identity, lifecycle, and delivery

`ConnectionId` is a cheap slot/generation value. Zero is invalid, reuse requires
a different generation, and no pointer, descriptor, or backend handle is part of
identity. The legal lifecycle is `Connecting -> Authenticating -> Connected ->
Closing -> Closed`; active states may fail directly toward shutdown, but closed
connections never reopen under the same identity.

`DeliveryMode` expresses higher-layer requirements only:

- `ReliableOrdered` requires healthy-connection delivery in declared order;
- `UnreliableUnordered` is best effort and carries no sequence metadata; and
- `UnreliableSequenced` is best effort and requires a state/event channel plus
  a strong sequence, so an older accepted value cannot replace a newer one.

`TrafficClass` identifies control, structural replication, reliable application,
realtime state, ephemeral application, and background intent. It does not assign
lanes, streams, backend priorities, or bandwidth percentages.

## Limits and statistics

`NetworkLimits` contains the core message, decoded-byte, reliable-queue,
in-flight-request, per-tick byte, and per-tick message ceilings. Every field is
nonzero, checked against a hard native safety ceiling, and validated before use.
The send-byte budget can hold either one maximum reliable message or one maximum
unreliable message, so a valid session never advertises a message size that its
own scheduler tick cannot submit.
Negotiation computes the component-wise minimum of two valid advertisements, so
the result cannot exceed either side. There is no handshake exchange yet.

`NetworkStatistics` uses `std::optional` for every backend-dependent measurement.
It can distinguish submitted, delivered, and received messages and can report
unreliable drops or duplicates where a backend measures them. Unavailable values
are not fabricated as zero. RTT must be nonnegative and a present loss ratio
must be finite and in `[0, 1]`.

## Failure and request termination

`DisconnectReason` distinguishes local and remote shutdown, timeout,
authentication failure, protocol violation, resource exhaustion, transport
failure, and version incompatibility. A disconnect is terminal. Backend-native
diagnostic codes are not Gargantuan semantics.

`RemoteRequestId` is a strong monotonic identity separate from every replication
and event sequence. `RemoteRequestOutcome` exhaustively represents success,
timeout, cancellation, disconnect, structured remote error, protocol rejection,
and resource rejection. All are terminal values; no exception or indefinite
wait is the sole outcome representation.

## Sequence domains

`ReplicationEpoch`, `ReliableReplicationSequence`, `RealtimeStateSequence`,
`RemoteEventSequence`, and `RemoteRequestId` are distinct template
specializations and cannot be implicitly interchanged. Zero is invalid. Local
increment refuses `uint64_t` exhaustion rather than wrapping. Network width,
acknowledgement, and wrap protocols remain deferred.

`ChangeJournal.Sequence` is not used by any of these types.

## Replication intent

`ReplicationView` stores one connection's epoch, known objects, relevant objects,
and optional latest realtime sequences keyed by both `ObjectId` and strong
`StateChannelId`. Independent state channels on one replica therefore cannot
overwrite each other's sequence knowledge. Forgetting a known replica clears
all of its channel sequence entries but removes only peer knowledge; it does not
destroy the authoritative object or erase relevance intent.

`ReplicationOperation` is an epoch-scoped variant for publish, property update,
extension-property update, reparent, attribute update, tag add/remove, and
unpublish. Operations use generation-safe `ObjectId`, stable `SchemaId`, exact
definition versions where current custom schema requires them, and validated
`WireValue`. They contain replication intent, not journal history, packet bytes,
visibility policy, or authority. There is intentionally no authoritative
`Destroy` operation in this vocabulary.

## Scheduler and transport boundary

`NetworkMessageIntent` is constructed only after validating destination,
delivery, traffic class, semantic ordering metadata, payload size, and active
limits. Unreliable messages use the unreliable ceiling and are never fragmented
by this contract. Application code cannot name a lane, stream, packet priority,
or backend resource.

`IGameTransport` is one narrow message-oriented interface. A validated start
configuration selects client or server role, a generic endpoint, advertised
limits, and bounded opaque handshake material. The interface starts/stops an
endpoint, disconnects one identified connection, sends validated message intents,
polls into caller-owned bounded event storage,
reports available datagram size and optional statistics, and returns structured
outcomes. It exposes no descriptor, GNS handle, QUIC stream, packet structure,
allocator, or mutation entrypoint. The same boundary can be implemented by
the deterministic simulator, GNS, or QUIC without changing
higher-layer semantics. Per-connection disconnect was added when simulator
evidence showed that server endpoint stop could not express peer-local closure.

Opaque handshake material is host-supplied transport setup data. It is separate
from application payload, `MutationAuthorityContext`, capabilities, and decoded
replication intent; it cannot grant DataModel authority.

`INetworkScheduler`, `SchedulerTickBudget`, structured submit/flush outcomes,
semantic traffic precedence, and scheduler-only statistics now define the
pre-transport policy boundary. `Submit` is queue admission, while `Flush` is one
explicit per-connection tick boundary that may submit eligible work without
promising remote receipt. The deterministic policy proof is test-only; the
production scheduler remains unimplemented. See `NetworkSchedulerContract.md`.

## Deliberately not implemented

There is no real transport, packet framing, codec, production scheduler execution,
coordinator, remote runtime, request timer, coroutine suspension,
multiplayer replication, authentication/ticket validation, player model,
interest management, socket listener, network thread, Node integration, or
Studio play session. The deterministic in-memory implementation is documented in
`SimulatedTransport.md`. No contract type is exposed to Luau.

Existing EditorHost request strings, `ChangeCursor`, `WireJournalRecord`, and
`MutationCompletion` remain specific to IPC, authoritative history/debug
projection, and in-process mutation respectively; they are intentionally not
reused as game-network identity, sequence, replication intent, or remote result.
