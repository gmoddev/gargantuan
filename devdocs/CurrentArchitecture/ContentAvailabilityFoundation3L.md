# Content Availability Foundation 3L

## Decision

Foundation 3L separates immutable authored package content from its current
authoritative runtime residency. Package version 2 may move bounded, spatially
described direct Workspace subtrees into independently verifiable content units.
The Engine owns one provider-neutral content service per world/session. A local
package provider and the private Gargantuan Node adapter supply bytes to the same
Engine validation and admission path.

This is the native foundation for local or remote residency. It is not the full
creator-facing streaming product and exposes no new Luau API.

```text
PackageContentNamespace + logical key + digest
                    |
       Local package or private Node provider
                    |
       immutable verified bounded payload
                    |
       detached Engine preparation and commit
                    |
 authoritative Instances and fresh ObjectIds
                    |
              3K -> 3H -> 3E -> 3J
```

Before 3L, a standalone package had one version-4 project snapshot and the
runtime deserialized the complete authored world before constructing `Engine`.
That behavior made immutable package presence, live world lifetime, and peer
materialization effectively coincide. It also left no provider-neutral seam for
server-side remote acquisition.

## Identity and manifest

The namespace is `{ProjectId, PackageVersion}`. Within it, a deterministic
logical key such as `workspace/00000000` identifies a content unit. The key is
stable for identical authored input and partition configuration. Its SHA-256
digest is separate verification metadata. The key is not an `ObjectId`, a
runtime `SpatialSpaceId`, a `SpatialCellAddress`, a connection identity, or a
materialization epoch.

`content/content.manifest.json` has format `GargantuanPackageContent`, version 1,
and records:

- exact project/package namespace and instance schema version;
- sorted logical key and safe package-relative blob reference;
- SHA-256, stored and decoded byte counts, and decoded object count;
- sorted hard dependency keys;
- bounded logical package-space key and optional coarse AABB;
- `RequiredAtBootstrap` and `ImmutableBaseline` flags.

The current writer uses no compression, so compressed and uncompressed lengths
must be equal. There is therefore no decompression allocation or compression
bomb surface. The format retains both fields so a later codec can introduce an
explicit bounded negotiation rather than reinterpret existing bytes.

The package writer visits direct Workspace children in serialized order. A
subtree becomes a unit only when it has a finite spatial bound, at most 512
objects, at most 8 MiB of version-4 JSON, and no bootstrap-sensitive Script,
ModuleScript, RemoteEvent, RemoteFunction, Character, KinematicCharacter,
Player, Camera, Terrain, or FileLink. Everything else remains in the bootstrap
snapshot. Current generated units are independent and have no dependency edges;
the format, parser, state machine, and tests support explicit hard dependencies.

Determinism comes from stable project serialization order, sequential keys,
canonical JSON encoding, sorted keys/edges, and content SHA-256. Filesystem
enumeration, machine paths, pointers, map iteration, and runtime identities do
not participate.

## Hard limits

| Boundary | Limit |
| --- | ---: |
| Manifest bytes | 32 MiB |
| Manifest JSON nodes | 2,097,152 |
| Units | 65,536 |
| Total dependency edges | 262,144 |
| Dependencies per unit | 64 |
| Logical key | 128 bytes |
| Package-space key | 64 bytes |
| Blob reference | 256 bytes |
| Stored/decoded unit payload | 8 MiB |
| Decoded objects per unit | 512 |
| Coarse cells overlapped by one unit/query | 4,096 |

The manifest reader supports 65,536 units for provider/backing-store use. The
current directory-package outer content table remains capped at 16,384 total
files (runtime, assets, shaders, notices, bootstrap, manifest, and units), so the
package writer reaches that stricter aggregate limit first.

Parsing validates exact object shapes, finite ordered bounds, checked aggregate
counts, sorted uniqueness, present dependency targets, and acyclicity with an
iterative Kahn traversal. Safe blob references are relative, normalized,
separator restricted, and reject root names, empty/dot segments, drive syntax,
and traversal. The local provider canonicalizes its trusted root, rejects every
symlinked component, proves the resolved file remains beneath that root, and
opens only a manifest-indexed reference.

## Ownership, provider, and policy

`ContentAvailabilityService` is owned by one `Engine` and one authoritative
DataModel. It owns request state, worker jobs, completion buffering, payload
cache, manifest/index, admission, eviction, metrics, cancellation, and the
session generation. It is never process-global, Player-owned, replication-owned,
or Node-owned.

`IContentAvailabilityProvider` can start/stop and return one bounded manifest or
immutable payload for an exact package identity. It cannot receive a DataModel,
construct Instances, invoke Luau, select peer relevance, or mutate runtime state.
The two concrete compositions are:

- `LocalPackageContentProvider`: exact files from the already installed package;
- private `NodeContentProvider`: authenticated TLS gRPC to Gargantuan Node
  `gargantuan.node.content.v1.ContentStreaming`.

Provider choice and residency mode are orthogonal. `FullyResident` requests all
manifest units; `OnDemand` admits bootstrap-required units plus explicit trusted
demand. Packaged offline and server entry points default to local
`FullyResident`, preserving the compatibility mode without requiring Node.
Local `OnDemand` proves streaming-like residency without network access. A Node
provider may use either mode when selected by trusted host composition.

## Acquisition state machine

The compact states are `Unavailable`, `Requested`, `Acquiring`, `Available`,
`Admitting`, `Resident`, `Evicting`, and `Failed`. Available means only that
verified immutable bytes are retained. Resident means the detached hierarchy
successfully crossed the authoritative commit boundary. Peer structural
convergence is later 3J state and is never represented here.

Direct demand is a saturating aggregate counter. The first direct request adds
one transitive dependency demand for every hard dependency; repeated requests
increment only the aggregate and count as acquisition deduplications. A unit has
at most one acquiring state, so 500 peers expressing the same server demand
still cause one provider fetch. Release removes dependency demand only when the
last direct demand leaves. Native `PinContent` exists for trusted runtime owners;
no Luau, protocol, or client operation exposes it.

Provider calls run on a session-owned `JobSystem`. Defaults are two workers,
four in-flight calls, 1,024 pending keys, a ten-second per-call deadline, and
four completions consumed per tick. Completion payload bytes are capped at
16 MiB and cached immutable payload bytes at 32 MiB. Queue and byte overload do
not grow memory: work stays unavailable/requested or becomes a retryable bounded
failure. No provider IO or gRPC wait occurs on the simulation thread.

Stop first marks the session cancellation token and advances its generation,
then joins/cancels jobs, stops the provider, and clears completion storage. Every
completion carries the captured generation. A late completion cannot enter a
replacement session.

## Transactional authoritative admission

On Main, the service verifies the provider namespace, key, advertised digest,
actual SHA-256, exact byte count, configured cache bound, manifest schema, and
hard dependency residency. It then performs bounded version-4
`DeserializeDetached`. Detached preparation allocates no authoritative world
identity and cannot run simulation, physics, 3K, 3H, 3E, 3J, Scripts, or Remotes.

The single `SetParent(Workspace)` is the feasible authoritative commit boundary.
Existing Instance/DataModel preflight validates the complete prepared subtree
before descendant lifecycle notifications become observable. Commit failure
leaves the prior world unchanged and the unit Failed; no prefix is left
resident. A successful commit allocates fresh scoped ObjectIds and normal
Instance lifecycle hooks derive all downstream subsystem state.

Default admission limits are two units, 512 declared objects, and 8 MiB per
tick. A unit larger than the object/byte hard ceiling cannot enter the manifest;
an offered budget smaller than the atomic unit ceiling is rejected at service
construction. The current transaction prepares one already bounded unit in one
tick; cross-tick preparation of a single larger atomic hierarchy is explicitly
deferred rather than claiming unsupported partial atomicity.

Bootstrap completion means the manifest is valid and every
`RequiredAtBootstrap` unit is Resident. Fully resident compatibility additionally
waits for every unit before normal script bootstrap. Ordinary on-demand units do
not delay world bootstrap.

## Coarse lookup and spatial pipeline

`PackageContentCoarseIndex` is a separate fixed-grid acceleration structure over
optional manifest AABBs and package-space strings. It returns bounded content
entry indices for a trusted interest volume. It does not scan the full manifest
per query, create demand, allocate ObjectIds, or insert ghosts into 3H.

Package space `default` is logical authored metadata. After admission, existing
runtime policy projects semantic Instances into the generation-safe runtime
DefaultSpace. The package never stores a runtime `{Slot, Generation}`. The
relationships remain:

- 3K observes committed semantic Instance lifecycle and owns projection facts;
- 3H indexes only live resident projections and returns candidates;
- 3E remains the sole peer relevance authority;
- 3J materializes already-authoritative relevant structure under its own budget.

Content availability, authoritative residency, and peer convergence are three
different completion points. 3L neither changes Character publication cadence
nor shares a scheduler with 3J. Content overload applies content backpressure;
it cannot consume Character or Remote queue authority.

## Eviction and mutation

When aggregate demand and explicit pins reach zero, eviction proceeds on Main
under defaults of two units and 512 objects per tick. A dependency cannot evict
while a dependent unit remains Resident. Normal recursive `Destroy` ends the
authoritative lifetime, which drives the existing 3K/3H/3E/3J, physics, script,
and Remote removal paths. A later load creates a fresh hierarchy and fresh
ObjectId generations.

The package is an immutable baseline. Runtime mutations are ephemeral unless an
independent authoritative persistence owner exists; 3L performs no write-back.
To avoid silently destroying state it does not own, any runtime-created child
inside a package-owned hierarchy pins that unit against automatic eviction until
the child is removed. Resident content survives Node/provider outage; failure to
fetch new content never revokes existing world authority.

## Node trust boundary

The private Node adapter sends only request correlation, project ID, package
version, and logical content key. The TLS endpoint, root certificate, workload
token environment variable, response bounds, and deadlines come only from
trusted host configuration. Responses must echo the exact request identity.
Engine still hashes every manifest and payload before admission.

Node derives tenant exclusively from the authenticated workload principal.
`GetManifest` requires `content.manifest.read`; `GetContent` requires
`content.blob.read`; both permit only tenant-bearing node or game-server
principals. No request contains tenant, provider, endpoint, path, ObjectId,
SpatialSpaceId, or peer authority. The development backend indexes a configured
absolute package directory at startup, validates its complete manifest/DAG and
every blob, and services exact namespace/key lookups. Runtime requests never
become paths.

The RPCs are bounded unary messages. There is no compression and no chunking in
v1; the maximum configured blob is 8 MiB and gRPC transport limits must be at
least the selected response maximum. A later chunked protocol would require
explicit ordinal/count/total-size bounds and whole-payload digest verification.

Node supplies immutable bytes only. It cannot assign ObjectIds or LocalPlayer,
grant Character control, choose 3E relevance, mark a peer Known, invoke a Remote,
or mutate the live DataModel.

## Diagnostics and retained memory

The Engine reports stable diagnostic codes for manifest acquisition/digest/
shape failures, payload acquisition/rejection, and admission decode/commit
failure. Counters cover requests, acquisitions, deduplications, cache hits,
cancellations, failures, admissions, evictions, objects, bytes acquired/verified,
and queue/cache high-water marks. Logs remain host-owned and use subsystem
prefixes; payloads, tokens, paths, and provider error internals are not emitted.

A pending request retains fixed record/counter state and the manifest key; an
in-flight request retains its exact identity/context. A completed or cached unit
retains at most its bounded immutable bytes. A Resident unit retains its normal
live hierarchy plus a pointer set used only to distinguish package-owned objects
from runtime-created children; source bytes are dropped after commit in the
current policy. All caches are session-owned.

Manifest key lookup is expected constant time. Coarse lookup is proportional to
bounded overlapped cells plus returned candidates, not total manifest entries.
The checked-in test covers deterministic round-trip parsing, coarse lookup,
cycle rejection, 500-demand coalescing, dependency-safe admission/eviction,
runtime-child eviction pinning, corrupt-payload rejection, reload identity, and
session cleanup. The benchmark measures parse/index/query through 65,536 entries
and reports 100K/1M manifests as deterministic hard-limit rejections rather than
allocating attacker-selected scale. Packaging and complete-game tests compose
the local provider. The private Node vertical uses a real TLS Node process and the same Engine
admission path, then compares the resulting hierarchy with local acquisition.

## Compatibility and deferrals

Package format version 2 adds `Startup.ContentManifest` and hashed region files.
The strict inspector continues to accept version-1 legacy packages as a complete
bootstrap project with no content manifest. New packaged runtimes use local
fully-resident composition. Studio, MCP, and Telemetry require no compatibility
change because package production remains in the Engine CLI/EditorHost contract,
no authoring UI or public content protocol was added, and existing diagnostics
can consume aggregate counters later.

Explicitly deferred are creator residency controls, direct spatial-focus policy,
client fetching, per-peer downloads, terrain partitioning, compression/chunking,
CDN/distributed storage, runtime persistence/write-back, hot package patching,
cross-server travel, server migration, topology/portals, and shared scheduler
infrastructure.

The recommended next foundation is **A — residency policy and creator streaming
controls**. The native acquisition/admission/lifetime seam now exists; measured
product work should decide when trusted server policy requests units without
exposing raw package keys or runtime cells to clients.
