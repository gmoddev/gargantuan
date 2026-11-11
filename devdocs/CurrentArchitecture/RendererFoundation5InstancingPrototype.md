---
status: experimental
owner: rendering
---

# Renderer Foundation 5 instancing prototype

This branch holds an opt-in renderer-owned instance cache and a native camera
draw prototype. `--sdl-instancing=on|off` selects it in the incremental SDL
benchmark; the default remains off. Do not count resource allocation or
planned uploads as an instancing speedup. The accepted production renderer
remains the Foundation 4 individual-draw and conservative opaque-grouping path.

## Record and update model

Each admitted publication `ObjectId` maps to one stable native slot containing
the 224-byte untextured opaque vertex record layout. The record includes model
and normal transforms, authored material factors, and flags. Generation-safe
removal deletes the identity mapping; insertion may reuse the freed native
slot. Picking continues to use the neutral `RenderProjection` and its original
ObjectId. Full resync resets slots; renderer recreation marks all slots for
upload. A 65,536-slot capacity bounds record storage to 14 MiB. Exceeding that
limit disables this optional cache until a full resync, while ordinary draws
remain the intended fallback.

Sparse changed slots coalesce into contiguous upload ranges. More than 256
ranges or more than one quarter of slots dirty selects one complete upload.
The GPU helper owns a persistent storage-read buffer and reusable upload
transfer buffer, growing to a power-of-two capacity. It records copy work
before a render pass and preserves untouched destination slots on sparse
updates. Transfer staging may cycle; partially updated device storage does
not. The cache clears dirty state only after the containing command buffer is
submitted. Aborted submission marks all records dirty for retry. Buffer
allocation, transfer mapping, and copy-pass failure are explicit exceptions
for the caller's renderer recovery path.

The visible-index stream selects slots after camera culling while retaining
original object order. Batches use Foundation 4's conservative pairwise
screen/depth independence proof. The first GPU path admits only rigid,
untextured, single-range, opaque objects sharing one immutable mesh and
compatible sidedness/shading. It additionally excludes shadow casters until a
separate shadow instanced path qualifies. Masks, blending, skinning, deformable
vertices, unavailable residency, and unproven overlap retain individual draws.
The rigid shader variants consume vertex storage slots 0 and 1 with no skin
palette binding.

The separate compact visible-slot buffer uploads only when its slot sequence
changes and acknowledges that sequence only after submission. Its 32-bit slot
indices are bounded by the same 65,536-record limit. The opaque pass records
real `SDL_DrawGPUIndexedPrimitives` calls with instance count above one and
reports draw, object, record-byte, and visible-index-byte counts. The shadow
shader is prepared but not bound. `gl_InstanceIndex`/first-instance behavior,
exact depth pixels, recovery, and throughput still require physical GPU
verification before admission.

The benchmark records cache application, record upload, and batch preparation
(including the visible-index upload) as separate coarse CPU phases when the
instancing control is on. These clocks are `NA` with the control off. Total
frame and submission phases remain the causal A/B outcomes; nested phases
must not be added together.

`tests/RendererInstanceRecordCacheTests.cpp` exercises full and sparse uploads,
unchanged updates, dense updates, generation-safe deletion/reinsertion,
recreation, clear, and capacity fallback. A bounded local MSVC `/O2` build and
run passed on 2026-11-11. `RendererDepthTests --instancing-ordering` compares
exact viewport pixels against individual draws across separated and overlapping
depth, material/range exclusions, frustum and visibility changes, rehash,
generation replacement, sparse and dense updates, full resync, and recreation.
That native test has passed syntax checks but awaits physical GPU execution.
Physical upload, draw-count, exact-depth, recovery, and performance
qualification remain required before integration.
