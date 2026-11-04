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
Captured snapshots retain
their own immutable shared pointer safely. Failed replacement leaves the old
artifact and decoded content available. `ReadCollisionStats` reports current
resident record count, decoded canonical bytes, currently retained Collision
artifact bytes, cumulative retired artifact bytes/count, and failed CPU
admissions. `TotalRetainedArtifactBytes` additionally reports the complete artifact
map across all kinds. These are service-owned resource counts. The physics projection owns
the exact active native shape/consumer counts and native byte estimates.

## Qualification state

Collision resolution continues to return the complete previous published revision
while a reimport is `Importing`. Both mesh and collision records are resolved under
one service lock. First imports without published content remain unavailable;
failed reimports retain their prior coherent collision. Geometry replacement still
occurs only at the normal physics safe point.

The headless asset tests cover OBJ and glTF companion publication, stable
identity, dependency propagation, repeated replacement with bounded retained
Collision artifacts, cancellation, stale last-known-good behavior, save/reopen,
Play clone, packaged runtime resolution, and tampered artifact denial. Build and
test qualification is pending integration with the geometry importer and physics
projection in the Asset 2B / Geometry 1A campaign.
