---
status: experimental
owner: rendering
related_code:
  - src/render/DirectionalShadowCache.cpp
  - src/render/passes/ShadowPass.cpp
  - src/render/Renderer.cpp
  - src/editor/EditorViewport.cpp
  - tests/DirectionalShadowCacheTests.cpp
  - tests/EditorViewportSmoke.cpp
---

# Directional shadow reuse

The SDL runtime and EditorViewport each retain a directional depth map and its
light matrix. A renderer-owned cache classifies immutable publication changes
against the previous projection. A reusable map skips caster traversal, draw
commands, and the depth clear. The shadow pass restores the matrix and the
submitted caster pose ledger consumed by world shading.

Visible caster additions, removals, transform and geometry changes, primitive
range changes, sidedness, and alpha coverage can invalidate the map. The cache
tracks the mesh identities and sampled alpha-mask identities used by the shadow
pass. Vertex changes and alpha texture rectangles invalidate only when their
identity is tracked. Pose updates and removals invalidate for visible casters.
Changes to RGB material channels and noncaster transforms preserve depth.
Full resync requires a fresh map. The active sun vector and transitions between
lit and sun-off states determine the directional projection. Exposure, sun color,
and positive sun-intensity adjustments preserve the depth map.
An empty committed map checks incoming objects for a newly eligible caster
without looking up every prior object. World shading skips depth sampling
when the shadow pass records no casters.

The dependency sets admit at most 32,768 mesh identities and 32,768 mask
identities. Overflow or missing caster residency leads to fresh shadow work.
A recorded map becomes reusable only after successful submission. A discarded
frame cannot publish its recorded candidate, and the next recording supersedes
it. Runtime counters separate pass encodes, actual caster draws, accepted cache
builds, and reuse. The viewport uses the same classifier and submission boundary.

Coverage uses a single 2048² D32 target, about 16 MiB of unpadded texels. The
world-origin light frustum covers 60 × 60 units in its light plane and spans
200 units in depth. It stays stationary under camera translation. One shadow
texel spans about 0.0293 units in the light plane. The four-tap filter scales
its disk by two texels, roughly 0.0586 units at unit radius. Receiver comparison
bias ranges from 0.0005 to 0.003 in depth, corresponding to 0.1 to 0.6 light-depth
units across the existing frustum. These settings define the compatibility
baseline for later acne, peter-panning, and coverage measurements. Moving
outside the fixed region uses unshadowed directional light.

The CPU gate checks publication relevance, submission, and overflow. The
offscreen gate compares static and camera-motion captures to reconstructed depth,
updates only alpha-mask texels, rejects a stale publication, and moves an actual
caster. The runtime gate checks that warm reuse emits zero additional shadow
passes and caster draws.
