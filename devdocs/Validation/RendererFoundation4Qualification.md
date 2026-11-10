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
| Studio spotlight and reference scenes | The attached spotlight changed center RGB from `(124,136,228)` to `(124,136,0)` when turned away. Restoring its aim reproduced the original pixels. Foundation 3 and 4 captured identical project and camera inputs for the FirstCompleteGame course, beacon close-up and an enclosed course derivative. The HDR candidate is brighter indoors under global Sky IBL, which has no spatial occlusion. `StudioPaired-42812724820-Lights` and `StudioMatchedReference-4a712aa3d-Final` |
| Engine `0b8ad43c8dfc0ddda46c21c560f195e21d699791`, RX 7900 XT | Viewport HDR, exposure, GUI isolation, mip, local light and shadow smoke passed. Opaque ordering, IBL readback and both material charts passed separately. `AMD-0b-full-gates` |
| Engine `8a6b90188` and `c32b47823`, Linux software Vulkan | Six headless CTest suites and all GPU gates passed in both Release builds. These include native viewport failure injection and recovery, opaque comparison images, integrated IBL, and both material charts. `Linux-42815115768` and `Linux-42816328777` |
| Engine `8a6b90188`, physical AMD and NVIDIA | Both passed the integrated HDR viewport, directional depth reuse, recovery, opaque order, IBL and material chart gates. For 1024px Sky input, preprocessing completion took 1.59 ms on AMD and 2.92 ms on NVIDIA, including submission and synchronization. Both GPUs reported material chart red 231, 89 and 172. `AMD-8a-full-gates` and `8a6b90188066-sdl-gates-20261110T124345Z` |
| Engine `c32b47823` and Studio `1e3c182` | Windows native Foundation CTest passed, including the two-delta Play regression. Native mailbox Play at 30 and 60 Hz and legacy Play capture passed with zero presentation failures. Edit pixels and authored project state were restored. Point, spot and live same-identity emissive reimport each reproduced the original frame byte for byte after restoration. `StudioPlay-42816328777`, `StudioPaired-42816328777` and `StudioMatchedReference-c32b47823` |

Preserved superseded runs `42810240640`, `42811055231` and `42812724820`
failed the same HDR pixel fixture after passing their CPU suites. The test
reused a publication ID across exposure values, so the second capture
returned the previously accepted state. Candidate `a2d0a5e9f` gives each
exposure its own publication. Run `42814023742` also recorded an independent
sanitizer setup failure before CMake compiled project code. The runner lacked
the `libclang-rt-19-dev` runtime. After installing it, the next sanitizer
run reached the SDL shared library link and failed on sanitizer symbols
because SDL enabled `--no-undefined`. The renderer workflow now uses the
same executable linker and SDL setting as the repository's native
sanitizer lane.

The Play viewport control on Engine `0b8ad43c8` reproduced five rejected
publications at 30 Hz, three at 60 Hz, and a failed legacy Play capture.
The mailbox nevertheless delivered later frames after resync. Therefore
frame delivery alone would pass a defective capture sequence. The corrected
path applies every queued publication before readback. The negative control
and input hashes are under `StudioPlayBeforeFix-0b`.

The first short SDR comparison on AMD used two runs of each source in
F3/F4/F4/F3 order. At 1K objects, 2,000 material bindings per frame fell to
four, but the F4 mixed workload paid 0.58 ms in mean resource application
versus 0.03 ms at the control. At 10K, the binds fell from 20,000 to 40 per
frame and resource application retained the same extra cost. The 30-frame
receipts are `AMD-4a-short-sdr-timing`. This measured a resource regression
even with HDR disabled and did not attribute fence wait to GPU execution.

An independent native probe repeated full and partial updates of 64²
textures and read back every base image and derived mip on AMD. The
existing path spent about 0.255 ms P50 creating one transient transfer,
under 0.001 ms mapping it and about 0.010 ms computing its mip chain. A
separate CPU heap scratch produced identical pixels over 90 frames and
added no clear speedup. The initial retained transfer probe observed stale
host readback after a fence. A second readback corrected both the new
download and the previously mapped bytes without changing the source
texture. The strengthened probe waited for device idle and took an
immutable CPU copy before comparison. That control and the retained
transfer candidate produced the same full 90-frame digest
`ca0591098d5c3c98`, with zero fallback.

The retained transfer cut cache apply P50 for the 64² full and partial
updates from 0.308 and 0.305 ms to 0.037 and 0.035 ms. Two-texture batch
apply P50 went from 0.541 to 0.311 ms. The selected path kept the existing
single command submission and retained 32 KiB for this workload. A split
base/derived submission also matched pixels and cost more. These are
isolated cache timings on physical AMD, with both hypotheses and raw
dispositions preserved in `material-transfer-probe`.

## Physical scaling checkpoint

The GTX 1650 SUPER ran 28 rows each for frozen baseline `32dce5218`
and candidate `8a6b90188`. Each row has 180 raw frame samples. The
analysis verified publication sequence, camera cull and frame admission
gates, identical logical input counts and summary percentiles.
`Scaling-32d-vs-8a.md` contains all rows. These selected results precede
the retained-transfer and dynamic-path fixes.

| Workload and presentation | Foundation 3 P95 | Foundation 4 P95 | World material binds/frame |
| --- | ---: | ---: | ---: |
| 1K mixed, SDR completion | 1.08 ms | 1.14 ms | 2,000 to 4 |
| 10K mixed, SDR completion | 7.81 ms | 6.54 ms | 20,000 to 40 |
| 25K mixed, SDR completion | 25.83 ms | 15.56 ms | 50,000 to 98 |
| 50K mixed, SDR completion | 53.96 ms | 41.88 ms | 100,000 to 196 |
| 50K mixed, Foundation 4 HDR | 54.20 ms | 40.17 ms | 100,000 to 196 |
| 50K dynamic, Foundation 4 HDR | 35.19 ms | 46.73 ms | 1 to 1 |

The baseline rows use direct SDR. Foundation 4 HDR includes the extra
target and resolve. Its SDR comparison still includes the newer material
shader and mip sampling. The large mixed improvement comes with the
same world draw count and a large decrease in bindings. The dynamic
regression is in part CPU work. Neutral projection and shadow classification
together rose from 11.08 to 16.71 ms mean at 50K, resource application
from 0.002 to 0.813 ms and submission from 13.46 to 15.81 ms.

The dynamic correction reuses sorted object validation scratch and keeps
validated entry addresses through apply. It validates duplicate IDs and
resource dependencies before mutation, including arbitrary publication
order. A focused MSVC Release 50K neutral apply probe moved from about
11.9 to 7.3 ms before full SDL qualification. The baseline probe was about
9.7 ms. Foundation 4's expanded material value is 216 bytes versus 120
in Foundation 3. The correction avoids per-update allocations and a second
hash lookup without changing publication shape. The renderer also skips
material demand scans with empty texture residency and skips previous
object lookups when a committed shadow map contains no casters.

The local-light matrix at 1K geometry reported 0.028 ms mean grid work
for one moving light and 0.150 ms for 500. The latter admitted 438 lights
and used about 120 KB of light uploads per measured frame. The equivalent
static views issued no light uploads after warmup. Frame P50 was 0.698 ms
moving and 0.563 ms static at 500 lights. The static 1K shadow case
reused all 180 measured depth maps and reduced P95 from 0.756 to 0.502 ms.
The moving caster cases rebuilt every measured map. These measurements are
in `8a6b90188066-sdl-lights-20261110T124629Z` and the two
`sdl-shadow-timing` receipts.

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
