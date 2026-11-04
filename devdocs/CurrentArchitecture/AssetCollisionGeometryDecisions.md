---
status: accepted
date: 2026-11-04
owner: runtime-assets
related_code:
  - include/gargantuan/assets/AssetTypes.hpp
  - include/gargantuan/physics/PhysicsTypes.hpp
  - include/gargantuan/geometry/SolidGeometryCompiler.hpp
  - src/services/AssetService.cpp
related_adrs: []
---

# Asset 2B and Geometry 1A decisions

These decisions govern the isolated `feature/asset2b-geometry1a` implementation.
The [collision contract](AssetCollisionFoundation2B.md) and
[compiler contract](SolidGeometryFoundation1A.md) describe the concrete APIs and
formats. Qualification and source identities belong in the campaign receipt;
this accepted decision does not assert that unexecuted tests have passed.

## Authority and representations

There is one AssetService catalog and compound transaction. Authored `.solid`
source is a versioned bounded graph, render geometry is an ordinary Mesh,
collision geometry is a separate Collision asset, and Box3D shapes are disposable
world-owned projections. None becomes a second project database, Instance graph,
history, spatial index, networking authority, or residency-demand selector.

`AssetKind::Collision` is appended as numeric value 6, preserving existing kinds.
Mesh artifact v4 explicitly references its Collision dependency; older supported
artifacts retain their existing interpretation. Stable source-group logical keys
preserve logical asset IDs across compatible imports. Content hashes identify
canonical bytes, while content revisions identify publication changes. Source
paths remain project-relative provenance, never runtime file-loading authority.

## Collision policy and lifetime

The first representation is finite, bounded indexed static triangles. It permits
open surfaces for collision; a solid Boolean operand separately requires a
closed oriented manifold. Render normals, UVs, materials and triangle buffers
are not implicitly trusted collision data.

`MeshPart.CollisionMode` is `Box` by default, preserving existing behavior.
`StaticMesh` requests the Mesh asset's canonical Collision dependency. An anchored
part uses that mesh with the same transform and per-axis Size as rendering.
Unanchored concave collision uses the documented Box alternative, with bounded
diagnostics. Sizes below Box3D's 0.01 mesh-scale floor also use an explicit Box
alternative. Other invalid or unavailable mesh replacements fail and retain the
previous complete body state; they cannot silently substitute geometry.

Main applies committed changes at the existing physics safe point. Body and
constraint IDs remain generation-safe and unchanged by successful shape
replacement. Native mesh data is shared by immutable asset identity/revision
within one physics world and retained until the last consuming shape is gone.
Native handles never enter assets, Instances, snapshots, Luau or renderer state.
This slice covers rigid contacts and queries. The sibling XPBD deformable
collider projection retains its existing primitive approximations.

Asset admission counts canonical collision bytes and retires unreferenced old
source-group artifacts after successful publication/deletion. Captured snapshots
retain their own immutable bytes. Physics separately accounts active resources,
consumers and native mesh-data bytes; unknown per-shape storage is labeled unknown.
Aggregate native admission is bounded without adding demand selection or eviction.

## Compilation and publication

`CompileSolidGeometry(Graph, Cancellation, Deadline, Limits)` returns owned
`SolidCompilation { Mesh, Collision }` candidates or a bounded diagnostic.
It validates the complete graph before evaluation. Union/intersection evaluate
listed operands deterministically; subtraction is ordered. Nested transforms and
imported operands are admitted only within the documented geometric bounds.

The `.solid` importer submits both candidates through the existing source-group
transaction. The complete pair is encoded, validated and admitted before either
revision is published. Failure preserves the previous valid pair. Published
content remains resolvable while reimport runs; project/runtime capture serializes
that prior complete revision as Stale rather than an unfinished Importing state.
Initial imports with no published content cannot enter a runtime package.

The kernel is the serial, minimal vendored Manifold v3.5.4 core at an immutable
commit with archive and per-file hashes, retained Apache-2.0 license, and no
optional downloads or cross-section dependencies. The implementation validates
canonical output and normalizes/sorts triangles. Same-profile repeat hashes and
measured toolchain corpus results are distinct from a universal determinism claim.

The initial surface policy regenerates flat normals and planar UVs. Imported
material groups and source-face attribution are not retained by Boolean cuts.
Empty solid results are explicit failures that preserve the previous bake.
Cooperative cancellation reaches kernel phase boundaries; topology/work limits
are admission guards, not a hard allocator quota or preemptive deadline. Adversarial
peak-memory and cancellation measurements are required qualification evidence.

## Scope and staged integration

1. Canonical collision value/codec and atomic mesh companion publication.
2. MeshPart projection, native sharing, scale validation and safe retirement.
3. Persistence, packaged runtime, reimport and resource accounting.
4. Pinned offline kernel and bounded graph/candidate compiler.
5. Boolean corpus, deterministic artifacts and transactional compiler import.
6. Windows/Linux, malformed-input, sanitizer and performance qualification.
7. Independent review, source-bound receipt, isolated feature PR and continuation.

Foundation 3M owns trusted demand selection, hysteresis and lookahead. Foundation
3L content admission/lifecycle and networking behavior remain unchanged. Shared
CMake additions are recorded independently; neither campaign incorporates the
other's unmerged branch. Production Studio, frozen Avalonia, a modeling interface,
runtime game-facing CSG, convex decomposition and dynamic concave simulation are
outside this slice.

Canonical mesh positions are transformed by `CFrame * Size`; Size is the existing
2A scale, not automatic normalization to a unit bounding box. Existing 3L package
coarse bounds derived only from Size/2 need not cover arbitrary mesh positions.
Consumers must supply conservative spatial metadata under their owning contract;
this campaign does not silently alter package bounds, 3K regions or 3M selection.
The concurrent 3M campaign records that pre-existing bounds limitation separately.
