# Multiplayer terrain delivery and collision admission

Terrain streaming runs on matching loopback Development packages over the
existing ordered control scheduler. Foundation 10A governs distribution
audience. LAN and Release terrain sessions require a separately qualified
content and identity path.

## Package and session identity

The unified Engine assigns GSES version 4 to hello, acceptance and
readiness for terrain. Each carries one nonzero 32-byte terrain binding.
The historical Terrain-only candidate used version 3. That version is
reserved in the unified Engine for the distinct managed package hello and
acceptance format. Ordinary terrain-free sessions retain version 2. The
Engine rejects a handshake that combines terrain and managed code identities.

Terrain peers compare SHA-256 over the project UUID, little-endian package
revision, canonical GTMF digest and little-endian terrain stream version.
An incompatible peer is rejected before Player creation. A matching trusted
loopback Development client pins its GTMF and verifies startup code and
assets. Offline, server and full validation admit the complete persisted
closure.

## Regional transfer

The server selects a 3×3×3 source neighborhood around the authoritative
Character or possessed part. A generation-safe session epoch and monotonic
regional serial identify each transfer. The sender acquires only the
region's immutable field snapshot on Main. A worker encodes the canonical
chunk artifacts and manifest for that snapshot. When the manifest matches
the corresponding package subset both peers derive it locally. After each
acknowledgment both peers can reuse matching leaf digests for the next
window. The receiver retains its verified encoded leaves. Ready server
peers retain the digest metadata.

| Frame | Bytes | Identity and payload |
| --- | --- | --- |
| `GTNW` offer | 96 | Epoch, serial, field revision, manifest SHA-256, signed center, manifest bytes and incoming leaf bytes |
| `GTNB` fragment | 28 plus payload | Epoch, serial, wire offset and 1–16,384 bytes |
| `GTNR` readiness | 64 | Exact epoch, serial, field revision and manifest SHA-256 from the offer. The client acknowledges local collision. The server confirms the current regional receipt to release movement. |

The terrain frames use little-endian version 4 and ordered reliable control
delivery. A region has at most 27 source leaves, a 1 MiB transfer ceiling
and a single outstanding queued fragment. Four transfer reservations
bound parallel work, including canceled encoding until its worker
releases the immutable snapshot. Admission rotates among ready peers.
Invalid offsets, sizes, bindings, digests or sequencing close the session.
Backpressure pauses production until control and native reliable capacity
return. Fragment size leaves room for the reliable envelope and other
traffic at the negotiated queue ceiling. A capture superseded before
offer admission is refreshed under its original deadline.

## Collision cutover

The initial detached field enters Workspace before the client Engine
starts. The client cooks native collision and queues the regional
acknowledgment before GSES ClientReady. Client movement, actions and
possession input wait for the server's validated readiness echo. If an
edit or a distant teleport supersedes the first receipt, the server
waits for the replacement before confirming movement. Later windows
decode and prepare terrain off Main. Main stages native collision and renderer-neutral
resources, publishes their paired state and retires departing chunks
before acknowledging. Matching collision geometry retains its provider
body through paint or normal changes. Visual residency does not govern
movement readiness.

During transfer normal movement stays inside verified collision coverage.
The server checks active local chunk revisions, so an edit outside the
current region leaves motion available. A moving peer stays within the
intersection of the installed, pending, and server-confirmed regions
during a same-revision transfer. A new revision holds movement until the
server confirms the current receipt, even when a simultaneous spatial
handoff carries a revision that changed farther away. Each capsule
sweep and correction stays within its admitted volume. Terrain-inclusive
queries can use installed collision and distinguish covered air from
unavailable client collision. A missing confirmation closes the session
at the client deadline.

Coverage uses a four-sample inset at each face. A stock Character uses its
capsule bounds and a possessed anchored part uses a conservative bounding
sphere. A part that exceeds the full available interval stays fenced.
Native half-spacing possession was qualified through a signed handoff
with a radius-9 part.

The authored field and mutation gateway remain server-owned. Multihost
connectivity and sustained revision churn have separate qualification
requirements.
