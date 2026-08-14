# Render extraction boundary

Gargantuan renders one immutable, owned `RenderSnapshot` rather than allowing
the renderer to traverse the mutable DataModel. The dependency rule is:

> Renderer/pass code consumes immutable extracted state and does not traverse
> the DataModel.

This pass establishes the state boundary only. The renderer, render passes,
targets, pipelines, and resource cache remain SDL GPU implementations. A
backend-neutral `RenderDevice` is explicitly deferred.

## Frame ownership and timing

Normal runtime frames execute committed simulation and `PreRender` callbacks
on the authoritative Main domain, then perform one extraction immediately
before `Renderer::Draw`:

```text
simulation and committed mutations
    -> PreRender
    -> RenderExtractor
    -> shared_ptr<const RenderSnapshot>
    -> SDLRenderer and passes
```

`RenderExtractor` is the only layer in this flow that reads `WorldRoot`,
`Camera`, `Part`, or other mutable renderable runtime objects. It completes and
validates a candidate snapshot before returning it. A failed fatal extraction
does not publish the candidate or advance snapshot identity.

Extraction currently runs synchronously on the Main domain. Drawing is also
synchronous today. Because a published snapshot owns only values and is passed
as `shared_ptr<const RenderSnapshot>`, a future render thread can retain a
complete snapshot without reaching back into authoritative state. No render
thread is implemented now.

## Snapshot shape

`RenderSnapshot` owns:

- a monotonic extraction identity;
- target width and height;
- camera position, orthonormal directions, view/projection/view-projection
  matrices, field of view, and clipping planes;
- normalized light direction;
- deterministically `ObjectId`-ordered render items; and
- explicit diagnostics for individually rejected items.

Each `RenderItem` owns the stable `ObjectId`, logical primitive geometry,
model/inverse-model matrices, color/opacity, and shadow flag needed by current
passes. It contains no `Instance*`, `shared_ptr<Instance>`, `WorldRoot`,
`Camera`, callback, or DataModel-owned container.

The inverse model matrix supplies frame-local picking bounds. Picking returns
the item's generation-checked `ObjectId`; no pointer-based picking map exists.
An old displayed snapshot may still identify an object that has since been
destroyed, but `ObjectRegistry` correctly rejects that stale identity. A newly
extracted snapshot excludes the destroyed object.

## Snapshot identity

`RenderSnapshotId` is an unsigned 64-bit extraction sequence. Zero is invalid,
the first successful extraction is 1, and each successfully published complete
snapshot increments it once. Fatal extraction failure does not increment it.
The counter fails closed at its maximum and never wraps.

This identity describes extraction, not presentation. It is independent from
`ObjectId` generation, runtime-schema generation, journal sequence, and the
EditorHost shared-memory frame number.

## Validation and failure behavior

Invalid viewport dimensions, camera vectors/projection values, light state,
dead world roots, off-Main extraction, and identity exhaustion are fatal to the
candidate frame. No snapshot is returned.

An isolated dead/stale item, unsupported primitive, singular/non-finite
transform, or non-finite visual state is omitted with an owned
`RenderExtractionDiagnostic`. The resulting snapshot is still a complete
description of all accepted items rather than a half-mutated structure. A
missing SDL primitive resource is a recoverable draw skip with an explicit
renderer diagnostic.

## Camera sources

Both render paths construct the same `RenderCameraInput`:

- gameplay copies the current runtime `Camera` at extraction time; and
- EditorHost stores its bounded viewport camera as plain session state rather
  than creating a camera Instance in the project.

The extractor derives all matrices once. Passes never call camera methods or
read camera properties.

## Geometry and resource ownership

`Part` exposes only logical shape state. `BasePart` and `Part` no longer include,
own, or return `GpuMesh`. Extraction maps the current `PartType` to the narrow
`RenderGeometry` key.

Each SDL renderer instance owns a `GpuMeshCache` for its own GPU device. Passes
resolve snapshot geometry through that cache. This also prevents the normal
window renderer and EditorHost offscreen renderer from sharing device-specific
GPU pointers accidentally. The primitive mapping preserves current visual
behavior; this is not an imported-mesh or material system.

## EditorHost viewport

EditorHost capture uses the same `RenderExtractor` and `RenderSnapshot` contract
as runtime rendering. Its offscreen target, readback, shared-memory ring, and
bounded protocol remain editor-specific presentation details. Picking consumes
the most recently displayed snapshot when available, so pixels and selection
identity refer to the same extracted frame.

The public EditorHost protocol and `ViewportControl` capability checks are
unchanged. Snapshots, GPU handles, SDL objects, and renderer internals are not
exposed over the protocol or to Luau.

## Deferred rendering work

Not implemented here:

- `RenderDevice` or backend-neutral resources;
- Vulkan, Direct3D, or Metal backends;
- a general asset/material/texture system;
- render threading, culling, batching, or scene replacement; and
- GUI, gizmo, animation, or particle rendering architecture.

Those concerns can now evolve below or alongside the immutable extraction
boundary without reintroducing DataModel traversal into render passes.
