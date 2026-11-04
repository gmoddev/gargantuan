---
status: current
owner: runtime-assets
last_verified: 2026-11-04
related_code:
  - include/gargantuan/assets/AssetTypes.hpp
  - include/gargantuan/services/AssetService.hpp
  - src/assets/AssetImporter.cpp
  - src/assets/GltfImporter.cpp
  - src/services/AssetService.cpp
  - tests/AssetFoundationTests.cpp
---

# Asset Foundation 2B: canonical mesh collision

## Identity and ownership

`AssetKind::Collision = 6` is appended after Animation, preserving the existing
numeric kinds. A Collision asset contains immutable local-space finite positions,
unsigned 32-bit triangle indices, and exact bounds. It is separate from render
geometry, MeshPart state, and disposable backend shapes. It currently represents
static triangle collision only. The importer rejects missing, oversized,
non-finite, out-of-range, and degenerate triangles before publication. Triangle
cross-product length squared must be at least `1e-10` in canonical units, a
conservative admission threshold for the pinned Box3D mesh builder. A backend
must still validate scaled geometry before native shape creation.

OBJ import produces one Mesh and one Collision child. A static glTF mesh produces
one Collision child beside each Mesh; skinned meshes do not receive a static
Collision child. The Collision child uses a deterministic logical key derived
from its Mesh key. The Mesh stores an explicit Collision `AssetId` dependency in
artifact version 4. This relationship survives packaged runtime catalogs, which
do not preserve authored source groups. Mesh and Collision candidates pass through
the same `AssetService` graph validation, bounded admission, and atomic commit.
Reimport retains stable logical IDs and publishes both completed artifacts or
retains the previous graph as stale content on failure. The compiler's native
source importer uses this same compound path; it does not create another catalog
or history.

An unchanged canonical content hash retains its existing decoded shared buffers
and artifact storage. Reimport cannot replace immutable content under the same
identity/revision with unrelated buffer ownership. This avoids false native-cache
identity conflicts and needless retry after an unchanged source import.

## Canonical artifact and resolver

Collision artifacts use `GARGAS01`, artifact version 4, kind 6, then little-endian
`uint32` position and index counts, six binary32 bounds components, packed
binary32 XYZ positions, and `uint32` indices. Mesh artifact version 4 extends
the version 2 unskinned Mesh layout with a final 128-bit Collision `AssetId`.
Existing Mesh artifacts without Collision retain their version 1, 2, or 3
encoding. Both decoders validate exact byte lengths and reject trailing bytes.
The catalog remains version 2. Hashes cover the complete canonical artifact.

`ResolveCollision(Reference)` and `ResolveCollisionForMesh(MeshReference)` return
an owned `AssetCollisionResource` with stable `AssetId`, immutable shared arrays,
content revision, and canonical decoded bytes. The Mesh resolver follows only
its explicit validated dependency under the service lock. Missing or failed
content returns no resource. The consumer never receives a backend handle or
mutable catalog storage. Save/reopen, Play clone, and runtime package loading use
the same artifact decoder and dependency check.

## Bounded residency and retirement

Collision positions and indices count against the existing 64 MiB decoded CPU
cache. Each artifact and compound graph retains the existing 64 MiB artifact
bound; no new eviction policy exists. A successful replacement or source-group
deletion removes each prior source-group artifact from service ownership once no
current catalog record references its content hash. This includes the paired Mesh
artifact; retaining obsolete render revisions would defeat collision memory bounds.
Equal collision bytes can be shared by distinct catalog records; retirement
therefore counts unreachable content hashes rather than import attempts.
Undrained render-mesh changes retain only the newest creation for each logical
resource. Replacing or deleting an unpublished generation cancels that creation
rather than emitting a removal for a resource the renderer never received.
The last already-delivered generation still receives its normal removal. This
also retires obsolete pending render buffers during repeated headless compilation;
it is derived-queue lifecycle management, not an asset eviction policy.
Captured snapshots retain
their own immutable shared pointer safely. Failed replacement leaves the old
artifact and decoded content available. `ReadCollisionStats` reports current
resident record count, decoded canonical bytes, currently retained Collision
artifact bytes, cumulative retired artifact bytes/count, and failed CPU
admissions. `TotalRetainedArtifactBytes` additionally reports the complete artifact
map across all kinds. Pending mesh publication/removal counts and vertex/index/
skin payload bytes are separately inspectable. These are service-owned resource counts. The physics projection owns
the exact active native shape/consumer counts and native byte estimates.

## Qualification state

Collision resolution continues to return the complete previous published revision
while a reimport is `Importing`. Both mesh and collision records are resolved under
one service lock. First imports without published content remain unavailable;
failed reimports retain their prior coherent collision. Geometry replacement still
occurs only at the normal physics safe point.

The same published-content rule applies to visual and other asset resolvers.
An in-flight project/runtime snapshot serializes a retained complete revision as
`Stale`, never as an unfinished `Importing` candidate, and includes all its
artifacts/dependencies. An initial import without published content still cannot
enter a runtime package. The existing synchronous import caller still waits for
its worker; this change does not introduce asynchronous Studio import or another
publication authority.

The headless asset tests cover OBJ and glTF companion publication, stable
identity, dependency propagation, repeated replacement with bounded retained
Collision artifacts, cancellation, stale last-known-good behavior, save/reopen,
Play clone, packaged runtime resolution, and tampered artifact denial. Integrated
Source13 passes the Windows/Linux focused suites and strict Linux ASan/UBSan/leak
checks, with the exact profile and evidence in the
[campaign report](../Validation/AssetGeometryCampaignValidation.md). This is scoped
qualification: Box3D's 256-triangle local rigid-contact candidate cap can leave
admitted dense meshes with incomplete contacts (KI-2B-001), as documented in the
[physics contract](PhysicsBackend.md#dense-mesh-contact-limit-ki-2b-001).
