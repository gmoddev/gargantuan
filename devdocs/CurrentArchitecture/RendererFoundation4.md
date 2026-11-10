---
status: experimental
owner: rendering
related_adrs:
  - RendererFoundation2C.md
  - RendererFoundation3.md
related_code:
  - src/render/Renderer.cpp
  - src/editor/EditorViewport.cpp
  - src/render/passes/SceneResolvePass.cpp
  - assets/shaders/scene-color.glsl
---

# Renderer Foundation 4

## Frozen baseline

Engine Foundation 3 ends at `81a5c5aff36e2149c3181ada6858d7143da19ea0`.
Its last code and binaries are `cee6b3d22690a7cc81e4bf75b349307b091b6960`.
Studio starts at `e4050a1bf55e0edfbb7790c7fe93fd912bc01179`. The exact
Windows package, original results, and recipe are recorded in
[Foundation 3 qualification](../Validation/RendererFoundation3Qualification.md)
and [the pinned Windows build](https://github.com/gmoddev/gargantuan/actions/runs/42716432024).
The known Windows software driver failure remains a separate result from
the physical Vulkan captures.

Foundation 3 applies the immutable publication to RenderProjection, uploads
generation-safe mesh and RGBA8 texture revisions, then records directional
shadow depth, Sky, opaque/masked geometry, sorted blends and GUI. The scene
depth target is D32 float with zero-to-one projection and strict LESS.
Shadow and camera depth have separate targets. The viewport downloads the
same final display-space image through its owned BGRA staging buffer.
Foundation 3 tones each surface independently. Its material path decodes
base-color texels before bilinear filtering and uses one mip level. The
texture cache owns GPU residency and uploads independent of scene authority.
At 25,000 mixed objects the qualified reference submitted 50,000 material
sampler binds per frame. This cost belongs to CPU submission. The completed
frame measurements include the driver and fence wait.

The original and repeated physical captures use the existing material and
packed-channel charts, viewport sky and GUI smoke, directional cutout
shadow probe, and matched incremental publication workloads. Subsequent
tests should reuse these fixture inputs and label any intended output
changes. Studio protocol captures and live runtime frame timing are separate
measurements. Test and benchmark evidence is tracked by source SHA.

## Linear scene target and SDR presentation

Runtime SDLRenderer and EditorViewport select RGBA16F when SDL reports that
the device supports a 2D color target with sampling. Each backend owns one
intermediate scene texture per viewport. Resize stages its replacement
alongside the existing output and depth resources. The previous allocation
remains available when a replacement fails. The RGBA16F level contributes
eight logical bytes per pixel, tracked separately from native allocation
padding. Unsupported devices select the direct SDR path at construction.
The test option also selects that path deliberately.
[SDL GPU texture capability query](https://wiki.libsdl.org/SDL3/SDL_GPUTextureSupportsFormat)

Sky, ambient, direct light, fog, and blended geometry feed one scene-linear
target. Existing six-face images use sRGB RGB decoded before interpolation.
The authored environment fallback and fog values remain scene-linear.
Back-to-front straight-alpha geometry blends into the HDR target before any
display transform. A fullscreen resolve applies the existing Lighting
`ExposureCompensation` multiplier and rational filmic curve once. The
authored exposure range remains ±8 stops. The resolve writes sRGB code values
to ordinary UNORM outputs and linear display values to sRGB outputs, where
the attachment performs the transfer. GUI draws after this resolve through
its established presentation path. Studio receives the existing SDR byte
layout. The direct SDR fallback retains Foundation 3 per-surface tone and
compositing behavior. The HDR path consistently encodes legacy world and Sky
colors, so those authored surfaces can appear brighter than the direct SDR
path at identical exposure.
[Filmic curve reference](https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/)

Manual exposure is deterministic and uses the existing schema and Lighting
publication. Filmic is the default and Reinhard is the selectable reference.
Automatic exposure would require a bounded luminance reduction
and a renderer-owned adaptation state reset on view transitions. The first
implementation uses manual exposure so authoring, replay, and per-frame
comparison have one defined output. The GPU smoke compares saturated and
low-exposure samples against double-precision display values and tests linear
alpha composition, GUI isolation, resize, recreation, and direct SDR fallback.

## Environment lighting and materials

Sky remains the single authored environment source. Its existing six images
feed bounded SDL preprocessing for diffuse irradiance, a GGX specular
prefilter and a split-sum BRDF lookup. A shared world-space rotation applies
to background and material lookups. Changing rotation or `IblIntensity`
does not rebuild the prefilter. Authored Ambient is an independent additive
term and remains available when environment preprocessing is unavailable.
[IBL resources and bounds](EnvironmentLightingFoundation4.md)

Emissive color uses linear factors and sRGB texture sampling. Occlusion
attenuates indirect lighting only. The normal scale multiplies decoded
tangent-space X and Y before normalization. The new canonical material
artifact adds the authored values and sampler state. Older encodings keep
their byte layout, and the existing asset identities still own all image
dependencies. Existing EditorHost clients keep the previous catalog shape.

SDL texture entries can lazily own a native sRGB view for color slots and
a linear mip view for data slots. CPU box filtering uses linear RGB for
sRGB images and linear scalar or vector components for data and alpha.
Normals normalize when sampled, so coarse levels represent an average
direction before reconstruction. Material sampler state selects nearest or
linear minification and magnification, mip filtering, and repeat, mirror,
or clamp per axis. The fixed cache bounds native sampler permutations at
108. Alpha mask camera and shadow samples use level zero to preserve the
authored cutoff. Texture updates preserve the base image and replace
affected mip content in the same publication-driven residency path.
Optional view and retained source budgets are each 128 MiB. At a capability
or memory limit the source image remains usable at level zero, including
linear-light fallback sampling of encoded color. GUI and Sky retain the
existing base-image path. [SDL texture upload contract](https://wiki.libsdl.org/SDL3/SDL_UploadToGPUTexture)

## Point and spot lighting

Schema-backed PointLight and SpotLight are ordinary scene Instances. The
WorldRoot membership and RenderPublisher project up to 512 active records
sorted by generation-safe ObjectId. A BasePart owner contributes its render
transform without its scale. The GPU pass consumes only the immutable
environment list. No GPU resource or live Instance crosses that boundary.

The SDL renderer conservatively projects finite light volumes to screen
tiles at least 32 pixels wide, with at most 64 tiles per axis. It admits
up to 24 light indices per tile, ordered by a bounded contribution
heuristic and ObjectId. Clip-plane rejection and conservative projected
bounds avoid per-object scans of every light. A light crossing the eye
plane covers the viewport conservatively. Static cameras and unchanged
light publications reuse their lists. Diagnostics count frustum-admitted
lights, saturated tiles, dropped memberships, and upload bytes.

Intensity is a candela-like relative scale. The fragment shader uses
inverse-square falloff with a 0.1-world-unit near clamp and a smooth
finite range cutoff. Spotlights multiply this by the inner-to-outer cone
response. Local illumination adds to the existing directional and ambient
terms before fog and the HDR resolve. The directional depth target
continues to serve sun shadows.
