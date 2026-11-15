---
status: experimental
owner: rendering
related_code:
  - src/render/ImageBasedLighting.cpp
  - src/render/passes/SkyPass.cpp
  - src/render/passes/OpaquePass.cpp
  - assets/shaders/ibl_convolve.frag
  - tests/EnvironmentLightingReferenceTests.cpp
  - tests/ImageBasedLightingGpuTests.cpp
---

# Renderer Foundation 4 environment lighting

The SDL world renderer derives image-based lighting from the effective six-face
Sky in the immutable render projection. Lighting and Sky define the authored
environment and AssetService owns its decoded images. The adapter consumes the
six resident identities in `+X, -X, +Y, -Y, +Z, -Z` order. Generated maps are
disposable, device-local resources. The same Sky pass owns preparation in the
runtime renderer and EditorViewport.

The current source is canonical RGBA8. Sky RGB is sRGB-encoded display image
content interpreted as relative environment radiance after decoding. Environment
lighting consumes RGB. The native cube keeps the published face and row
orientation. The source pass decodes individual texels before interpolation or
area reduction. Existing source validation bounds all six square faces to 1024
pixels per axis. The temporary linear source cube is capped at 128 pixels per
face and builds native mips to reduce filtering variance. [Existing Sky contract](EnvironmentLightingFoundation1.md)

The adapter writes a 16-pixel diffuse cube using cosine-weighted integration.
Its texels store irradiance divided by π, so the material applies the Lambert
normalization once. The 64-pixel reflection cube has seven levels. Level zero
samples the mirror direction and subsequent levels prefilter the GGX lobe for
perceptual roughness from zero to one. A 64×64 device-local lookup stores the
Schlick split coefficients for the same correlated Smith visibility model used
in direct PBR. The world shader samples the reflection cube with perceptual
roughness and multiplies it by `F0 * scale + bias`. These are single-scatter,
split-sum approximations with bounded sample counts. [Khronos glTF IBL sampler](https://github.com/KhronosGroup/glTF-IBL-Sampler)

The Sky pass caches one prepared environment by ordered texture identities,
content revisions, dimensions, and the texture cache's residency generation.
The generation changes when full resync reconstructs SDL texture residency,
even if a new projection reuses the same logical identities. An unchanged Sky
reuses all GPU resources. A successful face reimport prepares a complete new
set in one GPU submission. A nonblocking fence limits preparation to one in
flight. When a newer key supersedes pending work, authored ambient supplies the
world light until the latest key can be prepared. A failed build uses the same
fallback and suppresses retries for that exact key. Losing the Sky retires its
irradiance and reflection resources. The BRDF table stays device-local until
renderer shutdown.

The resident output surfaces contain about 300 KiB of unpadded texels, including
the BRDF table. One temporary source cube adds at most 1 MiB. Preparation uses
54 face and mip render passes per coherent Sky revision and one additional
BRDF pass on first use. Native source-mip blits are separate. The renderer
records successful preprocessing submissions, cache hits, pass
counts, and CPU preparation time. GPU commands run without CPU texture mirrors,
readback, or a preparation wait on the frame thread. GPU allocation and shader
failure preserve fallback shading. The optional IBL path requires sampled
RGBA16F color targets. [SDL color target contract](https://wiki.libsdl.org/SDL3/SDL_GPUColorTargetInfo)

The CPU reference integrates a uniform solid-angle grid and checks face UVs,
linear source energy, Lambert weighting, GGX broadening, and an analytic
rough white-furnace result. The offscreen GPU fixture reads actual half-float
texels, exercises cache and full-resync lifetimes, then renders a smooth metal,
rough metal, and dielectric under Sky alone through EditorViewport.
