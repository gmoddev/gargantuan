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

The existing viewport smoke executable checks linear-light color filtering and tangent-space normal response through an offscreen frame capture. Publication and asset checks exercise the same neutral material values. Software Vulkan validates rendering behavior and supplies functional comparison evidence. Hardware rankings require the same physical GPU and matched workloads.
