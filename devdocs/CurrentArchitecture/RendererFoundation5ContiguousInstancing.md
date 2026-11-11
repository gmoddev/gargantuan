# Renderer Foundation 5 contiguous instancing experiment

`--sdl-instancing-order=proof|contiguous` selects same-binary admission controls when
`--sdl-instancing=on`. The default is `proof`; both native instancing and contiguous
admission remain opt-in. The contiguous variant is gated to SDL's Vulkan driver.

The [Vulkan primitive order specification](https://docs.vulkan.org/spec/latest/chapters/drawing.html#drawing-primitive-order)
orders drawing commands by submission, then instances from lower to higher, then
indexed primitives by lower index-buffer address. The [rasterization order specification](https://docs.vulkan.org/spec/latest/chapters/primsrast.html#primsrast-order)
defaults to strict order unless the relaxed AMD mode is requested. The pinned SDL
Vulkan backend has no `VK_AMD_rasterization_order` request. Therefore a consecutive
sequence of single-range draws for the same mesh and pipeline can be expressed by
one draw with instance slots in the original object traversal order. This reasoning
does not cover multiple material ranges, transparent fragments, masks, cross-pipeline
barriers, or independent draw records separated by an intervening visible draw.

The experiment retains the proof control and all resource and lifetime gates.
Only rigid, untextured, immutable-mesh, single-range opaque records sharing mesh,
range, sidedness, and shading model can join. Culled records contribute no draw;
every other incompatible record flushes the pending batch. Camera draws for static
shadow casters may join because the shadow pass has already recorded its own
unchanged caster draw. The candidate verifies the same shadow pose revision guard
used by individual camera draws before admission. A mismatch falls back to the
individual path and its existing failure behavior.

The ordinary rigid vertex shader applies an identity skin palette. The instanced
variant binds the same identity palette at vertex slot 0, with instance records
and visible indices at slots 1 and 2, and calls the same `ApplySkinning` function.
The CPU instance record matches the ordinary part uniform layout and material
fields, but SSBO versus UBO access and shader optimization still need exact GPU
pixel qualification.

`RendererDepthTests --contiguous-instancing` compares exact pixels with individual
draws for separated, overlapping, coplanar, reversed creation, nearly tied depth,
intersecting, shadow-casting, nonunit normal/tangent, and 64-object dense overlap
fixtures. It asserts actual native draw/object counts on Vulkan, and checks fallback
for multiple ranges, masks, and separate meshes. The benchmark reports
`ContiguousInstancedDrawCalls` and `MaximumInstancedBatchSize` with frame raw samples.
Do not enable by default or claim speedup until the exact pixel, draw-count, and
counterbalanced physical GPU gates pass.
