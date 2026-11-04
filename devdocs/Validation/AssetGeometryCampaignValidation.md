---
status: qualification-in-progress
date: 2026-11-04
owner: runtime-assets
related_code:
  - tests/AssetFoundationTests.cpp
  - tests/PhysicsBackendTests.cpp
  - tests/SolidGeometryTests.cpp
  - tests/MeshCollisionBenchmark.cpp
  - tests/SolidGeometryBenchmark.cpp
  - tests/SolidGeometryAllocationProbe.cpp
---

# Asset 2B and Geometry 1A campaign qualification

## Source and evidence boundary

The integration branch is `feature/asset2b-geometry1a`, based on
`e682be08df84d776bfa455137728265c6494b5e8`. It includes completed Foundation 3L
and does not incorporate the concurrent 3M branch. The machine-readable
[source receipt](AssetGeometryCampaignSource.json) identifies the frozen source
archive, generated reflection packet and qualification state. Subsequent report
edits are documentation, not a claim that unexecuted checks passed.

Final candidate Source13 is `efbe1b88ef860c749c9edf335b5a6ff31c8009a4`.
`Candidate13.tar` SHA-256 is
`3623F4960260FDB73194389BB8D39E25C7B6E23EE40233E2D0B8C22D6B094375`.
Source12 contains the runtime code; Source13 adds the incremental CMake hash
guard and its isolated regression test. `Candidate01Generated.tar` supplies the
generated reflection files, SHA-256
`D054C25BC9188845AD2650E728EC8DE80F86CB74C621FBE1EF4DFAE77E687B32`.
Raw packets, commands, receipts and stdout/stderr are retained under
`C:/Users/aiden/.codex/artifacts/asset-geometry-20261104`, with worker logs in the
separate namespace documented in [worker coordination](AssetGeometryWorkerCoordination.md).

## Implemented behavior

Asset 2B publishes separate validated Collision artifacts with OBJ/static glTF
meshes and explicit Mesh dependencies. It preserves logical IDs, last-known-good
publication, save/reopen, Play clone and packaged runtime resolution. Successful
replacement retires unreferenced prior source-group artifacts and coalesces
unpublished render generations. Captured snapshots own their immutable revisions.

`MeshPart.CollisionMode` is a persistent schema property. `Box` remains the
default; anchored `StaticMesh` uses the canonical triangles. Physics shares native
mesh data by immutable identity/revision, replaces shapes at its existing safe
point, preserves generation-safe body identity, and reports admitted/retired
resources and consumers. Malformed replacements leave the old queryable body.

Geometry 1A accepts bounded native `SolidGraph` values or `.solid` JSON source.
Box, sphere, imported closed meshes, transforms, nested union/intersection and
ordered subtraction compile to ordinary Mesh and Collision candidates. The
existing AssetService compound commit publishes the complete pair atomically.
The minimal serial Manifold v3.5.4 dependency is pinned, offline, licensed and
independently checked against the official archive and Git blobs.

The [accepted decisions and API examples](../CurrentArchitecture/AssetCollisionGeometryDecisions.md),
[collision contract](../CurrentArchitecture/AssetCollisionFoundation2B.md),
[compiler contract](../CurrentArchitecture/SolidGeometryFoundation1A.md), and
[physics contract](../CurrentArchitecture/PhysicsBackend.md) are canonical.

## Correctness coverage

The asset suite exercises both never-delivered and previously delivered render
generations, repeated replacement and artifact retirement, real source-read
observation during reimport, identical-content buffer retention, failed imports,
compound dependency changes, captured revisions, tampered artifacts, project
reopen and runtime-loaded Boolean MeshPart raycasts. Physics tests cover shared
triangle data, rays, capsule movement and contacts, scaling and coordinate
rejection, non-rigid CFrame rejection, resource caps and recovery, dynamic Box
fallback, stale body IDs and preserved prior shapes.

Geometry tests check independent expected volumes (union 12, intersection 4,
subtraction 4), subtraction order, reusable compiled coplanar cuts, flat surface
UV/tangent validity, whole-graph validation, cycles and missing references,
malformed/open meshes, oversized JSON integers, vertex/depth/work bounds, expired
deadlines, cancellation, touching/disconnected/empty/thin/extreme geometry, and
same-profile canonical repeat hashes. The opt-in mutation corpus uses seed
`0x4ac8172d` and 128 valid/invalid cases. This is bounded corpus testing, not an
exhaustive proof of kernel geometry behavior.

## Demonstrated failures and corrections

1. Repeated headless imports could emit a removal for an unpublished render
   generation. Queue coalescing now cancels unpublished creates while retaining
   the removal of a previously delivered generation. Both lifecycle epochs have
   end-to-end tests.
2. A Windows archive overlay retained timestamps older than compiled objects.
   A partially rebuilt PhysicsTypes ABI produced invalid intermediate results.
   Source hashes were checked, changed files received a fresh worker timestamp,
   and the relevant core/backend/test objects rebuilt. The mixed-object run is
   excluded from qualification. Source10's rebuilt focused four tests passed;
   Source11's Linux Release focused four tests passed.
3. Strict Source11 UBSan found the pinned Box3D SSE2 `b3LoadV` unaligned and aliased
   `double*` load at `src/simd.h:66`. A recoverable diagnostic run completed all
   four processes but still had six reports at that site; it is failed evidence.
   A checked private build copy now uses `memcpy` to an aligned local value.
   Original vendor bytes remain unchanged. A 4-byte-aligned, exactly 12-byte
   fixture checks SIMD lane bits, and known/unknown source tests cover the patch.
4. Final independent review identified that `file(READ)` did not register the
   original SIMD header as an incremental configure dependency. Source13 tracks
   it explicitly and verifies automatic reconfiguration rejects an altered
   fixture. No shared vendor tree is mutated by the test.

The terminal Source13 execution table and paired performance results remain in
progress. A failed or canceled earlier attempt is never promoted to a pass.

## Measurement rules

Solid timing uses uninstrumented Release executables with `--full101 --scenario
NAME`, each scenario in a fresh process on the same quiet host. Ordinary cases
have 101 observations; near-pair accepted/denied and cancellation cases have one
observation and cannot establish tail latency or repeat determinism. The observer
change is compared against Source11 on the same Linux GCC 13 profile, not against
another machine or a pre-geometry implementation.

The native collision benchmark compares Box and static mesh within one binary
at 1/100/1000 consumers, with 101 query/contact-step samples, three revision
rounds, failed replacement, retirement and stale-ID checks. Its input is synthetic
canonical triangles; AssetService import and DataModel work are outside that
measurement. Native mesh-data and canonical position/index bytes are exact;
per-shape allocator overhead is unknown. Process RSS/private-commit observations
are not per-call allocation counts.

The separate allocation probe counts C++ global new/delete requested payload,
including aligned/sized/nothrow forms. Setup, warmup and formatting are excluded.
It does not count C malloc, heap metadata, custom allocators or GPU memory and
must not be used for latency comparisons. OS peak memory is a process-lifetime
high-water mark. Cross-toolchain canonical equality is limited to the actually
measured corpus; no universal byte-determinism claim is made.

## Remaining boundaries and continuation

Dynamic concave simulation is outside this slice; the explicit alternative is
Box collision. Tiny mesh scales use the documented Box alternative, and other
invalid replacements fail while retaining old state. Rigid mesh CFrames must be
proper orthonormal rotations. Native admission charges both old and candidate
resources during replacement, so a saturated world needs staging headroom.
Per-shape native allocation bytes and transient allocator peak are not a hard
quota. The XPBD deformable projection retains primitive approximations.

Empty solids are rejected. Boolean output regenerates flat normals, planar UVs
and tangents without source material groups or source-face attribution. Kernel
cancellation is cooperative between phases, with no hard allocator quota or
preemptive wall-clock deadline. The documented graph/triangle/pair bounds remain
mandatory. The native API is authoring work, not a runtime Luau CSG service.

No production Studio, frozen Avalonia, Geometry 1B modeling interface, network
authority, 3L content lifecycle or 3M demand selector changed. The pre-existing
Size/2 coarse package bounds can under-cover arbitrary mesh positions; the 3M
campaign owns that spatial-policy limitation and records it as `KI-3M-001`.

Resume from the exact source receipt: finish strict Source13 Windows/Linux gates,
paired benchmarks and evidence review; publish the isolated feature PR without
merging main. After this campaign, a separately scoped step can define Studio
authoring integration and surface/material provenance. A hard kernel memory or
deadline guarantee requires a separate allocator/process-isolation design.
