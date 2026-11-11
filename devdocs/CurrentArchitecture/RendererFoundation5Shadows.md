---
status: experimental
owner: rendering
related_code:
  - src/render/sdl/SDLDirectionalCascadePlan.hpp
  - src/render/sdl/SDLDirectionalShadowCache.hpp
  - src/render/DirectionalShadowCache.cpp
  - src/render/sdl/SDLSpotShadowAtlas.hpp
  - src/render/sdl/SDLShadowResources.hpp
  - src/render/passes/ShadowPass.cpp
---

# Renderer Foundation 5 shadow prototype

The Foundation 4 single world-origin shadow target remains the default until
the physical GPU path is qualified. The optional cascade path consumes only the
immutable render frame and projection. It uses one D32 2D-array texture with at
most four layers. The current private quality policy permits 256–4096 pixels
per layer and caps receiver distance. A practical logarithmic/uniform split
places more resolution near the camera. Every split fits a stable frustum-slice
bounding sphere, reserves a two-texel edge guard, and snaps its light-plane
center to a texel grid using double-precision calculations. The GPU matrix is
relative to a coarse camera origin; the shadow pass subtracts that origin from
each published model translation before vertex transformation. The depth center
snaps to 32-world-unit increments to permit reuse during small motion along
the sun axis. This depth interval conservatively includes off-slice casters.
The shading pass must use view-space receiver distance, blend within the
published transition interval, and use authored depth/normal bias. Outside
the last split, directional light is explicitly unshadowed.

Each layer independently compares its expected matrix, target identity, sun
state, and publication dependencies. A cache entry is committed only after
successful GPU submission. It records visible caster mesh and sampled mask
identities, and publication changes use a conservative light-plane footprint
test for old and new caster poses. The footprint ignores light-axis depth so
off-slice casters remain eligible. Skinned objects bypass footprint culling;
pose changes always invalidate relevant maps. Deformable vertex edits,
masked alpha changes, geometry replacement, visibility and caster movement
invalidate affected layers. Dependency sets cap at 32,768 per type; overflow
falls back to recording fresh depth. Cache hits encode no depth pass or caster
draw. The legacy one-map cache API remains available for the default path.

The spotlight prototype uses a second D32 array with eight 1024-pixel layers
(32 MiB of logical depth texels). Each selected spotlight owns a whole layer,
so a changed light can clear and redraw its layer without affecting neighbors.
Candidate selection is bounded and deterministic: visible authored spotlights
with shadows enabled sort by priority, estimated contribution, then ObjectId.
Retained winners keep their layer; new winners occupy a free layer. Excess
spotlights continue to illuminate without shadows. Point lights cannot
acquire an atlas layer. A static spotlight map is reused until its light cone,
target, or relevant caster/mesh/mask publication changes. Recording only
becomes committed after successful submission. Device recreation and full
resynchronization invalidate it.

The combined optional directional and spotlight depth allocations are at most
96 MiB of logical texels at the default four 2048-pixel cascades plus eight
1024-pixel spots. This excludes native padding and in-flight retirement.
Resource creation failure falls back to the established unshadowed local
light behavior or the Foundation 4 directional map. The private adapter
owns GPU resources; no Instance or backend handle enters publication.

Qualification still required before default enablement: a same-binary
feature-on/off GPU run, temporal captures for slow and fast camera movement,
near/far transition captures, alpha foliage and animated/deformable casters,
large-coordinate images, atlas saturation and eviction churn, pass/draw counts,
and Windows/Linux device recovery. Neutral frame positions and transforms
remain single-precision; texel snapping and an origin-relative GPU matrix
cannot recover position detail already lost in publication. The world shading
path must apply the same cascade origin.
