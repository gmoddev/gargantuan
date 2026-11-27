# Volumetric terrain campaign ledger

The campaign continues on `foundation/engine-terrain-1` from clean
`88aafd4c3`. Merge and public distribution decisions remain outside this
feature-branch campaign. Water is excluded.

## Accepted baseline

| Checkpoint | Disposition | Evidence |
| --- | --- | --- |
| `a40c1d0f4` bounded offline terrain | Windows Engine, GAS transactions, package startup, Vulkan, native collision and gameplay qualified in the recorded profile. Native Linux sanitizers and wire parity qualified separately. | [Offline qualification](TerrainFoundation1.md) |
| `2ebf1ea06` initial multiplayer | Matching loopback Development packages with bounded immutable bootstrap qualified over real Windows GNS. | [Network bootstrap](TerrainNetworkBootstrap.md) |
| `88aafd4c3` mixed density | CPU experiment at 32³. The four-pass reducer reduced triangles by 20.83% and took 4.49–10.75 ms per chunk. | [LOD qualification](TerrainLodQualification.md) |
| Hidden chunk metadata | Matched archetype, sparse-set, hybrid and specialized SoA layouts. The qualified terrain path uses the specialized generation-safe active registry and a separate sparse field directory. | [Metadata qualification](TerrainMetadata.md) |
| `f44924c04` spatial spawn | Matching Windows GNS packages and the native session regression passed. The fixture transferred 24,578 of 74,190 bytes and gated movement on native collision. | [Spatial spawn evidence](TerrainNetworkBootstrap.md#spatial-spawn-windows) |
| `c36a488a7` Engine brush authoring | Windows GAS authoring suite passed with native picking, scoped revisions, pending work, exact one-level history and save/reopen. | [EditorHost protocol](../../docs/src/content/docs/developing/editor-host.mdx) |
| `9463d3336` client package startup | Matching packaged client and server passed readiness with client GTCH files removed. Full audit and offline initialization rejected that package. | [Windows network evidence](TerrainNetworkBootstrap.md#rolling-windows-and-live-edits) |
| Rolling windows and live fill | Windows native GNS passed delayed delivery, delayed acknowledgment, exact patch delivery, overlapping late join, distant cutover and fresh reconnect epoch. Packaged loopback startup also passed. | [Window decision](../../docs/adr/TerrainNetworkBootstrap.md) |
| Bilateral movement release | Windows packages and Windows/Linux native GNS passed superseded spawn collision, held regional receipt and confirmation, timeout, and a 16,412-byte reliable queue. | [Bilateral qualification](TerrainNetworkBootstrap.md#bilateral-movement-release) |
| Guarded visual density | 18 analytic scenarios retained 20.83% total triangle savings and matched seam, material, normal and topology checks. Windows and Linux geometry sanitizers passed. | [LOD qualification](TerrainLodQualification.md) |
| Adaptive height columns | Optional native Field policy passed exact sample, GTCH, MC33 and collision checks on Windows and Linux. The Release flat fixture used 77,952 bytes and the cavern fixture used 101,000. | [Storage qualification](TerrainHeightColumns.md) |
| Native view-driven detail | Windows Vulkan cut far-view plateau triangles 6.3% and cave triangles 3.5–7.1% across opposite views. Fine detail returned in the first near frame. Captures and collision matched. Cold uploads increased. | [Vulkan trial](TerrainLodQualification.md#opt-in-engine-camera-and-vulkan-validation) |
| Persisted project tint | Windows project and packaged startup verified actual imported asset bindings, unchanged canonical field bytes and native collision. Primitive PBR remains a comparison. | [Material qualification](TerrainMaterials.md) |
| Half-spacing possession | Windows and Linux GNS completed a signed handoff of a radius-9 anchored part. Cutover held old coverage until confirmation and fenced an oversized part. | [Actor envelope](TerrainNetworkBootstrap.md#actor-envelope-at-half-spacing) |
| Repeated multiplayer edits | Both native platforms passed 360 consecutive operations across two established peers. The distant player kept moving with zero terrain payload. The nearby replica peaked at four field pages and two native colliders. | [GNS churn qualification](TerrainNetworkBootstrap.md#repeated-edits-in-established-regions) |
| Editable quarry | Two concurrent Windows Development packages and a fresh reconnect traversed all three edits. Linux native packages also restored the three edits after a clean SQLite-backed restart. | [Playable sample](../../samples/TerrainQuarry/README.md) |
| Warm mixed visual detail | Windows Vulkan restored both cave views with draw-only changes and identical collision. Native tests cover cache eviction, stale source rejection, and fine fallback at the publication byte limit. | [Warm renderer evidence](TerrainLodQualification.md#retained-fine-resources-and-measured-camera-cutover) |
| Stored field and contour scale | The 96-page mostly inactive world reduced native admission from 332.7 to 33.0 ms. Columns used 53% less field memory on the 64-page active ridge and produced the same canonical closure. | [Scale matrix](TerrainRuntimeScale.md) |

## Active work

| Lane | Work package | Owner | Status and acceptance gate |
| --- | --- | --- | --- |
| A/C | Spatial streaming, authority, and gameplay readiness | Supervisor | Native GNS and the packaged quarry passed on Windows and Linux. The qualified network audience is private loopback Development. |
| B | Mixed visual detail | Comparative | Qualified opt-in on native Windows and Linux Vulkan for the plateau and sequential cave camera paths. Fine-only is the default. Simultaneous views and isolated GPU execution timing remain open. |
| D/E | Creator editing and Studio | Studio lane | Studio local branch `terrain/studio-authoring-20261127` at `ac5e08647` built with the exact pinned WinUI dependencies. Native-VM control logic, the exact Studio adapter, Engine save/reopen and GASP v2 passed. Foreground WinUI input and render frames remain open. |
| F | Storage and resource costs | Supervisor | Qualified paired native admission up to a 64-page active ridge, a 96-page sparse world with four active surfaces, and 512 stored positive pages. Columns remain an opt-in native field policy. |
| 6 | Material quality | Engine | Project tint is qualified for startup-resolved opaque materials. Primitive PBR is experimental. Texture projection, live refresh and physical material definitions are separate gates. |
| G | Independent assurance | Supervisor, with separate reviewer at each boundary | Review corrected superseded movement release, starvation at reliable limits, retirement, the renderer byte-cap fallback, and solid-page contour accounting. |

The column investigation treats compact columns as a storage representation.
The acceptance gate is exact signed density and material reconstruction. A
column or brick falls back to volumetric storage when the compact form
cannot preserve those values. Measure against compressed Gargantuan terrain
bricks, including transitions and the cost of rebuilding the compact form.

## Qualification infrastructure

- The normal-LAN `dockerbox` SSH address timed out on TCP 22 during
  reconciliation. The direct fiber route later reached the same
  authenticated HostPC and found the retained Studio runtime and frame DLLs.
  The worker lease governs sustained Windows work.
- The retained Windows GNS build uses cached dependencies and bounded,
  below-normal incremental work. Detached build sources are under
  `C:/Sandbox/Codex/Workspaces/TerrainFoundation1-Win`.
- WSL Ubuntu reports `HCS_E_SERVICE_NOT_AVAILABLE`. The LAN Linux host
  had 400 MiB free on its root volume. Its cached Ubuntu 24.04 container
  completed bounded GCC 13 AddressSanitizer and UndefinedBehaviorSanitizer
  replay for storage and guarded geometry. The receipt is under
  `C:/Sandbox/Codex/Artifacts/TerrainNativeLinux/20261127-011420`.
  The same cached image later built the Engine headless with GNS
  disabled. The native terrain runtime, GAS authoring and full local
  session suites passed. A follow-up built the exact GNS transport pin
  with extracted, verified Ubuntu dependencies. Native terrain runtime,
  both version-4 GNS queue profiles, and the ordinary headless packages
  passed. The Linux package run used pinned SQLite 3.54.1 and its verified
  Ubuntu OpenSSL and Protobuf shared libraries. The same worker later
  passed the offscreen cave and plateau fixtures on its AMD Vulkan render
  node with staged Ubuntu Mesa. The replays are under
  `C:/Sandbox/Codex/Artifacts/TerrainEngineLinux/20261127-012954` and
  `C:/Sandbox/Codex/Artifacts/TerrainEngineLinuxGns/20261127-015113`.
- The isolated height-column source root is
  `C:/Sandbox/Codex/Workspaces/TerrainHeightColumn`.

## Qualified sample and profile

Terrain Quarry is generated through the bounded EditorHost protocol and
the runtime uses public Luau. It includes a long outdoor strip, a cliff,
an overhang, a branching cave, and a dynamic body. The project generator
saves and reopens the authored field. The packaged route exercises the
same native authority and terrain streaming used during ordinary play.
On Windows two clients and a fresh session received phases 1/0, 2/4 and
3/5, recorded as phase/material. The three script traversals took about
7–8 seconds each in the local run. Linux checked the same route after
the authoritative process restarted. The sample's versioned progress
document replays the fixed authorized edits against the authored package.
The archived project stays the baseline.

The native Windows suite passed all 17 selected terrain, authoring,
renderer and packaging tests. Windows package evidence is under
`C:/Sandbox/Codex/Artifacts/TerrainFoundation2/QuarryPromotion-Windows-20261127`.
That directory includes three 768×432 native EditorHost captures and a
GASP v2 export of the project. Restoring it to a separate copy of the
project origin and exporting again reproduced the 14,516-byte GASP and
original manifest exactly. A separate authoring replay completed 95
brushes at 13.08 ms median, 15.71 ms p95 and 16.33 ms p99 per RPC plus
commit wait. The measured interval includes the script's receipt polling.
Linux package evidence is under
`C:/Sandbox/Codex/Artifacts/TerrainEngineLinuxGns/20261127-015113/package/quarry-aligned`.
The aligned Linux runtime, authoring, GNS and renderer publication tests
passed. SQLite reported a healthy database and document revision 3 after
the dedicated restart fixture.
The independently authored Windows and Linux projects emitted the same
30 GTCH leaves by SHA-256, totaling 192,183 bytes. Each project had its
own project-bound GTMF manifest. The leaf receipt is
`quarry-gtch-parity.json` beside the Windows package receipt.

## Promotion boundaries

| Capability | Disposition |
| --- | --- |
| Offline field, editing, save, GASP, physics, packaging | Qualified within authored spacings of 0.5, 1 and 2 and the recorded resource limits. Offscreen native rendering passed on Windows and Linux Vulkan. |
| Multiplayer startup, streaming, runtime edits, reconnect | Qualified for matching private loopback Development packages. A supported public or multihost package path still needs its own authentication and content audience qualification under Foundation 10A. |
| Mixed detail | Opt-in native candidate with exact fine fallback. Single-world camera paths passed on Windows and Linux. The fixture exercises opposing views in succession. Simultaneous views need separate evidence. |
| Studio | Exact pinned Windows x64 Host build, 507 HostResponse checks, native-VM terrain control logic, and exact Studio-to-Engine requests qualified. The supported UIA run requires a foreground input desktop. The authenticated remote shell ran in Session 0. |
| Materials | Startup project tint is qualified. Primitive PBR showed higher draw and upload costs. Triplanar textures, continuous blending, live asset refresh and physical material properties require focused trials. |
| Storage and address space | Hidden sparse registry and signed bricks retain field authority. The optional compact column backend meets exactness gates. Playable extent and concurrent native capacity are bounded by the tested provider profiles. |
| Procedural generation | The editable quarry and the original public Luau terrain sample are implemented and qualified. General creator heightmap import, scattering, path authoring, decals and virtual texturing remain separate proposals. |

The Linux worker completed the ordinary headless distribution and native
offscreen graphics qualifications. A pinned Studio interactive run,
broader graphics coverage, and any expanded package audience retain their
own promotion decisions.

The pinned Studio build and control receipt is under
`C:/Sandbox/Codex/Artifacts/TerrainFoundation2/StudioPinned-20261127`.
The exact Studio adapter saved and reopened all three spacings, restored a
half-spacing brush with a native ray at Y=9.4921875, and kept the project
material binding through reopen. The UI logic tests used the pinned native
LUI VM with mock control objects. The worker lease was released after the
owned processes exited.

## Comparative decisions

- Roblox exposes a volumetric Terrain with public occupancy and material
  channels on a four-stud lattice. Its Studio workflow integrates brushes,
  region work, and image-based height and color import. The relevant
  creator gap for Gargantuan is file import and a fuller palette workflow.
  [Terrain API](https://create.roblox.com/docs/reference/engine/classes/Terrain) ·
  [Studio terrain workflow](https://create.roblox.com/docs/parts/terrain)
- Unreal Landscape divides heightmaps into square components for
  rendering, visibility and collision. Its documented cave workflow
  combines a painted visibility opening with static mesh interiors.
  Gargantuan uses the same signed field for the outdoor surface and the
  editable cave interior. The column experiment preserves that contract.
  [Landscape technical guide](https://dev.epicgames.com/documentation/unreal-engine/landscape-technical-guide-in-unreal-engine) ·
  [Landscape holes](https://dev.epicgames.com/documentation/en-us/unreal-engine/landscape-visibility-tool-in-unreal-engine)
- Godot Voxel Tools documents editable signed fields, generator and stream
  resources, and data LOD with camera-centered block residency. It is a
  public reference for larger spatial scales and for tuning data and
  collider residency independently. Gargantuan's measured visual
  derivative shares the authored boundary and keeps fixed-resolution
  collision inside a confirmed region. [Voxel Tools overview](https://voxel-tools.readthedocs.io/en/latest/overview/) ·
  [VoxelLodTerrain profiles](https://voxel-tools.readthedocs.io/en/latest/api/VoxelLodTerrain/)

These comparisons describe documented behavior. All measured figures in
the ledger use matched Gargantuan fixtures. The height-column trial
compares against the existing compressed bricks.
