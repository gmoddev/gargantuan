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
