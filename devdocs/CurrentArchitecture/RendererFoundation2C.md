---
status: accepted
date: 2026-08-22
owner: rendering
decision: A — keep SDL GPU
gui_start: yes
supersedes: Renderer Foundation 2 temporary Decision C
related_code:
  - include/gargantuan/render/RenderPublication.hpp
  - include/gargantuan/render/RenderProjection.hpp
  - include/gargantuan/render/SDLRenderer.hpp
  - src/render/Renderer.cpp
  - src/render/GpuMesh.cpp
  - src/render/TextureProvider.cpp
  - src/render/passes/GuiPass.cpp
  - tests/RendererFoundation2Benchmark.cpp
  - tests/RendererFoundation2GpuBenchmark.cpp
  - tests/RendererPublicationTests.cpp
---

# Renderer Foundation 2C

## Final decision

**DECISION A — KEEP SDL GPU.**

The temporary Foundation 2 Decision C is resolved. SDL GPU's real deformable,
texture, UI-proxy, and mixed workloads are clean on independent AMD and NVIDIA
hardware. Stable topology creates no measured vertex buffer, index buffer,
transfer-buffer, or UI-buffer replacement after warm-up. A 64K-vertex full
deformable update remains below 1.9 ms at P95 on both systems. The deliberately
unculled and uninstanced 25,041-draw mixed workload measures 15.589 ms P95 and
16.906 ms P99 on the primary, versus 12.357/14.413 ms on a weaker GPU than the
requested secondary target. That submission-heavy primary tail exceeds a
16.67 ms interval, so draw bucketing and culling remain explicit renderer
milestones rather than being hidden by an average-FPS claim.

The backend does not explain the expensive 50K fully dynamic engine path. That
path spends tens of milliseconds in authoritative mutation, final-state
extraction, immutable publication construction, and value-projection
reconciliation before SDL receives upload work. Replacing SDL with Filament,
bgfx, or Dawn would not remove those costs.

This is not a declaration that the current renderer is feature complete or
permanent regardless of future evidence. It is a positive selection of SDL GPU
for the early GUI, cloth, rubber, particles, and Studio renderer roadmap. The
next technology checkpoint is before a broad PBR/IBL/post-processing stack,
where owning those mature features may stop being economical.

**GUI START: YES.** The exact next milestone is **GUI Foundation 1: an
engine-owned retained UI tree, layout/text-shaping and glyph-atlas producers,
and immutable `RenderUiFrame` publication into the existing SDL GUI pass.**

## Architecture delivered

The immutable authority boundary remains:

```text
authoritative engine state
    -> committed mutation and render-domain classification
    -> RenderDirtyAccumulator
    -> shared_ptr<const RenderPublication>
    -> renderer-owned RenderProjection
    -> renderer-owned mesh, texture, UI-buffer, pipeline, and command resources
```

Foundation 2C extends the publication with generation-safe texture lifecycle:

- `RenderTextureCreate`: identity, revision, dimensions, format, and complete
  immutable RGBA8 pixels;
- `RenderTextureUpdate`: a complete or rectangular atlas-like subregion with a
  newer revision;
- `RenderTextureRemove`: explicit generation-safe teardown;
- material and UI texture references that must be resident after the candidate
  publication is applied;
- full resync that reconstructs texture residency after renderer restart.

SDL owns textures and persistent upload transfer buffers. Subregion upload
cycles the transfer-buffer backing but does not cycle the texture, because SDL
defines the untouched part of a cycled texture as undefined. Complete
replacement may safely cycle the texture. The same rule applies to deformable
buffers: a full vertex replacement can cycle both upload and destination
backings, while a partial range cycles only its upload backing and overwrites
the existing destination range. These rules follow SDL's documented
[resource-cycling contract](https://wiki.libsdl.org/SDL3/CategoryGPU) and
[`SDL_UploadToGPUBuffer`](https://wiki.libsdl.org/SDL3/SDL_UploadToGPUBuffer).

The GUI proxy is a real renderer pass, not a projection-only measurement. It
uses growth-only persistent vertex, index, and combined upload buffers;
complete per-frame buffer cycling; alpha blending; ordered scissored batches;
texture/sampler binding; per-batch opacity; and stable atlases with subregion
updates. No layout, widget, input, text-shaping, font fallback, or complete GUI
system is implemented here.

If projection or resource application fails, SDL discards its disposable
projection and dynamic resource caches and requires the next publication to be
a full resync. It never repairs or mutates authoritative engine state.

## Deformable and soft-body boundary

Gargantuan must support rigid bodies, kinematic controllers, constraints,
cloth, and rubber/deformable soft bodies. Physics and rendering meet only at
engine-owned semantic render data:

```text
soft-body solver
    -> engine-owned deformable positions, normals, topology, and bounds
    -> RenderDirtyAccumulator
    -> immutable RenderPublication
    -> renderer-owned persistent mesh
```

The renderer never receives Box3D objects, solver internals,
constraint/particle nodes, raw solver arrays, or writable solver memory. A
stable mesh identity carries versioned topology and monotonically revised
vertex ranges. A topology change requests full resync, discards old residency,
and recreates the logical mesh with a new topology revision. Foundation 2C established this seam;
the later [Soft-body Physics Foundation 1](SoftBodyPhysicsFoundation.md) now
implements its first CPU XPBD cloth/rubber producer without changing the seam.

The current publication permits one contiguous dirty range per mesh per
publication. Multiple separated ranges are deliberately not supported because
duplicate mesh operations are rejected. For early cloth and rubber, where most
vertices change, full or one bounding range is sufficient and measured well.
If future sculpting or tearing evidence shows many sparse islands, the neutral
contract can be extended to a bounded list of non-overlapping ranges without
changing renderer authority. That complexity is not justified now.

## Method

All GPU rows use the same Release executable and compiled SPIR-V shaders:

```text
gargantuan_renderer_foundation2_gpu_benchmark.exe
SHA-256 1e263411f23a982f64802f8f8d6f33756d027f8d130107b80c8104812c82f95b
source base 653344251e74c8dc33da5c9ba1b2773a97a596be plus this working milestone
SDL submodule f87239e71e42da91ca317a12eefb82cfbf3393eb (SDL 3.4.12)
1280x720 offscreen R8G8B8A8 target, Vulkan selected by SDL on both hosts
shadows disabled per object; the same opaque, GUI, upload, and projection code
```

The secondary artifacts were copied only to
`C:\Sandbox\Codex\Artifacts\gargantuan-renderer-2c-6533442`. SHA-256 hashes
were compared after transfer. No repository, existing service, or Docker
container was changed. Container names were identical before and after.

The requested secondary description said GTX 1660 Super. Windows hardware
enumeration on the SSH worker reported **NVIDIA GeForce GTX 1650 SUPER**, driver
`32.0.15.6094`. This report uses the observed device. It is a weaker independent
datapoint, not a declared minimum specification.

| System | CPU | GPU | Driver | OS |
| --- | --- | --- | --- | --- |
| Primary | Ryzen 9 7950X3D, 16C/32T | Radeon RX 7900 XT | 32.0.31035.1003 | Windows 11 Pro 10.0.26200 |
| Secondary | Ryzen 9 5900X, 12C/24T | GeForce GTX 1650 Super | 32.0.15.6094 | Windows 10 Pro 10.0.19045 |

The main run uses 30 warm-up and 300 measured frames and waits a fence every
frame. This includes end-to-end GPU completion latency and prevents asynchronous
work from contaminating later scenarios. SDL 3.4.12 does not expose a
timestamp-query API in its GPU header, so trustworthy pure GPU time is **NA**;
completion-wait is not renamed GPU time.

An additional 120 warm-up / 1,800 measured-frame run omits per-frame fences for
64K cloth, GUI, and mixed workloads. It exposes queue/backing pressure in the
normal queued path, then waits for GPU idle after measurement. Outer frame time
there is CPU pacing/queue-pressure time, not GPU duration.

Generated deformables modify positions and normals. Rubber modifies 32,761
vertices and continuously changes conservative bounds. GUI produces real
pixel-space geometry for 10,000 quads in 40 ordered/scissored/blended batches
over eight 64x64 atlases. Mixed contains 25,000 rigid objects, 500 transform
updates, a moving camera, a 16,384-vertex deformable, 5,000 GUI quads, 40 UI
batches, eight textures, and one atlas subregion update per frame. It records
25,041 opaque/UI draws per frame; culling and instancing are not credited.

## Primary GPU results — 7950X3D / RX 7900 XT

Fence-synchronized outer frame times in milliseconds:

| Workload | Mean | P50 | P95 | P99 | Max | Upload/frame | VB/IB/transfer creates | Realloc | Full resync |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Cloth full 4,096 | 0.220 | 0.161 | 0.557 | 1.210 | 1.338 | 128 KiB | 0 / 0 / 0 | 0 | 0 |
| Cloth full 16,384 | 0.445 | 0.355 | 1.110 | 1.387 | 1.852 | 512 KiB | 0 / 0 / 0 | 0 | 0 |
| Cloth full 65,536 | 1.218 | 1.153 | 1.806 | 2.272 | 3.064 | 2 MiB | 0 / 0 / 0 | 0 | 0 |
| Cloth partial 4,096/16,384 | 0.211 | 0.156 | 0.574 | 1.190 | 1.276 | 128 KiB | 0 / 0 / 0 | 0 | 0 |
| Cloth partial 16,384/65,536 | 0.467 | 0.367 | 1.126 | 1.410 | 1.755 | 512 KiB | 0 / 0 / 0 | 0 | 0 |
| Rubber full 32,761 | 0.767 | 0.627 | 1.444 | 1.849 | 2.387 | 1.000 MiB | 0 / 0 / 0 | 0 | 0 |
| Topology replacement 16,384 | 1.153 | 1.064 | 1.685 | 1.993 | 2.525 | 890 KiB | 300 / 300 / 300 | 0 | 0 |
| GUI 10K / 40 / 8 | 0.850 | 0.794 | 1.575 | 1.947 | 2.335 | 1.450 MiB | 0 / 0 / 0 | 0 | 0 |
| Mixed 25K + 16K + 5K | 12.954 | 12.912 | 15.589 | 16.906 | 17.510 | 1.225 MiB | 0 / 0 / 0 | 0 | 0 |
| Texture sub/full alternating | 0.141 | 0.115 | 0.234 | 0.942 | 1.209 | 8.125 KiB | 0 / 0 / 0 | 0 | 0 |

The topology row intentionally recreates one mesh generation per frame and
therefore records exactly 300 vertex, index, and transfer-buffer creations. It
is a separate cost path, not evidence against stable streaming.

| Mean phase | Projection | Mesh transfer | Texture transfer | UI prepare | Submit | Completion wait |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Cloth full 65,536 | 0.197 | 0.490 | 0.000 | 0.000 | 0.054 | 0.371 |
| GUI 10K | 0.264 | 0.000 | 0.058 | 0.107 | 0.177 | 0.303 |
| Mixed | 0.285 | 0.204 | 0.037 | 0.087 | 6.704 | 5.692 |

| Queued 1,800-frame workload | Mean | P50 | P95 | P99 | Max | Stable resources |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Cloth full 65,536 | 0.817 | 0.784 | 1.043 | 1.173 | 2.349 | PASS |
| GUI 10K | 0.253 | 0.249 | 0.351 | 0.415 | 1.499 | PASS |
| Mixed | 6.590 | 6.351 | 8.691 | 9.673 | 12.103 | PASS |

## Secondary GPU results — 5900X / GTX 1650 Super

Fence-synchronized outer frame times in milliseconds:

| Workload | Mean | P50 | P95 | P99 | Max | Upload/frame | VB/IB/transfer creates | Realloc | Full resync |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Cloth full 4,096 | 0.176 | 0.168 | 0.198 | 0.227 | 1.291 | 128 KiB | 0 / 0 / 0 | 0 | 0 |
| Cloth full 16,384 | 0.367 | 0.353 | 0.413 | 0.498 | 1.573 | 512 KiB | 0 / 0 / 0 | 0 | 0 |
| Cloth full 65,536 | 1.154 | 1.097 | 1.465 | 2.000 | 2.067 | 2 MiB | 0 / 0 / 0 | 0 | 0 |
| Cloth partial 4,096/16,384 | 0.178 | 0.170 | 0.198 | 0.261 | 1.297 | 128 KiB | 0 / 0 / 0 | 0 | 0 |
| Cloth partial 16,384/65,536 | 0.363 | 0.351 | 0.414 | 0.590 | 1.386 | 512 KiB | 0 / 0 / 0 | 0 | 0 |
| Rubber full 32,761 | 0.598 | 0.588 | 0.656 | 0.695 | 1.558 | 1.000 MiB | 0 / 0 / 0 | 0 | 0 |
| Topology replacement 16,384 | 0.770 | 0.792 | 0.980 | 1.079 | 1.770 | 890 KiB | 300 / 300 / 300 | 0 | 0 |
| GUI 10K / 40 / 8 | 0.573 | 0.566 | 0.626 | 0.655 | 1.413 | 1.450 MiB | 0 / 0 / 0 | 0 | 0 |
| Mixed 25K + 16K + 5K | 10.919 | 10.682 | 12.357 | 14.413 | 15.907 | 1.225 MiB | 0 / 0 / 0 | 0 | 0 |
| Texture sub/full alternating | 0.126 | 0.121 | 0.150 | 0.180 | 1.197 | 8.125 KiB | 0 / 0 / 0 | 0 | 0 |

| Mean phase | Projection | Mesh transfer | Texture transfer | UI prepare | Submit | Completion wait |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Cloth full 65,536 | 0.216 | 0.532 | 0.000 | 0.000 | 0.033 | 0.251 |
| GUI 10K | 0.135 | 0.000 | 0.028 | 0.040 | 0.091 | 0.291 |
| Mixed | 0.208 | 0.154 | 0.023 | 0.029 | 3.684 | 6.838 |

| Queued 1,800-frame workload | Mean | P50 | P95 | P99 | Max | Stable resources |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Cloth full 65,536 | 0.766 | 0.758 | 0.781 | 0.848 | 1.871 | PASS |
| GUI 10K | 0.260 | 0.252 | 0.313 | 0.380 | 1.503 | PASS |
| Mixed | 5.204 | 5.077 | 6.401 | 8.658 | 10.709 | PASS |

No sustained stable-topology row produced a public GPU-resource creation or
reallocation after warm-up. SDL may rotate private backing allocations when a
cycled resource is still bound; SDL exposes neither a backing-allocation
counter nor timestamp queries. Stable public handles, bounded tails, and the
queued results are the available evidence without inventing unavailable data.

## CPU publication results

Release means over 30 frames, in milliseconds. These are engine-side semantic
workloads; systems are reported separately and not averaged.

| Primary 7950X3D | Accumulate | Publish | Projection apply | Full resyncs |
| --- | ---: | ---: | ---: | ---: |
| 50K unchanged | 0.000 | 0.001 | 0.001 | 0 |
| 50K, 1% dirty | 0.870 | 0.301 | 0.084 | 0 |
| 50K dynamic | 73.569 | 36.362 | 9.244 | 0 |
| 50K dynamic, four writes/object | 151.961 | 44.112 | 11.169 | 0 |
| Deformable 4K generate/apply | — | 0.045 | 0.013 | 0 |
| Deformable 16K generate/apply | — | 0.180 | 0.048 | 0 |
| Deformable 64K generate/apply | — | 1.125 | 0.190 | 0 |
| GUI 10K generate/apply | — | 0.344 | 0.138 | 0 |

| Secondary 5900X | Accumulate | Publish | Projection apply | Full resyncs |
| --- | ---: | ---: | ---: | ---: |
| 50K unchanged | 0.000 | 0.001 | 0.001 | 0 |
| 50K, 1% dirty | 0.721 | 0.269 | 0.073 | 0 |
| 50K dynamic | 69.368 | 35.693 | 8.881 | 0 |
| 50K dynamic, four writes/object | 133.080 | 36.790 | 8.602 | 0 |
| Deformable 4K generate/apply | — | 0.058 | 0.014 | 0 |
| Deformable 16K generate/apply | — | 0.238 | 0.053 | 0 |
| Deformable 64K generate/apply | — | 1.226 | 0.207 | 0 |
| GUI 10K generate/apply | — | 0.286 | 0.142 | 0 |

Safe storage changes reduced the primary single-write 50K dynamic local result
from 79.98/49.62/10.07 ms to 73.57/36.36/9.24 ms: about 8% accumulation,
27% publication, and 8% projection improvement. The four-write path improved
from 155.46/53.94/11.14 ms to 151.96/44.11/11.17 ms: about 2% accumulation
and 18% publication, with apply unchanged. No property setter, MutationGateway,
validation, or immutable-copy semantic was bypassed.

Opt-in per-call profiling perturbs the setter loop, so unprofiled totals above
are authoritative. A separate 10-frame instrumented run locates the work:

| Primary instrumented phase | 50K dynamic | 50K × four writes |
| --- | ---: | ---: |
| Accumulation total | 111.936 | 184.119 |
| Property setter/other plus profiler overhead | 83.281 | 130.058 |
| Journal bookkeeping | 12.374 | 30.250 |
| Render-domain classification | 1.181 | 4.533 |
| Accumulator lookup/coalescing | 15.101 | 19.278 |
| Publication total | 55.733 | 49.779 |
| Dirty capture | 5.579 | 5.081 |
| Dirty expansion | 0.165 | 0.178 |
| Final-state extraction | 34.126 | 31.432 |
| Publication construction/copy | 2.532 | 2.325 |
| Publisher cache reconciliation | 4.989 | 3.768 |

The residual covers frame validation, dirty acknowledgement, allocation/runtime
overhead, and profiling overhead. Final authoritative state extraction is the
largest directly measured publication phase; repeated setters, journal work,
and accumulator lookup dominate accumulation. GPU transfer and SDL submission
are separate sub-millisecond costs for one 64K deformable and do not explain a
110–160 ms engine-side 50K mutation workload.

Safe implemented changes are contiguous `deque` journal storage instead of
per-record `list` allocation; batch commit through a complete replacement
candidate; hash-indexed dirty coalescing followed by deterministic `ObjectId`
sorting; publication vector reservation; and opt-in phase counters.

Object-local dirty epochs could reduce repeated lookup in the four-write case,
but correctly handling rollback, destruction, multiple consumers, and epoch
wrap is a semantic change. It is deferred until a real workload makes 200,000
render-relevant property writes per frame important. Skipping setters,
journals, classification, or validation is not acceptable.

## Texture and GUI findings

The texture lifecycle run alternates one 8x8 subregion with a complete 64x64
replacement. It creates no resource after warm-up, uploads an average 8,320
bytes/frame, and passes create, update, remove, teardown, new renderer, and
full-resync recreation. Material and UI references cannot retain an absent
texture identity.

Atlas residency and UI sampling are real. The current opaque 3D shader does not
yet sample `RenderMaterialState` base-color/normal textures; those identities
are validated and resident, but the material pipeline implementation is a
later milestone. This does not block GUI because the GUI pass samples atlases.

The proxy shows no structural SDL blocker. Scissors, alpha blending, ordered
texture switching, pipeline binding, persistent buffers, and atlas subupdates
all work through backend-neutral values. A real producer should batch adjacent
compatible primitives but does not need backend objects.

## Backend comparison

| Direction | What it supplies | Foundation 2 equivalence | Cost and conclusion |
| --- | --- | --- | --- |
| SDL GPU custom | Cross-platform low-level GPU API already integrated with platform/window/input | Fully measured here on AMD/NVIDIA Vulkan with real deformation, texture, GUI, and mixed work | Keep. Small dependency, existing shader path, direct feature access, and acceptable tails. Gargantuan still owns renderer systems. |
| Filament | Mature PBR, lights, shadows, materials, culling, instancing, post effects, broad platforms | Current adapter accepts a publication but builds a compatibility snapshot, scans every object, and drops dynamic mesh/texture/UI semantics | An equivalent adapter rewrite is substantial. Historical SDK/toolchain cost was multi-GB with matching `matc` and a newer MSVC ABI. It cannot remove authoritative publication cost and has no demonstrated 2C advantage. Do not migrate now. |
| bgfx | Cross-platform graphics abstraction, backend selection, transient buffers, encoders, sorting, debugging | No persistent semantic scene, material/lighting system, retained GUI, or adapter | It changes the low-level API while leaving nearly the same engine renderer systems to Gargantuan. Rule out for this milestone. |
| Dawn/WebGPU | WebGPU over D3D12/Metal/Vulkan/OpenGL, WGSL/Tint, validation and web alignment | No engine renderer or adapter | Gargantuan would still build resources, materials, culling, batching, GUI, and recovery. Chromium-style tooling/dependencies are much larger. No measured benefit justifies migration now. |

Official references support the maintenance comparison:

- SDL 3.4.12 is a stable release and documents resource cycling and GPU
  synchronization: [release](https://github.com/libsdl-org/SDL/releases/tag/release-3.4.12),
  [GPU API](https://wiki.libsdl.org/SDL3/CategoryGPU).
- Filament is a cross-platform PBR engine with a compiled material system and
  host tools: [documentation](https://google.github.io/filament/),
  [materials](https://google.github.io/filament/Materials.md.html). The current
  official release is 1.75.0; Gargantuan's adapter remains pinned to 1.71.5.
- bgfx calls itself a bring-your-own-engine graphics library and documents its
  render thread, encoders, backends, limits, and BSD-2-Clause license:
  [documentation](https://bkaradzic.github.io/bgfx/index.html),
  [internals](https://bkaradzic.github.io/bgfx/internals.html),
  [license](https://github.com/bkaradzic/bgfx/blob/master/LICENSE).
- Dawn is native WebGPU over major platform APIs; its primary build environment
  uses Chromium GN/depot_tools and a large dependency closure:
  [overview](https://dawn.googlesource.com/dawn),
  [build documentation](https://dawn.googlesource.com/dawn/+/HEAD/docs/building.md).

SDL requires normal engine renderer systems: resource catalogs,
batching/instancing, visibility/culling, material/pipeline caches, passes,
device recovery, profiling, and the GUI paint backend. That is acceptable for
direct dynamic geometry and a bounded feature set. Recreating a broad material
graph, production PBR/IBL, advanced shadows, temporal effects, and a large
post-processing suite would be reinventing a mature renderer. Re-evaluate an
engine-level renderer before that work, using incremental publication rather
than a compatibility snapshot.

## Validation

- The final Windows Release graph builds successfully. Its GNS-disabled local
  configuration passes 19/19 registered tests, including publication,
  projection, backend-headless, EditorHost, networking, physics, and telemetry
  coverage.
- `gargantuan_editor_viewport_smoke` passes against the real SDL GPU viewport.
  The publication and GPU benchmark smokes also pass after the final build.
- An isolated Linux Clang 20 build instruments production core/renderer code
  with ASan and UBSan, `-fno-sanitize-recover=all`, and frame pointers. With the
  CI `SDL_VIDEODRIVER=dummy` contract, 17/17 GNS-disabled headless tests pass
  without a sanitizer report. This independent Alpine/musl check complements,
  but does not replace or weaken, the checked-in Ubuntu 24.04/Clang 19 CI job.
- The sanitizer run exposed an unnecessary GPU dependency in EditorHost
  picking. EditorHost now maintains a CPU `RenderProjection` of the same
  publications used for capture, so `PickViewport` remains usable in headless
  CI while rendered capture stays on the real GPU path.
- The final GPU benchmark SHA-256 matches on both machines. The six pre-existing
  Docker services on the secondary host were present before and after; all
  Codex-created remote files stayed beneath `C:\Sandbox\Codex`.

## Gates and next milestones

**Deformable GPU acceptance: PASS.** Stable topology/resources, full and
partial correctness, rubber positions/normals/bounds, topology replacement,
two vendors, 300-frame fence-complete tails, and 1,800-frame queue pressure are
covered.

**GUI START: YES.** GUI Foundation 1 must produce only renderer-neutral values:

```text
engine-owned retained GUI semantics
    -> layout, text shaping, clipping, paint ordering
    -> engine-owned glyph/image atlas allocator and immutable RGBA8 updates
    -> RenderTexture create/update/remove + RenderUiFrame
    -> SDL-owned texture residency and GUI pass
```

The producer owns widgets, text, glyph identity, layout, DPI semantics, hit
testing, and paint order. The renderer owns only texture residency, buffers,
scissors, blend/pipeline state, and commands.

The exact next milestone is **GUI Foundation 1**: retained UI semantics,
deterministic layout/paint extraction, text shaping/font fallback, glyph/image
atlas production, incremental atlas publication, runtime and EditorViewport
presentation, and input hit-test integration.

Following renderer milestones, in order:

1. adjacent-compatible UI batching and atlas lifecycle/eviction diagnostics;
2. opaque material texture sampling, opacity classification, and pipeline/
   sampler caches;
3. rigid draw bucketing/instancing and visibility culling before raising the
   target beyond 25K immediate draws;
4. device-loss/restart fault injection and full-resync recovery tests;
5. particle-oriented transient/instanced publication;
6. re-open the engine-level comparison before broad PBR/IBL/post-processing.

## Reproduction

```powershell
cmake --build build-release-native --config Release --target `
  gargantuan_renderer_publication_tests `
  gargantuan_renderer_foundation2_benchmark `
  gargantuan_renderer_foundation2_gpu_benchmark `
  gargantuan_editor_viewport_smoke

build-release-native/gargantuan_renderer_foundation2_benchmark.exe 30
build-release-native/gargantuan_renderer_foundation2_benchmark.exe 10 --profile
build-release-native/gargantuan_renderer_foundation2_gpu_benchmark.exe `
  --scenario=all --frames=300 --warmup=30 --width=1280 --height=720
build-release-native/gargantuan_renderer_foundation2_gpu_benchmark.exe `
  --scenario=mixed --frames=1800 --warmup=120 --synchronize=off
```

The GPU executable is a manual hardware benchmark, not a CI test. Projection,
publication, headless, and EditorViewport smoke coverage remain CI-compatible
and do not require a physical benchmark device.
