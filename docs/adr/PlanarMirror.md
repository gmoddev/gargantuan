---
title: First-class planar Mirror
status: proposed
owners: [renderer, runtime-schema, editor-host]
last_verified: 2026-11-24
evidence_revision: 7c0aca159
---

# First-class planar Mirror

Foundation 9 delivers the ViewportFrame offscreen pass and establishes a path
for Mirror. This proposal reserves no schema identity and adds no Mirror
runtime, shader, Studio control, or promotion dependency.

## Decision

Implement a dedicated, visual-only `Mirror` Instance. It represents a finite
oriented rectangle and automatically projects its owning view's reflected
world into that rectangle. Developers place it by transform and size in
Workspace. They can parent it to a rigid object through the existing semantic
spatial resolver if that path supports a local transform. The initial release
ships standalone world transforms and adds attachment support after the
resolver contract is verified. Gameplay collision and backing geometry remain
ordinary separately authored Parts or MeshParts.

A component on BasePart would provide automatic following, but makes the
reflection plane and active face depend on inherited shape and collision
choices. A general ReflectiveSurface would need arbitrary mesh UV, face
selection and nonplanarity rules before it could provide a deterministic
plane. A special physics body would couple an optional visual pass to
collision residency. The dedicated rectangle keeps the reflection plane
explicit and lets existing parts continue to model decorative or physical
frames. It can later serve as a renderer primitive for richer reflective
materials.

## Public proposal

| Property | Initial semantics |
| --- | --- |
| `CFrame` | Rectangle center and orientation. Local +Z faces viewers and defines the positive plane half-space. |
| `Size` | Positive finite Vector2 width and height in world units. |
| `Enabled` | Enables contribution to camera-specific scheduling. Default true. |
| `Tint` | Linear RGB multiplier for the reflection. |
| `Reflectivity` | Finite scalar [0, 1] mixing the base surface and the cached reflection. Default 1. |
| `BaseColor` | Opaque material fallback and the back side. |
| `ResolutionScale` | Requested scale of projected rectangle pixels, clamped by runtime budget. |
| `MaxUpdateRate` | Upper bound on redraw cadence. |
| `MaxRenderDistance` | Distance beyond which the renderer uses the fallback. |

Keep roughness at a sharp fixed response initially. An optional authored
`Roughness` can follow a bounded reflection mip design once pixel equivalence
and cost evidence exist. Do not expose a camera, render texture handle,
replication override, arbitrary render-layer mask, or per-Mirror recursion
depth. A later filtering property may narrow a client's existing visible
snapshot by an accepted renderer filter capability. It must preserve network
visibility. Quality toggles can disable or reduce all reflections locally.

## Alternatives

| Approach | Fit for the intended reflection |
| --- | --- |
| Reflected camera with a plane clip | Correct parallax and off-screen geometry for the supported world. Adds a bounded scene draw. |
| Screen-space reflection | Reuses visible depth and color. Cannot recover geometry outside the main screen or behind occluders. Useful later as a supplement for rough surfaces. |
| Reflection probe | Current static cube probes approximate ambient response. A probe cannot reproduce dynamic rectilinear room-scale parallax or intersection. Useful as a material fallback. |
| Hybrid | A clipped planar view can supply the main rectangle, with probes for distant rough fallback. Avoid two techniques on sharp pixels initially. |
| Hardware ray tracing | Deferred until device requirements, scene acceleration structures, animated topology, fallbacks and SDL GPU/backend portability are established. |

The current SDL GPU renderer already compiles GLSL to SPIR-V, creates explicit
sampled color and depth targets, submits independent passes and binds
generation-safe resources. Planar rendering needs no new device feature.
Fragment-plane clipping works with existing vertex/fragment interface
extension, subject to proof on the pinned shader path. Hardware
`gl_ClipDistance` or depth-clamp assumptions need separate SDL/Vulkan evidence.
See the [renderer backend boundary](../../devdocs/CurrentArchitecture/RendererBackendBoundary.md)
and the [render publication rules](../src/content/docs/developing/render-extraction.mdx).

## Renderer integration and reflection math

Publication reports mirror ID, live generation, transform, dimensions and
settings as immutable render values. It never sends the live Instance or a
camera reference. A secondary pass reuses the main view's accepted
`RenderProjection`, material descriptors, mesh identities, animation
palettes, and current environment values. The pass performs its own
world-space frustum, authoring-plane and distance tests. Extracting a second
copy of the world or sharing the main screen's visible draw list would omit
objects visible solely in reflection.

For a finite unit plane normal `n`, plane point `p`, and
`d = -dot(n,p)`, reflect a world point by
`R(x) = x - 2*n*(dot(n,x)+d)`. Reflected directions use the linear term
`I - 2*n*n^T`. Prefer the derived view matrix `V_reflect = V_main * R`
over rebuilding a LookAt that silently flips horizontal parity. The
reflection has determinant -1. Use an explicit flipped-front-face pipeline
variant for single-sided drawing and verify double-sided normal response
including tangent handedness and normal maps. Query reports, picking, and
gameplay view visibility still use the ordinary camera.

Clip reflected geometry to the same side of the plane as the source camera.
For the first backend proof, pass the world plane to the reused material
shader and discard fragments on the rejected side with a small world-space
bias. Apply it to masked and transparent variants too. A later oblique
near-plane projection can improve precision and early depth rejection.
Derive its row replacement for the engine's right-handed zero-to-one
convention, including the far corner opposite the plane and main projection
recovery. Validate the clipped boundary before enabling that variant. When
the camera lies on or behind the plane, use the opaque fallback and avoid an
unstable oblique view. A rectangle crossing the main near plane can remain
eligible via conservative coverage clipping.

Project each main-pass rectangle fragment through the cached reflected
view-projection, divide by W, and use the SDL GPU target UV convention.
Screen-aligned UVs are insufficient under movement and off-axis capture.
Record the rectangle material pass after opaque world depth and before the
main transparent pass so occluding world geometry wins. The reflection target
is scene-linear HDR when supported, with main view exposure and tone mapping
applied once during its existing scene resolve. The direct SDR backend needs
a separate tested color variant and fallback. When resources or a valid view
are unavailable, draw the regular tinted surface immediately.

## World contents and bounded recursion

Render opaque, masked and the existing per-view sorted transparent geometry
in the reflection. The pass uses the same skinned mesh and native palette
generation as the main view. Dynamic objects and animated Characters remain
in local snapshot scope and receive a reflected camera visibility signal once
the pass proves their visibility. Schedule extra animation evaluation through
the existing policy and one bounded merged feedback set. Preserve world pose
publication authority. Effects unsupported by the current world draw
pipeline, including future particles, beams or trails, remain absent until
their own secondary-view capability is implemented.

The initial policy permits one reflection level. Other Mirrors render their
base material without reflection inside the secondary pass. Cameras sample no
other active target during reflection recording and no target ever samples
itself. Reflection through arbitrary transparency chains remains limited by
the main material ordering model. Recursion beyond one level would need a
distinct promotion with explicit graph-cycle rejection, budget and measured
benefit.

World-space directional and local shadow resources may be reused only when
their coverage contains the reflected frustum subset being drawn and their
caster generation matches. Main camera cascades typically do not cover the
secondary view. Initially use shared Lighting, environment maps and no
reflected shadow pass. Enable qualified reuse by light/caster/generation,
otherwise retain the unshadowed fallback. Do not allocate a new shadow atlas
for each mirror.

## Scheduling, memory and synchronization

Start with at most two drawable mirror targets per camera surface and one
recorded update per frame. Bound the long edge to 512 pixels and round it up
in dimension buckets. Two RGBA16F/D32 pairs at 512 square account for 6 MiB
of active attachment payload before driver padding, staging or retired
resources. The scheduler uses screen coverage, positive-side facing and
distance, then age and stable identity for deterministic tie-breaking.
Camera changes, animated/dynamic scene revisions, mirror property changes and
surface resize mark a target dirty. Use rate clamps and last-update age in a
round robin within comparable priorities so one close Mirror cannot starve
all others. Start with conservative coverage and measured pixel-cost caps.
Occlusion queries can arrive later if profiling shows view draw cost exceeds
visibility-query cost.

Cache targets by renderer generation, mirror identity, size bucket and
format. A texture is never owned by an Instance. Resource replacement first
allocates a complete target pair, then defers release of the old pair through
SDL's submission lifetime. Reuse a retained attachment when possible and
avoid cycling an unchanged cached image. Track current allocated bytes, peak
active bytes, creation/retirement counts and in-flight allocations; reject or
reuse when a hard retirement bound is reached. Use command ordering for
secondary attachment writes before the main shader samples them. Validate
queue sync and fence recovery on Vulkan. A cached camera/rectangle pair may
remain visible within a bounded staleness window, using the matched
view-projection that produced it. Mode changes, invalid generation and
camera plane crossing invalidate stale captures. Device recovery rebuilds
targets from an ordinary full resync.

ViewportFrame supplies a pattern for backend-owned offscreen targets,
recording, identity, composition, metrics and retirement. Mirror shares these
private mechanisms where measured duplication justifies it. ViewportFrame
publishes an authored, isolated GUI scene with its own Camera and lighting.
Mirror derives a reflected view of the current world's allowed projection.
They have no shared public class hierarchy and use separate priority/budget
policies. If both compete for renderer resources, an aggregate attachment
budget prevents one feature from saturating the device.

## DataModel, replication and editor

Mirror uses ordinary reflected instance writes and journal revisions through
the mutation gateway. Validate finite bounded transforms and dimensions,
authenticate mutation normally, and cap creation by existing graph limits.
An owner can change only its local presentation setting. A client can render
only geometry already admitted by normal world relevance, streaming and
asset residency, even when a mirror frustum points at unrevealed regions.
A mirror cannot change streaming demand, create a hidden DataModel, expose
private PlayerGui, resolve arbitrary asset paths, or transmit GPU images.
The headless server retains serialized state and allocates no visual
resources. Readback remains the existing capability-protected EditorHost
surface.

Studio creates and inspects Mirror as a normal authorable object. Its gizmo
shows the rectangle and active normal and lets the editor rotate and resize
the plane. Use normal SetProperty/transform transactions and undo. Static
authoring mode may display the real render result, with an editor-only
reflection quality toggle that writes no project properties. Picking returns
the physical rectangle's ID and never redirects to reflected objects.
JSON/GASM/GASP store reflected semantic properties with stable schema ID and
no transient targets or world camera state. Package dependencies only expand
from actual authored asset references, if a future material property adds
them.

## Phases and acceptance

| Phase | Evidence |
| --- | --- |
| Offscreen backend probe | Isolated Vulkan experiment compares a normal view and `V_main * R` using near-plane, asymmetric and off-screen fixtures. Prove winding, viewport UV direction and draw/retire ordering before public schema work. |
| Bounded runtime | Authoring and runtime lifecycle, plane-side camera edge cases, dynamic rigid and skinned geometry, alpha masking, transparency, active-front fallback, distance limits, resize, retry, teardown and device recreation. Prove views with only off-screen reflected geometry. |
| Studio and persistence | Creation, property edits and undo, transform gizmos, EditorHost capture and picking, GAS/JSON round-trip, authored preview off/on, independent client quality, no secret/private data, negative permission fixtures. |
| Performance | Baseline, one big mirror, many tiny/hidden mirrors, overlapping mirrors, fast camera, low-rate dirty scenes, animated/transparent scene, churn, with and without simultaneous ViewportFrames. Measure CPU extraction, record and submit, available GPU timing, fence wait, maximum active and retired bytes, allocation counts and frame percentiles. |
| Additional quality | Evaluate oblique depth, shadow coverage reuse, roughness/mip filtering, graphics backend parity and allocation pooling. Keep each feature behind its own measured acceptance and fallback. |

Current dependencies are renderer resource/projection lifetime, reusable
material/mesh/animation state, secondary visibility feedback and SDL
shader/pass support. This proposal does not prove performance or implement
plane clipping, front-face inversion, Mirror editor UI, reflected shadows,
multi-bounce rendering, or per-backend parity.
