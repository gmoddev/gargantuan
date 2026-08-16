---
status: current
owner: rendering
last_verified: 2026-08-16
related_code:
  - include/gargantuan/render/
  - src/render/sdl/
  - src/render/
  - include/gargantuan/editor/EditorViewport.hpp
  - src/editor/EditorViewport.cpp
related_adrs: []
---

# Renderer backend boundary

## Replaceability contract

The established high-level seam remains unchanged:

```text
DataModel / WorldRoot
    -> RenderExtractor
    -> immutable RenderSnapshot
    -> BaseRenderer
    -> backend implementation
       -> backend-owned meshes, shaders, pipelines, passes, textures, and handles
```

`RenderSnapshot` contains value-oriented camera, lighting, geometry,
transform, color, shadow, diagnostic, and stable `ObjectId` data. It owns no
Instance or GPU pointer. `BaseRenderer` accepts immutable snapshot ownership,
resize dimensions, and exposes viewport size; it contains no SDL type.

This milestone does not add a buffer/command/descriptors/barrier RHI. Primitive
geometry is already identified by the semantic `RenderGeometry` value, which
is sufficient for current persistent resource lookup. No speculative texture,
shader, material, or asset identity was added.

## SDL backend ownership

The explicit `SDLRenderer` public entry point uses an incomplete private
implementation. Its public header contains no SDL include or handle. Backend
helpers live under `src/render/sdl/`:

| Private helper | Owns |
| --- | --- |
| `SDLGpuMesh` | SDL vertex/index/transfer buffers and upload lifetime. |
| `SDLMeshCache` | Device-local primitive mesh upload/cache. |
| `SDLShader` / `SDLFileShader` | SDL shader selection, creation, and release. |
| `SDLPipelineBuilder` | SDL vertex layout and graphics-pipeline creation info. |
| `SDLRenderPass` / `SDLFrameContext` | SDL command, target, sampler, pipeline, and pass state. |

The opaque, shadow, and dormant GUI pass implementations are SDL backend code.
Their generic-looking former names did not establish engine semantics. Shader
and pipeline resources are now released by the shared private pass base; this
also closes the previous hidden-member lifetime leak where a derived pass's
shader handles were not the handles destroyed by `RenderPass::Destroy`.

`Mesh` and `Vertex` remain backend-neutral CPU semantic/source data. SDL vertex
buffer descriptions were removed from `Mesh.hpp` and are built inside the SDL
pipeline implementation. Primitive `Mesh` construction remains general code;
upload and device lookup are backend-private.

## Editor viewport

`EditorViewportRenderer` exposes only dimensions, immutable snapshots, RGB
frame bytes, and stable picking identity. Its header uses a private
implementation and contains no SDL or GPU handle. The current implementation
still deliberately uses the SDL GPU backend and reuses its private mesh/pass
helpers, but Studio and EditorHost semantic contracts remain pixels and
`ObjectId`, never backend resources.

Shutdown waits for the device, destroys mesh resources and passes before
textures/samplers and the GPU device, and releases any SDL video-subsystem
ownership last. Resize creates a complete replacement color/depth/download set
before releasing the current targets, so allocation failure does not leave a
partially replaced viewport.

## Leakage audit

Before hardening, these general/public headers exposed SDL GPU types:

| Previous header | SDL GPU leakage | Resolution |
| --- | --- | --- |
| `GpuMesh.hpp` | buffers, transfer buffer, copy pass, device | Replaced by private `SDLGpuMesh`. |
| `MeshProvider.hpp` | device and backend mesh cache | Replaced by private `SDLMeshCache`. |
| `Mesh.hpp` | SDL vertex descriptions/attributes | Vertex layout moved into SDL pipeline code. |
| `Shader.hpp` | shader handles, formats, create info, device | Replaced by private SDL shader helper. |
| `PipelineBuilder.hpp` | SDL shaders, formats, pipeline/create info | Replaced by private SDL pipeline builder. |
| `RenderPass.hpp` | command buffers, textures, sampler, pipeline, pass, device | Replaced by private SDL pass/context. |
| `EditorViewport.hpp` | device, textures, sampler, transfer buffer | Replaced by PIMPL. |
| `SDLRenderer.hpp` | window/device/textures/sampler and pass constructors | Replaced by PIMPL; remains an explicit backend class. |

The post-change source audit finds no `SDL_GPU*` symbol in
`include/gargantuan`. Remaining SDL GPU references are confined to
`src/render/`, `src/editor/EditorViewport.cpp`, explicit Filament/SDL backend
implementation code, backend-specific tests, and dependency/build setup.

## Replacement expectations and accepted dependencies

A future renderer backend can implement `BaseRenderer` and consume
`RenderSnapshot` without modifying DataModel, input, networking, serialization,
physics, Luau semantics, or exposing SDL GPU types. The remaining backend
coupling is implementation-only: the current executable selects `SDLRenderer`,
and the current offscreen editor viewport constructs the SDL-private pass
stack. Replacing the viewport renderer requires an implementation change in its
PIMPL, not a Studio or EditorHost contract change.

SDL remains the default platform and rendering implementation. GLM remains an
accepted foundational C++ value representation, Luau remains product
semantics, and the STL is not abstracted. Renderer replacement, a generic RHI,
materials, textures/assets, advanced lighting, retained UI paint, render
threading, and Play-mode Studio integration remain deferred.
