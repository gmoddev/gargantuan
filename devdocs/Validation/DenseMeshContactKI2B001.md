---
status: current
date: 2026-11-04
owner: physics
related_code:
  - src/physics/Box3DPhysicsBackend.cpp
  - src/physics/Box3DMeshChunks.hpp
  - src/classes/WorldRoot.cpp
  - tests/PhysicsBackendTests.cpp
  - tests/Box3DMeshChunkTests.cpp
  - cmake/Box3DCompatibility.cmake
---

# KI-2B-001: bounded dense mesh contacts

This follow-up corrects the candidate omission found during
[Source13 qualification](AssetGeometryCampaignValidation.md). It keeps the same
canonical Collision assets, MeshPart policy, physics authority and PR14 branch.
Source13 measurements and failures remain historical evidence.

## Deterministic missed contact

The test-only old-runtime source is
`7cc17f28a630ad7c2ce2bf64fb308f1d4f7f4d31`, archive
`Candidate14-Regression.tar`, SHA-256
`AF6780BAE1A815F8EE979F80063B00CA1708BAB144CC4E258361240B5E48C832`.
Three hundred upward triangles at y=-0.05 overlap the contact query's expanded
AABB without touching a radius-0.5 Ball centered at y=0.49. An upward support
triangle at y=0 does touch the Ball. Tiny distinct x/z shifts keep the decoys
separate while the pinned BVH visits them before the support. A filtered downward
ray proves the support exists independently of rigid-contact candidate collection.

Windows Release, Linux Release and strict Linux ASan/UBSan all reproduce:

```text
[Physics:DenseContactRegression] decoys=300 cache=256 sparse_ray=1 dense_ray=1 sparse_began=1 dense_began=0
```

The sparse reference contacts; the dense body misses the support. The fixed
candidate reports sparse_began=1 and dense_began=1. The regression runs normally
in the physics suite or alone with `GARGANTUAN_DENSE_CONTACT_REGRESSION=1`.
Old run evidence is bound by `KI001-Old-Manifest.json`, SHA-256
`9DF416E52C86824D0C1429AA59A755ACD8B07E49FEA04F75522FC988AC2829EE`.

## Bounded correction and authority

`Box3DMeshChunks.hpp` builds one globally edge-identified mesh, then partitions
its spatial BVH triangle order into chunks of at most 255 triangles. It compacts
vertices per chunk and carries global edge flags through the native builder's
triangle reorder using temporary ordinal labels. Labels are removed, material
indices restored to zero, and native hashes recomputed before publication.
This preserves seam adjacency without changing the pinned vendor builder or
discarding geometry. Meshes of at most 255 triangles retain the single-blob path.

One shared native resource owns all chunks of one immutable asset/revision.
Each consuming body owns one native shape per chunk. Canonical bytes count once;
native bytes sum chunk blobs. A separate world mesh-shape ceiling is 65536 and
can be lowered. Old and candidate shapes remain charged during replacement.
All candidate shapes roll back on failure before the old body/pose changes.
The existing resource, consumer and byte ceilings remain, with conservative
blob-size bounds charging the global adjacency mesh and staged chunks together.
Primitive admission, 3M selection and 3L lifecycle/networking are unchanged.

All native shapes map to the same generation-safe neutral body. Raycasts return
the nearest hit per body and coalesce chunk duplicates before applying the
candidate limit. Exact duplicate mover planes are coalesced. Native contact and
sensor pair membership aggregates across the complete step to one neutral body
Begin/End transition. A seam transfer does not end an existing contact. CanTouch
changes stage shape replacement because the pinned native contact copies its
event flags when created.

Read-only residency exposes native chunk count, 255-triangle policy, projected
mesh-shape limit, active/created/retired mesh shapes, and completed-build peak
native blob bytes. Per-shape overhead, builder scratch and failed-build peaks
are excluded from that peak metric. Contact observation tracks at most 65536
native pairs and changed body pairs; overflow latches `EventsTruncated` until
world teardown, marking event history incomplete. It does not omit physical
triangles. No native handle or chunk configuration is serialized or exposed to
Instances, Luau, networking or renderer state.

## Additional demonstrated failures

The prior delivery's complete hosted CI failed character networking and real
transport. The new collision observer called `GetDataModel()` on stack WorldRoots;
that path used `shared_from_this()` and threw `bad_weak_ptr`. Prediction suspended,
causing the history/action failures and localhost non-movement. The old runtime
reproduces 88 character checks failing locally. The correction skips asset
observation for unowned stack worlds and adds a direct detached-motion regression.
Real DataModel roots retain asset observation. Character and transport policy
were not changed.

The exact prior c62 hosted failures are Windows Native 2/110, Linux Native
sanitizer 1/66 and GNS sanitizer 1/11. Full logs and job metadata are retained in
`Hosted-C62-Manifest.json`, SHA-256
`922E292E38F29B9ADD2CA14F0B16A07A98343DD5A402487984FC58D77A9A25B4`.

The first chunk candidate changed the native-byte denial text; an existing test
rejected it. Source16 restores `Mesh collision native-byte limit reached` without
relaxing admission. A CRLF manifest parsing error created empty CR-suffixed
task-owned paths on Linux, leaving the real source older than an object; that
ASan16 no-op build is excluded. Synchronization now uses `splitlines`, path
containment, SHA verification and current timestamps on the real files, followed
by actual rebuild evidence. The long Windows TEMP prefix also reproduced the
known package staging-path denial; the unchanged binary passes with the existing
short task-owned TEMP. Earlier failed attempts remain failed evidence.

Strict Source16 sanitizers then found an upstream Box3D free-list alignment error
in `block_allocator.c`: elements with 12/20-byte stride can be four modulo eight,
while pointer-cast load/store requires eight-byte alignment. Source17 replaces
both accesses with `memcpy` in the checked private build copy. Stride, layout,
allocation size and the borrowed vendor source remain unchanged. Direct C17 tests
exercise growth, preallocation, LIFO reuse, allocation counts and final byte
balance. Hash/smoke/incremental guards cover both corrected source files. Hosted
sanitizer recipes now instrument C as well as C++, with all existing gates intact.

## Qualification and source

The functional source is `90d7cacec62f3e4e3da9453892f266df1e270b5b`,
`Candidate17.tar` SHA-256
`0200E14556F5C5712926FB5DB272C52DF62C461F5BFF298CDE59E84BC6393058`.
The following CI-only commit `88c65737b3d8155135aa0150ea0f5dfa915e3a57`
adds C sanitizer flags and their documentation; it changes no runtime source.
The first hosted runs containing those flags, Native `41811972911` and GNS
`41811972560` at `f0e73878ecbaa7b63740d68ac66aa68c8cc14f49`, demonstrated a
shared SDL link failure before Linux tests: `--no-undefined` rejected sanitizer
symbols that Clang supplies from the final executable. The follow-up recipes
preseed SDL's `LINKER_SUPPORTS_WL_NO_UNDEFINED=OFF`; SDL remains shared and fully
instrumented, and executable sanitizer flags and test gates remain intact.
Those failed raw logs are retained separately under `Hosted-Final-*`; the
frozen Source17 local evidence remains unchanged. Full hosted qualification must
use the following delivery head, not infer a pass from this earlier attempt.
The generated reflection packet remains `Candidate01Generated.tar`, SHA-256
`D054C25BC9188845AD2650E728EC8DE80F86CB74C621FBE1EF4DFAE77E687B32`.

Local Source17 qualification passes:

| Profile | Result |
| --- | --- |
| Windows MSVC 19.51.36260 Release, normal shader profile | 10/10 focused CTests; dense regression; 128-case seeded corpus |
| Windows related, real glslc and short owned TEMP | 6/6: shaders, animation, preview EditorHost, renderer, packaging and packaging EditorHost |
| Linux GCC 13.3 Release, focused console profile | 10/10 focused CTests; dense regression; 128-case seeded corpus |
| Linux GCC 13.3 RelWithDebInfo, strict ASan/UBSan/leaks | 10/10 focused CTests; dense regression; 128-case seeded corpus; C and C++ instrumented |

The ten focused tests are Solid geometry, allocation probe, Box3D compatibility,
chunk geometry/flags/bounds, source guard, smoke, incremental guard, asset
foundation, physics backend and Character networking. Strict runtime options use
leak detection and halt on first error; alignment remains enabled. Linux retains
the narrower fail-closed headless qualification profile and pre-existing
`-Wno-error=changes-meaning` naming concession. This does not replace the complete
hosted Clang 19 profile. Local Windows GNS is disabled; real transport is gated
by the final hosted workflows.

The final aggregate `KI001-Candidate17-Qualification.json` has SHA-256
`DA4DD3DE17DF08EDE3D2448062073ABE0AFABD6C2A6551B2D9A109BB1151326C`.
`KI001-Candidate17-LogManifest.json` binds 177 raw files, SHA-256
`5C70A6D59F8DE9E3E7EAB769270CD4599F09782A9D2C5E4BDE72E4FBFBA0826F`.
This frozen revision includes the explicit excluded-ASan16 no-op diagnostic note;
the measured run results are unchanged from the initial 176-file packet.
Eighteen final build/test/benchmark child receipts exit zero. The four final
benchmark processes are distinct serial default/stress runs on each host.
The original Box3D sources are unchanged; private allocator hashes match across
hosts after LF normalization. Both final worker inventories show no owned
compiler, benchmark or task container remaining. Caches/services are preserved.

The [machine-readable continuation](KI2B001Continuation.json) identifies exact
evidence and hosted follow-up. All raw packets, logs and receipts remain under
`C:/Users/aiden/.codex/artifacts/asset-geometry-20261104`. This receipt records the
local gates before the final PR-head hosted runs; use
[PR14 checks](https://github.com/gmoddev/gargantuan/pull/14/checks) for that immutable
delivery head's full qualification. No full hosted pass is implied at preparation.

## Native execution and memory observations

Default and stress cases run 1/100/1000 logical consumers, three revision rounds,
query/contact samples, failed replacements, retirement and stale-ID reuse. Each
query/contact/mover distribution has 101 observations; additional creation has
999 observations at the 1000-consumer row. These are synthetic native backend
measurements, excluding AssetService import and DataModel work.

| Source17 mesh case, 1000 consumers | Ray P50/P95/P99 µs | Contact step P50/P95/P99 µs | Mover P50/P95/P99 µs | Additional create P50/P95/P99 µs |
| --- | ---: | ---: | ---: | ---: |
| Windows, 512 triangles | 1.3 / 1.4 / 1.9 | 1.9 / 3.5 / 27.2 | 73.5 / 77.6 / 87.4 | 5.6 / 8.5 / 31.5 |
| Windows, 2048 triangles | 1.4 / 1.7 / 1.9 | 1.8 / 3.8 / 76.9 | 447.7 / 472.1 / 486.3 | 18.3 / 25.1 / 51.8 |
| Linux, 512 triangles | 0.53 / 0.55 / 0.57 | 1.45 / 2.54 / 13.57 | 42.901 / 46.741 / 47.641 | 3.64 / 5.91 / 12.351 |
| Linux, 2048 triangles | 0.64 / 0.68 / 1.30 | 1.53 / 3.01 / 45.171 | 296.974 / 310.234 / 326.104 | 15.11 / 19.03 / 33.901 |

Current-binary Box controls remain in the raw records. Source13's same-host Linux
512-triangle ray/contact P95 was 0.46/2.07 µs; Source17 is 0.55/2.54 µs. The
observed additional cost is 0.09/0.47 µs in this fixture. Windows Source13 stress
reached the contact cache cap, so its contact time is not an equivalent complete-
contact baseline and cannot establish a speedup. Tail observations are samples,
not worst-case frame-time guarantees.

| Mesh triangles | Canonical bytes | Source13 native blob bytes | Source17 native bytes | Shared chunks | Native shapes at 1000 consumers | Peak charged completed-build blobs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 512 | 9612 | 18888 | 19344 | 3 | 3000 | 57576 |
| 2048 | 37644 | 74568 | 78672 | 9 | 9000 | 231912 |

Native blob growth is 456 and 4104 bytes respectively, while canonical bytes
remain unchanged. Build peaks include the still-live prior resource plus global
adjacency and candidate chunk blobs during reimport. They exclude builder scratch,
C++ arrays, failed-build peaks and unknown per-shape storage. All resource,
consumer, native mesh-shape and resident byte counters return to zero after
retirement. Benchmark legacy `activeStaticShapes*`/`retiredStaticShapes` fields
count logical body/shape consumers; native chunk shapes are reported separately
in `activeMeshShapes`/created/retired mesh-shape fields. Final default/stress
stdout and stderr contain no 256-triangle capacity warning.

## Remaining boundaries

This fixes omission by the 256-candidate cache: every admitted chunk has fewer
triangles than the cache capacity. It does not claim mathematically exact rigid
simulation, remove the native solver's manifold limits, introduce dynamic concave
bodies, or remove query/event/resource bounds. Global adjacency is preserved,
but separate native contacts may use different solver manifold groupings than
a hypothetical unlimited single-mesh contact. Exact seam/event, lifecycle,
resource and missed-contact tests qualify the stated correction.

Kernel compilation still has cooperative cancellation and no hard allocator quota
or preemptive deadline. Boolean surface/material policy and project formats are
unchanged. Production Studio and unrelated worktrees remain untouched. Keep PR14
unmerged; use its final-head hosted checks for full qualification.
