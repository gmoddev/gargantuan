---
status: implementation-pending-qualification
date: 2026-11-03
owner: runtime-assets
related_code:
  - include/gargantuan/geometry/SolidGeometryCompiler.hpp
  - src/geometry/SolidGeometryCompiler.cpp
  - src/geometry/SolidAssetImporter.cpp
  - cmake/Manifold.cmake
  - tests/SolidGeometryTests.cpp
  - tests/SolidGeometryBenchmark.cpp
---

# Solid geometry foundation 1A

## Ownership and source format

The headless compiler consumes a `SolidGraph` and returns a mesh and collision
candidate without changing authoritative state. The AssetService compound
import path owns validation, canonical artifact encoding, identity, revision,
and atomic publication. A `.solid` file is UTF-8 JSON with `Version: 1`, a
stable `Root` node ID, and a bounded `Nodes` array. Node IDs are unique and
independent of display order. Supported `Kind` values are `Box` (`Size`),
`Sphere` (`Radius`), `ImportedMesh` (`Positions` and `Indices`), `Transform`
(`Inputs` and column-major affine `Matrix`), `Union`, `Intersection`, and
`Subtraction`. Every Boolean node has 2–16 `Inputs`. Subtraction applies each
input after the first in listed order. Union and intersection also evaluate
in listed order, making evaluation order explicit. An imported operand is an
inline closed, oriented triangle mesh in canonical Gargantuan coordinates;
the file importer does not resolve external paths or asset references.
Before kernel ingestion, imported geometry vertices with bit-identical finite
positions are welded exactly, with negative zero normalized to positive zero.
This permits a compiled flat-shaded Mesh to be used as a later Boolean input
without treating its per-face render vertices as open geometry. Distinct
positions are never fused by tolerance, and malformed topology is still
rejected by the kernel.

The `.solid` importer emits logical child `asset` (Mesh, primary) and
`collision` (Collision) through one compound AssetService commit. The mesh
depends on the collision child. Both child identities are stable across
compatible recompiles; changed canonical bytes advance their revisions
through the existing asset transaction. Failed source parse, compile, encode,
or admission leaves a previous complete bake available under the existing
last-known-good reimport contract. Runtime receives only ordinary Mesh and
Collision artifacts. No runtime or Studio Boolean evaluation is introduced.

## Kernel decision and provenance

The [Manifold v3.5.4 release](https://github.com/elalish/manifold/releases/tag/v3.5.4)
is pinned to tag commit `ce50d78021d64507f89e8c9fc2c2e51018117857`.
The downloaded GitHub tag archive SHA-256 is
`ce2f8b7798409b9f07e092c0ec0524ed0806431e01aea4020b65d0e934ea9438`.
`vendor/manifold/` contains its unchanged core `src/`, `include/`, `LICENSE`,
and `AUTHORS` files; excluded upstream CMake, cross-section sources, bindings,
tests, documentation, and sample assets are not needed for 3D Booleans. The
per-file hashes are recorded in `vendor/manifold/SOURCE_MANIFEST.sha256`.
The repository `.gitattributes` pins vendored source line endings to LF so
those file hashes remain meaningful on Windows checkouts.
The [upstream license](https://github.com/elalish/manifold/blob/v3.5.4/LICENSE)
is Apache-2.0 and is retained in the vendored subset. Gargantuan's private
`cmake/Manifold.cmake` builds a static, serial 3D kernel with no downloads,
TBB, optional bindings, or cross-section dependency. This makes a clean
offline build possible from the repository checkout on supported toolchains.
The private build recipe is Gargantuan-authored, not an upstream build file.

Manifold was selected after a bounded comparison with exact [CGAL Nef Boolean
operations](https://doc.cgal.org/latest/Nef_3/group__PkgNef3Ref.html), whose
package is GPL/commercial and whose full dependency is materially larger.
A voxelized Boolean fallback would introduce resolution-dependent loss of
thin surfaces and cubic working-set growth, so it is not used as the canonical
surface compiler. This is an engineering fit decision for this initial slice,
not a universal accuracy ranking.

## Bounds, failure, and determinism

The hard input caps are 1 MiB of source JSON, 32 nesting levels, 128 graph
nodes, 24 graph edges in a dependency path, 8192 input triangles, 24576 aggregate
imported vertices (including unused vertices), 32 Boolean
steps, one million estimated pairwise triangle comparisons, 32768 triangles
per intermediate and final result, and ±100000 coordinates. Matrices must be
finite, affine, and invertible; primitive sizes must be positive. The compiler
validates the entire graph, including unreachable nodes, before evaluation.
Cancellation is checked before validation and during imported vertex traversal.
Input-reference count and ID lengths are checked before lookup, including for
the public in-memory graph API; its bounds do not rely on the file-size limit.
Cycles, missing and duplicate IDs, malformed arrays, open/nonmanifold imported
meshes, degenerate final triangles, and empty final surfaces return structured
diagnostics. The caller receives no partial candidate. The AssetService layer
rechecks canonical mesh and collision limits at commit.

An observer forwards cancellation and deadline expiry into Manifold's
`ExecutionContext`. Manifold's [documented cancellation checks](https://github.com/elalish/manifold/blob/v3.5.4/include/manifold/common.h)
occur between sub-Booleans and at input-ingest phases. A single Boolean phase
is not preemptible. The kernel has no allocator budget, so these topology and
work caps are admission guards, not a hard peak-memory or wall-clock bound.
An allocation failure becomes a diagnostic; it never publishes partial assets.
Editor work should run this compiler off the main/render thread.

Output triangles are rotated and sorted by finite float position after the
kernel result, then emitted with deterministic flat normals, planar UVs, and
derived tangents. This gives identical canonical mesh bytes for repeated
compilation of the same graph in the same supported build profile. Cross
toolchain byte identity is not yet claimed; it needs a measured Windows/Linux
compiler corpus. Imported material groups, source UVs, and source-face
attribution are not preserved in this slice. The importer emits an untextured
single-primitive mesh using the existing default material policy. An author
who needs retained source materials must use the ordinary imported Mesh path.

`tests/SolidGeometryTests.cpp` checks Boolean behavior, ordered subtraction,
imported-operand denial, whole-graph validation, cancellation, limits, and
repeat hashes. `GARGANTUAN_SOLID_BENCH=1` runs a small repeated Boolean timing
hook; `GARGANTUAN_SOLID_FUZZ=1` runs 128 seeded valid and invalid graph cases.
`SolidGeometryBenchmark` emits one JSON record per scenario with P50/P95/P99
compile time, input/output triangles, evaluated pair counts, repeat hash, and
process current/peak memory. The OS peak is a process-lifetime high-water
mark; run `--scenario NAME` in a fresh process for an isolated peak. The
`--full101` option uses 101 repetitions for useful percentile estimates.
On Windows memory is private commit with the OS peak pagefile-usage counter;
on Linux it is current/peak RSS. The before/after difference is observed
retention, not an allocation count. The dedicated executable intentionally
does not override the production kernel allocator; it does not claim complete
C++ allocation accounting. `--stress` adds near-limit prism intersections
with 906304 requested triangle pairs accepted and 1008016 denied by the
one-million-pair guard, plus 128 seeded mutation cases. A measurement-only
probe reports whether cancellation was requested while the Boolean kernel
call was active; a completed call without that observation is not evidence
of cancellation responsiveness. Join latency and outcome are recorded in
either case.
`SolidGeometryAllocationProbe` is a separate optional executable for one-call
allocation accounting. It intercepts the complete program's C++ global
new/delete family, including aligned, sized and nothrow forms, and reports
allocation count, summed requested bytes, peak live requested bytes, live bytes
with the candidate, and bytes retained after candidate release. Graph setup,
one-time warmup and result formatting are outside its counted scope. This is
not total process memory: C malloc, custom native allocators, heap overhead,
and GPU memory are not observed. It must not be used for compile-time latency
comparisons; use the uninstrumented benchmark for those.
The implementation remains pending qualification until those tests and
the compound asset tests run on the integrated build.
