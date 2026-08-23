---
status: current
owner: rendering
last_verified: 2026-08-22
related_code:
  - include/gargantuan/render/
  - src/render/sdl/
  - src/render/
  - include/gargantuan/editor/EditorViewport.hpp
  - src/editor/EditorViewport.cpp
related_adrs: []
---

# Renderer backend boundary

Foundation 2B makes immutable `RenderPublication` the supported runtime and
EditorViewport contract. `RenderSnapshot` remains compatibility scaffolding and
the input to full-state extraction, not the normal renderer seam. Foundation 2C
selects SDL GPU after two-hardware deformable/GUI validation; see
[Renderer Foundation 2C](RendererFoundation2C.md).

## Replaceability contract

The established high-level seam remains unchanged:

```text
DataModel / WorldRoot
    -> render-dirty classification and RenderPublisher
    -> immutable RenderPublication
    -> BaseRenderer
    -> backend-owned persistent RenderProjection
    -> backend implementation
       -> backend-owned meshes, shaders, pipelines, passes, textures, and handles
```

`RenderPublication` contains value-oriented frame state, deterministic
create/update/remove operations, persistent mesh lifecycle/ranges, transient UI
batches, diagnostics, and stable generational identities. It owns no Instance
or GPU pointer. `BaseRenderer` accepts immutable publication ownership, resize
dimensions, and exposes viewport size; it contains no SDL type. The compatibility
snapshot overload translates a complete snapshot into a full-resync
publication before calling the production virtual method.

This milestone does not add a buffer/command/descriptors/barrier RHI. Primitive
geometry remains a semantic `RenderGeometry` value. Generation-safe mesh,
material, and texture identities are renderer-neutral values; their device
residency and handles remain backend-owned.

Asset Foundation 1 feeds canonical image and mesh residency into this existing
seam. `AssetService` derives generation-safe render identities and publishes
texture create/update/remove plus mesh create/remove values. `RenderPublisher`
retains asset mesh state for full resync. The renderer receives canonical bytes,
vertices, indices, bounds, and revisions; it never receives source paths,
importers, `AssetId`, or catalog authority.

## SDL backend ownership

The explicit `SDLRenderer` public entry point uses an incomplete private
implementation. Its public header contains no SDL include or handle. Backend
helpers live under `src/render/sdl/`:

| Private helper | Owns |
| --- | --- |
| `SDLGpuMesh` | Persistent SDL vertex/index/transfer buffers and bounded vertex-range uploads. |
| `SDLMeshCache` | Device-local primitive and dynamic mesh lifecycle/cache. |
| `SDLTextureCache` | Generation-safe resident textures and persistent atlas upload buffers. |
| `SDLShader` / `SDLFileShader` | SDL shader selection, creation, and release. |
| `SDLPipelineBuilder` | SDL vertex layout and graphics-pipeline creation info. |
| `SDLRenderPass` / `SDLFrameContext` | SDL command, target, sampler, pipeline, and pass state. |

The opaque, shadow, and active GUI pass implementations are SDL backend code.
Their generic-looking former names did not establish engine semantics. Shader
and pipeline resources are now released by the shared private pass base; this
also closes the previous hidden-member lifetime leak where a derived pass's
shader handles were not the handles destroyed by `RenderPass::Destroy`.

SDL passes iterate the persistent `RenderProjection`, not a complete snapshot.
Unchanged objects perform no publication/application work. Transform, material,
and visibility updates preserve geometry resources. Mesh create/remove follows
generation-safe publication identities; stable-topology deformable updates copy
only the requested vertex range.

The GUI pass consumes renderer-neutral ordered batches and owns growth-only
vertex/index/upload buffers, atlas bindings, alpha blending, scissors, and draw
commands. Texture lifecycle values remain neutral; SDL textures, samplers,
transfer buffers, cycling, and teardown stay backend-private. Partial updates
preserve untouched destination bytes; full replacements may cycle the whole
destination resource.

`Mesh` and `Vertex` remain backend-neutral CPU semantic/source data. SDL vertex
buffer descriptions were removed from `Mesh.hpp` and are built inside the SDL
pipeline implementation. Primitive `Mesh` construction remains general code;
upload and device lookup are backend-private.

## Editor viewport

`EditorViewportRenderer` exposes only dimensions, immutable publications, RGB
frame bytes, and stable picking identity. Its header uses a private
implementation and contains no SDL or GPU handle. The current implementation
still deliberately uses the SDL GPU backend and reuses its private mesh/pass
helpers, but Studio and EditorHost semantic contracts remain pixels and
`ObjectId`, never backend resources.

The viewport has its own publisher consumer. EditorHost maintains a CPU-only
persistent projection for picking, while `EditorViewportRenderer` maintains a
separate disposable projection plus GPU resources for capture. Both consume the
same immutable publications. Creation or recreation requests full resync,
resize replaces targets and frame-global state, and ordinary Edit/Play changes
are incremental. Picking never initializes a GPU device, preserving headless
CI coverage without changing the capture path.

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
`RenderPublication` without modifying DataModel, input, networking, serialization,
physics, Luau semantics, or exposing SDL GPU types. The remaining backend
coupling is implementation-only: the current executable selects `SDLRenderer`,
and the current offscreen editor viewport constructs the SDL-private pass
stack. Replacing the viewport renderer requires an implementation change in its
PIMPL, not a Studio or EditorHost contract change.

SDL remains the selected platform and rendering implementation. GLM remains an
accepted foundational C++ value representation, Luau remains product
semantics, and the STL is not abstracted. Renderer replacement, a generic RHI,
advanced asset material/mesh-consumer semantics, advanced lighting, retained UI paint production, render
threading, and advanced Play rendering remain deferred. Minimal Play reuses the
current offscreen EditorHost renderer through the publication boundary.
