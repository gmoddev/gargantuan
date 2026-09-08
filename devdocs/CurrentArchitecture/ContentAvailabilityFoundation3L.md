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
objects, at most 1 MiB of version-4 JSON, and no bootstrap-sensitive Script,
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
| Stored/decoded unit payload | 1 MiB |
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
`Admitting`, `Resident`, `Evicting`, and `Failed`. Available means verified
immutable bytes and a worker-validated parsed document are retained without a
live Instance hierarchy. Resident means the detached hierarchy successfully
crossed the authoritative commit boundary. Peer structural convergence is later
3J state and is never represented here.

Direct demand is a saturating aggregate counter. The first direct request adds
one transitive dependency demand for every hard dependency; repeated requests
increment only the aggregate and count as acquisition deduplications. A unit has
at most one acquiring state, so 500 peers expressing the same server demand
still cause one provider fetch. Release removes dependency demand only when the
last direct demand leaves. Native `PinContent` exists for trusted runtime owners;
no Luau, protocol, or client operation exposes it.

Provider calls run on a session-owned `JobSystem`. Defaults are two workers,
four in-flight calls, 1,024 pending keys, a ten-second per-call deadline, and
four completions consumed per tick. Hard configuration ceilings are eight
workers, 16 in-flight calls, 1,024 pending keys, and 16 completions consumed per
tick. The service reserves each declared payload against the 16 MiB completed
payload ceiling before starting provider work. Cached immutable payload bytes
are independently capped at 32 MiB and evicted under pressure. Queue and byte
overload do not grow without bound: work remains unavailable/requested or enters
an explicit bounded failure. No provider IO or gRPC wait occurs on the
simulation thread.

Stop first marks the session cancellation token and advances its generation,
then joins/cancels jobs, stops the provider, and clears completion storage. Every
completion carries the captured generation. A late completion cannot enter a
replacement session.

## Transactional authoritative admission

Provider IO, response identity and length checks, SHA-256 verification, JSON
parse/tree validation, and manifest indexing run on a worker. Main consumes a
bounded completion, verifies current session demand/generation and dependency
residency, materializes the already parsed version-4 document into a detached
Instance hierarchy, preflights its decoded object count, and commits it.
Detached preparation allocates no authoritative world identity and cannot run
simulation, physics, 3K, 3H, 3E, 3J, Scripts, or Remotes.

The single `SetParent(Workspace)` is the feasible authoritative commit boundary.
Existing Instance/DataModel preflight validates the complete prepared subtree
before descendant lifecycle notifications become observable. Commit failure
leaves the prior world unchanged and the unit Failed; no prefix is left
resident. A successful commit allocates fresh scoped ObjectIds and normal
Instance lifecycle hooks derive all downstream subsystem state.

Default admission limits are two units, 512 declared objects, and 1 MiB per
tick. Hard ceilings are the same. A unit larger than the object/byte hard ceiling cannot enter the manifest;
an offered budget smaller than the atomic unit ceiling is rejected at service
construction. The current transaction materializes one already bounded unit on
Main in one tick; cross-tick preparation of a single hierarchy is explicitly
deferred rather than claiming unsupported partial atomicity. Measurement showed
that retaining the old 8 MiB ceiling would be unsafe, while worker parse plus
atomic Main materialization at 1 MiB remains within the validated frame budget.

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

The Node adapter, promoted from the original private 3L integration fixture to
the internal production Server-host target by Runtime Host Foundation 1.1,
sends only request correlation, project ID, package version, and logical content
key. The TLS endpoint, root certificate, workload token environment variable,
response bounds, and deadlines come only from trusted host configuration.
Responses must echo the exact request identity. Engine still hashes every
manifest and payload before admission.

Node derives tenant exclusively from the authenticated workload principal.
`GetManifest` requires `content.manifest.read`; `GetContent` requires
`content.blob.read`; both permit only tenant-bearing node or game-server
principals. No request contains tenant, provider, endpoint, path, ObjectId,
SpatialSpaceId, or peer authority. The development backend indexes a configured
absolute package directory at startup, validates its complete manifest/DAG and
every blob, and services exact namespace/key lookups. Runtime requests never
become paths.

The RPCs are bounded unary messages. There is no compression and no chunking in
v1; the maximum configured blob is 1 MiB and gRPC transport limits must be at
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
from runtime-created children. Its verified raw bytes may remain in the bounded
immutable cache for reload, but the parsed document is dropped after commit;
the cache never owns mutable Instances or runtime ObjectIds. All caches are
session-owned.

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

## Foundation 3L.1 production validation

Foundation 3L originally shipped the provider-neutral architecture and bounded
unit format with partial production validation. In particular, the old 8 MiB
unit ceiling was a memory bound, not evidence that atomic Main-thread decode and
commit fit a frame. The 3L.1 work measured that path first and did not introduce
a partially visible or generic incremental scheduler.

### Admission decision and measured cost

The Windows measurements below were captured on the trusted MSVC Release worker
on 2026-09-08. Times are milliseconds. Worker preparation is JSON parsing and
tree validation; Main preparation is detached Instance materialization; commit
is the single authoritative `SetParent(Workspace)` transaction.

| Profile | Objects | Bytes | Worker mean / p99 / max | Main mean / p99 / max | Commit mean / p99 / max |
| --- | ---: | ---: | ---: | ---: | ---: |
| lightweight | 10 | 2,060 | 0.044 / 0.046 / 0.067 | 0.042 / 0.050 / 0.057 | 0.039 / 0.046 / 0.064 |
| lightweight | 100 | 20,510 | 0.427 / 0.466 / 0.492 | 0.473 / 0.508 / 0.516 | 0.310 / 0.422 / 0.443 |
| lightweight | 256 | 52,490 | 1.056 / 1.092 / 1.110 | 1.191 / 1.243 / 1.305 | 0.873 / 1.258 / 1.316 |
| representative Parts | 512 | 262,902 | 9.993 / 10.326 / 10.663 | 7.602 / 7.955 / 8.354 | 7.044 / 7.810 / 8.289 |
| final legal maximum | 512 | 1,048,576 | 14.883 / 14.623 / 15.540 | 8.834 / 8.655 / 9.371 | 5.734 / 5.492 / 7.108 |
| rejected former maximum | 512 | 8,388,608 | 65.023 / 63.465 / 69.036 | 28.507 / 27.748 / 30.073 | 11.468 / 11.169 / 16.160 |

The service-path benchmark repeats each case with a fresh DataModel and includes
completion handling, detached materialization, commit, and the remainder of
`ContentAvailabilityService::Step`. At the final 512-object/1 MiB maximum it
measured worker preparation mean/p50/p95/p99/max of
`14.780/14.807/15.093/15.093/15.142`, total preparation
`23.432/23.440/23.747/23.747/23.958`, commit
`4.142/4.125/4.336/4.336/4.346`, time to Resident
`31.213/31.206/31.614/31.614/31.900`, and Main service step
`13.704/13.696/13.962/13.962/14.280` ms. These figures include the
allocation-instrumented executable's replacement-allocation overhead and are
therefore conservative relative to the earlier native-allocator run.

Two MiB service cases crossed the 60 Hz frame budget in repeated runs and four
MiB cases reached about 24–30 ms Main steps. The former 8 MiB ceiling had a
30.073 ms detached-materialization maximum before its separate commit cost.
Consequently the independently streamable payload limit changed from 8 MiB to
1 MiB while the 512-object limit remained. This is a package-builder partition
change, not a package-format version change: newly built packages partition or
retain an oversized subtree in bootstrap, and an existing version-2 package
whose manifest declares a unit above 1 MiB is rejected and must be rebuilt.

JSON parse/validation was moved off Main because it was both safe to detach and
measurably significant. Main still materializes the complete detached hierarchy
and commits it atomically. The supported maximum's worst measured
3L-attributable Main service step is 14.280 ms on the validation worker. Commit
is not the sole dominant cost; detached semantic construction is larger for the
legal-maximum string case, while representative Part lifecycle work makes
commit comparable.

### Deterministic work and memory bounds

The scheduler remains content-specific and uses these hard ceilings:

| Resource | Default | Hard maximum |
| --- | ---: | ---: |
| Worker jobs | 2 | 8 |
| In-flight requests/completions | 4 | 16 |
| Pending requested keys | 1,024 | 1,024 |
| Completions consumed/tick | 4 | 16 |
| Admission units/tick | 2 | 2 |
| Admission objects/tick | 512 | 512 |
| Admission bytes/tick | 1 MiB | 1 MiB |
| Eviction units/tick | 2 | 2 |
| Eviction objects/tick | 512 | 512 |
| Completed payload bytes | 16 MiB | 16 MiB |
| Immutable cache bytes | 32 MiB | 32 MiB |

Declared completion bytes are reserved before either a provider fetch or cached
reparse starts. A test holds 16 legal 1 MiB completions at exactly 16 MiB, proves
the seventeenth stays Requested with an explicit capacity deferral, then proves
the backlog drains incrementally. Configuration tests cover limit-minus-one,
exact limit, and limit-plus-one. Cache tests retain only verified shared raw
bytes, hit that cache on reload without another provider acquisition, exceed the
32 MiB accounting ceiling with 33 legal units, observe eviction, and never
exceed the byte high-water.

Each parsed JSON document is bounded by the 1 MiB protocol document limit and
protocol depth/node/string limits, while concurrent work is constrained by the
16 in-flight ceiling and encoded-byte completion reservation. The detached graph
is additionally bounded by 512 decoded objects. Foundation 3L.2 adds a separate
16 MiB retained decoded-document ceiling; this is not charged to the 32 MiB
encoded-byte cache budget. Worker completions, Main's drain batch, and Available
records share one RAII charge, released with the final document owner. A blocked
dependency drops its decoded document while preserving only bounded verified
cache bytes. Aggregate pressure defers re-preparation through the existing
worker queue; an individually over-budget document fails before materialization.
The charge conservatively counts STL-owned container/string capacity, not
allocator bookkeeping or transient parser memory. See
[`ContentAvailabilityFoundation3L_2.md`](ContentAvailabilityFoundation3L_2.md)
for the memory attribution and measured validation status.
There is no historical event queue, per-object future, or timer. The benchmark
executable replaces global allocation only for the benchmark process and
records requested allocation count, peak live requested bytes, and bytes still
live at the end of worker preparation, detached materialization, and commit. It
does not claim allocator metadata, fragmentation, or operating-system RSS. At
the 512-object representative Part profile, worker/Main/commit allocation
counts were 128,848 / 57,801 / 44,557 at maximum, with peak requested bytes of
1.31 / 1.83 / 5.44 MiB and end-of-stage live requested bytes of 1.31 / 1.83 /
5.18 MiB. At the legal 512-object/1 MiB profile they were 128,950 / 55,864 /
42,542 allocations, 2.12 / 3.05 / 7.22 MiB peak requested bytes, and 2.06 /
2.99 / 6.21 MiB live requested bytes. The difference between the reported
maximum peak and maximum end-of-stage live values is about 0.068 / 0.063 /
1.00 MiB for the legal maximum (and 0.002 / 0.001 / 0.253 MiB for the
representative Part profile). The remaining memory gap is an
accelerated long-run process-RSS plateau measurement, especially for parsed
documents retained by multiple Available records; finite structural bounds and
raw-byte high-water counters are not represented as an RSS result.

### Providers, latency, and real client proof

The Node integration test now launches four real roles: the Go Node host with
TLS/gRPC, an authoritative Engine server process, a GNS client process, and the
client Engine runtime created from the received snapshot. The server requests a
Node-originated Folder and Part, validates digest and schema, commits them,
derives 3K membership, 3H candidates and 3E Desired, lets 3J publish ordinary
GRPL structure, and observes the exact hierarchy, Origin attribute, Anchored,
CFrame, and Size on the separate client. It then evicts the unit, observes
client removal, reloads it with fresh server and client ObjectIds, continues
Character movement plus RemoteEvent/RemoteFunction traffic, and disconnects
without retaining Player or Character authority.

For the first streamed unit, the measured Node timing was:

| Interval | Microseconds |
| --- | ---: |
| T0 demand to T2 payload complete | 3,075 |
| T2 to T3 SHA-256 verification | 2 |
| T3 to T4 preparation complete | 1,560 |
| T4 to T5 authoritative commit | 69 |
| T5 to T7 server structural commit | 1 |
| T7 to T8 client observation | 11,101 |
| T0 to T8 | 15,970 |

Its mixed streaming loop measured Main tick mean/p50/p95/p99/max of
`79/45/103/103/296` microseconds while accepting 34 Character states and 2,516
Character bytes. The stream lifecycle selected and committed 57 structural
transitions. The retained lifecycle fixture validates five paced RemoteFunction
round trips and three RemoteEvent acknowledgements during load/evict/reload.
The five-call latency sample measured mean/p50/p95/p99/max of
`12,913/13,251/13,939/13,939/15,693` microseconds with no timeout, reconnect, or
protocol error. A 20-call paced run and a 100-call stress run instead
reproducibly terminated the separate Windows client with access violation
`0xc0000005`. Those runs do not implicate content admission or Node gRPC, but
they prevent a production-health claim for sustained RemoteFunction traffic and
remain a release blocker pending a dedicated Remote investigation.

Canonical final-world comparison passes for Local FullyResident, Local
OnDemand, Node FullyResident, and Node OnDemand. Sample wall/Main-tick p99 values
in microseconds were Local FullyResident `77,150/183`, Local OnDemand
`46,095/7`, Node FullyResident `105,958/204`, and Node OnDemand `73,916/10`.
Deterministic Node delays of 0, 5, 25, 100, and 500 ms changed wall time but kept
Main-tick p99 at 8, 11, 11, 12, and 10 microseconds respectively. Throttled
64 KiB, 256 KiB, 512 KiB, and 1 MiB cases converged; the 1 MiB case accounted
exactly 1,048,576 completed and cached bytes. Node outage while A was Resident
made B fail without revoking A; restarting Node and invoking trusted
`RetryContent(B)` admitted B in the same session.

### Lifecycle, overload, and authority results

The checked-in content test dynamically covers 500 demands producing one
provider acquisition and one authoritative admission; cache load/evict/reload;
ephemeral runtime mutation reset on reload; runtime-created descendant eviction
pinning; fresh ObjectIds plus stale ObjectRegistry rejection; dependency fan-in,
ordering, failure/retry and two-/three-node cycle rejection; 100 cancellation
cycles; cancellation during legal-maximum worker preparation; ten
Stop-during-request cycles; 1,026 requests against the exact 1,024-key queue;
exact completion/cache pressure; and a deterministic 10,000-operation
demand/release reference model. Malformed coverage includes
duplicate/unsorted/oversized keys, zero sizes, truncation, invalid UTF-8,
invalid shape/schema, wrong object count, and validly digested corrupt content.
Detached Script and Remote instances stay outside every DataModel and cannot
execute or become usable.

The full Windows 46-test CTest contract covers the existing 3K/3H/3E/3J,
Character, Remote, package, headless, and stale pending-Enter regressions. The
65,536-entry manifest parsed in 850.574 ms and its fixed-grid index built in
19.881 ms; 65,537/100,000/1,000,000 compact attacker cases rejected before DOM
allocation in 0.917/1.169/9.398 ms. An escaped-key 65,537-entry form rejected in
0.858 ms. The pre-scan recognizes semantically
equivalent escaped top-level `Entries` keys and checks duplicate occurrences.
A manifest can therefore describe many
unavailable units without inserting any live 3H projections.

Authority remains unchanged. No client or Luau API can request a package key,
retry, choose a provider/endpoint, or pin content. Node cannot select ObjectIds,
Character authority, LocalPlayer, 3E Desired, or 3J Known. Every payload is
identity/size/digest checked before parsing, detached content has no gameplay
context, cache entries contain no live state, local filesystem requests resolve
only manifest-indexed safe paths beneath the configured canonical root, and
provider failure cannot revoke a Resident unit. Ordinary Script, Remote,
Character, relevance, and materialization security contexts are not bypassed.

### Validation status and remaining deferrals

On 2026-09-08, the MSVC Release build and all 46 Windows CTests passed. A
focused working-tree security review found no lower-privileged reportable issue;
it retained the multi-record decoded-memory/RSS plateau measurement as explicit
follow-up. A focused Linux Clang 19 ASan/UBSan/LSan build passed the content
test, benchmark smoke test, and the complete benchmark including the legal and
former payload maxima with leak detection enabled. Hosted full-suite sanitizer
CI is still the terminal broad Linux gate.

The real GNS TLS Node-to-server-to-separate-client lifecycle passed with the
five-call RemoteFunction sample above. Node `go test ./...`, `go vet ./...`, and
`go test -race ./...` passed. Publication CI and documentation deployment, a
content-coupled 32/100/500-peer matrix, long-run RSS plateau measurement, and a
stable sustained RemoteFunction distribution must be recorded as terminal
green before Foundation 3L can be promoted from partial validation under the
3L.1 acceptance contract.

## Compatibility and deferrals

Package format version 2 adds `Startup.ContentManifest` and hashed region files.
The strict inspector continues to accept version-1 legacy packages as a complete
bootstrap project with no content manifest. Offline `GargantuanPlayer` and the
default `GargantuanServer` composition use local fully-resident content through
the same Engine service; a trusted Server host can instead inject the private
Node provider and residency mode. Player has no corresponding server-provider
configuration path. Studio, MCP, and Telemetry require no compatibility
change because package production remains in the Engine CLI/EditorHost contract,
no authoring UI or public content protocol was added, and existing diagnostics
can consume aggregate counters later.

Explicitly deferred are creator residency controls, direct spatial-focus policy,
client fetching, per-peer downloads, terrain partitioning, compression/chunking,
CDN/distributed storage, runtime persistence/write-back, hot package patching,
cross-server travel, server migration, topology/portals, and shared scheduler
infrastructure.

The recommended next foundation, once every 3L.1 production gate is terminal
green, is **Foundation 3M — Trusted Spatial Residency Policy**. The native
acquisition/admission/lifetime seam now exists; measured product work should
decide when trusted server policy requests units without exposing raw package
keys or runtime cells to clients.
