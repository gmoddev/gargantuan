---
status: decision
owner: rendering
last_verified: 2026-08-15
related_code:
  - include/gargantuan/render/Renderer.hpp
  - include/gargantuan/render/SDLRenderer.hpp
  - include/gargantuan/render/FilamentRenderer.hpp
  - include/gargantuan/render/RenderSnapshot.hpp
  - include/gargantuan/render/RenderExtractor.hpp
  - include/gargantuan/render/RenderProjection.hpp
  - src/render
  - src/editor/EditorViewport.cpp
  - tests/RendererBenchmark.cpp
  - tests/FilamentRendererTests.cpp
related_adrs: []
---

# Renderer backend evaluation

> **Superseded by Foundation 2C (2026-08-22):** equivalent deformable, GUI,
> texture, and mixed workloads on the incremental publication path resolve the
> backend checkpoint as **Decision A — keep SDL GPU**. See
> [Renderer Foundation 2C](RendererFoundation2C.md). The Foundation 1 results
> below remain historical evidence about the former full-snapshot path.

> **Foundation 2 update (2026-08-22):** this document records the Foundation 1
> full-snapshot experiment. The accepted next decision is **C: retain SDL
> temporarily, implement immutable incremental publication first, and defer a
> backend migration until SDL and any alternative can run equivalent rigid,
> deformable, GUI, and mixed GPU workloads.** Foundation 2B now drives the
> supported SDL runtime and EditorViewport, and its semantic dirty accumulator
> publishes 50,000-object dynamic frames without journal-overrun resync. See
> [Renderer Foundation 2](RendererFoundation2.md). The
> original evidence below remains historical input and is not a current order
> to keep SDL permanently.

## Decision

**KEEP SDL CUSTOM RENDERER**

Filament v1.71.5 was provisioned, compiled, integrated behind the existing renderer seam, and measured against the current SDL-GPU renderer. It preserves Gargantuan's ownership boundaries and proves that a mature persistent renderer can make the GPU cost of 50,000 identical Blocks small. It does not improve the end-to-end CPU path for Gargantuan's current publication model.

In optimized Release measurements, the decisive 50,000-Block mostly-static workload took:

```text
engine extraction                         14.50 ms
Filament full-snapshot projection scan     7.94 ms
Filament 1% changed-object application     0.08 ms
Filament render submission                13.40 ms
Filament backend total                    24.46 ms
Filament reported GPU frame           1.20-1.34 ms

engine extraction through SDL             15.35 ms
SDL backend submission                     6.72 ms
SDL GPU frame                                  NA
```

Filament's private persistent objects make the actual 1% update cheap, but the required O(N) scan and Filament submission make its backend CPU time 3.64 times SDL's measured submission time. The same result holds for static and fully dynamic 50,000-Block scenes: Filament backend CPU time was 4.26 and 4.62 times SDL, respectively. This is not an integration-boundary failure and does not justify an RHI or renderer redesign. It is a workload/result mismatch under the current immutable full-publication contract.

Keep the Filament adapter experimental and opt-in. Do not select it from the runtime, expose it to Luau or EditorHost consumers, vendor the SDK, or replace SDL. A later incremental render-publication milestone is justified by the 7.94 ms scan versus 0.08 ms changed-object application result, but that work is deliberately outside this task and would need to benefit the supported renderer or another adopted persistent backend.

## Preserved architecture and invariants

The runtime path remains:

```text
Authoritative DataModel
    -> RenderExtractor::Extract
    -> shared_ptr<const RenderSnapshot>
    -> backend-neutral BaseRenderer
    -> supported SDLRenderer or experimental FilamentRenderer
```

`RenderExtractor` remains the only renderer-path layer that traverses mutable engine state. `RenderSnapshot` contains copied values: snapshot identity, viewport, camera, light, `RenderItem` values, and diagnostics. No `Instance*`, `WorldRoot*`, `Camera*`, mutable runtime container, registry, or engine owner enters either backend.

`BaseRenderer` remains the narrow engine-facing seam:

```cpp
virtual void Draw(RenderSnapshotPtr Snapshot) = 0;
virtual void Resize(int WidthValue, int HeightValue) = 0;
virtual void Destroy() = 0;
virtual std::pair<std::uint32_t, std::uint32_t> GetViewportSize() const = 0;
```

SDL declarations remain in `SDLRenderer.hpp`. Filament declarations are isolated in `FilamentRenderer.hpp`, whose PImpl prevents any Filament type from entering the public engine contract. Backend entities, materials, meshes, transforms, lights, views, and swapchains are disposable renderer projections. `ObjectId` remains the only identity carried across the seam.

The supported SDL behavior is unchanged apart from the previously recorded header isolation and lifetime/error hardening. Backend selection was not added.

## Exact Filament dependency

The experiment used the official [Filament v1.71.5 release](https://github.com/google/filament/releases/tag/v1.71.5), released 2026-05-27 from commit `b0f2090`.

| Property | Evidence |
| --- | --- |
| Archive | `filament-v1.71.5-windows.tgz` from the official GitHub release |
| Archive size | 768,447,764 bytes |
| SHA-256 | `9bc1f145f26a628d1f61f5e0627263de0ec937b565e46927f011c280e6cfc53f` |
| Extracted SDK size | 4,581,011,170 bytes |
| License | [Apache License 2.0](https://github.com/google/filament/blob/v1.71.5/LICENSE), plus packaged third-party notices |
| Host/target used | Windows x64, MSVC, Vulkan |
| Compiler ABI found necessary | MSVC 19.50 / toolset 14.50 for the official v1.71.5 Windows libraries |
| Material tool | matching archive's `bin/matc.exe`, 7,619,072 bytes; material format version output `71` |
| Linked static library inputs | 110,319,342 bytes across `filament`, `backend`, `filaflat`, `filabridge`, `utils`, `zstd`, `bluegl`, `bluevk`, and `smol-v` Release libraries |
| Release benchmark executable | 6,907,904 bytes with Filament versus 2,918,912 bytes without it; the comparison used MSVC 19.50 and 19.40 respectively, so treat the approximately 4.0 MB increase as indicative |

The archive was downloaded and extracted only under the local temporary directory. No SDK content, generated material package, benchmark output, or vendor revision was added to Gargantuan.

The official Windows libraries failed to link against the existing MSVC 19.40 cache because their standard-library ABI referenced newer symbols. The experimental CMake path now fails early unless it is a 64-bit Windows MSVC build with `MSVC_VERSION >= 1950`. The current adapter is intentionally Windows-only; Filament itself supports Windows, Linux, macOS, iOS, Android, and web-oriented builds through its platform-specific artifacts and [documented build process](https://github.com/google/filament/blob/v1.71.5/BUILDING.md). A production adoption would need pinned per-platform SDK artifacts or reproducible source builds rather than this one Windows archive.

`matc` is a build-time dependency. It compiles `assets/filament/benchmark.mat` into a generated package header. It is not loaded or invoked at runtime. The material package is statically embedded in the executable. Runtime deployment in this experiment uses static Filament libraries and needs no Filament DLL; Gargantuan's existing SDL runtime dependency remains.

Recommended production arrangement, if revisited: pin exact release URLs and hashes in a dependency bootstrap/cache outside the repository, retain license and notice files in packaging, make host `matc` and target libraries explicit build inputs, and build a minimal redistributable closure. Do not commit the multi-gigabyte SDK.

## Minimum experimental backend

The implementation contains only the functionality needed for the comparison:

- Vulkan `Engine`, `Renderer`, Win32 swapchain, `View`, `Scene`, and camera initialization;
- a headless swapchain path used by smoke and lifecycle tests;
- viewport resize and headless swapchain recreation;
- complete immutable `RenderSnapshot` validation and consumption;
- a private `unordered_map<ObjectId, Entry>` persistent projection;
- exact generational-ID create, update, remove, and unchanged classification;
- generated Block, Sphere, and Cylinder vertex/index buffers;
- model transforms, shared exact-color material instances, directional lighting, and optional shadows;
- enabled view frustum culling and opt-in Filament automatic instancing;
- deterministic, idempotent teardown with `Engine` destroyed last.

Wedge and CornerWedge intentionally fall back to Block in this experiment. Transparency, imported meshes, textures, the Gargantuan material system, GUI, terrain, particles, post-processing design, render graphs, generic RHI work, scripting APIs, and Studio GPU sharing were not implemented.

The color cache shares one material instance for equal RGBA values. This preserves the benchmark's identical solid-color semantics and allows Filament's automatic instancing path. It is not a complete transparency implementation. Filament reported automatic instancing enabled with a maximum automatic group size of 64 on the measured device.

## Benchmark methodology

### Hardware and builds

- CPU: AMD Ryzen 9 7950X3D, 16 cores / 32 logical processors.
- GPU: AMD Radeon RX 7900 XT.
- Filament driver report: AMD proprietary driver 26.7.1 (LLPC), Vulkan API 1.4.
- OS: Windows build 26200.
- source revision: `3d0b29b` plus the working renderer-spike changes recorded here.
- Release: MSVC 19.50, CMake Release configuration, 30 measured frames after 5 warm-up frames.
- Debug: MSVC 19.50, 10 measured frames after 2 warm-up frames.

Every windowed backend used the same SDL3 resizable, maximized, high-DPI window flags and its queried drawable size, 2560 by 1417 pixels in the Filament runs. Both received the same copied camera, light direction, transforms, colors, object counts, and viewport. Shadows were disabled on both. The primary Block workloads therefore compare equivalent basic opaque scene data.

Filament's material is standard lit opaque color while SDL uses Gargantuan's simple directional-light shader. They are semantically comparable but not pixel-identical. Filament used D32 depth while SDL uses its existing D16 window depth target. The mixed workload is not visually identical: Filament renders generated Sphere and Cylinder meshes while SDL currently substitutes Blocks for those logical shapes. The mixed result is reported as an integration/stress result, not an exact triangle-work comparison.

The fixture directly populates `WorldRoot::Parts` after normal object registration so the test isolates extraction from physics-body construction. Extraction still performs the real traversal, registry/type checks, value validation, matrix work, value copying, and sort. Every backend receives a newly published full snapshot every frame.

The reported phases are:

- **extraction:** authoritative world to immutable value snapshot;
- **projection scan:** complete Filament snapshot validation, lookup, comparison, and removal discovery;
- **changed apply:** Filament entity/material/geometry/transform changes only;
- **render submit:** Filament `beginFrame`, view render-command generation, and `endFrame`;
- **backend total:** outer `Draw` call, including the three Filament phases and small measurement overhead, or the complete SDL `Draw` call;
- **GPU:** valid completed Filament `Renderer::FrameInfo::gpuFrameDuration` samples after a flush.

SDL exposes no timestamp-query result in the current path, so SDL GPU time is `NA`. Neither backend exposes trustworthy renderer-owned VRAM bytes through the integrated public APIs, so memory/VRAM is `NA`. Filament's public frame information does not expose actual draw calls; SDL's reported count is nominal command-recording draws. No unavailable value is inferred.

The first Release run supplied the CPU and GPU means in the table below. A second identical run added an explicit GPU-sample-count field: completed history contained 14 valid GPU samples for Static 50,000 (1.213 ms), Dynamic 10,000 (0.307 ms), Dynamic 50,000 (1.310 ms), Mostly-static 50,000 (1.197 ms), and Mixed 50,000 (3.195 ms); the lower-count static/dynamic cases returned zero valid samples. Filament also logged frame-history queue pressure because the benchmark deliberately submits every publication even when `beginFrame` recommends a pacing skip. GPU means are therefore completed-sample evidence, not a claim that all 30 measured publications had timestamp data.

Both backends published all requested objects. The layout and camera place the Block workloads inside the view, but an actual post-cull visible count was not exposed and remains `NA`. Filament enables frustum culling, sorting, persistent scene state, and automatic instancing. SDL currently performs none of those operations and records one draw per published item per enabled pass.

### Reproduction

Configure the external SDK without adding it to the repository:

```powershell
cmake -S . -B build-release-filament -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DGARGANTUAN_FILAMENT_ROOT=C:/path/to/filament-v1.71.5-windows
cmake --build build-release-filament --target gargantuan_renderer_benchmark
```

The normal project still requires its existing `glslc` configuration. The matching `matc` is discovered only through `GARGANTUAN_FILAMENT_ROOT`.

Representative runs:

```powershell
build-release-filament/gargantuan_renderer_benchmark.exe --backend=sdl --scenario=all --frames=30 --warmup=5 --shadows=off
build-release-filament/gargantuan_renderer_benchmark.exe --backend=filament --scenario=all --frames=30 --warmup=5 --shadows=off
build-release-filament/gargantuan_renderer_benchmark.exe --backend=projection --scenario=all --frames=30 --warmup=5 --shadows=off
build-release-filament/gargantuan_renderer_benchmark.exe --backend=filament-headless --scenario=static --count=1000 --frames=3 --warmup=2 --shadows=off
```

## Release results

All times are means in milliseconds. `Fil total P95` and `SDL P95` are included because presentation/queue scheduling makes some low-count CPU submission results noisy.

| Scenario | N | SDL extract | SDL backend | SDL P95 | Fil extract | Fil scan | Fil apply | Fil render | Fil backend | Fil total P95 | Fil GPU | SDL nominal draws |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Static | 1,000 | 0.231 | 8.781 | 20.282 | 0.129 | 0.058 | 0.001 | 0.277 | 0.348 | 0.453 | NA | 1,000 |
| Static | 10,000 | 2.787 | 13.817 | 18.168 | 2.605 | 1.120 | 0.002 | 2.205 | 3.418 | 4.542 | NA | 10,000 |
| Static | 50,000 | 14.077 | 5.257 | 5.498 | 13.456 | 7.109 | 0.004 | 12.477 | 22.401 | 24.961 | 1.288 | 50,000 |
| Dynamic | 1,000 | 0.234 | 15.548 | 19.987 | 0.156 | 0.049 | 0.047 | 0.285 | 0.393 | 0.462 | NA | 1,000 |
| Dynamic | 10,000 | 2.605 | 5.812 | 11.483 | 2.787 | 1.457 | 1.154 | 2.650 | 5.446 | 6.276 | 0.060 | 10,000 |
| Dynamic | 50,000 | 15.267 | 6.599 | 9.197 | 14.201 | 7.453 | 6.246 | 13.706 | 30.493 | 37.065 | 1.334 | 50,000 |
| Mostly static | 50,000 | 15.350 | 6.715 | 7.834 | 14.496 | 7.942 | 0.078 | 13.404 | 24.462 | 29.790 | 1.336 | 50,000 |
| Mixed | 50,000 | 14.666 | 8.508 | 12.278 | 17.902 | 7.914 | 0.004 | 20.183 | 29.097 | 32.266 | 2.904 | 50,000 |

The standalone value-only `RenderProjection` confirms the complete-publication cost independently of Filament:

| Scenario | N | Extraction | Reconciliation | Result across 30 measured frames |
| --- | ---: | ---: | ---: | --- |
| Static | 1,000 | 0.122 | 0.064 | 30,000 unchanged |
| Static | 10,000 | 1.793 | 0.724 | 300,000 unchanged |
| Static | 50,000 | 13.474 | 6.473 | 1,500,000 unchanged |
| Dynamic | 1,000 | 0.126 | 0.046 | 30,000 updated |
| Dynamic | 10,000 | 1.745 | 0.639 | 300,000 updated |
| Dynamic | 50,000 | 13.034 | 4.823 | 1,500,000 updated |
| Mostly static | 50,000 | 15.135 | 7.607 | 15,000 updated; 1,485,000 unchanged |
| Mixed | 50,000 | 14.714 | 7.271 | 1,500,000 unchanged |

Initial construction and resize data for the 50,000-object cases:

| Scenario | Backend | Fixture setup | Backend startup | Resize round trip | First extraction | First submission / scene construction |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Static | SDL | 100.500 | 314.075 | 12.487 | 13.193 | 10.341 |
| Static | Filament | 94.849 | 202.597 | 0.004 | 12.919 | 53.290 |
| Dynamic | SDL | 115.577 | 349.600 | 11.855 | 17.892 | 14.665 |
| Dynamic | Filament | 104.903 | 221.600 | 0.005 | 13.473 | 67.878 |
| Mostly static | SDL | 121.237 | 349.580 | 12.135 | 14.740 | 15.109 |
| Mostly static | Filament | 104.086 | 225.329 | 0.004 | 13.679 | 65.557 |
| Mixed | SDL | 151.201 | 363.052 | 11.603 | 12.992 | 20.537 |
| Mixed | Filament | 125.365 | 210.772 | 0.004 | 12.991 | 111.910 |

Filament startup was lower in this configuration, but construction of 50,000 persistent renderables cost 53-112 ms on first publication versus 10-21 ms for SDL's first immediate submission. Filament's windowed resize measurement changes view state and requests the SDL window size; headless resize separately flushes, destroys, and recreates its swapchain and is covered by the lifecycle test.

## Debug results

Debug confirms scaling behavior but is not used to draw the decision. Filament's Debug build also reported its per-render-pass command arena at 305% high-water usage for 50,000 objects and fell back to a slower system heap.

| Scenario | N | Projection extract / reconcile | SDL extract / backend | Fil extract | Fil scan | Fil apply | Fil render | Fil backend | Fil GPU |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Static | 1,000 | 3.022 / 0.761 | 3.214 / 13.360 | 3.189 | 0.753 | 0.013 | 2.527 | 3.375 | 0.367 |
| Static | 10,000 | 32.578 / 8.391 | 32.472 / 4.610 | 32.425 | 7.707 | 0.032 | 17.487 | 26.486 | 0.346 |
| Static | 50,000 | 183.298 / 54.704 | 198.522 / 26.973 | 180.832 | 46.841 | 0.040 | 96.383 | 151.040 | 1.213 |
| Dynamic | 1,000 | 3.285 / 0.702 | 4.232 / 0.861 | 3.504 | 0.763 | 1.699 | 2.859 | 5.483 | 0.294 |
| Dynamic | 10,000 | 40.545 / 9.349 | 37.981 / 4.971 | 35.999 | 8.130 | 17.020 | 19.846 | 46.416 | 0.555 |
| Dynamic | 50,000 | 197.547 / 48.168 | 209.488 / 26.888 | 197.027 | 43.692 | 86.390 | 89.514 | 227.250 | 1.272 |
| Mostly static | 50,000 | 202.899 / 53.745 | 191.090 / 28.106 | 198.160 | 43.733 | 0.825 | 94.484 | 146.610 | 1.259 |
| Mixed | 50,000 | 204.395 / 52.279 | 197.921 / 33.177 | 201.988 | 47.389 | 0.041 | 177.293 | 230.474 | 2.858 |

The previous approximately 204 ms Debug extraction and 58 ms reconciliation observations were directionally correct, but optimized extraction is approximately 13-15 ms at 50,000 Blocks. Release evidence is therefore the architectural basis.

## Full-snapshot publication finding

The experiment separates all four relevant costs for the expected workload:

| Mostly-static 50,000 Blocks | Release mean | Share of extraction + Filament backend |
| --- | ---: | ---: |
| Engine snapshot extraction/publication | 14.496 ms | 37.2% |
| Full projection scan | 7.942 ms | 20.4% |
| 500 changed-object applications | 0.078 ms | 0.2% |
| Filament render submission | 13.404 ms | 34.4% |
| Remaining measurement/dispatch overhead | about 3.04 ms | 7.8% |

The persistent projection itself works: changing 500 transforms costs only 0.078 ms. The full publication scan is more than 100 times that changed-object application cost and is almost the same for static, mostly-static, and dynamic scenes. Full snapshot construction plus scanning consumes about 22.44 ms before most backend frame work is useful. It materially masks the persistent-scene advantage.

This supports a later renderer-neutral incremental-publication investigation with periodic full snapshots for reconstruction. It does not authorize a renderer to consume `ChangeJournal`, traverse the DataModel, or make backend scene state authoritative. No extraction redesign is part of this decision.

## Studio feasibility

The previous Studio finding remains true. The existing EditorHost path renders offscreen, downloads to a transfer buffer, submits with a fence, synchronously waits, maps, converts RGBA to RGB row by row, and copies into shared memory or Base64. That readback/transport work would distort a renderer benchmark and was not used for the primary measurements.

Filament headless rendering and headless swapchain resize both passed smoke/lifecycle tests. The current CPU-frame EditorHost contract is implementable with a custom Filament `RenderTarget` and asynchronous `Renderer::readPixels`, followed by the same completion, format/orientation conversion, and `SharedFrameRing` publication. Required implementation work would include:

- private offscreen color/depth textures and a render target;
- explicit readback callback lifetime and bounded in-flight buffers;
- row stride, origin, and RGB/RGBA conversion validation;
- resize synchronization and target recreation;
- deterministic cancellation/drain before destroying the engine.

No Filament resource or entity needs to cross EditorHost. No shared-GPU-texture transport is required or recommended by this experiment. Because readback/transport remains and Filament was not selected, a second EditorHost backend was not implemented.

## Maintenance and integration comparison

Measured facts and qualitative judgments are labelled separately.

| Criterion | SDL custom | Filament |
| --- | --- | --- |
| Static many-Part CPU | **Measured:** 5.257 ms backend at 50k | **Measured:** 22.401 ms backend at 50k |
| Dynamic many-Part CPU | **Measured:** 6.599 ms backend at 50k | **Measured:** 30.493 ms backend at 50k |
| Mostly-static many-Part CPU | **Measured:** 6.715 ms backend at 50k | **Measured:** 24.462 ms; only 0.078 ms was change application |
| GPU performance | Unavailable; current path exposes no timestamp | **Measured:** 1.197-1.336 ms for the 50k Block cases across two runs; the repeat returned 14 valid samples per case |
| Draw-call reduction | **Fact:** one nominal draw per item per enabled pass | **Fact:** automatic instancing enabled, maximum group 64; actual draws unavailable |
| Culling | **Fact:** none | **Fact:** view frustum culling enabled |
| Instancing | **Fact:** none | **Fact:** automatic instancing enabled and shared geometry/material state |
| Modern lighting | **Judgment:** substantial custom work remains | **Fact/judgment:** built-in physically based lighting is mature |
| Material capability | **Fact:** fixed solid-color shaders | **Fact:** `matc` material system; much broader but version/tool coupled |
| Platform coverage | **Fact:** current SDL GPU shader path targets Vulkan-class SPIR-V and Apple Metal outputs | **Fact:** Filament ships multiple desktop/mobile/web backends; this adapter is Win32/Vulkan only |
| Binary/dependency size | **Fact:** no new SDK; 2.92 MB Release benchmark executable | **Fact:** 768 MB archive, 4.58 GB extracted SDK, about 4.0 MB indicative executable increase |
| Build complexity | **Judgment:** low; existing `glslc` path | **Fact/judgment:** exact SDK, matching `matc`, per-platform packages, and MSVC 19.50 ABI requirement |
| Debugging complexity | **Judgment:** direct ownership and transparent passes, but all bugs are ours | **Judgment:** less shader/renderer code, but a large asynchronous engine and fixed internal arena limits |
| EditorHost suitability | **Fact:** implemented; expensive synchronous CPU readback | **Fact:** implementable; readback/transport cost remains |
| Engine-boundary fit | **Measured by implementation:** exact fit | **Measured by implementation:** exact fit through PImpl and private `ObjectId` projection |
| Long-term maintenance burden | **Judgment:** high if Gargantuan builds culling, materials, lighting, and batching itself | **Judgment:** lower feature maintenance, higher dependency/toolchain/upgrade burden |
| Ability to customize deeply | **Judgment:** maximal | **Judgment:** strong at material/view level, constrained by Filament's scene/material architecture |

Filament is the stronger ready-made renderer and a clean ownership fit. Its maintenance advantage is real. For this adoption decision, however, that advantage does not offset a 3.6-4.6 times backend CPU regression on the three decisive 50,000-Block workloads, a large SDK/toolchain commitment, and no measured end-to-end frame-time win. The result also does not justify Ogre-Next next: it establishes that the current full-publication path taxes any persistent scene backend before an alternative renderer can demonstrate its strengths.

## Correctness, lifetime, and security

- **Authority:** both backends consume only `shared_ptr<const RenderSnapshot>`; no authority or mutation boundary changed.
- **Identity:** the Filament map uses the complete `ObjectId`, including generation. Reusing a slot removes the old-generation entity and creates a distinct new-generation entity.
- **Malformed input:** invalid and duplicate IDs are rejected before projection mutation. A rejected publication leaves the previous projection usable.
- **Removal:** entities are removed from the scene, their components are destroyed through `Engine`, and their entity identities and material references are released.
- **Partial initialization:** the constructor catches every initialization failure and runs teardown. The regression suite uses SDL's dummy video driver to create a window and Filament engine, then intentionally fails native Win32-handle acquisition; cleanup succeeds.
- **Resize:** headless resize flushes, destroys the prior swapchain, recreates it, and is exercised by a subsequent draw. Windowed resize and viewport state were smoke-tested.
- **Shutdown:** work is flushed; projection entities, lights/camera, material instances/material, buffers, view/scene/renderer/swapchain, and finally `Engine` are destroyed. `Destroy` is idempotent and the destructor calls it.
- **Backend isolation:** no Filament type is present in `BaseRenderer`, `RenderSnapshot`, DataModel, Luau, EditorHost protocol, or Studio.
- **SDL support:** the Filament source is excluded unless `GARGANTUAN_FILAMENT_ROOT` is set. Normal SDL builds acquire no Filament dependency.

## Verification

Verification performed for this decision:

- Debug MSVC 19.50 Filament benchmark and lifecycle builds;
- Release MSVC 19.50 Filament benchmark and lifecycle builds;
- complete Debug and Release `projection`, windowed `sdl`, and windowed `filament` workload matrices;
- windowed SDL and Filament Vulkan smoke runs;
- Filament headless render/resize/lifecycle smoke;
- induced partial Filament initialization failure cleanup;
- generation reuse, stale-generation isolation, removal, duplicate publication, post-rejection recovery, resize, and idempotent shutdown tests;
- normal no-Filament SDL build path;
- Foundation, render extraction/projection, EditorHost, and pre-run regression coverage through CTest;
- native renderer ownership/security diff review;
- `git diff --check`.

GPU time, actual post-instancing draw counts, actual visible-object counts, and renderer-owned CPU/VRAM bytes are explicitly unavailable where shown as `NA`. Their absence does not block the CPU and integration comparison that determines this decision.
