# Networking and security architecture

## Non-negotiable model

Clients are hostile. The server owns gameplay truth, object creation permissions,
damage, inventory/currency, persistence, competitive movement outcomes, and every
privileged service. A client sends input or a constrained intent—not an arbitrary
property write or “I hit this player” result.

Network ownership, if exposed, is a revocable simulation lease used for latency
and bandwidth. It never grants gameplay authority. The server validates state,
can correct it, and owns final event generation.

## Connection and session layers

Keep control plane separate from gameplay transport:

- **Discovery/session service:** finds deployments, issues short-lived join
  grants, selects protocol/build/content versions, and returns endpoints.
- **Game transport:** authenticated encrypted connection, connection migration if
  needed, reliable ordered streams plus bounded unreliable datagrams.
- **Game protocol:** versioned handshake, clock/tick synchronization, replication,
  events/requests, snapshots, acknowledgements, budgets, and disconnect reasons.

Prefer a mature encrypted transport (for example QUIC where platform support is
acceptable) behind a transport interface. Do not invent cryptography or bake
game semantics into socket callbacks. Self-hosted deployments use the same
protocol and can replace discovery/auth providers through server-only adapters.

Handshake fields should include protocol range, engine build, project/package
identity, schema hash, required asset manifest hash, feature flags, compression,
authentication proof, and server nonce. Reject mismatches before allocating a
world/player or loading large content.

## Replication model

The schema registry marks each class/property as `ServerOnly`, `OwnerOnly`,
`RelevantClients`, `InitialOnly`, `UnreliableState`, or `NotReplicated`, with
quantization and validation rules. Never infer security from a container name.

Replication records are explicit and bounded:

- create: `ObjectId`, class/schema ID, parent ID, initial property mask/values;
- update: baseline/tick ID, object ID, property mask, delta/absolute values;
- reparent and destroy: IDs plus ordered journal sequence;
- event: channel ID, sequence/tick, schema-validated payload;
- acknowledgement: received baseline/ranges and client timing;
- correction: authoritative state and prediction reconciliation metadata.

Use per-client baselines and deltas, periodic recovery snapshots, deterministic
ordering, and generation-checked IDs. Unknown IDs, duplicate creates, stale
generations, invalid parent graphs, non-finite values, and payloads outside schema
limits are protocol errors—not unchecked engine operations.

## Interest and streaming

An `InterestService` computes relevance from spatial cells, player/party/team
relationships, explicit subscriptions, and always-relevant objects. It returns a
budgeted priority set; it does not serialize data itself. Replication maintains a
per-client view and streams create/update/remove transitions.

Start with a uniform spatial grid plus explicit always-relevant roots. Measure
before adopting a complex spatial structure. Define hysteresis so objects do not
thrash at cell boundaries, and prioritize player-critical state over decoration.

## Events and requests

Use two deliberately different APIs:

- `NetworkEvent`: one-way, asynchronous, schema-defined payload, explicit
  direction, delivery class, rate and size budget.
- `NetworkRequest`: asynchronous request/response with request ID, schema-defined
  input/output, deadline, cancellation, concurrency limit, and structured error.

No synchronous cross-network function illusion. Handlers run through the bounded
script scheduler. Server handlers receive authenticated player/session context
out-of-band; clients cannot provide or replace it. Channel definitions are built
or registered ahead of time, not created from untrusted arbitrary strings.

## Prediction and simulation

The first multiplayer slice should use server-authoritative movement with client
input sequence numbers, local prediction, server snapshots, reconciliation, and
remote interpolation. Begin with a deliberately simple character controller; do
not attempt general client-owned rigid-body replication first.

For physics objects, server simulation is the default. Later simulation leases
require ownership eligibility, bounded velocities/forces, validation windows,
server resimulation/correction, revocation, and abuse telemetry. Contact-based
gameplay events are emitted by the authority.

## Bandwidth and abuse budgets

Enforce limits before allocation/deserialization and again after schema decode:

- handshake bytes/time and unauthenticated connection count per source;
- packet, message, collection, string, and nesting sizes;
- events/requests per channel, player, server, and time window;
- concurrent requests and response bytes;
- replicated objects/properties and interest churn per tick;
- asset download size/rate and decompression ratio;
- script time/memory caused by one peer; and
- logs/metrics cardinality to prevent operational denial of service.

Use token buckets with small bursts, backpressure where meaningful, priority
queues, and explicit disconnect policy. Record structured, privacy-conscious
reason codes. Never let a rejected message interpolate into a log format string
or emit raw terminal control characters.

## Execution domains and capabilities

| Domain | Allowed examples | Forbidden examples |
|---|---|---|
| Server experience | Authoritative World, Players, approved storage/HTTP connectors, NetworkEvent send | Host process control, arbitrary filesystem, editor commands |
| Client experience | Replicated World view, local player/input/camera/UI/audio, client-to-server channels | Server-only storage/secrets, arbitrary replicated writes, other-player identity fabrication |
| Editor tool | Selected document/asset APIs granted by project/user | Silent host-wide filesystem/network/process access |
| Plugin | Manifest-declared brokered APIs, ideally isolated process/VM | Ambient credentials, unrestricted native loading, direct engine pointers |
| Build tool | Explicit project and output roots | Paths outside granted roots, runtime secrets |

Capabilities are unforgeable native handles bound to a domain. Permission checks
must occur inside native operations, not only in Luau wrappers. Sensitive server
connectors should use deployment secret references resolved outside project data.

## Project, asset, and plugin trust

- Opening a new project starts in restricted mode: parse manifests and display
  metadata, but do not run scripts, imports, plugins, build hooks, or network calls.
- Trust is recorded against canonical path plus project identity; changed
  capability declarations require renewed consent.
- `SourceMount` and build outputs are confined to canonical allowed roots with
  explicit symlink/junction policy.
- Assets are imported by type-specific sandboxed workers, bounded before and
  after decompression, hashed, validated, and promoted into a content-addressed
  cache only on success.
- Plugins declare version, publisher/signature, capabilities, network origins,
  and project scopes. Prefer out-of-process execution and a revocable broker.

## Operational and supply-chain baseline

- immutable dependency/action pins, reviewed lock updates, SBOM, signed builds,
  provenance attestations, and published checksums;
- server secrets from a deployment secret store with rotation and audit logs;
- structured security telemetry, rate-limit metrics, crash dumps with secret
  redaction, protocol-version dashboards, and emergency feature/channel kills;
- security policy and supported-version window; reproducible disclosure intake;
- routine fuzzing of wire/parser/import surfaces and sanitizer builds; and
- replayable protocol test vectors plus malicious-client integration tests.

## Network implementation sequence

1. Freeze stable IDs, schema flags, change journal, and server/client domains.
2. Build loopback transport and version handshake with strict limits.
3. Replicate a read-only primitive scene from server to client.
4. Add player session and input-command channel.
5. Add one predicted/reconciled character controller.
6. Add typed events/requests and abuse tests.
7. Add interest grid, baselines/deltas, recovery, and bandwidth profiling.
8. Add remote encrypted transport, discovery/auth adapter, operations tooling, and
   multi-process soak tests.

The multiplayer exit test is two real client processes joining one server,
moving independently under latency/loss simulation, seeing replicated parts and
UI, using one validated gameplay event/request, reconnecting or failing cleanly,
and rejecting a malicious-client corpus without crashes, unbounded work, or
unauthorized state changes.
