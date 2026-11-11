---
status: qualified-experimental
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

Solid casters use a dedicated fragment shader without alpha-test resources or
discard. Masked casters keep the established texture/alpha path; transparent
casters retain the existing solid-shadow policy. Both variants share the same
vertex shader, palettes, D32 depth/LESS/write and sidedness state. Pipelines are
created lazily at the first recorded non-masked caster, so shadow-off, reused
maps and masked-only frames do not allocate the optional pair. Failed creation
cleans partial resources and falls back to the established shader until renderer
recreation. This is adapter-owned optimization, with no authored/public switch.
The private test switch retains matched off/on and allocation-count controls.

Physical A/B/B/A at isolated source `0e717eed5b5af97c4ba8757125c347e9094538a1`
found lower opaque maximal-overlap cost on both adapters while masked controls
remained near flat. Exact single/double-sided, mirrored, transparent-as-solid,
cutout and mixed solid/mask pipeline-switch controls guard depth equivalence.
This supports splitting solid and masked work; it does not isolate an early-Z
hardware mechanism because fragment bindings/resources also change.

The runtime and viewport invalidate recorded directional and spotlight candidates
when command encoding, submission, or fence wait fails. A later frame with no
shadow work cannot commit depth from a canceled command buffer. The native GPU
fixture injects a one-shot post-shadow failure through a private test hook,
then checks configured-empty coverage followed by a fresh four-cascade rebuild
and exact reference pixels. Private viewport counters contain engine values,
without exposing GPU handles through the public editor transport.

The Foundation 4 single world-origin shadow target remains the default. The
physically qualified optional cascade path consumes only the
immutable render frame and projection. It uses one D32 2D-array texture with at
most four layers. Published settings permit 512, 1024 or 2048 pixels per layer;
the numerical planner's wider 256–4096 range is not the publication contract.
Receiver distance is capped. A practical logarithmic/uniform split
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
The next cascade fits from the previous cascade's transition start, so both
maps contain every receiver in their blend interval. Receiver selection uses
positive view depth. The array comparison sampler uses the established
Y-inverting shadow bias matrix and four rotated taps. World-space bias is
limited by the cascade's texel size before conversion to D32 depth units.

Each layer independently compares its expected matrix, target identity, sun
state, and publication dependencies. A cache entry is committed only after
successful GPU submission. It records visible caster mesh and sampled mask
identities, and publication changes use a conservative light-plane footprint
test for old and new caster poses. The footprint ignores light-axis depth so
off-slice casters remain eligible. Skinned objects bypass footprint culling.
A new pose or changed deformable bounds can move a previously culled caster
into any layer, so these changes invalidate all active layers when they affect
a visible caster. Masked alpha changes, geometry replacement, visibility and
ordinary caster movement invalidate affected layers. Dependency sets cap at
32,768 per type; overflow
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
resynchronization invalidate it. Each spot matrix is relative to its published
light position, which is subtracted from caster model translations before the
vertex shader and from receiver positions before shadow lookup.
The authored spotlight half-angle stops at 89° (outer cosine about 0.01745).
Direct render publications with an outer cosine below 0.01 remain fully
unshadowed, since the finite atlas projection cannot cover their wider rim.
The spotlight light-plane basis is continuous through both vertical directions
and the default -Z direction; its unavoidable singularity is at +Z.
An atlas assignment is sampleable on its first frame only after all caster
resources are resident and the layer pass has completed. The local-light grid
packs a one-based layer index for sampleable assignments and includes this
mapping in its cache key. A denied, evicted, or incomplete light shades
unshadowed rather than sampling another light's layer.

The combined optional directional and spotlight depth allocations are at most
96 MiB of logical texels at the default four 2048-pixel cascades plus eight
1024-pixel spots. This excludes native padding and in-flight retirement.
When authored cascades have no valid receiver interval or their array cannot
be allocated, the configured cascade path has zero layers and directional
lighting remains unshadowed. Local spotlights can still record. The private adapter
owns GPU resources; no Instance or backend handle enters publication.

`ShadowFoundation5GpuTests --point-cost <evidence-directory>` is a separate
bounded cost gate. It uses the production D32 spotlight array and shadow pass
with one, six, or eight active layers at 1024². The six orthogonal 90-degree
spot cones model the six depth faces that one point light would require;
production point shadows are not enabled. By default one double-sided masked
36-index cube encloses the light origin, so every active layer records one
equal caster draw. `--casters=N` (1–1024) repeats overlapping cubes and asserts
exactly layers × N caster draws. The 1000-caster receipts are synthetic maximal
overlap of alpha-masked geometry, not typical point-light scene cost. A small
published transform change invalidates every layer per frame. Four warmups and
24 measured frames alternate the workload order. The CSV records actual depth
passes and caster draws, upload operations/bytes, CPU shadow recording and
submission clocks, whole-frame GPU fence wait, and wall time for each frame.
It flushes each row and writes a failure status if a gate aborts. These host
clocks and whole-frame waits do not isolate GPU shadow time; receiver shading
and ordinary frame work are included in the fence. All three cases allocate
the same eight-layer atlas, so their resource allocation is 32 MiB logical
even though one, six, and eight active layers touch 4, 24, and 32 MiB.
Measured ratios must be reported per adapter before using this as a point-light
budget decision.

Physical Windows Vulkan qualification passed on RX 7900 XT and GTX 1650 SUPER:
same-binary no-shadow controls, temporal slow/fast camera captures, near/far
transitions, alpha foliage, animated/deformable casters, large-coordinate
images, atlas saturation/eviction, cache pass/draw counts, cancellation and
recreation. Final `ad58172cbd8516c9f95ba65c223efc123d6330b7` controls include a
far receiver at distance 200 and exact warm/fresh/lit comparisons at all three
published resolutions. Hosted Linux Vulkan/sanitizers are separate lanes;
physical driver removal and native Linux hardware were not exercised. These
are optional qualified features, not a default-enablement decision.
Neutral frame positions and transforms
remain single-precision; texel snapping and an origin-relative GPU matrix
cannot recover position detail already lost in publication. The world shading
path must apply the same cascade origin.

The focused GPU fixture in `tests/ShadowFoundation5GpuTests.cpp` compares
actual viewport pixels against no-shadow controls and fresh full publications
after camera motion. It exercises split boundaries, distant directional and
spot coordinates,
alpha reimport, deformable and newly posed casters, spotlight toggles, atlas
saturation/eviction, and rejected publications. A paired offscreen runtime
renderer checks actual depth-pass and caster-draw counters. Exact source,
binary, shader and raw physical capture identities are recorded in
`../Validation/RendererFoundation5Qualification.md`.
