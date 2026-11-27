# Terrain Foundation 1 qualification

Baseline `ff5cf28f8089426251e761fb1ce532bf572a4698`, research `040ee80ba`.
Worktree `foundation/engine-terrain-1`. Foundation 11 is already integrated in
the baseline. Studio Foundation 12, package audience Foundation 10A, hosted
runtime and lifecycle qualification retain their own branches and evidence.

## Development checkpoint evidence

| Scope | Evidence |
| --- | --- |
| Field correctness | Focused Windows Release checks for exact negative sample ownership, sparse and dense brick use, sign/material semantics, capture validity, stale and disjoint commits, paint revisions, memory denial, cancellation/retirement, and brush stencil halos. |
| Terrain metadata architecture | Matched sparse-set, archetype, hybrid and packed active registry workloads. Lifecycle and stale-generation regression tests. [Receipt](TerrainMetadata.md) |
| Native mesher | 4,096 sign and magnitude cases plus retained matched fields, independent seam/vertex-link checks, winding and conditioning, and work/cancel denials. [Receipt](TerrainSurface.md) |
| Native collision | Pinned Box3D resource-limited rollback, shape and ray queries, adjacent chunk grounding, conditioned tunnel traversal and fill, and late-owner teardown. [Receipt](TerrainPhysics.md) |
| Native runtime pipeline | Windows Release fixture runs actual Field, JobSystem coordinator, conditioned Surface, Box3D stage and immutable RenderProjection for fill, paint and excavation. It verifies the collider is absent before commit, paint retains the body and changes its render swatch, and clearing retires both derived surfaces. [Fixture](../../tests/terrain/TerrainRuntimeTests.cpp) |
| Render projection | Scoped immutable binding tests accepted two chunks with one semantic owner and independently tagged scene mesh. Invalid removal retained state, valid retirement removed only terrain resources, and late token installation failed. Existing SDL pass extensions were syntax-checked in changed translation units. Actual terrain frame capture in a full Engine remains open. |
| Adversarial | MSVC address sanitizer coordinator test covers disjoint ordering, explicit expected revision, stage and field budget failure, cancellation, owner teardown and a modeled final provider failure. Native geometry and codec tests cover separately scoped malformed and adverse cases. |
| Canonical storage | GTCH and GTMF exact encoding and malformed-input checks. Windows in-memory Field reconstruction succeeds. Actual GAS save/load, authenticated CAS closure and cross-platform byte parity remain open. [Receipt](TerrainChunkCodec.md) |
| Engine and Studio | System source wires offline authoring through a typed gateway and includes a Luau sample and integration fixtures. Full linked Engine and EditorHost session qualification is pending. [Sample](../../samples/TerrainFoundation1/README.md) |

## Release measurements

Windows 11, Ryzen 9 7950X3D, MSVC 19.50 x64, `/O2 /DNDEBUG`, two JobSystem
workers and the Engine-patched pinned Box3D library. One below-normal-priority
headless process performed 25 recorded iterations per operation at a one-chunk
interior and a two-chunk seam. Two warmups preceded each sequence. The warm
sequence uses the same sphere, paint and subtraction workloads. Timings are
nearest rank, so p99 equals the maximum at this sample count.

| Fixture | Operation | Admission p50 / p95 µs | Readiness wall p50 / p95 / max ms | Final poll p50 / p95 / max µs |
| --- | --- | --- | --- | --- |
| Interior | Fill | 9.7 / 12.7 | 16.20 / 17.15 / 17.52 | 681 / 889 / 962 |
| Interior | Paint | 8.3 / 11.3 | 14.75 / 15.87 / 16.19 | 110 / 148 / 203 |
| Interior | Remove | 9.3 / 15.0 | 15.20 / 16.11 / 16.12 | 74 / 103 / 107 |
| Seam | Fill | 11.7 / 18.3 | 16.02 / 16.88 / 17.71 | 636 / 843 / 897 |
| Seam | Paint | 10.7 / 12.3 | 14.69 / 15.64 / 16.01 | 78 / 111 / 113 |
| Seam | Remove | 11.3 / 19.6 | 15.29 / 16.30 / 16.94 | 91 / 136 / 151 |

Readiness wall time includes polling sleep and OS scheduling. Final poll
includes provider cook, field commit and the CPU render stage. It excludes
render projection and actual draw. The interior fill peaks in the retained
observations were 40,860 B charged Field, 23,504 B CPU render state and
26,184 B resident Box3D mesh blobs. Peak Box3D build blob charge was 51,440 B
and includes overlap with staging. The seam recorded four native mesh
shapes. Total process memory, shaded frame time, provider shape allocations,
GPU allocation and retirement, gameplay under load, and network costs
require integrated measurements. [Raw Release samples](TerrainRuntimeRelease.csv)

## Windows integration continuation from `78c773342`

The local Release qualification uses the normal Engine CLI, a normal SDL
opaque/shadow renderer with an offscreen Vulkan target, the standard EditorHost
protocol, and the packaged Windows player. The original sample's plateau
exceeded the eight-chunk edit admission profile. The reduced sample reached its
collision-ready marker through ordinary gameplay Luau. A separate script
verified material replacement, smoothing, stale-revision rejection,
cancellation and excavation against the live provider. A stock
`KinematicCharacter` traveled from X=28 to 36.996 with eight grounded
movement observations and an upward floor normal through the X=32 seam.
After the supporting terrain was removed it descended from Y=10.599 to
8.599. The stock Character also stayed grounded through seven steps in
a narrow cave and reported ceiling contact with a downward normal. A
dynamic Part settled at Y=10.905 and fell to Y=9.883 after excavation.
A custom possessed KinematicCharacter stayed grounded through 14 rolling
terrain steps, then stopped against a newly filled cliff with normal X=-1.
A separately possessed ordinary Part used a terrain sphere cast to stop
short of a steep slope. [Gameplay fixtures](../../tests/terrain/gameplay).
Native provider tests separately cover provider rollback and the internal
shape partition.

The Windows package was built and validated both from a saved project and
from its GASP export. In both cases the actual packaged player confirmed
half-unit spacing, the saved sample material, a native terrain ray hit and
a successful gameplay paint. The GASP refers to the original project
closure. The package carries its own content-pinned closure. Both missing
and corrupted chunk fixtures made the actual packaged player exit with
package integrity failure before gameplay.
The [persistence receipt](TerrainPersistence.md) records 98,304 exact
sample comparisons, bounded native measurements and the incremental leaf
test.

The linked GAS fixture exercised Save, Save As, JSON reopen, GASP open and
restore, editor preview replacement, and saves while a real runtime paint
was pending and after it committed. Its six-source canonical closure was
111,036 bytes. In the final Windows run setup plus first save took 48.71 ms,
scene reopen took 11.64 ms and an unchanged save took 13.64 ms.

The initial RX 7900 XT Vulkan captures exposed sawtooth color boundaries
from per-triangle material selection. The corrected renderer interpolates
matching swatch tints at the vertices. Final captures show cleaner color
transitions, an island, cave mouth and interior, and the excavation before
and after. Coarse geometry is still visible where the two sample spheres
intersect. Full texture and material blending remain separate fidelity work.

| Final Vulkan fixture, 121 steady frames | p50 µs | p95 µs | Maximum µs |
| --- | ---: | ---: | ---: |
| Engine step wall | 180.8 | 295.0 | 626.5 |
| CPU render projection | 1.9 | 2.5 | 3.9 |
| CPU mesh transfer work | 0.2 | 0.3 | 0.4 |
| CPU submission | 57.3 | 77.8 | 115.3 |
| Completion fence wait | 88.4 | 197.1 | 529.5 |

The public excavation took 21.1 µs to submit. The harness observed a
completed visual fence after 7.61 ms and the public collision-ready
receipt after 8.20 ms. These are separate observation points. The run
recorded 11 terrain mesh creates, 9 retires, 6,482,736 bytes of aggregate
upload, 92 aggregate shadow draws, 99,389,440 bytes of peak working set
and 615,096,320 bytes of peak process commit with shadows enabled. The
completion fence measures host wait. Device execution timestamps were
unavailable. Large-world frame budgets remain unqualified.

Across a separate ordinary Luau pressure run, 24 disjoint fills and
retirements published their expected hits or misses. The fifth edit in
each burst was refused under the outstanding-work cap. Oversized region
and brush requests returned diagnostics before allocating their full
workloads. A cancellation probe retired its canceled ticket and committed
the three other disjoint edits. [Admission pressure fixture](../../tests/terrain/gameplay/AdmissionPressure.luau).

The independent review identified old-document loss on terrain admission
failure, an absent edit-viewport terrain projection, and a mismatch between
successful edit capacity and reload capacity. EditorHost now verifies the
terrain closure and prepares a bounded preview before a terrain-bearing
replacement, and edits apply the same materialized-source and contour
limits as startup. A distinct field lifetime invalidates old work. Global
schema replacement still owns cross-schema graph and catalog admission.
The review also found the risk of changing the identity of an existing
Save As destination. Save As validates lineage before changing sidecars
and stages new destinations before publishing the directory. Save
revalidates identity at promotion. Project and package preflight check
the complete contour footprint and refuse the 129 disjoint sources
whose 1,032 contour candidates exceed the 1,024 offline cap.

The final source snapshot passed the full Windows Foundation suite, ten
terrain CTests, and eight existing GAS, host, package and shader CTests.
The native Vulkan captures, frame CSVs and package launch receipts are
retained in the task-owned Windows qualification artifacts.

## Separate acceptance gates

- The native Linux codec and field suite ran with address, undefined-behavior
  and leak sanitizers. All four native wire fixtures matched Windows
  byte-for-byte by size and SHA-256. A full Linux Engine build, GAS disk
  qualification and native provider startup remain open.
  [Linux native qualification](TerrainWireParity.md).
- Mixed LOD stays at equal-resolution coverage. Raw 2:1 pairing missed
  boundary segments in every case and lost thin passages. Tested
  collar-locked simplification preserved the measured boundary but
  reduced triangles by 4.19–5.33%. Camera-driven transitions and
  a complete-path gain remain open.
  [LOD qualification](TerrainLodQualification.md).
- Terrain peer delivery, required collision acknowledgments, late join and
  reconnect still require matching client/server qualification. The
  [network handoff](../FutureArchitecture/TerrainFoundation2.md) defines the
  existing readiness boundary. Offline package startup is qualified separately.
- The [Studio handoff](../CurrentArchitecture/TerrainFoundation1.md#bounds-and-authoring-handoff)
  records the GASP v2 adapter change, preview and picking behavior, and the
  required revision-aware brush and undo/redo contracts.

Dockerbox remained unreachable and the installed WSL distribution could
not start its HCS VM. Windows qualification ran locally with bounded
below-normal build work. The Linux native probes ran in a bounded,
isolated container on the authorized LAN host.
