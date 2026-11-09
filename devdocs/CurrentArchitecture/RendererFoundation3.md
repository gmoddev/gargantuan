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
