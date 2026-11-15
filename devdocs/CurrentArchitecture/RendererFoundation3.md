---
status: experimental
owner: rendering
related_code:
  - src/services/AssetService.cpp
  - include/gargantuan/render/RenderPublication.hpp
  - src/render/passes/OpaquePass.cpp
  - assets/shaders/opaque.frag
related_adrs:
  - RendererFoundation2C.md
---

# Renderer Foundation 3

Foundation 2C selects SDL GPU for the current renderer and sets a comparison checkpoint before image-based lighting and post-processing. The new comparison begins with the same immutable incremental publications, including stable identities, resource lifetimes and visible content. First-frame admission, incremental projection, submission, fence completion and viewport readback are distinct measurements. A scene missing material, geometry or lighting features belongs in a separate capability row.

## Direct material lighting

AssetService resolves existing imported material records into the metallic-roughness shading model. Primitive parts and the built-in fallback use the existing legacy model. A model is part of the immutable material value validated by RenderProjection. This selection uses the existing asset, texture, material and object identities. Import and persistence keep the existing representation.

In the SDL material pass, glTF base color factors are linear. The color texture path decodes sRGB RGB before bilinear filtering, using the same resident RGBA8 image that the normal slot or GUI may use. Alpha remains linear. The normal slot reads linear texels through the existing sampler and uses the canonical tangent and handedness. The vertex stage transforms tangents with the position transform and normals with the inverse-transpose transform. The fragment stage reconstructs the orthogonal basis and handles double-sided orientation.

Directional light uses a GGX microfacet distribution, correlated Smith visibility, Schlick Fresnel and a diffuse term reduced by metallic and Fresnel response. A roughness floor of 0.045 bounds the specular lobe. Authored ambient supplies diffuse lighting. Fog and exposure feed the existing tone curve. PBR output uses the sRGB transfer on linear UNORM targets, with hardware transfer on sRGB targets. The sky and GUI have their own color paths. Single-level repeat-filtered textures and a single directional shadow remain the comparison baseline.

Material channel interpretation follows [glTF 2.0 materials](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#materials). The [Filament shading reference](https://google.github.io/filament/Filament.md.html) documents the direct lighting model family used for the comparison.

The optional glTF metallic-roughness image follows the existing compound import, dependency validation, residency and publication path. The green channel scales perceptual roughness and blue scales metallic. The renderer interprets both channels linearly. Material artifacts with this reference use version 5, which appends a required image reference after the version 2 payload. Materials without the map keep version 2 and the same canonical bytes. Established color and normal uses take precedence when deriving embedded image keys, so adding a packed map use to a shared image preserves its identity. The existing asset catalog adds the optional metadata field.

## World submission

The SDL camera pass uses the same clip-space matrices as the shader. It tests conservative box support against the six zero-to-one frustum planes, using canonical mesh bounds from the projection and unit bounds for the current primitive meshes. The GPU-palette path retains admission because its posed bounds are distinct. The shadow pass still considers casters independently of camera culling. Geometry, palette and material texture bindings are reused across adjacent draws on the same pipeline, with actual bind counters.

Opaque and masked draws write depth. Blended draws test depth and composite back to front using object center depth, with stable ObjectId ties. Transparent scratch storage is bounded by the projected object count. Intersecting transparent surfaces and scene-wide linear-light compositing remain separate work.

Shadow submission honors each primitive's sidedness and evaluates the same alpha factor, base color image and cutoff used by the camera pass. Casters without a mask share a device-local white fallback with the camera pass through the existing texture cache. Blended casters retain solid shadows when explicitly enabled.

The existing viewport smoke executable checks linear-light color filtering, tangent-space normal response, packed metallic data, blend order, camera culling and directional cutout shadows through offscreen rendering. Publication and asset checks exercise the same neutral material values. Software Vulkan validates rendering behavior and supplies functional comparison evidence. Hardware rankings require the same physical GPU and matched workloads.

## Physical backend qualification

Both backends passed the shared material captures on a GeForce GTX 1650 SUPER
using Vulkan and NVIDIA driver 591.86. The same executable and immutable
publications exercised base-color sampling, partial texture updates,
tangent-space normals, packed green roughness and blue metallic, and alpha
factor times texture cutoff. The additional Filament lifecycle executable
checked canonical camera motion and off-center projection, readback row
orientation, stable texture and mesh identities, teardown, and explicit
projection retirement. The SDL viewport executable exercised the existing
Sky and UI paths alongside the new material and shadow behavior.
[Exact Windows package](https://github.com/gmoddev/gargantuan/actions/runs/42713708415)

Studio's in-row asset pickers use the existing kind checks and revision-guarded
property edits. The material catalog exposes imported factors and referenced
images. Material factors remain source-authored and update through reimport.
All 43 Studio headless suites passed on the pinned LUI runtime. The paired
EditorHost test assigned imported materials and six Sky faces, edited Lighting,
denied a stale revision, and reopened the saved document after reimport with
stable asset identities. Swapping only the assigned imported material changed
946 pixels in the offscreen capture. Restoring the material reproduced the
original image byte for byte.

The following 1280×720 completion measurements include submission and a
completed frame. The ranges at 10,000 objects are two 180-frame runs with
reversed backend order. The 25,000-object results use 120 measured frames.
The desktop compositor and wallpaper ran on the same GPU during the test.

| Objects | Shared workload | SDL GPU P95 ms | Filament P95 ms |
| ---: | --- | ---: | ---: |
| 10,000 | Static | 2.64–2.74 | 3.96–3.98 |
| 10,000 | 1% transform updates | 2.65–2.78 | 4.34–4.54 |
| 10,000 | 100% transform updates | 3.95–4.27 | 10.09–11.15 |
| 10,000 | Half behind the camera | 2.16–2.26 | 3.09–3.20 |
| 10,000 | Two textured ranges and texture updates | 8.90–8.91 | 7.41–7.52 |
| 25,000 | 1% transform updates | 6.92 | 10.27 |
| 25,000 | Half behind the camera | 4.18 | 6.78 |
| 25,000 | Two textured ranges and texture updates | 21.85 | 16.10 |

The 10,000-object dynamic row applied all transform updates on both backends.
SDL spent about 1.34 ms in projection and 1.94–2.05 ms in submission.
Filament spent about 1.98–2.02 ms in projection, 2.49–2.52 ms applying native
updates, and 2.32–2.34 ms in submission. Each phase uses its backend's
instrumentation boundary. In the 25,000-object mixed row, SDL recorded
50,000 material binds per frame and averaged 17.35 ms of submission, versus
9.29 ms in Filament. Both configurations exceeded the 16.67 ms frame interval
at P99 for this row. SDL culled every expected offscreen object, reused the
shared geometry binding, and uploaded the changed RGBA8 bytes once. Filament
uploaded twice the changed bytes for its linear and sRGB native textures.

Filament's unpaced mode can skip a frame when the earlier GPU fence remains
unsignaled. The early 1,000-object static row submitted 23 of 60 attempts.
The comparison excludes rows with skipped frames. With the same two-frame
queue bound, both backends submitted all 180 frames for all five workloads
at 1,000 and 10,000 objects in both orders. At 10,000 fully moving objects
the bounded-mode P95 was 4.07–4.29 ms for SDL and 9.16–9.30 ms for Filament.
[Pinned upstream frame skipper](https://github.com/google/filament/blob/v1.71.5/filament/src/FrameSkipper.cpp) ·
[Bounded-mode Windows package](https://github.com/gmoddev/gargantuan/actions/runs/42716432024)

The Windows hosted SwiftShader run failed during Vulkan instance creation
with `VK_ERROR_INCOMPATIBLE_DRIVER`.
Both native backends passed the shared physical Vulkan capture gates and their
respective viewport or lifecycle checks.

Linux software Vulkan passed the asset, publication, environment lighting,
viewport, and both SDL material checks. The far-depth matrix initially
reported two missing interior pixels in each of two cases. Rebuilding exact
mainline `5b6782e35` on the same runner reproduced the same four pixels.
All 36 raw frames were byte-identical between mainline and the candidate.
[Linux qualification and depth control](https://github.com/gmoddev/gargantuan/actions/runs/42716432083)

## Opaque draw grouping experiment

An isolated SDL prototype groups adjacent rigid opaque objects with the same
mesh and compatible ordered material ranges. The prototype uses the current
camera cull, native mesh, and resident image bindings. It keeps the existing
masked, skinned, and blended paths. Two reference runs bracketed two prototype
runs at each object count, with 180 measured frames per run.

| Mixed objects | Reference P95 ms | Prototype P95 ms | Reference binds per frame | Prototype binds per frame |
| ---: | ---: | ---: | ---: | ---: |
| 1,000 | 1.00–1.02 | 0.75–0.78 | 2,000 | 4 |
| 10,000 | 7.45–7.88 | 4.64–4.67 | 20,000 | 40 |
| 25,000 | 22.87–24.40 | 10.55–10.76 | 50,000 | 98 |

The draw counts were identical. At 25,000 objects the mean submission cost
fell from 17.14–17.56 ms to 8.17–8.30 ms. The prototype passed the physical
viewport and material checks, including its own stable-order overlap test.
It stays on the experimental branch because range grouping can choose a
different winner for exactly coplanar opaque surfaces under strict depth
testing. Promotion requires an explicit equal-depth ordering policy.
[Prototype package](https://github.com/gmoddev/gargantuan/actions/runs/42717255083)

## Architecture recommendation

Invest in SDL GPU as Gargantuan's renderer. Its lower transform, mostly-static
and culled frame tails align with the Engine's publication and Studio
workloads. Its material submission bottleneck has a concrete path through
draw grouping and fewer state changes. Preserve Filament as an independent
material reference and optional backend for isolated experiments. A migration
would also require qualifying GUI, Sky, fog, GPU skinning, EditorViewport
integration, recovery, and deployment on the supported platforms.

The direct PBR foundation gives the SDL renderer a qualified basis for richer
lighting. The next rendering investment should first bring textured draw
submission within budget, then establish HDR color and exposure behavior before
adding broad image-based lighting and post-processing. Keep authored lighting
and material state in the existing neutral publication contract as those passes
are introduced.
