---
status: current
owner: networking
last_verified: 2026-08-15
related_code:
  - include/gargantuan/network/GameNetworkingSocketsTransport.hpp
  - src/network/GameNetworkingSocketsTransport.cpp
  - cmake/GameNetworkingSockets.cmake
  - tests/GameNetworkingSocketsTransportTests.cpp
related_adrs:
  - docs/src/content/docs/developing/networking-architecture.mdx
---

# Real game transport

## Backend selection and build

`GameNetworkingSocketsTransport` is Gargantuan's first real `IGameTransport`
adapter. It uses Valve GameNetworkingSockets 1.6.0 at immutable source revision
`2cb93a06350bb065db53abdb0d87cf297e0bfd34`. GNS was selected because its
connection-oriented, message-oriented API directly supplies reliable ordered
and best-effort unreliable delivery, encryption, congestion control, connection
diagnostics, and compatible statistics without dictating Gargantuan replication
or scheduling policy. The dependency is BSD-3-Clause; its required notice is
preserved in `cmake/gns/LICENSE.txt`.

The adapter is opt-in with `GARGANTUAN_WITH_GNS=ON`. GNS is fetched at the exact
revision, built static, and exposed only through the optional
`gargantuan_gns_transport` target. Backend-neutral `gargantuan_core` and its
unrelated tests do not inherit GNS or Protobuf; no GNS header or native handle
appears in Gargantuan's public transport contracts. Examples, tools, GNS tests,
ICE, Steam WebRTC, and the shared GNS target are disabled.
Windows uses BCrypt and the reference Curve25519 implementation. Other platforms
use OpenSSL and remain configured but were not verified in this milestone.

GNS requires Protobuf. `cmake/gns/vcpkg.json` records a pinned vcpkg manifest
for Protobuf and OpenSSL on non-Windows systems. A normal manifest build can use
the vcpkg toolchain with `VCPKG_MANIFEST_DIR` set to `cmake/gns`. The milestone
verified 64-bit Windows MSVC Debug and Release configurations; Linux and macOS
are supported by the selected upstream backend but require platform CI evidence
before Gargantuan claims them as verified configurations.

## Identity and lifecycle mapping

GNS connection and listener handles remain private implementation details.
Every live backend connection is mapped to a Gargantuan `ConnectionId` slot and
generation. Closed slots are reusable only after generation increment; exhausted
generations fail closed. Callback lookup validates the currently registered
backend handle, and queued events retain only `ConnectionId`, never a pointer or
GNS handle. A delayed callback for an unregistered handle cannot address a new
generation.

The mapping is:

| GNS state or action | Gargantuan semantic state |
| --- | --- |
| outbound creation | `Connecting` |
| inbound accepted or outbound handshake progressing | `Authenticating` |
| GNS connected | `Connected` |
| explicit endpoint/connection close | `Closing -> Closed` |
| peer close or local backend failure | one structured terminal disconnect |

`Authenticating` currently denotes the transport cryptographic handshake only.
It does not establish a Gargantuan user, ticket, capability, or mutation
authority. Endpoint `Stop` and per-connection `Disconnect` are idempotent at the
backend level and return explicit invalid-state/invalid-connection results when
repeated through the Gargantuan contract. Terminal closure removes pending
message events and backend mappings before a slot may be reused.

## Message and delivery mapping

`ReliableOrdered` maps to the GNS reliable message flag. GNS owns fragmentation,
reassembly, acknowledgement, retransmission, congestion control, encryption,
and reliable ordering after it accepts the message. `UnreliableUnordered` and
`UnreliableSequenced` both map to GNS unreliable messages. Sequenced stale-state
rejection remains scheduler policy; the adapter carries the strong order/channel
metadata without interpreting application state.

The adapter adds a private 24-byte compatibility envelope to each GNS message.
It carries a fixed magic, adapter-envelope version, validated delivery mode,
traffic class, order-domain kind, state channel, and strong sequence. It contains
no authority, capability, schema, DataModel command, or backend handle. This is
an internal adapter compatibility boundary, not the future gameplay packet codec
or a public wire-layout promise. Receive rejects truncated frames, bad magic or
version, forged enums, invalid sequence metadata, delivery/backend-flag mismatch,
empty payloads, and values outside active limits before exposing an event.

Unreliable messages use a conservative 1,200-byte complete-frame ceiling and
are never fragmented by Gargantuan, yielding 1,176 application bytes after the
envelope. Reliable messages are bounded by both active `NetworkLimits` and GNS's
512 KiB send-message ceiling, including the envelope. The adapter exposes the
effective unreliable application ceiling through `GetAvailableDatagramBytes`.

## Polling, threads, and lifetime

The adapter creates no Gargantuan worker thread. GNS may use its own internal
service thread for transport mechanics. Gargantuan lifecycle callbacks are run
only when `PollEvents` calls GNS `RunCallbacks`, and application code receives
only copied `TransportEvent` values through caller-owned output storage. No
DataModel mutation, Luau callback, or scheduler action occurs in a GNS callback.

GNS global initialization, callback routing, adapter lifetime, and public adapter
operations are serialized by one recursive mutex. Registries are removed before
connection/listener destruction and the last adapter releases the global GNS
instance. A stopped adapter can be polled to drain terminal events, but it cannot
restart until all old events are consumed; this prevents event generations from
crossing endpoint lifetimes.

## Bounds and backpressure

Configuration validates bounded connection count, pending event count, and
pending receive bytes before startup. Event capacity reserves all lifecycle and
terminal events needed by admitted connections. Message events cannot consume
that reserve. Inbound connections are refused before identity allocation when
the connection or event budget is exhausted.

Every receive is checked before allocation/exposure against the GNS native
message maximum, envelope rules, active reliable/unreliable and decoded-message
limits, the per-poll receive byte/message limits, pending receive bytes, and
pending event capacity. Malformed input closes with `ProtocolViolation`;
reliable receive-resource exhaustion closes with `ResourceExhaustion`. No valid
prefix of an invalid message is emitted.

Before reliable submission, the adapter compares GNS pending reliable bytes
against the validated Gargantuan reliable-queue ceiling without overflowing.
GNS acceptance results map to structured success, temporary blocking, resource
exhaustion, invalid state/connection, message rejection, or transport failure.
The future scheduler remains responsible for semantic admission, prioritization,
per-tick policy, unreliable supersession, and retrying a temporary submission.

## Statistics and disconnect mapping

Application bytes/messages sent and received are cumulative Gargantuan counters.
Current queued reliable bytes and smoothed RTT are populated from compatible GNS
metrics. GNS metrics without the same Gargantuan semantic meaning—application
messages delivered, unreliable application drops, duplicates, and loss ratio—
remain unavailable rather than being fabricated. Scheduler pre-transport drops
remain separate from all transport statistics.

Local/remote shutdown, timeout, resource exhaustion, protocol/version failures,
and other transport failures map to `DisconnectReason`. Backend-native integer
codes never leave the adapter. Caller diagnostics remain in local Gargantuan
events; only fixed bounded ASCII reason text is sent to GNS. The adapter's private
application end-reason range maps resource, protocol, and version failures
symmetrically at the peer.

## Security and authority boundary

GNS supplies encrypted transport mechanics, not Gargantuan peer authentication.
The host-owned `OpaqueHandshakeMaterial` hook remains distinct from application
payload and authority; this adapter rejects nonempty material until a ticket and
identity protocol is designed. Payload cannot assign capabilities or create a
`MutationAuthorityContext`. The adapter has no DataModel, `MutationGateway`,
schema registry, script context, replication coordinator, or Luau dependency.

## Simulator comparison and deferred work

The deterministic simulator remains the semantic fault laboratory: explicit
time, seeded loss/duplication/reordering, deterministic bandwidth, and exact
trace reproduction. GNS supplies real localhost/OS networking, encryption,
congestion behavior, reliable delivery, real connection failure, and backend
metrics; it does not promise deterministic timing or injected faults through
this adapter.

The production `NetworkScheduler`, `ReplicationCoordinator`, versioned basic
replication codec, and client applicator now run over the localhost GNS test
path. Authentication/tickets, negotiated-limit exchange, `RemoteManager`, Luau
remotes, Players, realtime replication, Node, spatial interest management, and
Studio play mode remain unimplemented.
