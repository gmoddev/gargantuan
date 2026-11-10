# Renderer Foundation 4 qualification

## Provenance

Foundation 4 is stacked on the qualified Engine Foundation 3 source
`81a5c5aff36e2149c3181ada6858d7143da19ea0` and Studio Foundation 3 source
`e4050a1bf55e0edfbb7790c7fe93fd912bc01179`. The original Windows package
was built from the Foundation 3 code checkpoint `cee6b3d22690a7cc81e4bf75b349307b091b6960`.
The comparison branch `experiment/renderer4-f3-raw-baseline` adds benchmark
output and packaging to the same implementation. It keeps the SDL renderer
and fixtures independent of Foundation 4. [Foundation 3 qualification](RendererFoundation3Qualification.md)

The campaign evidence root on the controller is
`C:/Users/aiden/.codex/artifacts/renderer-foundation4-20261110`. Every hosted
Windows package records its exact source and each executable, SDL library
and SPIR-V hash in `renderer-evidence/renderer-package.json`. Physical
worker results preserve that manifest, command lines, status, stdout,
stderr, adapter details and lease receipts. The hosted Linux package
keeps the software ICD device, SPIR-V validation and test output separate
from physical measurements.

## Observed controls

| Evidence | Result |
| --- | --- |
| Foundation 3 Windows Vulkan, GTX 1650 SUPER 4 GB, driver 591.86 | All 20 repeated scaling rows admitted their 180 measured frames at 1280 × 720. The 50K mixed row measured 49.38 ms P50, 54.18 ms P95 and 100,000 world material bindings per frame. `cee6b3d22690-sdl-matrix-20261110T115950Z` |
| Foundation 4 `39fb0087b6736f00b315743e42239db4fedf026f`, Radeon RX 7900 XT | Nine opaque ordering controls passed exact RGB comparison. Two-range separated cases used two bindings across the two objects. Coplanar, near-equal, intersecting and masked cases retained four. Both existing material charts passed color, normal, packed-channel and mask gates. `AMD-39fb-ordering-materials` |
| IBL resource probe `e9d18f299bcb9cb27aa91a07989593953fd6c208`, physical Windows Vulkan | GTX 1650 SUPER and RX 7900 XT measured rough red 0.265381/0.265137, directional diffuse red 0.285156/0.284912, opposite diffuse red 0/0 and BRDF sum 0.319622 on both. These are GPU readbacks of derived resources. `e9d18f299bcb-ibl-probe-20261110T115947Z` and `AMD-e9d18f-ibl-resource` |
| Integrated GPU IBL `4a712aa3d443461631ba64f8f2a7db0dd8bb8614`, RX 7900 XT | 1024px input retained the expected linear energy. The world chart reported red 231 for smooth metal, 89 for rough metal and 172 for dielectric. The cache hit test, depth controls and both material charts passed. `AMD-4a-integrated-ibl-ordering` |
| Studio `1e3c182c8663b6a33af3be4121e0a7949f5abed6` | Passed all 43 suites and Main on the pinned native LUI runtime. The earlier Engine `39fb0087b` pair passed advanced catalog opt-in, dependency closure, stable identity with source reimport and fresh reopen using real responses. `StudioLui1e3c182` and `StudioPaired-42811055231-Material2` |
| Engine `4a712aa3d` with Studio `1e3c182` | Real paired commands created a MeshPart, PointLight and attached SpotLight, rejected an inverted cone without changing revision, edited the environment and reopened persisted state. Hidden 96 × 72 EditorHost capture measured center RGB `(124,136,0)` with the green point light and `(124,0,0)` without. Re-enabling restored the first frame byte for byte. `StudioPaired-42812724820` |

Preserved superseded runs `42810240640`, `42811055231` and `42812724820`
failed the same HDR pixel fixture after passing their CPU suites. The test
reused a publication ID across exposure values, so the second capture
returned the previously accepted state. Candidate `a2d0a5e9f` gives each
exposure its own publication. Run `42814023742` also recorded an independent
sanitizer setup failure before CMake compiled project code. The runner lacked
the `libclang-rt-19-dev` runtime. A later source checkpoint installs it.

## Reproduction

Use `.github/workflows/renderer-foundation4.yml` to build the Release
gates and Linux ASan/UBSan gates. Download `renderer4-windows`, preserve
its relative directory layout and verify the manifest before executing
the packaged tests. Use the same SDL GPU driver and report its actual
physical adapter. Keep unrelated worker workloads outside the measured
reservation.

Run the following from the packaged `build-renderer` directory with the
SDL Vulkan driver selected. Every capture path must be fresh.

```text
gargantuan_editor_viewport_smoke --require-hdr
gargantuan_renderer_depth_tests --opaque-ordering --output=<directory>
gargantuan_ibl_gpu_tests
gargantuan_renderer_benchmark --incremental --backend=sdl --scenario=material-check --width=640 --height=360 --capture=<prefix>
gargantuan_renderer_benchmark --incremental --backend=sdl --scenario=material-extensions --width=640 --height=360 --capture=<prefix>
gargantuan_renderer_foundation2_gpu_benchmark --scenario=environment --frames=45 --warmup=10 --capture=<directory>
```

Use the last fixture runner for texture lifecycle, GUI, cloth, rubber,
topology replacement, rigs, palettes, animation shadows and the mixed
25K-rigid, 16K-cloth and 5K-UI workload. Its optional reference capture
replays the first publication and 16 deltas in a separate viewport after
the timed run. The first and updated PPM files share the same fixture and
camera across checkpoints. The replay excludes capture and readback cost
from runtime timing. The Studio paired fixture uses actual imported
material assets and authoring commands.

For scaling, use `--count=1000`, `10000`, `25000` and `50000`, and run
`static`, `mostly-static`, `dynamic`, `mixed` and `culled` at 1280 × 720
with 30 warmup and 180 measured frames. Use `--samples=<fresh-csv>` to
keep raw timings and `--synchronize=on` for one completed frame per sample.
Use `--sdl-hdr=off` for the Foundation 3 SDR comparison and
`--sdl-hdr=on` for the full candidate. The same-binary grouping control
uses `--sdl-opaque-grouping=off` with fixed HDR mode, repeats each condition
in alternating order, and uses `--synchronize=bounded-2` for paired
completion. CPU submission, explicit fence wait and end-to-end frame
latency are separate columns. Fence wait can include queued work, so it
cannot establish GPU-only execution time.

For local lights, run `--scenario=local-lights --count=1000` with
`--lights=1`, `10`, `100` and `500`, each with and without
`--static-lights`. The shadow timing fixture enables `--shadows=on` and
compares static, 1% dynamic, fully dynamic and mixed publications. These
fixtures drive the renderer boundary directly. Asset and world extraction
belong to the independent publication and native tests.

See [Renderer Foundation 4](../CurrentArchitecture/RendererFoundation4.md),
[opaque ordering](../CurrentArchitecture/RendererFoundation4Opaque.md),
[environment lighting](../CurrentArchitecture/EnvironmentLightingFoundation4.md)
and [directional depth reuse](../CurrentArchitecture/DirectionalShadowReuse.md)
for resource bounds and design decisions.
