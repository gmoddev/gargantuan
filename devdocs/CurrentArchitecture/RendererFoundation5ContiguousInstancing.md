# Renderer Foundation 5 contiguous instancing experiment

`--sdl-instancing-order=proof|contiguous` selects same-binary admission controls when
`--sdl-instancing=on`. The default is `proof`; both native instancing and contiguous
admission remain opt-in. The contiguous variant is gated to SDL's Vulkan driver.

The counterbalanced AMD reference at source `acf088179` is retained in
`C:/Users/aiden/.codex/artifacts/renderer-foundation5-20261111/AMD-acf-instance-compact-ABBA/`.
At 50K static objects, proof instancing reduced actual camera draws from 50,000
to 98 per frame but raised CPU submission from 16.56–17.16 ms to 24.39–24.58 ms.
Detailed profiling attributed 19.80 ms to instance preparation, dominated by
per-object coverage and pairwise proof. The dynamic 50K case also uploaded
11.2 MB of instance records per frame. This is a failed proof-mode performance
experiment, not a throughput qualification for contiguous admission.

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
unchanged caster draw. Pose-bearing objects cannot enter this batch. The
individual path still verifies submitted pose revisions for GPU-skinned casters;
static casters have no pose receipt, and the frame's separate legacy caster flag
retains their shadow sampling.

The ordinary rigid vertex shader applies an identity skin palette. The instanced
variant binds the same identity palette at vertex slot 0, with instance records
and visible indices at slots 1 and 2, and calls the same `ApplySkinning` function.
The CPU instance record matches the ordinary part uniform layout and material
fields, but SSBO versus UBO access and shader optimization still need exact GPU
pixel qualification.

`RendererDepthTests --contiguous-instancing` compares exact pixels with individual
draws for separated, overlapping, coplanar, reversed creation, nearly tied depth,
intersecting, shadow-casting, nonunit normal/tangent, and 64-object dense overlap
fixtures, plus two consecutive native batches whose second draw has a nonzero
`first_instance`. It asserts actual native draw/object counts on Vulkan, and checks fallback
for multiple ranges, masks, and separate meshes. The nonunit-normal fixture uses an angled sun and compares with a flat-normal
control, so identical candidate/reference pixels alone cannot mask unused normals.
The benchmark reports
`ContiguousInstancedDrawCalls` and `MaximumInstancedBatchSize` with frame raw samples.
Do not enable by default or claim speedup until the exact pixel, draw-count, and
counterbalanced physical GPU gates pass.
