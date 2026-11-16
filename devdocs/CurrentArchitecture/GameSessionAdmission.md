# GameSession admission and GSES v2

`GameSession` owns Player creation. In `DevelopmentLocal`, its GSES version-1
client hello retains the existing 48-byte frame and creates a session-scoped
development identity after bounded limit negotiation. The parsed loopback rule
and explicit native insecure-development override still apply. Version 1 has no
proof and must not be interpreted as authenticated admission.

`Authenticated` is an explicit startup configuration. A server requires a
startup-injected `IGameAdmissionProvider` and no client proof; a client requires
one 32-byte proof and no provider. Neither mode may be inferred from the
endpoint, a transport connection, or a received frame. Authenticated mode can
use a non-loopback endpoint. The development override is invalid in that mode.
The generic provider interface contains no Node, SQL, or host protocol types.

## Wire format

All integers are little endian. Every frame starts with `GSES` (4 bytes),
version (uint16), kind (uint8), and a zero reserved byte. The maximum complete
frame remains 256 bytes. Version 1 retains its original payloads and identity
kind `DevelopmentLocal=1`.

Version 2 uses the same message kinds and exact legacy fields, with these
differences:

| Kind | Version-2 rule |
| --- | --- |
| ClientHello (1) | Existing nonzero nonce (uint64) and eight uint32 network limits, followed by exactly 32 opaque proof bytes. |
| ServerAccepted (2) | Existing fields and reserved zeros; identity kind must be `Authenticated=2`. |
| ClientReady (3) | Existing fields; version remains 2 and must match the accepted peer. |

The v2 ClientHello is 80 bytes. No proof is echoed in an acceptance or a log.
Truncation, trailing bytes, invalid limits, unknown versions or identities, and
cross-version message sequences reject the peer. A v1 server never accepts a v2
hello, and an authenticated server never accepts a v1 hello. A client rejects a
server acceptance with the wrong version or identity.

## Admission lifecycle

The server negotiates limits and registers the connection before asking the
provider. It allows at most 64 pending verifications among the existing
512-peer bound. Each request carries a nonzero process-unique request ID, the full
`ConnectionId` including generation, client nonce, and exact proof. A provider
can answer inline or asynchronously with one `PlayerIdentity` or denial; refusal
or exception denies. The GameSession thread validates provider identity syntax
before the existing Player, relevance, replication, and Character bootstrap.

The existing bounded handshake tick deadline covers provider latency. Completion
is processed after timeout checks, so timeout wins a same-tick race. Disconnect,
timeout, Stop, and session failure close the completion slot and call `Release`.
The request ID remains owned by the peer after approval, so accepted disconnect
and Stop also release the provider seat exactly once. The provider must make
release nonblocking and idempotent, including for failed partial handshakes.
The callback retains only a weak slot, never Engine or a session pointer. A
late or duplicate callback cannot revive a peer or authorize a replacement
generation. Provider implementation must keep release nonblocking and
enforce proof audience, one use, current lease, and account provenance. Engine
does not treat transport encryption or an opaque proof as identity by itself.

The native GNS transport can opt into one process-local Ed25519 server leaf key
before opening a managed listener. It signs GNS's existing key-exchange
handshake. The client requires an exact public key, rejects missing or wrong
key material and non-AES-GCM cipher state before reporting Connected, and sends
no GSES proof on that path. Engine does not export the private key. This is a
live server process pin, not a certificate authority or a durable host identity.

The generic server host can now be composed with one trusted admission provider
and one managed lifecycle observer. It emits copied package identity, manifest
digest, bind endpoint, and the actual GNS server public key only after package/content bootstrap and a listening
GameSession. The observer supplies a cached authority/READY snapshot; the host
does not poll the session until authority is current and the external READY
acknowledgement is observed. Its readiness gate expires no later than six
seconds from host entry; the external supervisor retains the process-wide
startup deadline for synchronous package and runtime work. Authority loss or a
drain request closes the session before
Engine teardown, and the observer is notified before drain and after stop.
The default official server remains DevelopmentLocal.

The generic boundary is exercised by simulated-session tests. Private Node
binding of the live key into an authenticated ticket, durable proof redemption,
and real separate-process client/server qualification remain required before
non-loopback production hosting.

The combined Creator/hosting component regression additionally runs a
nonpredictable semantic action with a nested ReplicatedStorage Character
template through authenticated v2 provider approval. It checks no Player before
approval, the provider-qualified identity, authoritative action presentation and
end, unchanged runtime materialization epochs, and exactly one Player/seat
release after disconnect and Stop. This uses the simulated transport and manual
test provider; it does not establish real Node redemption or private package
qualification. The original development-local and admission denial tests remain
separate and unchanged.
