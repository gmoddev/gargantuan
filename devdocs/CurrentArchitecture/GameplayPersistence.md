# Gameplay persistence

Foundation 11 puts game storage behind `IPersistenceProvider`. Engine owns
server-only Luau admission and result delivery. Providers own storage
authorization, atomic record operations, durability, deadlines and uncertainty.
One trusted platform host selects the provider before executing gameplay. An
unconfigured runtime is unavailable. The provider configuration is native and
stays outside project serialization, replication, and the Player launch path.

## Provider contract

The provider receives one native request and a stop token on a worker. The
request owns its bytes or document data, logical store, key, optional expected
revision, correlation ID, and absolute steady deadline. The Engine capability
flags describe supported operations. A provider can independently implement
bytes, documents and the bounded document query subset. A capability does
not promise current availability. Revision comparison, mutation and revision
allocation are one storage operation. Revision zero is reserved for creating
an absent key. Compare positive revisions exactly and treat them as opaque
in gameplay. Native values and public results are documented in the
[Luau API](../../docs/src/content/docs/api/networking-and-persistence.mdx).

Providers return a confirmed result, a confirmed rejection, or
`UnknownOutcome` for a mutation whose commit cannot be determined. Cancellation
or expiry before a worker starts is definite. After dispatch the provider
classifies the boundary. The runtime treats a throwing provider or a malformed
mutation result as uncertain. Request IDs correlate diagnostics. Application
idempotency can use a stable reward identifier stored in the same atomically
updated record. A cross-record transaction, general deduplication ledger or
automatic mutation retry requires an additional contract.

## Runtime ownership and bounds

The pool starts on demand, uses two native workers and admits up to 32
operations per runtime, including no more than eight per known managed script.
Callbacks and tasks without direct managed-Script attribution share eight
runtime-owned admissions. The next safe point requests cancellation for
retired, detached or disabled owning scripts. Games should explicitly cancel
player-related work at departure. A cancelled mutation may still commit.

Each Main step makes at most eight native results visible through operation
handles. No worker holds a VM, executes a script, or resumes a continuation.
Successful reads reserve capacity at admission through observation or handle
retirement, limited to 16 MiB of conservative record reservations per runtime.
Luau consumes a successful Get or Query payload once. Shutdown closes admission,
signals stop, discards queued jobs and joins the up to two executing jobs. Native
providers must obey the steady deadline, including internal contention. Local OS
I/O can overrun a timer during a storage stall. Compose a new runtime to change
provider scope. Operation handles cannot cross runtime identity.

## Provider composition and deployment

| Mode | Trusted configuration and behavior |
| --- | --- |
| Disabled | Default in Engine, dedicated Server, and Studio. |
| Memory | Per-run bounded development provider. Dedicated Server uses `--persistence-provider memory`. Studio labels this Disposable and creates a fresh provider for each Play. |
| SQLite | Opt in at build with `GARGANTUAN_WITH_LOCAL_SQLITE=ON`. Use a trusted local filesystem. Dedicated Server uses `--persistence-provider sqlite --persistence-local-db <path>` with an existing parent directory. The database binds to the packaged project identity. Studio selects its own app-data profile outside the project tree. |
| Node | Build the Server with `GARGANTUAN_WITH_NODE_PERSISTENCE=ON` and `GARGANTUAN_NODE_PROTOCOL_ROOT` pointing to the pinned Node protocol tree. Use `--persistence-provider node`, `--persistence-node-endpoint`, `--persistence-node-root-ca` and `--persistence-node-token-file`. The token file must be a protected regular file on a local filesystem and can be replaced atomically with an equivalent tenant credential. |

SQLite 3.54.1 is bundled as an opt-in static build with a pinned source
hash. Local providers cap each profile at 10,000 records and 32 MiB of
accounted payload by default. The SQLite profile also has a 128 MiB database
page cap and rejects externally supplied journals over 256 MiB. It uses a
versioned typed binary encoding and acknowledges mutations only after the
storage transaction commits. Keep the native profile, potential rollback
journal and reserved initialization sibling together during recovery.
[SQLite source](https://www.sqlite.org/download.html)

Node binds tenant authority to the workload token and method grants. A game
requiring storage isolation has its own tenant. Enable DataStore v1 and
PostgreSQL in Node, configure tenant-constrained database roles, and grant the
server workload the precise DataStore capabilities. The adapter requires TLS,
validates bounded response shape and correlation, and uses the same channel
for requests. Reads may wait for reconnection within their existing deadline.
Mutations fail fast during channel failure and do not replay. Engine cancellation does not by itself establish that PostgreSQL
rolled back. Existing Node providers filter and sort up to a configured
collection ceiling after retrieving and decoding candidate documents. Query
page size controls network output. Indexed scans and SQL pushdown still need
result, ordering, cursor and stale-epoch parity.
[Node DataStore](https://github.com/gmoddev/gargantuan-node/blob/cd6294818670f3ddad5a4d93d949c711e495ec22/docs/datastore-postgres.md)

## Related backend boundaries

GameSession still distinguishes a transport peer, a runtime Player and an
authenticated identity. Native Player setup can bind an immutable verified
identity, which `EntitlementService` uses for semantic checks and preserves
`Unavailable` on provider failure. DevelopmentLocal and runtime `PlayerId`
cannot identify a durable real account. A production integration must thread
a verified, replay-safe provider assertion through admission before Player
allocation, bind the canonical provider/subject, and expose an opaque
server-only profile key. Coordinate that seam with hosting and qualify
expiry, rejection, reconnect and rejoin across servers.

The official Server content adapter still consumes ContentStreaming v1.
A private V2 consumer needs an immutable publication pin, per-chunk and
whole-artifact verification, bounded resume/cache accounting, and the existing
detached canonical admission path. Foundation 10A retains its own
client-audience confidentiality gate before public multiplayer distribution.
See the [backend owner contracts](BackendProviderIntegration1.md).
