---
status: development candidate
owner: Terrain Foundation 1
---

# Terrain Foundation 1 implementation

This document describes the development branch based on remote Engine main
`ff5cf28f8`. The earlier research and its source are retained separately at
`040ee80ba`. The observation boundary is recorded in the
[qualification receipt](../Validation/TerrainFoundation1.md).
The [terrain campaign ledger](../Validation/TerrainCampaign.md) records
the subsequent multiplayer, Studio, storage and rendering profiles.

## Authority, storage and internal indexing

The canonical Workspace controls the lifetime of one protected Terrain shell.
Its native Field uses immutable copy-on-write pages backed by implicit air,
constant tuples and dense mixed 8³ bricks with sorted material palettes. Each
sample has one owner determined by mathematical floor division in 32³-sample
chunks. A chunk owns the 32³ cells with those minima. Meshing reads the low
gradient halo and shared high corners through pinned neighboring pages.
Exterior mesh halo positions use saturated air. The integer storage boundary
exceeds the tested Box3D and renderer working extent.

Negative density means solid and requires a nonzero stable uint32 material ID.
Zero lies on the air side. Brush quantization clamps to a four-spacing band and
keeps sign across rounding. Default spacing is one world unit. The native grid
also represents exact binary spacings 0.5 and 2 with immutable dataset meaning.
EditorHost accepts those spacings when creating a project. Min/max
constructive edits and integer stencil smoothing keep an extractable signed
field. A metric distance guarantee would require independent qualification.

Only materialized data and empty-chunk version tombstones consume directory
records. A separate index tracks the tombstones so idle reclamation never scans
stored pages. A capture lease retains their dependency versions until all
captured work is released. Idle reclamation has a fixed work cap. Material and
geometry versions advance independently. Explicit requested global revisions
are checked at submission and commit. Disjoint requests without that precondition
can commit using their exact page dependency set.

The private active registry keeps chunk keys, activation generations and
geometry/material versions in contiguous arrays. It owns no field pages or
provider resources. One bounded pending cohort owns immutable input, target
tickets and a cancellation state. Preparation runs on the existing JobSystem
with two lazily started workers. Main checks field epochs, dependency versions
and active tickets before staging. Overlapping pending cohorts retain their
order. Disjoint ones can become ready independently. The coordinator computes
affected cells and dependencies from admitted bounds rather than discovering
them by scanning stored chunks.
[Ownership decision](../../docs/adr/TerrainFoundation1.md) ·
[Active metadata evaluation](../Validation/TerrainMetadata.md)

## Production code paths and readiness

Offline Engine initialization attaches one coordinator to the canonical live
Workspace. The public API can read or edit bounded lattice regions and submit
primitive fill, dig, paint and smooth requests. It exposes opaque tickets scoped
to the field lifetime. Worker completions publish before simulation. Non-offline
worlds have a protected shell with readiness appropriate to their local field
availability. [Luau, runtime and query contracts](TerrainObject.md)

The native MC33 candidate emits chunk-local indexed surfaces with shareable
edge choices, stable normals and material source references. Paint avoids
geometry extraction. It refreshes render material grouping while retaining
provider geometry. The initial SDL material descriptors use simple PBR color
swatches by stable material ID, with a fallback descriptor for other IDs.
Project-defined PBR assets, triplanar texture sampling, blend weights and
physical materials remain deployment work.
[Native surface qualification](../Validation/TerrainSurface.md)

Box3D receives bounded conditioned meshes through the neutral PhysicsWorld
interface. At the Main safe point both old and candidate bodies can temporarily
exist during validation. No query or simulation interleaves. An ordinary
failure discards staged bodies and publishes no field revision. On success,
superseded provider bodies retire and the immutable render state becomes
available. If final native retirement violates the provider contract, field
revision may already have advanced. That edit reports unavailable, no new
visual state installs, and world physics stepping pauses until teardown.
Earlier successfully installed receipts retain their dispositions.
[Provider tests](../Validation/TerrainPhysics.md)

Each runtime render publication transports a separate immutable terrain draw
binding list and ordinary mesh creates/removes under a distinct mesh domain.
Chunk draws share the semantic Terrain owner. Draw records never become
ordinary projected Instance objects. The SDL opaque and shadow passes consume
the bindings and GPU resources follow the existing mesh cache. Changed
terrain invalidates shadow reuse conservatively. Frame publication and driver
success have separate failure paths. A committed edit receipt certifies field,
native collision and prepared CPU render state. Its readiness field does not
acknowledge a GPU draw.

Terrain uses one opaque range per chunk with four-byte vertex tints derived
from canonical material IDs. The opaque pipeline interpolates the swatches
across vertices and keeps matching sample tints on chunk edges. Paint
updates the immutable render resource without replacing the native collider.
The fallback tint is a visualization choice. Stored material IDs remain exact.

## Persistence and startup

Scene JSON version 6 stores a canonical `TerrainManifest` SHA-256 reference
on the DataModel. GASP version 2 carries the same reference under the
origin-bound place profile. Untouched projects still use scene version 5
and GASP or GASM version 1. Terrain payloads are ordinary project content
stored as typed, immutable GTMF and GTCH files beneath
`.gargantuan/terrain/`. The manifest binds the project identity, terrain
dataset, grid spacing and ordered signed chunk coordinates. The loader
validates the byte limits, content hashes, key bindings and canonical
encodings before installing a field with a new runtime lifetime. Runtime
revisions and tickets are local to that lifetime.

Main captures committed pages and scene identities together. Worker
preparation can continue against immutable inputs while the capture
observes the previously committed state. Ordinary Save reuses the last
successful closure index for the same field lifetime and destination. It
re-encodes changed chunks and checks immutable existing files against the
manifest. Save As captures the complete closure and preserves the existing
project lineage. The scene file is replaced after all referenced terrain
artifacts are present. Package capture writes its own pinned, verified
closure. Packaged offline startup reconstructs collision and drawable
surfaces before running gameplay scripts. The
[Terrain Foundation 2 bootstrap](../../docs/adr/TerrainNetworkBootstrap.md)
handles terrain during matching loopback Development sessions.

A packaged player connecting to a loopback endpoint with a Development
package uses a scoped client startup mode. It verifies the pinned GTMF against
the scene reference and validates each declared GTCH pin and size in the
package table. The temporary packaged code world gets an empty terrain shell.
The session later verifies the received leaf bytes and installs the field in
the replica world before collision admission. Other startup content, including
code and assets, retains its usual file and content-hash verification. The
static distribution can still contain dormant GTCH leaves. The player defers
opening and hashing those local files in this mode. Offline and server startup
and the standalone package validator verify all terrain artifacts.

The current store retains immutable artifacts from earlier saves, failed
publications and exported GASP documents. An older GASP can reference a
manifest absent from the current scene. Safe collection would require a
verified inventory of retained references or deletion of an entire
explicitly retired project store. See the
[native persistence qualification](../Validation/TerrainPersistence.md).

## Bounds and authoring handoff

Default Field admission caps directory entries at 65,536 and logical owned
bytes at 64 MiB. An edit covers at most eight affected mesh chunks with no
more than 128 captured dependency chunks. There are four outstanding cohorts
and 512 active metadata records per world. Prepared field and geometry payloads
are independently capped at 16 MiB per cohort. Renderer state limits and
native provider limits apply separately. Old and candidate resources can
coexist during stage. The contour path uses bounded equal-resolution coverage.
The offline startup profile admits at most 512 materialized source chunks and
1,024 lower-neighbor contour candidates. Live edit admission checks prospective
occupancy before committing so a successful edit stays within the same
reopen profile. Positive-density samples can occupy a source chunk even when
its region currently has no surface.
The supported authored spacings are 0.5, 1 and 2 world units. Spacing governs
field samples. Visual contour fidelity and native collision conditioning
have separate admission limits. The offline startup loads bounded complete
residency. The [qualification receipt](../Validation/TerrainFoundation1.md)
records separately measured runtime and provider limits.

Studio discovers the canonical shell and uses the scoped EditorHost
terrain commands. A brush binds the project scope, expected revisions
and a bounded region or world-space brush. It returns a ticket with a
terminal receipt after paired provider admission. Terrain history
replays the exact samples for the most recent changed stamp.
Terrain targeting uses the dedicated native raycast and the viewport
draws the committed terrain. [EditorHost terrain protocol](../../docs/src/content/docs/developing/editor-host.mdx)

EditorHost advertises `TerrainGasPlaceV2` in `Handshake`. The Studio
terrain branch accepts v2 place documents through its existing bounded
transfer. A terrain export reports `DocumentKind: Place` and
`FormatVersion: 2`. The selected project origin supplies its verified
terrain closure. Project Save As and offline package capture carry
their own closure. The campaign ledger records the Studio, streaming
and visual detail qualification scopes.
[Codec qualification](../Validation/TerrainChunkCodec.md) ·
[Campaign evidence](../Validation/TerrainCampaign.md)
