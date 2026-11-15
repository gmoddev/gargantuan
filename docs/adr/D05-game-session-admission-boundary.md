---
status: accepted
date: 2026-11-15
supersedes: null
superseded_by: null
---

# D05 — Game session admission boundary

Engine owns the point at which a connected peer becomes an authoritative Player.
The server may select an authenticated GameSession mode at startup and inject one
`IGameAdmissionProvider`. The provider is a host-owned dependency, not a network
message or a Luau capability. It verifies an opaque, one-use proof against its
configured audience and current hosting authority, then returns a validated
`PlayerIdentity`. Engine does not depend on Node, SQL, a ticket issuer, or a
particular host API.

Authenticated mode uses GSES protocol version 2. The client hello carries exactly
32 opaque proof bytes. A version-1 hello cannot enter the authenticated path;
the version-2 response identifies authenticated admission. Version-1
`DevelopmentLocal` remains the explicit development mode and keeps its parsed
loopback restriction and existing insecure-development override. A failed or
unavailable provider never falls back to `DevelopmentLocal`.

Only the GameSession thread may create a Player. Provider completions can run on
another thread and enter a one-result slot with no Engine pointer. GameSession
processes that result only while the same generation-safe connection and session
lifetime remain pending, before the existing bootstrap path. The peer count,
pending admission count, frame length, and handshake time are bounded. Disconnect,
timeout, Stop, and session failure close the slot and request provider
cancellation. Late or duplicate completions cannot create a Player.

The provider contract defines verification ownership; it does not itself
implement durable proof issuance or redemption. A private host adapter must bind
proofs to the exact server/session audience, enforce one use across retries, and
return the qualified account identity. Packaged host wiring, external identity
service, and real-client admission remain separate integration work. The wire
format and current generic boundary are specified in
[GameSessionAdmission](../../devdocs/CurrentArchitecture/GameSessionAdmission.md).
