# Native MC33 mixed visual density investigation

Runtime collision uses authored resolution. Visual detail defaults to that resolution and can opt into a boundary-compatible derivative. The native probes also exercise raw 2:1 lattice pairing as a negative control. [Current implementation](../CurrentArchitecture/TerrainFoundation1.md), [native probe](../../tests/terrain/lod/TerrainLodQualification.cpp), [original raw CSV](../../tests/terrain/lod/TerrainLodQualification.csv).

## Candidate and scope

Generate both chunks with the existing MC33 extractor at authored spacing. Keep a one-sample collar around every face of a reduced visual chunk. The candidate keeps original border vertices, winding, indices relative to their positions, normal values, and stable material IDs. It reduces only the interior by contracting a bounded set of independent edges onto existing endpoints.

A contraction requires manifold edge incidence, a full triangular link-condition check, no blocked vertex in either one-ring, equal material across both rings, close endpoint normals, and nonreversed replacement faces above the area floor. Endpoints move at most 1.25 authored sample units. The probe accepts disjoint one-ring contractions in deterministic length and index order. Locking all face collars prevents each accepted contraction from touching same-level seams, multi-axis boundaries, or material changes inside retained faces. Source scalar samples, contact meshes, and camera-independent collision stay under their current owners.

The approach gives different visual triangle densities with one common MC33 boundary trace. It still samples the complete fine field. It can contract tiny or locally safe structures only when its checks allow it, so protecting the border does not establish interior accuracy by itself. Dey et al. supply the simplicial topology condition. Meshoptimizer is an independent optimization reference for attribute-aware, boundary-locked simplification. [Topology preserving edge contraction, 1999](https://aperture.stanford.edu/courses/cs468-02-fall/readings/contraction.pdf), [meshoptimizer simplification](https://meshoptimizer.org/#advanced-simplification).

## Native observations

The MSVC x64 Release probe uses six sign-preserving fields at 16³ cells per authored chunk. It compares equal-resolution reference meshes, a spacing-2 neighbor at the same world extent, and the collar-locked candidate. Each field runs on X, Y and Z adjacency. It extracts indexed open seam segments, quantizes tangential positions at 1e−5 world units and normals at 1e−3, and compares matching material IDs and opposing boundary half-edge winding. Reference and candidate topology checks cover edge incidence, one connected path/cycle per vertex link, connected components and Euler characteristic. A stale-vs-new boundary test uses a changed cave field.

| Result | Measured value |
| --- | --- |
| Controlled cases | 18, including planar, rolling, chamber/tunnel, thin tunnel, floating arch and saddle fields |
| Raw 2:1 unpaired seam segments | 24–70 per case. The planar case has corresponding geometry along one line but nonmatching segmentation. |
| Raw 2:1 thin cave | Each coarse neighbor had one component instead of the fine reference's two. Coarse lattice samples missed the narrow tunnel. |
| Collar-locked seam receipt | Zero unpaired segments in all cases and matching material, normal, and opposing winding receipts. |
| Collar-locked indexed topology | Components, Euler characteristic, boundary edge count and manifold edge/vertex-link checks equal to the fine references in all cases. |
| Triangle reduction on reduced side | 15,816 → 15,056 triangles over all cases, 4.19–5.33% fewer per case. Raw coarse emitted 3,720 across the same side extents with failed seam checks. |
| Largest accepted endpoint movement | 1.21219 authored sample units. This is an endpoint error observation. |
| CPU reducer | 0.303–0.986 ms for the tested 16³ chunks on Windows Ryzen 9 7950X3D. One bounded pass per case, no frame work. |
| Edit dependency check | Both sides at the new cave revision matched. Pairing the previous left face with the new right candidate had unmatched segments. |

The limited savings and standalone preparation cost do not support promoting the 16³ prototype. This original fixture covers the CPU geometry path. The local link condition and orientation checks leave nonlocal self-intersection qualification open.

## Required protocol before enablement

1. Freeze both sides' authored geometry and material revisions, shared halo inputs, active/owner lifetime, mesh generation, and intended LOD. Build each face receipt from the native winding and crossing IDs. Use canonical lattice edge keys and exact source samples for production correspondence, with an explicit tolerance only for output float checks. Batch coarse candidate, matching neighbor cover and associated render resources in one publishable dependency set.
2. Prefer a sparse visual candidate with an exact authored boundary collar as the first optimization target. Fast interior simplification needs measured shape deviation, self-intersection/overlap checks where required, and preservation of protected cave and overhang visibility. Keep collision independent. Include face, edge, and corner combinations, negative coordinates, material-only updates, cancellation, replaced dependencies, and stale result denial. Keep the old matching fine coverage when a new candidate is unavailable or invalid.
3. For genuine 2:1 scalar extraction, prototype a transition representation whose two oriented boundary traces are constrained to the exact native MC33 regular case output on each side. Test both face asymptotic decisions and volume ambiguity over sign complements and exact ties. Test neighboring transition faces where their edge and corner traces meet. Use cheap fine/coarse topology or feature-loss summaries to force equal detail for relevant caves or thin surfaces. A compatible transition mesh can close a geometric crack while the coarse samples still lose the tunnel.
4. Compare actual Camera movement with exterior and cave paths, two neighboring transitions and moving visibility, render swap/retirement, edit churn, frame uploads and memory. Check stable geometry/material normals, culling, shadow continuity, hysteresis and view-dependent error. Admit only a candidate that remains valid and yields measured complete-path gains. An exact transition stitch is a distinct gate from this fixed-boundary density test.

Transvoxel provides a documented 2:1 transition construction with its own regular case convention. Consuming those tables under the current MC33 case policy would require an explicit compatible construction and exhaustive proof of output agreement. [Author's transition algorithm and tables](https://transvoxel.org/).

To reproduce from a Release Engine build with testing enabled, run
`ctest -C Release -L terrain -R "terrainlodqualification|terrainvisualdensityrender" --output-on-failure`.
For the standalone geometry probe, compile the test with
`src/terrain/Surface.cpp`, `src/terrain/VisualDensity.cpp`, and
`src/terrain/VisualDensityOverlap.cpp`, using
`/std:c++latest /O2 /DNDEBUG /EHsc /W4 /WX /Iinclude /Isrc` under MSVC x64.
The `--profile-reference` mode prints the map reducer's phase timings.
`--compare-indexed` runs the 32³ guarded parity and timing comparison.

## Foundation 2 production-size experiment

Run the same executable with `--runtime-density` to test 32³ authored
chunks and up to four bounded interior passes. The retained endpoint of
each contraction carries a displacement bound for all its original
vertices. The next pass admits a contraction only when the accumulated
bound stays within 1.25 sample units. Border and material constraints are
rechecked on every pass.

The Windows Release run reduced 50,064 triangles to 39,638 across the 18
cases, a 20.83% aggregate reduction. Individual reductions ranged from
18.22% to 22.75%. All oriented seam, normal, material, and indexed topology
checks passed. Raw 2:1 still had 48–96 unmatched edges and lost the thin
cave component on each axis. The reduced candidate kept that component.
The reducer alone took 4.49–10.75 ms per chunk in this run.

A second fixture checked all 16 fine/reduced combinations across a
four-tile flat corner at negative coordinates. A deterministic view
sequence exercised hysteresis and immediate fine fallback when any
generation, geometry, material, or halo stamp changed or a candidate
was missing. These checks exercised the original CPU prototype.

## Campaign indexed reducer and native publication

Profiling the 18 production-size cases put 99.30 ms of 131.35 ms of
instrumented reference work in adjacency construction. Candidate ranking,
contraction checks and compaction accounted for 24.11, 2.43 and 5.50 ms
respectively. The reference uses a tree keyed by edge, with individually
allocated face fans and neighbor lists.

| Adjacency layout | Median reducer time per case | Speedup against its matched reference |
| --- | --- | --- |
| Reference map | 4.01–9.98 ms across the final comparison | 1× |
| Packed edge-use array with one global sort | 2.57–6.50 ms | 1.48–1.65× |
| Contiguous edge rows keyed by lower endpoint with local sorts | 0.50–1.68 ms | 4.59–8.25× |

Each case has seven alternated reference and indexed runs in an optimized
MSVC x64 build on Windows. The final layout uses packed vertex fans and
sorted neighbor rows. It intersects adjacent vertex rows linearly and
keeps ranking and contraction order deterministic. Both indexed layouts
produced identical vertex attribute values, triangle index order, collapse
counts and displacement bounds to the map reference in all 18 cases. The
final layout also matched all 16 mixed-density corner combinations and the
changed cave seam. Production spacing 0.5 and 2 gave the expected scaled
result on the flat control. Malformed, oversized, and canceled work preserve
prior output. Triangle savings remain 20.83% overall, and the raw 2:1
negative control still loses the thin cave. [Matched timings](../../tests/terrain/lod/TerrainVisualDensity.csv), [indexed implementation](../../src/terrain/VisualDensity.cpp).

The second native fixture routes the selected mesh through the existing
`RenderStage` and `RenderProjection`. It starts each timing from detached
state, alternates fine and reduced cases nine times, and measures CPU
publication plus prospective vertex and index upload bytes. The cold
measurement adds authored MC33 extraction from a prepared halo and, for
the reduced case, visual variant preparation. Generating or reading the
halo, queue latency, drawing, GPU transfer and shadows are outside these
measurements.

| Fixture | Cold fine → reduced, median ms | Prepared fine → reduced, median µs | Fine → reduced upload bytes |
| --- | ---: | ---: | ---: |
| Flat | 0.617 → 1.317 | 52.2 → 41.7 | 81,204 → 63,496 |
| Rolling | 0.625 → 2.170 | 69.8 → 57.7 | 108,072 → 89,224 |
| Thin cave | 0.616 → 1.695 | 59.1 → 46.1 | 91,140 → 71,380 |

Actual painted, moved-surface and outer-halo-only meshes force fine fallback
until a derivative of that exact immutable source exists. Fine source
ownership distinguishes a replacement even when its contents match the old
source. Native publication installed the new mesh and retired the previous
projection identity. A captured old render state remained immutable.
Collision conditioning used the same authored face indices throughout.
[Native publication probe](../../tests/terrain/lod/TerrainVisualDensityRender.cpp), [publication timings](../../tests/terrain/lod/TerrainVisualDensityRender.csv).

### Bounded prefetch trial

The private `VisualDensityPrefetch` consumes up to 16 unique current
committed visual interests in caller priority order. Each instance reserves
one shared `JobSystem` job. Completed variants occupy at most 4 MiB of
cached CPU mesh payload. Cache eviction follows changes to interest or fine
source identity. The one in-flight job separately retains its immutable
source and bounded simplification work. It holds no Field, provider, render
stage or Main-owner pointer. A stale job is canceled and its reservation
persists until the worker group completes. Selection continues to return
current fine coverage while a candidate is pending, absent or invalid.

The native fixture exercises priority, cancellation, oversized and duplicate
admission, cache eviction, owner retirement with work outstanding, and
coordinator precommit publication denial. A denied paint edit preserves the
field revision, exact committed fine source and cached visual variant.

The cold cost still rises even with smaller CPU publication and upload
payloads. Prefetch needs a view consumer with a stable interest/hysteresis
policy and frame-budgeted swaps, plus measured reuse and retirement during
camera motion and editing. Hard terrain also needs nonlocal overlap and
silhouette/cave clearance qualification. Actual driver work, draw savings,
shadows, memory across simultaneous views and render retirement remain
open. Terrain edits and provider admission continue to use authored sample
and collision resolution. The private experiment has no live camera caller.
Raw mixed scalar sampling remains gated by the separate MC33 transition
protocol. [Prefetch implementation](../../src/terrain/VisualDensityPrefetch.cpp).

### Nonincident triangle contact audit

The optional geometry probe mode `--audit-overlap` sweeps triangle
bounding boxes and checks triangle separating axes on surviving pairs.
It skips pairs that share a vertex and uses an absolute 1e−8 world-unit
projection tolerance. Crossing and coplanar overlap controls are
detected, and a coplanar separation control with overlapping bounds stays
separate. The 18 analytic authored/reduced pairs had zero detected
nonincident contacts. [Separating axis method, section 5](https://www.geometrictools.com/Documentation/MethodOfSeparatingAxes.pdf), [audit fixture](../../tests/terrain/lod/TerrainVisualOverlapAudit.hpp).

A separate synthetic witness adds a small detached triangle to the rolling
mesh. Its authored mesh has zero detected contacts, and the unguarded
reference introduces one. The inserted triangle was constructed directly
as mesh geometry. This establishes a limitation of the local contraction
guard and leaves the frequency of overlap on production MC33 fields
unmeasured.

### Bounded overlap guard

Each reduction pass carries forward whether a surviving face has ever
substituted an authored endpoint. Compaction retains that flag when vertex
indices are renumbered. The final candidate builds contiguous face lists
for two-sample bins in chunk space. Expanded bounds cover bin-plane
contacts. It checks each potentially overlapping pair with a changed face
once. Face pairs that share no vertices must have a separating axis.
Pairs that share an edge or a vertex may meet at their shared edge or
vertex. The guard tests incident triangle planes and inward directions or
the wedges around a shared vertex. It denies the derivative when it
cannot prove that contact stays on the shared geometry. The scan limits
cell memberships to 524,288 and candidate visits to 1,000,000. Cancellation,
limits, and overlap denial preserve the prior reducer output.

The guarded reducer preserved the reference's exact vertex and index
values in all 18 analytic cases. It passed all 16 corner combinations,
the changed cave seam, and 0.5 and 2.0 authored spacing controls. The
eligible output still contains 39,638 of 50,064 authored triangles
across these fixtures. Seven paired Release runs per case gave
1.889–3.963 ms median guarded preparation on Windows x64, or 2.06–2.81×
faster than the unguarded map reference from the same run. The guard
tested 6,909–13,815 nearby pairs per case. One-sample bins produced the
same geometry and pair counts with 1.978–4.114 ms medians. Two-sample bins
had the lower mean of the case medians, 2.828 vs 3.040 ms.
[Matched guard timings](../../tests/terrain/lod/TerrainVisualDensityGuard.csv), [guard implementation](../../src/terrain/VisualDensityOverlap.cpp).

Direct controls reject overlapping coplanar faces along a shared edge,
ambiguous nearly coplanar folds, overlapping vertex wedges, and a
noncoplanar crossing through a shared vertex. Safe incidence controls
retain faces. The synthetic detached triangle used by the independent
audit denies reduction. The worker fixture confirms the same source
produces a cached denial and native publication receives the exact fine
mesh. Its provider-denial, stale-source, and collision independence
checks still pass. Both the indexed parity and independent overlap audit
modes passed a standalone MSVC x64 AddressSanitizer run.

| Fixture | Guarded cold fine → reduced, median ms | Prepared fine → reduced, median µs | Fine → reduced upload bytes |
| --- | ---: | ---: | ---: |
| Flat | 0.587 → 2.660 | 49.3 → 40.7 | 81,204 → 63,496 |
| Rolling | 0.648 → 3.614 | 68.7 → 57.4 | 108,072 → 89,224 |
| Thin cave | 0.612 → 3.238 | 57.5 → 45.8 | 91,140 → 71,380 |

The cold timings include authored MC33 extraction, derivative preparation,
and native CPU publication. The guard checks final triangles and treats
the unchanged authored faces as its starting surface. Its local numerical
tolerances and fixture results qualify the private experiment. General
authored-scene overlap, screen-space silhouette and cave clearance,
camera reuse, and GPU performance remain acceptance gates. Authored
geometry and collision continue through the current runtime path.
[Guarded publication timings](../../tests/terrain/lod/TerrainVisualDensityGuardRender.csv).

### Headless perspective view sweep

The private `VisualDensityView::Advance` hook receives a perspective
camera, a monotonic view publication ID, and up to 16 visible chunks with
their immutable committed fine meshes. The selector binds to one
`RenderStage` and one authored grid. It publishes through the existing
render token and uses the published mesh identity to yield to later
authoritative edits or retirements. Passing no camera restores fine
coverage before disabling a live selector.

The pixel bound combines the reducer's maximum corresponding-point
displacement with the nearest possible camera-space depth of the chunk.
For focal length `F` in pixels, displacement `D`, lower depth bound `Z`,
and a bound `S` on the radial screen slope of source points, the estimate
is `F × D × sqrt(1 + S²) / Z`. The chunk sphere bounds both `Z` and
`S`. Corresponding points follow barycentric interpolation through the
accepted endpoint contractions. A chunk whose sphere reaches behind the
camera receives an infinite error. The sweep compares the bound to
projected lateral and depth displacements at all three supported authored
spacings. The mathematical estimate covers perspective displacement.
Projection clipping, raster coverage, lighting, and visibility need
separate qualification.

The selector prefetches at or below 2 pixels, enters reduced detail at or
below 1 pixel, and holds reduced detail up to 1.5 pixels. Work priority
follows smaller current projected error and stable chunk order. Repeated
camera updates coalesce into one set of at most 16 candidates and one
active simplification job. Ready variants can promote at most two draws
per successful advance. A closer camera, changed committed source,
leaving interest, or disabling the selector restores all affected fine
draws together before optional promotions resume.

Each promotion carries the fine draw's renderer admission bytes as a
floor. The native sweep fills the remaining admission of the 32 MiB
renderer profile with an independent draw, verifies that an overcommit
is denied, and restores all 16 original fine meshes at the same
admission limit. The actual reduced resource bytes are tracked
independently. Fine meshes retained for rollback and reduced CPU
variants each have separate 4 MiB limits. A denied render token leaves
the last complete state available for retry.

The native test sweeps 16 fine tiles at large signed coordinates and
checks pixel priority, hysteresis, frame-sliced replacement, changed
paint fallback, coalescing, and retirement. All changes are applied
through the native CPU projection. This fixture uses one perspective
view and exercises its CPU implementation and prospective buffer
payloads. In five Windows x64 Release runs the largest selector and
publication time per run ranged from 27.8–44.9 µs for a stable frame,
99.2–140.5 µs for a promotion frame, and 0.864–1.055 ms for a restoration
frame. The two-draw promotion reported 126,992 bytes of prospective
uploads. The 16-draw urgent restore reported 1,299,264 bytes. Peak charged
rollback and variant payloads were 1,021,376 and 797,696 bytes.
The same standalone native sweep passed under MSVC x64 AddressSanitizer.
[View sweep timings](../../tests/terrain/lod/TerrainVisualDensityView.csv).

A multi-view consumer must account for the finest requirement
of every simultaneous view. Screen-space feature visibility, actual
GPU transfer and draw performance, and shadow continuity remain
qualification gates. Cameras have no role in collision readiness or
field authority. [View hook](../../src/terrain/VisualDensityView.hpp), [native sweep](../../tests/terrain/lod/TerrainVisualDensityRender.cpp).
## Opt-in Engine camera and Vulkan validation

`EngineProviderConfiguration::ExperimentalTerrainVisualDensity` enables
the native camera path. The Engine considers resident fine sources in the
current view, requires each candidate sphere to stay within the clip
planes, and selects up to 16 interests by projected error. Collider
publication continues to use authored geometry. The renderer reserves
the admitted fine bytes while a derivative is active, allowing an
immediate return to fine coverage under the same admission cap.

The Windows offscreen Vulkan plateau used a 640×360 camera and two
directional shadow cascades. At the far viewpoint the optional mode
reduced full scene terrain triangles from 44,540 to 41,744 (6.3%).
Both modes submitted 35 steady draws per frame. Moving the camera close
restored 44,540 triangles in the next frame. The native ground hit stayed
at Y 9.25. Both 640×360 capture pairs matched pixel for pixel on this
small projected plateau.

| Counter | Fine | Mixed |
| --- | ---: | ---: |
| Engine step p50 / p95 | 212 / 239 µs | 211 / 237 µs |
| GPU completion wait p50 / p95 | 106 / 114 µs | 96 / 119 µs |
| Cumulative upload before approach | 2,364,840 B | 3,014,828 B |
| Terrain resource retirements by the near frame | 0 | 12 |

This run establishes a camera-driven switch, retirement and a native
rendered fallback. Its steady timing did not establish a frame time
advantage, and replacing already uploaded fine meshes increased cold
upload. The opt-in stays off by default. Large camera traversals, cave
variety, concurrent view budgets and streaming churn remain separate
acceptance cases.

Build `gargantuan_terrain_lod_offscreen` and run it with an evidence
directory. The executable uses SDL's offscreen Vulkan target and writes
raw captures and frame CSVs. The Windows receipt is under
`C:/Sandbox/Codex/Artifacts/TerrainFoundation2/LodOffscreen-20261127-013549`.

### Cave and overhang camera handoff

The second Windows Vulkan fixture builds a chamfered passage through a
three-by-three authored plateau with an intact floor and overhanging
roof. It presents one 640×360 view at a time, moving from the far front
to the near rear, retreating from the rear, and returning to the near
front. Each near capture is the first native frame after its handoff.
Fine and mixed runs use the same field, camera transforms, lighting and
aperture crop. The near views cover the opening and the roof silhouette.

| View | Fine triangles | Mixed triangles | Differing pixels in full viewport | Strong edges in aperture crop |
| --- | ---: | ---: | ---: | ---: |
| Front far | 71,160 | 66,124 | 0 | 118 |
| Rear near | 71,160 | 71,160 | 0 | 397 |
| Rear far | 71,160 | 68,642 | 0 | 134 |
| Front near | 71,160 | 71,160 | 0 | 273 |

All four image pairs also have matching SHA-256 hashes, including the
complete viewport outside the crop. The two far views reduced full-scene
terrain triangles by 7.1% and 3.5%, respectively. Both near views restored
the complete fine mesh immediately. The two terrain-only rays through the
passage register misses in both directions. Exact position, normal and
distance agree for the five hit rays at every checkpoint. The interior
ceiling hit stays at Y 20.75 with a downward normal, the floor at Y 2.25,
and the roof above at Y 26.25. The authored field revision remains
constant, and the two collision CSVs match byte for byte.

| First frame after handoff | Fine terrain upload payload | Mixed terrain upload payload | Mixed mesh creates and retirements | Engine step with capture, fine → mixed |
| --- | ---: | ---: | ---: | ---: |
| Rear near | 0 B | 1,162,384 B | 6 each | 2.093 → 10.199 ms |
| Front near | 0 B | 501,948 B | 3 each | 2.012 → 5.206 ms |

The terrain payload column counts the vertex and index bytes in each
native publication. The measured Engine step includes viewport readback,
so the single handoff observation does not isolate GPU transfer time.
Across the complete sweep the fine run published 2,814,528 terrain bytes
and the mixed run published 5,856,140. The extra replacement uploads
motivate a comparison with resident fine meshes and separately
budgeted pre-uploaded reduced meshes. The same two near frames can test
a draw-binding change with zero terrain mesh creates, retirements or
uploads. That comparison should account for the preceding prewarm bytes
and extra peak GPU residency.

This evidence covers one controlled passage and a sequential main-view
handoff. It leaves natural cave complexity, simultaneous camera policy,
live terrain edits, and production frame budgets open. The cave images
and raw comparison, frame and collision CSVs are under
`C:/Sandbox/Codex/Artifacts/TerrainFoundation2/LodOffscreen-20261127-020458`.
[Native cave fixture](../../tests/terrain/lod/TerrainLodOffscreen.cpp).
## Retained fine resources and measured camera cutover

The opt-in runtime can retain up to 16 exact fine/reduced pairs and
4 MiB of additional reduced payload. It admits up to two reduced
introductions and 256 KiB of reduced resource bytes in one frame.
Each optional mesh warms under its own identity. A subsequent camera
selection changes the draw binding. Returning to the original fine
identity uses the resident mesh. Authoritative edits and removed chunks
retire the superseded pair. Full resync publishes both exact identities
when space is available.

The Windows Vulkan cave repeat reported zero terrain creates,
retirements, and uploaded bytes on both first near frames. The previous
run had charged 1,162,384 and 501,948 bytes to those frames. The two
new near Engine steps took 1.48 and 1.46 ms in the capture harness.
Far triangle counts and four matched images remained byte-identical
to the earlier comparison. Collision receipts matched the fine
profile. The extra cached data peaked at 1,382,680 bytes, and optional
introductions peaked at 247,400 bytes per frame. The total cave sweep
published 4,191,808 terrain mesh bytes versus 5,856,140 before the
change. The fine-only sweep published 2,814,528 bytes.

The aggregate renderer byte limit still governs both incremental and
full publication. A focused Windows test exceeded that limit with
optional meshes, then accepted exact fine terrain and ordinary scene
changes together. It verified suppression of the duplicate rejected
state, later full-resync recovery, and the existing byte limit for
required scene data. The Vulkan comparison used sequential views
and capture-inclusive Engine times. GPU execution and performance
in many simultaneous views require further qualification.

The authored Workspace Boolean `TerrainVisualDensity` enables the
candidate on the next Engine startup. Default scenes use authored
fine detail. The warm-cache evidence is under
`C:/Sandbox/Codex/Artifacts/TerrainFoundation2/LodOffscreen-20261127-022004`.

The same authored setting and cave fixture passed on native Linux AMD
RADV with Mesa 25.2.8. Far counts matched the Windows results, the four
Linux capture pairs were pixel-identical, and both near frames rebound
the cached exact fine resources with zero creates, retires, and uploads.
The Linux run used the host render node inside an isolated container.
Vulkan validation layers were unavailable. The report, captures and CSVs
are under `C:/Sandbox/Codex/Artifacts/TerrainEngineLinuxGns/20261127-015113/vulkan`.

The Engine's Workspace publication currently owns one terrain draw list
and selects its detail from the main camera. Embedded viewport scenes
own independent scene projections. A shared list servicing the near and
far plateau views together would need the near view's 44,540 fine
triangles. Separate per-view selection could use 41,744 for the far
view while reusing the cached fine resource. Simultaneous timing
requires a real shared-Workspace multi-camera publication fixture.
