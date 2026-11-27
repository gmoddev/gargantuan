# Terrain material palette and render qualification

The private Foundation 6 candidate resolves a bounded snapshot of project Material assets in `TerrainMaterialPalette`. A binding pairs a nonzero `MaterialId` in the signed field with an existing stable `AssetId` from `AssetService`. IDs are looked up by binary search across sorted bindings. The snapshot retains canonical material content revisions and the linear base color, metallic, roughness, and emissive factors. Binding admission checks the exact asset and rejects unresolved, non-material, masked, transparent, zero, and duplicate bindings. Unbound field IDs use the existing built-in swatches.

The 64-entry profile bounds a single active render snapshot. Field IDs still span the existing nonzero 32-bit range. The project asset catalog retains its existing limits. `RenderStage(Owner)` continues to use the existing swatches when no resolved palette is supplied. Primitive PBR remains a private native comparison mode.

| Path | Draws per chunk | Material behavior |
| --- | ---: | --- |
| Tint | 1 | Existing linear vertex tint and terrain roughness. |
| Project tint | 1 | The project asset's linear base color is quantized into the existing RGBA8 vertex tint. Mixed triangles interpolate as before. |
| Primitive PBR | 1 to 5 | Triangles whose three vertices agree on a selected ID use the asset's native PBR factors. Mixed, unmapped, and excess triangles share the tint range. |

Primitive PBR selects at most four mapped IDs per chunk in ascending stable-ID order. It emits one contiguous index range per selected material and at most one fallback range. It preserves positions, normals, triangle winding, and triangle count. Only a vertex shared between a mapped range and the fallback range is duplicated, so each source vertex has at most one duplicate. Base-color values at a transition agree up to RGBA8 quantization. The PBR factors on the two sides can still diverge. Render-state bytes include vertex and index vector capacities, primitive records, and per-chunk state. The shared 32 MiB admission budget retains the larger of that payload and each requested fine-coverage byte floor.

## Project tint setting

Set the ordinary serialized Workspace string attribute `TerrainMaterialBindings` to a JSON array. Each row contains a nonzero unsigned 32-bit `Id` and a canonical `Asset` identifier of 32 lowercase hexadecimal digits.

```json
[{"Id":1,"Asset":"00000000000000160000000000000002"}]
```

The startup resolver reads the attribute once and resolves the project Material assets into one immutable tint palette. Omit the attribute, use an empty string, or use `[]` for the built-in tint. Invalid rows, duplicate IDs across rows, missing canonical content, incompatible asset kinds, and masked or transparent materials fail the whole palette. A source reimport can retain its previously verified canonical revision through the existing asset availability rules. Changing the attribute or asset during play affects a later startup.

The attribute shares the normal 4096-byte encoded WireValue limit. Compact JSON with 60 distinct ten-digit IDs uses 4049 encoded bytes. Sixty-one ten-digit IDs require 4116 bytes. Shorter IDs allow the full 64-row palette profile within the same limit. Apply compact formatting for large binding tables. The asset catalog retains the canonical materials and dependencies. The setting adds no scene attribute type, GTCH data, or physical material binding.

The Windows project integration imported a real glTF material, saved
its stable ID binding in Workspace, reopened the scene, and checked
the native terrain vertex tint. The canonical terrain manifest digest
was identical before and after the palette edit. A matching packaged
player resolved the palette and completed its terrain collision and
editing fixture. The packaged receipt is under
`C:/Sandbox/Codex/Artifacts/TerrainFoundation2/Palette-20261127-014743`.

The Studio branch `terrain/studio-authoring-20261127` at `ac43f7b91`
adds a Terrain materials panel. It uses the trusted snapshot to address
the canonical Workspace and the revision-checked `SetAttribute` command.
The Windows cross-process replay generated requests with the Studio
modules. It proved conflict rejection, Undo and Redo, exact terrain brush
history across save, restored bindings for IDs 5 and 7 after restarting
EditorHost, and successful GASP v2 export. Its receipt is under
`C:/Sandbox/Codex/Artifacts/TerrainFoundation2/StudioStory-20261127-020220-258391`.
The initial headless LUI smoke used a retained alternate runtime. The
subsequent exact pinned WinUI Host build and headless native-VM material
control and adapter checks also passed. Foreground input and rendered
Studio frames remain open. The pinned evidence is under
`C:/Sandbox/Codex/Artifacts/TerrainFoundation2/StudioPinned-20261127`.

## Native fixture

The existing `gargantuan_terrainrendertests` target checks Workspace attribute admission, empty defaults, malformed bindings, the full ID range, the WireValue byte limit, immutable resolution, exact topology through range splitting, range bounds, render projection, paint publication locality, and render admission rollback. It creates its canonical assets through `EncodeAssetArtifact` and loads them through the runtime `AssetService` catalog path. The fixture includes a real canonical image on one project Material. The terrain modes in this probe use material factors and emit zero texture bindings.

Run `gargantuan_terrainrendertests --material-probe <existing-output-directory>` from a build with native shader resources beside the executable. The optional headless fixture extracts the same MC33 mesh for all three modes. One scene is a rolling surface and the other is a lit cave tunnel. Each spans the negative-coordinate seam between two 32³ chunks at 1 m spacing. The native SDL renderer runs offscreen with the Vulkan driver and waits on completion. `EditorViewportRenderer` captures the same publication at 704×396 pixels.

Windows Release qualification used MSVC 19.50.35728 and an AMD Radeon RX 7900 XT with Vulkan driver 26.10.1. The material and test units were rebuilt at this source revision, then linked against the retained Engine objects. Vulkan validation layers were unavailable on the test host. Each row uses 15 cold CPU stage/publication/projection repetitions, one cold mesh upload, and 15 resident native draws. Recorded draw calls include renderer passes. Host wall clocks and fence waits do not measure isolated GPU execution time.

| Scene | Path | Primitive ranges | Render-state bytes | Projection upload bytes | Native upload bytes | Native draw calls | CPU p50 (µs) | Resident frame p50 (µs) |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Rolling | Tint | 2 | 224,964 | 223,796 | 295,628 | 7 | 92.8 | 151.8 |
| Rolling | Project tint | 2 | 224,964 | 223,796 | 295,628 | 7 | 93.1 | 161.4 |
| Rolling | Primitive PBR | 6 | 251,736 | 249,224 | 332,792 | 17 | 139.4 | 149.9 |
| Cave | Tint | 2 | 451,736 | 450,568 | 623,392 | 8 | 182.5 | 203.3 |
| Cave | Project tint | 2 | 451,736 | 450,568 | 623,392 | 8 | 183.4 | 194.5 |
| Cave | Primitive PBR | 6 | 516,000 | 513,488 | 715,352 | 20 | 279.8 | 191.8 |

Project tint changed the visible palette at the same draw, upload, and state sizes. Primitive PBR raised projection upload by 11.4% on rolling terrain and 14.0% in the cave, and it increased draw calls in both fixtures. Its specular response visibly followed the authored metallic and roughness factors. Both project modes expose a cell-shaped paint edge under close inspection. The primitive transition adds strong, jagged highlight boundaries at that edge, particularly under the cave's point lights. The small resident frame sample does not establish a frame-time gain. Keep primitive ranges as the comparison and project tint as the lower-cost color candidate.

The native readbacks, timing CSV, and execution log are under `C:/Sandbox/Codex/Artifacts/TerrainMaterialProbe/Windows-Release`. The six captures are `Rolling-Tint`, `Rolling-ProjectTint`, `Rolling-PrimitivePbr`, `Cave-Tint`, `Cave-ProjectTint`, and `Cave-PrimitivePbr` as PPM and PNG. The linked CPU test also passes with no SDL video initialization.

## Remaining integration

- **Live catalog and authoring.** The startup resolver uses a static palette for each runtime. Import and reimport events could refresh a visual binding revision while retaining the field revision. That needs a reverse index from referenced asset to visible chunks and bounded republish work. The Studio authoring panel applies saved changes on a later Play or package startup.
- **Paint and physics.** Material-ID paint retains the existing field semantics. The coordinator can refresh material samples on unchanged MC33 positions, and `PhysicsStage` preserves the existing body for an unchanged geometry candidate. The renderer republishes the affected mesh payload, and unrelated chunks retain their mesh identities. Project visual properties are independent of provider contact properties and public terrain hit ownership. Adding friction, restitution, or sound selection needs a separate gameplay binding and provider capability qualification. Current terrain bodies use the default provider properties.
- **Texture coordinates and normal maps.** The SDL opaque renderer supports UV and tangent based base-color, normal, metallic-roughness, emissive, and occlusion textures. Current MC33 render vertices carry placeholder UVs and tangents. Planar XZ coordinates collapse on vertical walls and introduce orientation and repetition problems in caves. Dominant-axis world projection adds a sharp switch as the surface normal changes. A bounded terrain shader probe can test world-space triplanar projection with signed world phase, a per-material scale in world units, and axis-reoriented normals. Use the same phase on both sides of a chunk seam and preserve an anchor precise enough at far signed coordinates. For two blended materials, naive triplanar base-color, normal and packed metallic-roughness consume up to 18 texture samples per fragment before extras. Qualify both a one-material path and the two-material blend on actual GPU hardware.
- **Material blending and LOD.** Existing MC33 vertices carry one ID sampled from the field. A future continuous blend needs deterministic weights from a fixed canonical neighborhood, including the halo on both sides of a chunk seam. Top-two material weights are a bounded hypothesis. Test loss of narrow paint bands, three-way intersections, and geometry and paint edits near an active LOD boundary. The current probe covers a same-resolution signed chunk seam and cave lighting. It does not exercise mixed terrain LOD, large-world texture phase, or cross-renderer behavior.
- **Texture residency and repetition.** Start with shared catalog assets under explicit active texture bytes and upload budgets. Arrays can reduce binding changes when layer formats and mip layouts agree. Atlases add gutter and mip-bleed constraints. Bindless needs a capability and descriptor-lifetime fallback for each provider. Virtual textures add page residency, feedback, and material fallback complexity that these fixtures cannot justify. Measure far repetition and close cave-wall detail with normalized world scale first, then consider macro color variation or stochastic tiling with derivative and normal checks.
