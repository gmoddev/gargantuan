---
status: qualified-experimental
owner: rendering
related_adrs:
  - RendererFoundation2C.md
related_code:
  - src/render/RenderPublisher.cpp
  - src/render/ReflectionProbes.cpp
  - src/render/passes/OpaquePass.cpp
  - src/render/passes/ShadowPass.cpp
---

# Renderer Foundation 5

This isolated continuation extends Foundation 4 at
`94f920485564798742bc22a061cbc3a415034fa3`. It does not merge experiments into
main or change production physics. SDL GPU remains the renderer receiving
long-term investment; Filament remains a material reference. Nothing in this
campaign changes Instance authority, asset identity, journal history, or Studio
command authority.

## Implemented boundaries

ReflectionProbe is an ordinary schema-backed Instance. The existing publisher
extracts bounded, immutable probe values and existing Image identities; the SDL
adapter owns every prefilter, cube-array layer, staging buffer and fence. Up to
32 coherent probes publish after at most 256 enabled-source inspections. Twelve
are resident, with at most two sampled per pixel and one local prefilter in
flight. Authored oriented volumes, priorities, blending, capture offsets and box
projection support spatial environment lighting. These are imported/authored
environments, not automatic scene capture or global illumination. Artistic
global ambient remains independent of the local/global IBL weights.

Optional directional shadows use up to four camera-relative cascades with
per-layer dependency caches, texel alignment, overlapping transition coverage,
origin-relative matrices and bounded bias. Publication accepts 512, 1024 or
2048 pixels per layer; the default is 2048. Cascade count zero retains the
Foundation 4 map. Eight 1024-pixel spotlight layers have deterministic admission,
stable assignments, static reuse and explicit unshadowed fallback. At these
defaults the two optional depth arrays total 96 MiB of logical texels. Point
shadow requests remain unsupported; no stale or foreign layer is sampled.

Radiance RGBE square faces and 2:1 panoramas import through AssetService. The
panorama produces six stable compound Image records. Canonical version 7 stores
finite linear float32 pixels; existing asset ownership and reimport identity
remain intact. Known unsupported color transforms, conflicting FORMAT fields,
nonunit pixel aspect, truncation and excessive inputs fail before catalog
commit. Per-face decoding supports mixed RGBA8 and linear HDR inputs. EXR and
an unrestricted high-resolution environment pipeline are not implemented.

The existing texture cache now bounds strong source backing and material mip
views independently at 128 MiB each. Demand queues, cold eviction, bounded
uploads and weak exact-revision source reacquisition preserve defined base-level
fallback. Base upload staging is capped at 16 MiB and retained material staging
at 8 MiB. These logical budgets do not describe total native GPU memory or
replace AssetService's existing canonical-data budget. Compression and complete
base-texture streaming are deferred.

Instance GPU records are renderer-owned derivatives keyed by complete ObjectId.
They support sparse/dense updates, removal, generation replacement, full resync,
abort acknowledgement and bounded capacity. Vulkan contiguous single-range
batching preserves original object/primitive order. Textures, masks, transparent,
posed/deformable and multiple-range cases retain ordinary draws as applicable.
Picking remains projection-based. The 224-byte record has explicit layout
proofs; byte equality avoids a measured scalar-comparison cost while retaining
every final-state field. Dirty-domain hints do not mask material updates.

The default candidate walk uses the same map order as ordinary drawing and
reuses a preclassified cull outcome. The optional compact traversal rebuilds the
actual map order after reserve and retains its storage capacity. Neither changes
the Foundation 4 coplanar winner. A separate optional scene envelope rejects
only a conservatively bounded, wholly outside static scene; intersecting,
unknown and changing cases retain ordinary classification. No BVH, occlusion
system, authored mesh LOD chain or destructive simplifier is claimed.

## Qualification and admission

Runtime checkpoint `5250798037753bb9d8998576701b0ec29e78f2f4` and test checkpoint
`ad58172cbd8516c9f95ba65c223efc123d6330b7` have physical Windows Vulkan evidence
on RX 7900 XT and GTX 1650 SUPER. CPU tests, shader/software Vulkan and sanitizer
lanes are recorded separately in the qualification document. Exact-pixel
controls cover ordering, nonunit normals, positive first-instance offsets,
fallbacks, probe/HDR equivalence, sky transitions, shadow masks, noon motion,
camera motion, far receivers, three shadow resolutions, large coordinates,
reimport and recovery. Two independent 4096-frame probe churn cycles per adapter
exercise source changes, camera movement, resync, resizing and resource retirement.
These are bounded qualification runs, not a multi-hour or universal-device claim.

Final 84-run controls per adapter use A/B/B/A, 120 measured frames and 30 warmup
frames at 1280x720 HDR with two frames admitted. At 50K static objects, contiguous
batching records 98 world draws instead of 50,000. Mean CPU submission across
the two runs per arm falls from 14.96 to 12.56 ms on AMD and 21.46 to 11.86 ms on
GTX. Mean per-run frame P95 falls from 21.03 to 18.43 ms and 28.99 to 17.07 ms,
respectively. The frame P95 for 50K fully dynamic objects worsens on both
adapters; fewer draws alone do not qualify a speedup.

Instancing remains opt-in. It is a qualified foundation for selected homogeneous
rigid workloads, not a universal default. The first coverage-proof approach is
rejected for throughput, while its ordering controls remain useful. Compact
traversal also regresses the measured 50K controls and is not promoted. The
scene envelope reduces wholly culled/camera-away submission to approximately
0.02–0.06 ms after its bounded build, with matching draw and cull counts. It
remains an explicit option; it does not accelerate intersecting scenes.

Native GPU execution timestamps are unavailable through the pinned SDL public
API. Host recording, submission, readback and full-frame fence latency have
separate labels. Profiling-only sample clocks and allocation counts are
attribution evidence, not performance comparisons. C++ allocation counting uses
a separate binary and excludes SDL C allocations, driver allocations and other
threads.

## Investment and limits

Keep the qualified SDL foundation and these renderer-neutral authoring contracts.
Use local probes and bounded cascades/spots where their authored semantics fit.
Preserve the ordinary draw path as the default and reference while admitting
instancing for measured workloads. The next scalability work should address
publication-derived candidate/update costs and texture/geometry residency before
adding a broad temporal/post stack. Point shadows require a six-face allocation,
selection, filtering and cost qualification; six spotlight-equivalent passes are
only a cost proxy. The narrow spotlight implementation is the qualified local
shadow feature.

Large-coordinate matrices improve shadow coverage but do not recover precision
already lost in float32 scene values. Probes do not provide geometry-dependent
diffuse visibility, dynamic GI or automatic baking. Texture views are not a
complete streaming world, and multi-view resource sharing is not implemented.
Physical driver removal was not forced on the user's computers. Fault injection,
full resync, recreation, rejection and retirement provide the recorded recovery
scope. No cross-platform claim substitutes software Vulkan for physical hardware.

Studio authoring uses the existing schema, asset, property, journal and EditorHost
surfaces. The complete Engine-side handoff is
`RendererFoundation5StudioHandoff.md`; the concurrent Studio/LUI campaign remains
untouched. Detailed subsystem contracts and raw source/binary identities are
linked from `../Validation/RendererFoundation5Qualification.md`.

## Capability disposition

| Future workload | Present foundation and remaining work |
| --- | --- |
| Large outdoors | Qualified experimental cascades, far receivers and explicit static envelope; terrain LOD/streaming and float32 world precision remain separate work. |
| Dense interiors | Qualified authored local probes and bounded spots; geometry-dependent diffuse occlusion/GI unsupported. |
| Many moving objects | Correct sparse/dense instancing foundation, partially qualified for performance; 50K dynamic tails prevent general promotion. |
| Large material libraries | Bounded source/view retention and upload admission; complete base streaming and block compression deferred. |
| Multiple shadowed lights | Qualified spotlight-first eight-layer policy; point cube shadows unsupported pending their full cost/quality gate. |
| High-resolution environments | Bounded RGBE import and stable six-face assets; unrestricted capture resolution and EXR deferred. |
| Streaming worlds / terrain | Existing immutable publication/asset boundaries remain production reference; no new mesh LOD or terrain system claimed. |
| Multi-view | Existing independent renderer ownership retained; shared device-level residency/history remains deferred. |
| Studio workflows | Engine schema/asset/EditorHost contracts ready for integration; creator UI is handed off to the independent Studio campaign. |

Production-ready here refers to preserved existing Foundation 4 contracts, not
mainline acceptance of this isolated campaign. New features above are qualified
experimental unless explicitly labeled partial, unsupported or deferred.
