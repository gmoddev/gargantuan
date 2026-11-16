# Authenticated Player join input

The official Player accepts --connect host:port --join-proof-env NAME. The
launcher places the short-lived opaque 32-byte join proof in that environment
variable as exactly 64 lowercase hexadecimal digits. The proof value does not
appear in command arguments, diagnostics or package files. This input contains
no Node workload credential, provider identity, database URL or server authority.

Supplying the option explicitly selects authenticated GSES v2. Missing, malformed,
zero or oversized proof, invalid source name, missing endpoint, or an insecure
development override fails before package bootstrap. A failed authenticated join
never falls back to DevelopmentLocal. Existing Player invocations without the
option retain their existing mode and loopback boundary.

Engine treats the proof as opaque. The dedicated server's startup-owned provider
must redeem it against the exact authoritative session before creating a Player.
The public Player neither calls the private admission service nor links its
protocol bindings. The input boundary and CLI denial tests do not qualify the
real Node/PostgreSQL/TLS/server/client lifecycle; that requires separate evidence.

For a non-loopback authenticated join, the launcher also supplies
`--server-key-env NAME`. Its variable contains the expected 32-byte GNS server
public key as 64 lowercase hexadecimal digits. The native GNS adapter checks
the remote certificate key and the encrypted handshake before reporting the
connection to GameSession; a wrong or missing key is closed before the join
proof is transmitted. This key must come from an authenticated Node ticket
bound to the exact live server session. It is not a secret or a replacement for
the short-lived one-use proof. The private server key never leaves GNS.

The Engine key-pin path is not yet qualified with Node ticket propagation and
real separate-process client/server tests. Existing deployment containment keeps
managed hosting on loopback until those gates pass. An authenticated
configuration cannot use the development override to escape this restriction.
