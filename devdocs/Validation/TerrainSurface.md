---
status: qualification
owner: Terrain Foundation 1
last_verified: 2026-11-26
---

# Terrain surface and conditioning

The production candidate uses one CPU MC33 extractor at the project lattice spacing. A job owns cells with minima in `[0, N)` on each axis. The `N+1` corner samples are read from the authoritative sample owners, with a one-sample halo on both sides for central differences. Derived chunk geometry is local to its integer chunk origin. The same canonical edge direction and bilinear face determinant bind both sides of a shared face, including exact determinant ties. Negative samples are solid. A stored zero contributes a half-code air bias during extraction so rounded crossings retain distinct vertex positions. The integer field and renderer revisions are owned by their respective subsystems.

The corner-sample interpretation, packed patterns and case dispatch use the MC33++ v5.4 data from David Vega and Javier Abache at `252dabd7ea6dea3d0529e66b047adbe878caf2fa`, including the interior diagonal behavior published by Vega, Abache and Coll. The exact 2,310 words were compared with the source table at that pin. The translation unit carries the MIT notice. Native sampling, edge indexing, normals, position conversion, material provenance, cancellation, work admission and collision conditioning are implemented for Gargantuan. Lewiner scikit-image 0.25.2 is an independent research oracle. [Reference paper](https://jcgt.org/published/0008/03/01/paper.pdf), [Pinned table](https://github.com/dvega68/MC33_cpp_library/blob/252dabd7ea6dea3d0529e66b047adbe878caf2fa/source/MC33_LookUpTable.h), [Pinned license](https://github.com/dvega68/MC33_cpp_library/blob/252dabd7ea6dea3d0529e66b047adbe878caf2fa/LICENSE.txt), [Oracle API](https://scikit-image.org/docs/0.25.x/api/skimage.measure.html#skimage.measure.marching_cubes).

All regular edge vertices use indexed positions, centrally differenced gradients interpolated along the edge, and material IDs read from the solid endpoint. Interior vertices used by a limited set of MC33 patterns use the cell center, an averaged halo gradient, and the strongest solid corner's material. Material provenance is retained as a dense halo index so a geometry-identical paint revision can refresh values without re-extracting indices or normals. Geometry and material output are complete before publication; a bounded call can deny an invalid halo, invalid material, cancellation, allocation failure, exhausted output budget, or degenerate geometric result.

## Collision representation

Box3D's pre-cook area test rejects triangles whose squared cross-product magnitude is below `1e-10` in local world units. The conditioner uses the same triangle indices at the same authored spacing. Each edge crossing remains on its original edge and is constrained to the central `[1/64, 63/64]` fraction of that edge. Both chunks apply this rule even when only one has small-area triangles. The maximum movement is `spacing/64` plus floating-point rounding. Barycentric correspondence then bounds the pointwise distance between the original and conditioned triangle by the same largest vertex movement. Cell-center vertices stay fixed. The recipe preserves vertex and edge incidence and therefore performs no topology-changing deletion or welding. Per-triangle material IDs use the majority of the input vertex IDs or the smallest ID when all three differ.

The conditioner fails the whole result if it cannot meet the area floor or maintain original face orientation. Removing failed triangles could introduce holes. General topology-preserving edge collapse or local retriangulation also needs seam controls, geometric validation and provider contact evidence. Those strategies are prospective options for remaining rejected input. A caller keeps the prior revision coherent on failure. The inset is a geometric policy with a finite resolution limit. It can change contacts and requires provider tests and gameplay qualification.

## Standalone Release qualification

Windows MSVC 19.50 x64 with `/O2 /W4 /WX` ran `tests/terrain/surface/TerrainSurfaceTests.cpp`. The lightweight standalone executable compiles with `src/terrain/Surface.cpp` and the Engine's `include` directory. Run it without arguments for the deterministic checks, or pass the research `--oracle-fields` output directory for the matched scene comparison. The comparison reconstructs the q16 codes from the exported f32 values and extends the outer fixture boundary by clamping for normals. It tests both a whole 64³ extraction and eight 32³ chunk extractions. Research evidence is pinned by Gargantuan commit `040ee80ba`.

- All 256 sign masks passed at 16 integer magnitude assignments each, including same-magnitude determinant ties and reproducible pseudorandom values. The audit checked interior boundary edges, directed edge incidence, welded degenerate triangles and vertex links as connected cycles or boundary paths. A targeted interior saddle changed connectivity from two separate patches to one connected patch while holding the signs fixed. The strict same-cell intersection audit found zero crossings in these 4,096 inputs.
- Negative-origin adjacent chunks matched their exact seam positions, normals and material IDs on all axes and for both a tied and untied face predicate. Tests exercised exact-zero air, outward planar winding, deterministic replay, material-only refresh, shape limits, pre-cancellation and failure-atomic replacements.
- A 0.8-unit slab is present at favorable alignment and absent when located between consecutive samples at spacing one. A single interior solid sample at density -1 against maximum positive density gives closed source topology with slivers. The conditioner produces geometry above the area floor at 0.5, 1.0 and 2.0 world-unit spacings.

| Retained q16 fixture | Native and Lewiner triangles | Native components | Unconditioned triangles below provider area floor | Conditioned per-chunk denials |
| --- | ---: | ---: | ---: | ---: |
| Rolling | 14,164 | 1 | 2 | 0/8 |
| Mountain | 18,496 | 1 | 9 | 0/8 |
| Cave | 21,710 | 1 | 65 | 0/8 |
| Island | 21,404 | 2 | 12 | 0/8 |
| Sparse | 3,136 | 2 | 3 | 0/8 |
| Dense | 130,880 | 1 | 68 | 0/8 |
| Collision | 11,680 | 2 | 6 | 0/8 |

Both native partitions produced these counts, zero interior open edges, zero nonmanifold or unoriented edges, zero nonmanifold vertex links, zero weld-degenerate triangles at a 1e-6 lattice-position tolerance and zero strict same-cell intersections in the tested inputs. All seven whole extraction results also passed conditioning, with the largest measured vertex movement 0.0155392 world units across the two partitions. The strict intersection audit excludes coplanar overlap and uses triangle centroid cell buckets. The original paper documents difficult geometries in the MC33 family, and finite coverage does not prove intersection freedom. [MC33 topology considerations](https://www.sci.utah.edu/~etiene/pdf/mc33.pdf), [Corrected interior tests and limitations](https://jcgt.org/published/0008/03/01/paper.pdf).

This component evidence covers geometry preparation and the provider's numerical area predicate. Native Box3D construction and stage ownership have their own checks. Character contact continuity, movement through cave passages, actual provider subshape memory, publication with renderer upload, cross-platform byte-level determinism and mixed-LOD traversal remain distinct qualification work. The regular mesher emits equal-resolution boundaries and supplies no mixed-resolution transition mesh.
