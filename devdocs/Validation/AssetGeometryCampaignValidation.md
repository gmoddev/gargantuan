---
status: historical
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

This is the frozen Source13 delivery record. The
[KI-2B-001 correction](DenseMeshContactKI2B001.md) supersedes its open dense-contact
limitation and hosted-CI continuation. Measurements and earlier verdicts below
retain their original source scope.

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

The completed local aggregate `Candidate13-Qualification.json` is 211534 bytes,
SHA-256 `C2DF273FD5DC43FFF8A6D555980DFF699F60917EE90B6835E7EEE2BA79C9C1AA`.
`Candidate13-LogManifest.json` binds 233 retained files, SHA-256
`E0FF3FC4C59A17D45F6939FE7336EAD7940EE0B9776744BEF6BBC6AA6DB486CC`.
It includes binary/cache/header hashes, source overlays, profiles, raw result
records and failure history. The repository receipt provides the compact portable
checkpoint; the larger artifact packet remains in the stated task artifact directory.

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

| Milestone | Implementation | Qualification boundary |
| --- | --- | --- |
| A1 canonical collision | Complete | Collision codec, import and malformed-input tests |
| A2 MeshPart projection | Complete | Static triangle queries/contact, safe replacement and stale identity |
| A3 persistence/residency | Complete | Reopen, runtime-loaded collision, revisions, shared resources and caps |
| A4 bounded kernel integration | Complete | Pinned archive/Git provenance, offline build and license notices |
| A5 Boolean compiler | Complete | Union/intersection/ordered subtraction, nesting/transforms/imported operands |
| A6 asset publication | Complete | One compound Mesh/Collision commit; previous bake survives failure |
| A7 qualification | Locally qualified within stated limits | Source13 Release/sanitizer gates, paired measurements and independent review; hosted CI separate; KI-2B-001 open |

## Inspection and resource accounting

`AssetService::ReadCollisionStats()` is a bounded read-only snapshot of service
ownership: resident Collision records and canonical bytes, retained Collision
artifacts, cumulative retired artifacts, failed cache admission, the complete
artifact-map bytes, and pending render-mesh counts/payload. It does not mutate
Instances or append an unbounded event history. `AssetCollisionResource` carries
the stable ID and content revision alongside shared immutable geometry.

`PhysicsWorld::GetCollisionResidency()` reports resource/consumer counts,
canonical bytes, exact Box3D mesh-data bytes, configured limits, creation and
retirement totals, failed admissions and the last bounded failure reason.
Per-world hard ceilings are 1024 collision resources, 65536 consumers and 64 MiB
each for canonical and native mesh-data bytes; configuration can lower them.
Native handles stay inside the adapter. Shared ownership retains geometry until
the last consuming shape or captured resource is released. Snapshot-owned bytes
outside the service are not included in its current residency total; per-shape
allocator overhead is explicitly unknown. No eviction or 3M demand policy is
implied by these counters.

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
5. The first related Windows run passed six of seven tests but the packaging
   fixture failed its generic two-build assertion. A standalone CLI build with
   the same runtime distribution succeeded. The unchanged packaging executable
   then passed both Release builds with task-owned TEMP/TMP shortened from the
   55-character `C:/Sandbox/Codex/Artifacts/asset-geometry-20261104/Temp` to the
   25-character `C:/Sandbox/Codex/Tmp/AG13`. All seven related tests passed in
   15.95 seconds of CTest time (17.88 seconds outer recipe wall time). The
   96-character content artifact paths plus unique staging
   siblings are consistent with the existing 240-character guard being crossed
   by the longer fixture prefix. The failed test removes its transient suffix,
   so its exact full staging path was not captured. This is direct unchanged-
   binary environment A/B evidence, not a product guard removal or an assertion
   about an uncollected diagnostic. Both runs remain in the evidence.

A failed or canceled earlier attempt is never promoted to a pass.

## Source13 execution qualification

| Profile / gate | Result | Retained raw evidence |
| --- | --- | --- |
| Windows MSVC 19.51 Release, Ninja, C17/C++23 | Focused 8/8; 128 seeded graph cases | `Test13.stdout.log`, `Fuzz13.stdout.log` |
| Windows related regressions, short owned TEMP | 7/7; CTest 15.95 s | `TestRegressionsShortTemp13.stdout.log` and recipe receipt |
| Linux GCC 13 Release, focused console profile | Focused 8/8; 128 seeded graph cases | `test-release.stdout.log`, `test-release-fuzz.stdout.log` |
| Linux GCC 13 RelWithDebInfo, ASan/UBSan/leaks | Focused 8/8 in 2.46 s; 128 seeded cases; no reported sanitizer error | `test-asan.stdout.log`, `test-asan-fuzz.stdout.log` and stderr |
| Windows production shader path | Real glslc build and packaged-shader test pass | Related build/test receipts |
| Linux focused profile shader guard | Expected exit 1; refuses graphical/package shader target | `guard-release.stdout.log` and receipt |

The eight focused CTests are asset foundation, physics backend, solid geometry,
solid allocation probe, Box3D lane compatibility, unknown-source guard,
known-source smoke, and incremental source guard. Related Windows tests cover
packaged shaders, animation foundation, animation preview EditorHost, renderer
publication, physics query benchmark smoke, standalone packaging and packaging
EditorHost.

Linux used existing image
`sha256:d71a9fc779ab954d99b9ddce26a497e2b5e83921484cd645540dd0e973bd8176`
with CMake 3.28.3, Ninja 1.11.1, GCC 13, SDL console mode and
`GARGANTUAN_HEADLESS_GEOMETRY_QUALIFICATION=ON`. Its only naming concession is
`-Wno-error=changes-meaning` for pre-existing PascalCase field/type names, not
blanket `-fpermissive`. Both C and C++ use `-fsanitize=address,undefined
-fno-omit-frame-pointer`; runtime options are
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. Alignment remains enabled.
Compilation is capped at two owned jobs per host; tests are serial.

This Linux profile does not qualify graphical packaging, shader generation or
the full supported hosted Clang 19 production profile. Standard PR CI runs
separately; no manual networking/3L physical campaign was launched.

Final worker censuses show zero owned compilers/benchmarks on Windows at
06:36:03 UTC and zero task containers/compilers on Linux at 06:34:59 UTC. Existing
services, dependency caches and unrelated worktrees were preserved. The pinned
Box3D header remains unchanged on both workers; private patched headers have the
same canonical LF SHA-256, recorded in the source receipt.

## Independent review and PR status

Read-only architecture, lifecycle, performance/resource and final evidence review
covered the integrated source through Source13. Review-driven corrections include
published content availability during reimport, immutable identity/buffer reuse,
retirement and queued generations, scaled/non-rigid geometry validation,
whole-graph/depth/integer bounds, reusable compiled flat meshes, and incremental
dependency hash checking. The final evidence audit corrected elapsed-time labels
and distinguished fifteen executed benchmark children from sixteen receipt files.
KI-2B-001 remains open; reviewers did not treat a zero stress-process exit as proof
of complete dense contacts. No further blocking source finding was reported for
the stated foundation scope.

[Draft PR 14](https://github.com/gmoddev/gargantuan/pull/14) targets main without
merging. Its first delivery `7ea1e81c30581d781bda83e9f4df19e582530c96` contains
the qualified Source13 code plus documentation. The automatic
[native run](https://github.com/gmoddev/gargantuan/actions/runs/41784250200) and
[GNS sanitizer run](https://github.com/gmoddev/gargantuan/actions/runs/41784250179)
were still building when this receipt was prepared. Final documentation updates
may supersede those runs; use the PR's checks for the current delivery head.
No hosted full-profile pass, physical networking qualification or merge approval
is implied by the focused local results.

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
The timing hash encodes the compiler's Mesh candidate before source-group
Collision dependency IDs are assigned. It does not compare whole project catalogs
or generated asset identities between independent projects.

## Linux paired compiler measurements

Source11 is `ec71672ba2ac504dedb0c1ca2a49f1687d8f4b32`, archive SHA-256
`4D4749937ABA39AF43E7240C339D2CDEC95FA634931EECBA89E39531CB790E1A`.
The comparison is Source11 to Source13 on the same GCC 13 Release host/container
profile. The runtime change replaces the observer's sleeping teardown with a
stop-token-aware wait; it preserves the 2 ms cancellation polling interval.
The Box3D compatibility correction does not execute in this solid benchmark.

Source13's owned sanitizer container was paused from 06:21:23 to 06:21:26 UTC,
then resumed by a shell trap. Available memory was 20,295,467,008 bytes. The
recorded process `%CPU` is a lifetime average for paused compiler children, not
instantaneous CPU activity during timing. All 15 serial benchmark/probe processes
exited zero. Raw records are in `LinuxBenchLogs/`, `LinuxAllocLogs/`, and
`LinuxBench13Logs/`; `LinuxBench13Receipts/bench13-window.receipt.txt` records
the pause and peer census.

Times below are milliseconds; every row has 101 calls per profile.

| Scenario | Before P50 | P95 | P99 | Source13 P50 | P95 | P99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| simple union | 2.10441 | 2.12161 | 2.13903 | 0.093171 | 0.112941 | 0.150551 |
| medium nested | 2.08895 | 2.11563 | 2.14599 | 0.840287 | 0.887368 | 1.11524 |
| complex union | 2.08952 | 2.12098 | 4.14747 | 1.86146 | 1.94065 | 1.97038 |
| dense intersection | 4.14784 | 4.18763 | 4.20606 | 2.35518 | 2.43152 | 2.49642 |
| coplanar touching | 2.10374 | 2.11691 | 2.14036 | 0.090640 | 0.099971 | 0.124782 |
| near coplanar | 2.10145 | 2.12273 | 2.14366 | 0.076151 | 0.112581 | 0.186142 |
| thin cut | 2.10600 | 2.12234 | 2.15492 | 0.076660 | 0.094711 | 0.156661 |
| incremental edit | 2.09978 | 2.13784 | 2.14948 | 0.099791 | 0.151712 | 0.232763 |

The seven fixed successful scenarios preserve their pre-change canonical hashes
and repeat exactly across 101 calls. Incremental edit produces 100 changed hashes
in 101 calls in both profiles. The near-cap intersection charges 906304 pair
units, returns 928 triangles, and takes 30.9252 before versus 27.3085 ms after in
one call each. Its matching pre/post hash is not a repeatability measurement.
The 1008016-pair request is denied with `GeometryLimit`, charging zero Boolean
pair units, in 2.16768 versus 0.923208 ms (one observation each). Cancellation was
requested while inside a Boolean call in both probes; both returned `Cancelled`,
with join observations 3.01005 and 2.31986 ms. These single observations do not
establish P95/P99 or a worst-case cancellation deadline.

The separate C++ requested-payload probe measured:

| Scenario | Allocation calls before → after | Requested bytes | Peak live bytes | Candidate-live bytes | Retained after release |
| --- | ---: | ---: | ---: | ---: | ---: |
| simple | 835 → 836 | 50914 → 50986 | 12053 → 12125 | 5944 | 0 |
| medium | 3703 → 3704 | 278685 → 278757 | 94162 → 94234 | 62472 | 224 |
| near pair limit | 34504 → 34505 | 2104246 → 2104318 | 267653 → 267725 | 189544 | 0 |

The wait change adds one allocation and 72 requested/peak-live bytes in these
cases. Candidate-live and post-release retained counts are unchanged. This is a
measured latency/allocation tradeoff, not an allocation reduction. Process RSS
peaks rose about 1–1.8 MB between the fresh-process profiles; those high-water
observations are not explained by the C++ payload counter alone. The Source13
near-cap process peak is 12,582,912 bytes. No hard heap bound or leak conclusion
is inferred from RSS differences.

## Linux native collision baseline

The Source13 current-binary Box control and 512-triangle mesh cases report the
following microsecond observations. Additional-create and retirement percentiles
sample individual shapes; query, mover and contact rows use 101 steps each.

| Kind / consumers | Additional create P50/P95/P99 | Ray P50 | Mover P50 | Contact step P50 | Retirement P50 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Box / 1 | — | 0.13 | 0.58 | 1.52 | 0.28 |
| Mesh / 1 | — | 0.31 | 45.101 | 1.51 | 0.26 |
| Box / 100 | 0.52 / 0.83 / 1.44 | 0.22 | 0.73 | 1.50 | 0.10 |
| Mesh / 100 | 3.06 / 3.67 / 9.081 | 0.43 | 45.321 | 1.50 | 0.09 |
| Box / 1000 | 0.54 / 1.94 / 3.33 | 0.25 | 0.79 | 1.50 | 0.14 |
| Mesh / 1000 | 3.07 / 3.42 / 5.63 | 0.43 | 44.601 | 1.50 | 0.13 |

Mesh first-shape observations are 118.511, 69.870 and 69.911 µs for the 1/100/1000
cases; they are single observations, not distributions. Each case shares one
native resource: 9612 canonical position/index bytes and 18888 native mesh-data
bytes, independent of consumer count. Three revision rounds temporarily retain
two resources where old consumers remain. At final retirement resources,
consumers and both byte totals return to zero; stale body IDs remain invalid.
The 1000-consumer case retires 4000 consumer generations and four resources across
its initial and revised generations. Both intentional malformed/scaled replacement
denials preserve the old queryable body. Source11 and Source13 sharing counts and
bytes agree; no historical collision speedup is claimed. Triangle mover work is
substantially costlier than the Box control in this fixture.

## Windows measurements and measured cross-profile hashes

The quiet Windows window contains 15 named zero-exit benchmark processes plus
one aggregate window receipt: eleven solid scenarios, mesh default and stress,
AssetService import and full physics-query benchmarks. There is no matched
pre-change Windows timing run. These are current absolute observations.

| Compiler scenario | P50 / P95 / P99 (ms), 101 calls |
| --- | ---: |
| simple union | 0.3425 / 0.4359 / 0.4639 |
| medium nested | 2.1098 / 2.5478 / 2.8307 |
| complex union | 5.2705 / 5.6470 / 5.8985 |
| dense intersection | 6.5226 / 7.0427 / 7.0873 |
| thin cut | 0.2807 / 0.3804 / 0.8465 |

The near-cap Windows observation is 105.857 ms (one call). The single cancellation
probe requests while inside a Boolean call and returns `Cancelled`, with a
19.2403 ms join observation. It does not establish a latency bound.

All seven successful fixed 101-call scenarios have identical candidate Mesh hashes,
input/output triangle counts and charged pair estimates on Windows and Linux.
The near-cap one-call hash also matches. Incremental edit changes 100 hashes in
101 calls on both. Over-limit input returns `GeometryLimit` on both. These
observations establish this corpus only, not universal cross-toolchain identity.

The 512-triangle Windows native fixture shares 9612 canonical and 18888 native
bytes at 1/100/1000 consumers. At 1000, ray/contact-step/mover medians are
1.6/2.9/92.8 µs. The 2048-triangle stress fixture shares 37644 canonical and
74568 native bytes; its corresponding medians are 1.9/2.2/810.4 µs. Both retire
all resources and consumers. The stress stdout first warns
`Box3D: WARNING: complex mesh detected, triangle buffer capacity of 256 reached`.
That run proves execution, at least one contact and retirement, **not complete
dense rigid-contact correctness**. KI-2B-001 below records the limitation;
the warning must remain in the raw log when parsing its following JSON.

The one-run AssetService import baseline includes the new collision companion
path: OBJ mesh import 0.7346 ms; glTF canonical conversion 1.6762 ms; artifact
serialization 0.0578 ms; hashing 0.4600 ms; small/medium/four-material glTF imports
1.2412/4.7344/1.3983 ms; unchanged/one-texture reimport 2.1786/3.0961 ms. These do
not isolate collision-only preparation cost. Creating 1000 shared default-Box
MeshParts takes 523.694 ms, render publication 3.7219 ms and full resync 3.1824 ms.
These current observations are not compared with historical 2A numbers from a
different producer/profile. The full existing 1/100/1000/10000 collider query
benchmark also exits zero; raw layout/query percentiles are in
`WindowsBench13Logs/Bench13Physics.stdout.log`.

## Remaining boundaries and continuation

Dynamic concave simulation is outside this slice; the explicit alternative is
Box collision. Tiny mesh scales use the documented Box alternative, and other
invalid replacements fail while retaining old state. Rigid mesh CFrames must be
proper orthonormal rotations. Native admission charges both old and candidate
resources during replacement, so a saturated world needs staging headroom.
Per-shape native allocation bytes and transient allocator peak are not a hard
quota. The XPBD deformable projection retains primitive approximations.

**KI-2B-001 — dense rigid contacts:** admitted meshes whose local Box3D rigid
contact query reaches 256 triangle candidates can have incomplete contacts.
The backend processes the gathered subset and logs once per process; it does
not reject the asset or expose a neutral per-asset truncation counter. Rays and
mover queries are separate paths. The stress fixture demonstrates the capacity
observation without proving a missed contact or complete coverage. The next
targeted step is a deterministic overflow/missed-contact fixture and a measured,
observable bounded backend strategy. This limitation is not resolved by raising
an unmeasured constant or describing admitted meshes as rejected/unsupported.

Empty solids are rejected. Boolean output regenerates flat normals, planar UVs
and tangents without source material groups or source-face attribution. Kernel
cancellation is cooperative between phases, with no hard allocator quota or
preemptive wall-clock deadline. The documented graph/triangle/pair bounds remain
mandatory. The native API is authoring work, not a runtime Luau CSG service.

No production Studio, frozen Avalonia, Geometry 1B modeling interface, network
authority, 3L content lifecycle or 3M demand selector changed. The pre-existing
Size/2 coarse package bounds can under-cover arbitrary mesh positions; the 3M
campaign owns that spatial-policy limitation and records it as `KI-3M-001`.

Resume from the exact source receipt: verify the final delivery head and hosted
PR checks, preserve the qualified Source13 packet, and address KI-2B-001 before
claiming complete dense rigid contacts. Do not merge main automatically. After
this campaign, a separately scoped step can define Studio
authoring integration and surface/material provenance. A hard kernel memory or
deadline guarantee requires a separate allocator/process-isolation design.
