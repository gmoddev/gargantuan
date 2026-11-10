---
status: experimental
owner: rendering
---

# Renderer Foundation 4 opaque ordering

The qualified Foundation 3 path traverses the current RenderProjection object
map, submits opaque and masked ranges in their published order, and uses strict
`LESS` with a cleared conventional-Z `D32_FLOAT` attachment. The opaque winner
for equal attachment depths depends on that traversal. This pass keeps its
relative order for every pair it cannot prove independent. Transparent sorting
still uses back-to-front object depth and `ObjectId` ties. The shadow pass has
its own submission order.

The Vulkan fast path considers runs of visible, rigid objects using the same
resident mesh and an equivalent ordered layout of two to eight opaque ranges.
A layout compares sidedness, shading model, and the five generation-safe
material texture bindings with their sampler states.
The material's other values remain per draw. Single-range objects, masks,
transparent-containing objects and animation poses retain ordinary submission.
The existing state cache still handles adjacent compatible draws on every path.

Immutable local bounds are derived once from the mesh positions submitted to
the GPU. A vertex-range upload invalidates this optimization for that residency.
The published bounds remain the existing camera-culling input. The ordering
proof uses the same view, projection and model matrices as the vertex shader.
It bounds the homogeneous box in double precision with an absolute-product
error allowance for the float vertex transform. A positive lower bound on w
permits outward bounds on the divided coordinates. Partially clipped boxes,
extreme transforms and unproven bounds drain the run and follow ordinary order.

A fixed 128 by 128 grid indexes conservative screen rectangles. A pair may
reorder when their padded screen rectangles are disjoint or their expanded
depth intervals are disjoint. Polygon rasterization interpolates depth from
vertex depth inside the triangle. This path excludes clipping and pads screen
edges by one eighth of a pixel and depth by 1/4096 per bound. Depth conversion,
interpolation and the possibility of a quantized tie therefore receive margin
beyond vertex transform error. A possible overlap and depth tie drains the
pending batch before admitting the current object. This preserves relative
order even when a conflict moves or a full resync changes projection traversal.
Other GPU APIs use ordinary order pending their own precision qualification.
[Vulkan rasterization](https://docs.vulkan.org/spec/latest/chapters/primsrast.html)

One batch holds at most 512 objects and each candidate may occupy at most 16
cells. A candidate exceeding 96 pair checks drains the batch. The grid and its
cell links have fixed capacity, with at most 8,192 links. Batch submission
visits ranges first and retains original admission order inside each range.
Because a batch contains only pairwise independent objects, this never adds an
`ObjectId` winner for an opaque depth tie. The previous Foundation 3 prototype
remains a separate performance receipt. It could reorder an actual tie.
[Foundation 3 experiment](RendererFoundation3.md#opaque-draw-grouping-experiment)

An opaque prepass would resubmit geometry. A subsequent `EQUAL` color pass
alone cannot recover which of two coplanar fragments won the depth prepass.
Depth bias, changing the comparison function and globally sorting by object ID
would each define a different visible rule. The bounded proof keeps the current
rule and reports when that rule limits reordering.

## Qualification

`gargantuan_renderer_depth_tests --opaque-ordering --output=<fresh-directory>`
compares exact RGB against a control using separate mesh identities with the
same positions, ranges and projection traversal. It covers screen separation,
depth separation, coplanar and near-equal layers, reversed creation order,
intersections, moving overlap, coverage budget, alpha masks, double-sided
ranges, shadow casters, resync and renderer recreation. Keep both PPMs even
when a pixel comparison fails. The test also verifies admission, conflict and
binding counts through the runtime SDL path.

Run the existing `mixed` timed fixture at 1K, 10K, 25K and 50K objects with
`--incremental --backend=sdl --synchronize=bounded-2 --samples=<fresh-csv>`.
The summary includes logical candidates, grouped objects, unproven bounds,
conflict and budget barriers, pair tests and depth proofs. Compare grouped
objects against candidates. Pair tests and depth proofs count cell visits and
can include the same pair twice. The raw CSV holds full-precision per-frame
host timings written outside the measured section. Compare the same driver,
target resolution and authored workload against the Foundation 3 checkpoint
with `--sdl-hdr=off`. Use `--sdl-hdr=on` for a separate HDR cost row. The timed
summary reports the requested and active HDR state, live HDR bytes and measured
resolve draws. The same switch controls the SDL material capture paths.
For a same-binary control use `--sdl-opaque-grouping=off` with the identical
timed fixture. Alternate enabled and disabled order across repeated runs.
CPU submission and completion tails measure different boundaries. Physical
GPU and software runs remain separate evidence.
