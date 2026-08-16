---
status: current
owner: networking
last_verified: 2026-08-15
related_code:
  - include/gargantuan/network/SimulatedTransport.hpp
  - src/network/SimulatedTransport.cpp
  - tests/SimulatedTransportTests.cpp
related_adrs:
  - docs/src/content/docs/developing/networking-architecture.mdx
---

# Deterministic simulated transport

## Purpose and boundary

`SimulatedTransport` is the first executable `IGameTransport` backend. It is an
in-memory semantic test backend, not a socket emulator. It accepts only validated
`NetworkMessageIntent` values and emits ordinary `TransportEvent` values. It has
no DataModel, `MutationGateway`, script security, schema decoder, authentication,
packet framing, runtime thread, or external network dependency.

`IGameTransport` gained one evidence-driven correction: `Disconnect` closes one
connection without stopping a server endpoint. Time advancement, seeded faults,
and scheduled disconnect injection remain concrete simulator APIs rather than
polluting the backend-neutral interface.

## Topology and deterministic clock

A `SimulatedNetwork` owns one topology, clock, seeded SplitMix64 random stream,
and globally ordered schedule. Server and client transports use the existing
`TransportStartConfiguration`. The endpoint host and port form an opaque
simulator key; no name lookup, socket, port binding, or address resolution occurs.
One server endpoint can accept multiple clients.

Time starts at zero. `Advance` moves it monotonically and rejects negative or
overflowing movement. `Pump` is the only operation that processes due work.
There are no sleeps or background workers. Scheduled work is ordered by absolute
microsecond time and then by one monotonic insertion counter, so equal-time events
have stable insertion order. Clock chunking does not change final delivery order.

## Configuration and fault model

`SimulatedTransportConfiguration` validates before a network is created. It
bounds latency, jitter, reorder delay, probabilities, bandwidth, reliable bytes,
unreliable messages, datagram size, transports, connections, scheduled work, and
pending events. Probabilities must be finite and within `[0, 1]`; durations must
be nonnegative and below the native simulated-duration ceiling. Seed zero is
valid and deterministic. Startup also rejects a reliable queue incapable of
holding one advertised valid reliable message.

The seeded fault model supports:

- nonnegative base latency plus bounded nonnegative jitter;
- unreliable loss;
- unreliable duplication;
- bounded additional delay that produces reordering;
- per-direction connection bandwidth serialization;
- explicit or configured-time disconnects; and
- negotiated/configured datagram and queue ceilings.

The simulator does not use global randomness. Identical configuration, start and
send order, and clock operations produce an identical observable event trace.

## Reliable and unreliable semantics

Accepted reliable messages are message-oriented, never affected by the
unreliable loss control, and are clamped to nondecreasing delivery time for each
connection direction. This is at least as strong as every currently declared
reliable logical ordering domain. Accepted reliable bytes count against the
minimum of negotiated and simulator queue ceilings until delivery. Hard overflow
returns a structured terminal `ResourceExhaustion` and schedules connection
closure; reliable work is never silently dropped while the connection remains
healthy.

Unreliable application messages must fit the effective datagram and are never
fragmented. Loss, duplication, and reorder decisions occur once at send time
from the topology RNG. In-flight unreliable copies are bounded. Loss or
congestion returns best-effort send success while incrementing drop statistics;
no retry state is retained.

The existing `UnreliableSequenced` delivery contract reaches the transport with
strong channel and sequence metadata and reaches the receiver as the same
metadata. The simulator therefore enforces that contract at delivery: per
connection and sequence domain/channel, a value no newer than the latest
accepted sequence is dropped before a receive event is queued. It does not
inspect or decode the payload. Unordered unreliable traffic remains available
to test raw duplication and reordering.

"Accepted sequence" means observed by the destination transport at delivery
time. If consumer event capacity then drops that unreliable value, an older
value in the same channel remains stale and cannot resurrect superseded state.

## Backpressure, lifecycle, and close policy

Bandwidth is modeled as deterministic serialization time on each connection
direction. It does not implement scheduler traffic-class priority. Pending
reliable bytes, unreliable copies, scheduled work, and receive events all have
hard ceilings. Receive-event admission accounts for cumulative reservations for
pending activation transitions and terminal lifecycle outcomes before accepting
another connection or message. Terminal events therefore cannot disappear behind
an already saturated receive queue. Reliable receive exhaustion closes the
connection; unreliable receive exhaustion drops the message.

Delivery-time calculation stages bandwidth availability, reliable ordering time,
fault RNG state, and send statistics. Time overflow rejects the send without
committing any of that prospective state. Internal reliable-byte and unreliable-
copy accounting underflow closes the affected link as a structured transport
failure rather than wrapping.

Connection slots are reused only with an incremented generation. Startup queues
`Connecting -> Authenticating -> Connected` and exposes it only after `Pump`.
Local close becomes `RemoteShutdown` for the peer; timeout, transport, protocol,
and resource failures retain their structured reason. Duplicate pending closes
are idempotent. Terminal close cancels all scheduled link work, removes unpolled
link events, discards unreliable work, and discards pending reliable work under
the explicit terminal-failure policy. Exactly one disconnect event is retained
per endpoint and no later scheduled item can resurrect the generation.

## Statistics

The simulator meaningfully populates bytes sent/received, messages sent,
delivered, and received, unreliable drops and delivered duplicates, reliable
queued bytes, estimated base-latency RTT, and an unreliable loss estimate. These
fields remain optional in the backend-neutral contract so a real backend need
not fabricate unavailable measurements. Cumulative `uint64_t` diagnostics
saturate at their maximum instead of wrapping; current queue gauges remain exact
and bounded by their configured resource ceilings.

## Deliberate differences and deferred work

This backend does not reproduce TCP, UDP, GNS, QUIC, kernel buffers,
retransmission packets, congestion control, encryption, address resolution, or
OS errors. Reliable loss is modeled by preserving the semantic outcome rather
than emulating a retransmission protocol. Jitter is a bounded nonnegative delay.
Bandwidth remains a clear per-direction serializer. The production
`NetworkScheduler` is a separate higher layer and is exercised against this
backend.

`NetworkScheduler`, `ReplicationCoordinator`, the basic binary replication
codec, reliable replica execution, and bounded Luau Remotes are implemented
above this backend. Authentication/tickets, Players, realtime replication,
Node, and Studio play mode remain unimplemented.
