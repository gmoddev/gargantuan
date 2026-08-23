# Render publication boundary

Gargantuan publishes immutable renderer values rather than allowing renderer
code to traverse the mutable DataModel. The dependency rule is:

> Renderer/pass code consumes immutable publication state and does not traverse
> the DataModel.

Foundation 2B makes incremental `RenderPublication` the supported runtime and
EditorViewport path. `RenderSnapshot` and `RenderExtractor` remain full-state
compatibility/recovery helpers, not the normal per-frame renderer contract.
Foundation 2C adds neutral texture lifecycle and selects SDL GPU after real
deformable/GUI tests; see [Renderer Foundation 2C](RendererFoundation2C.md).
Soft-body Physics Foundation 1 now supplies the first real cloth/rubber producer
for the previously established deformable mesh contract; see
[Soft-body Physics Foundation 1](SoftBodyPhysicsFoundation.md).

## Frame ownership and timing

Normal runtime frames execute committed simulation and `PreRender` callbacks on
the authoritative Main domain, then publish immediately before drawing:

```text
simulation and committed mutations
    -> semantic render-dirty accumulation
    -> PreRender
    -> RenderPublisher
    -> shared_ptr<const RenderPublication>
    -> BaseRenderer
    -> renderer-owned RenderProjection and GPU resources
```

Publication and drawing are synchronous today. A publication owns only values,
so a future render thread may retain it without reaching into authoritative
state. No render thread is implemented.

## Dirty ownership and coalescing

Authoritative committed changes feed one renderer-specific
`RenderDirtyAccumulator`. It stores stable `ObjectId`, semantic domain flags,
version cursors, byte estimates, and bounded diagnostics. It stores no raw or
shared Instance pointers and is independent of the general `ChangeJournal`
retention window.

Domains are transform, material, visibility, geometry, deformable geometry, and
hierarchy/render presence. Repeated writes union flags into one entry and
`RenderPublisher` reads the final authoritative object state once. Create plus
updates produces one create; create plus destroy disappears; update plus destroy
produces remove. Generational identities keep remove/recreate unambiguous.
Irrelevant property writes create no render work.

Capture does not clear dirty state. The publisher acknowledges an exact captured
version only after constructing and validating the complete immutable candidate.
Multiple consumers, including the runtime and Play viewport, have independent
cursors; entries are reclaimed only after every live consumer acknowledges
them.

## Publication and projection shape

`RenderPublication` owns:

- a monotonic identity and exact incremental base;
- full-resync state;
- viewport, camera, matrices, and lighting;
- deterministic object creates, classified updates, and removes;
- generation-safe mesh create/remove/binding and deformable vertex ranges;
- generation-safe texture create/update/remove state;
- transient renderer-neutral UI batches; and
- bounded owned diagnostics.

No publication contains an `Instance*`, `WorldRoot*`, `Camera*`, physics object,
callback, DataModel-owned collection, or GPU handle.

`RenderProjection` applies a candidate atomically. Stale, duplicate, skipped,
or malformed incremental state is rejected without partial mutation. A valid
full resync independently replaces the disposable projection. The projection
contains value objects ordered by stable identity and supplies the state used by
render passes and frame-local picking.

## Bounds and failure behavior

The default semantic limits are 64 scopes, 64 live consumers, 131,072 distinct
dirty identities per scope, 32 MiB estimated deformable dirtiness, 32 MiB UI
data, a 64 MiB publication, and eight retained diagnostics per scope. These
bounds count final semantic state rather than raw property mutation volume.

Crossing a hard limit requests deterministic full resync and emits no partial
delta. Full resync is also used for initialization, renderer/viewport restart,
future device-loss recovery, and explicit debug comparison. It is generated
from authoritative state, never from the renderer projection. Renderer failure
may discard GPU/projection state and request recovery but cannot mutate the
DataModel.

Invalid viewport/camera/light values, stale operations, duplicate identities,
revision regressions, invalid indices/ranges, invalid clipping, and non-finite
geometry fail validation. Individually unrenderable authoritative objects may be
omitted with an owned extraction diagnostic.

## SDL resources and EditorViewport

Each SDL renderer owns its `RenderProjection`, primitive/dynamic mesh cache,
pipelines, targets, and handles. Transform/material/visibility updates preserve
geometry resources. Dynamic topology allocates persistent vertex/index buffers;
stable topology updates only validated contiguous vertex ranges.

EditorViewport uses the same publisher, projection, and mesh path. Viewport
creation/recreation requests full resync, ordinary Edit/Play changes are
incremental, resize updates frame-global state and replaces targets, and picking
reads the projected values associated with the displayed frame. Studio receives
only RGB frame data and stable `ObjectId`, never renderer state.

## Remaining rendering work

Not implemented here:

- a general renderer-neutral asset/texture residency system;
- generation of UI batches from GUI layout, text shaping, and semantic objects;
- the production SDL UI pass, clipping/blending/transient buffer allocator;
- render threading, instance batching, culling, or scene streaming;
- soft-body self/soft collision, tearing, and networked deformation; or
- a generic RHI or backend migration.

Backend Decision C remains in force until SDL and an alternative consume the
same publication and run visually equivalent rigid, deformable, UI, and mixed
GPU workloads.
