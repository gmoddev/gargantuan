---
status: current
owner: runtime-animation
last_verified: 2026-08-29
related_code:
  - assets/classes/Attachment.luau
  - include/gargantuan/classes/Attachment.hpp
  - include/gargantuan/runtime/SemanticSpatialResolver.hpp
  - src/classes/Attachment.cpp
  - src/runtime/SemanticSpatialResolver.cpp
  - src/animation/AnimationRuntime.cpp
  - src/audio/AudioRuntime.cpp
  - src/services/InteractionService.cpp
  - tests/AnimationFoundationTests.cpp
  - tests/AnimationFoundationBenchmark.cpp
  - tests/FirstCompleteGameTests.cpp
  - tests/FirstCompleteGameGpuTests.cpp
---

# Animation Foundation 2B: semantic animated anchors

## Outcome and authority boundary

Foundation 2B makes one evaluated animation pose serve two independent
consumers:

```text
AnimationTrack sampling and blending
    -> CPU joint model transforms
       -> skin matrices -> RenderPublication -> GPU or CPU rendering
       -> SemanticSpatialResolver -> Attachment.WorldCFrame
                                  -> positional Sound
                                  -> ProximityPrompt
                                  -> future effects, sockets, and constraints
```

The runtime joint model transforms are semantic authority. The resolver neither
reads a palette back nor asks a renderer for skinned vertices. Headless, GPU-
skinned, and forced CPU-fallback Engines therefore use the same anchor path.
The renderer can be destroyed and recreated while the resolver continues from
the next runtime pose revision.

Bones remain immutable records in the canonical Mesh skeleton artifact. They
are not Instances, do not consume ObjectIds, cannot be reparented independently
of their skeleton, and never enter persistence, replication, or ChangeJournal.
Creating a fake Bone Instance hierarchy would duplicate canonical skeleton
identity and make renderer state appear authoritative. Attachment remains the
public, generally useful spatial object.

## Public Attachment contract

`Attachment` has three spatial properties:

| Property | Contract |
| --- | --- |
| `CFrame` | Saved, future-replicated socket-local offset. Animation never mutates it. |
| `JointPath : string` | Optional saved, future-replicated canonical skeleton path. Empty means ordinary static semantics. The value is UTF-8, at most 256 bytes, and rejects leading/trailing or repeated `/`, `\\`, and `.`/`..` segments. |
| `WorldCFrame` | Read-only, noneditable, transient script-visible resolved world transform. It is neither saved nor replicated. |

A public numeric joint index, glTF node index, palette slot, or ambiguous leaf
name does not exist. `JointPath` must match the exact unique canonical path in
the current Mesh artifact, for example `BeaconRoot/BeaconTip`. A plain schema-
driven string editor is the current Studio UI; an asset-backed joint picker is
useful UX debt, not a reason to add a bespoke rig editor or mutation command.

The supported hierarchy is an Attachment chain terminating at a BasePart. A
nonempty binding becomes animated only when that owner is a skinned MeshPart.
The usual shape is:

```text
MeshPart
    Animator
    Attachment (JointPath set)
        Attachment (optional unbound child offset)
            Sound / ProximityPrompt / gameplay consumer
```

The Animator is a sibling of the Attachment. An unbound child Attachment
inherits its resolved animated parent. If a descendant supplies its own
`JointPath`, it reanchors to that canonical joint on the same owning MeshPart.
A binding beneath a non-MeshPart or without a usable skeleton fails closed to
the ordinary authored chain.

## Exact transform semantics

Matrices use GLM column vectors. For a joint-bound Attachment directly under a
MeshPart:

```text
WorldMatrix = Matrix(MeshPart.CFrame)
            * Scale(MeshPart.Size)
            * CurrentJointModelTransform
            * Matrix(Attachment.CFrame)
```

This is the same owner scaling model used by skinned rendering. Current joint
model transform is the final blended runtime result, including joint
translation, rotation, and scale. Root-joint motion therefore moves a socket
inside MeshPart model space but does not mutate the MeshPart CFrame or rigid
physics body. Owner translation, rotation, atomic CFrame/Size commits, joint
pose, and local socket offset are observed as one resolver-step result.

For an unbound chain, the pre-existing equation remains:

```text
WorldMatrix = Matrix(BasePart.CFrame)
            * Matrix(Attachment0.CFrame)
            * ...
            * Matrix(AttachmentN.CFrame)
```

`SemanticSpatialTransform::Matrix` retains nonuniform scale and shear for
trusted native consumers and equivalence tests. `CFrame` cannot represent
scale. `WorldCFrame` therefore preserves the exact matrix translation and an
orthonormalized orientation; it intentionally omits scale. Sound and
Interaction consume the exact resolved position. A future scale-bearing public
transform type should extend this contract rather than smuggling scale into
Euler angles.

## Bind pose and deterministic fallback

A compatible skeleton is sufficient to resolve a binding. If no track is
playing, the Animator has not produced a pose, or the Animator is absent or
destroyed, the resolver uses the skeleton's hierarchical bind-pose model
transform. Pausing freezes the current runtime pose; resuming advances it.
Stopping explicitly returns the binding to bind pose, while the existing
natural-end AnimationTrack policy continues to hold its final pose.

If the Mesh/skeleton is missing, the path is absent, the path is malformed, the
owner is not a MeshPart, the transform is non-finite, or a reimport changes the
skeleton compatibility identity, resolution fails closed to ordinary static
Attachment composition. It never snaps to origin and never reuses an old
numeric slot. One bounded diagnostic per ObjectId/code identifies unavailable
rigs, paths, compatibility, transform, depth, and admission failures.

Compatible Mesh reimport rebuilds the path-to-index and bind-transform cache at
the new content revision and keeps the binding. Incompatible reimport retains
the authored string but invalidates its animated interpretation. Restoring the
original compatible skeleton restores the same canonical binding. Animation
asset reimport does not own Attachment identity.

## Resolver ownership, registration, and cache

Each Engine owns one `SemanticSpatialResolver` between AnimationRuntime and all
spatial consumers. Engine descendant binding registers Attachments once. The
resolver owns generation-safe weak Instance references and ordered structures:

- `ObjectId -> Entry` for every registered Attachment;
- parent Attachment `ObjectId -> ordered child ObjectIds` for bounded dirty
  propagation;
- rig `ObjectId -> RigRecord` with a sorted bound-Attachment vector;
- canonical joint path -> numeric artifact index, private to the rig record;
- cached bind transforms, the shared current joint-model-transform buffer,
  owner matrix, content/compatibility identity, and pose revision; and
- a sorted dirty Attachment set and reused work vectors.

Registration, mutation, reparenting, destruction, Mesh changes, and Engine
shutdown rebuild or remove only the affected links. Registration order defines
bounded admission; accepted registrations and same-joint fan-out are stored and
traversed in sorted ObjectId order, so no unordered-container choice can decide
which anchor updates. No pose step walks the DataModel hierarchy. A rig with
zero bound anchors has no rig record, so it causes zero anchor resolutions and
zero rig visits.

Every step compares the rig's monotonic Animation pose revision, current
MeshPart CFrame/Size, Mesh reference/content revision, and skeleton
compatibility. Attachment CFrame, JointPath, ancestry, and destruction signals
mark the affected cached chain. Only dirty Attachments and Attachments indexed
under changed rigs enter the reused work vector. An unchanged cache returns the
last matrix and semantic revision. Lighting, viewport, unrelated Parts, and
renderer frames do not invalidate it.

The hard bounds are:

| Resource | Limit |
| --- | ---: |
| canonical joints per rig | 256 |
| JointPath bytes | 256 |
| Attachment-chain depth | 64 |
| registered Attachments per resolver | 65,536 |
| animated semantic anchors per resolver | 65,536 |
| animated semantic anchors per rig | 1,024 |

The general DataModel Instance ceiling remains an outer bound. The 50K static-
Part and 50K static-Attachment regressions are separate worlds so neither
fixture exceeds that ceiling.

## Transient update domain and ChangeJournal

When a cached world matrix changes, the resolver increments a private semantic
revision and fires `Attachment`'s transient `WorldCFrame` property signal. This
signal does not call a property setter, advance the DataModel authoritative
revision, serialize a matrix, or create a ChangeJournal record. Reusable signal
snapshots avoid allocation during non-reentrant steady firing while preserving
safe nested/reentrant signal behavior.

Changing authored `JointPath` still uses the ordinary mutation lane. The test
reads the DataModel-scoped journal and observes exactly one replicated
`PropertyUpdatedChange` for `JointPath`. Continuous animation, Sound movement,
and prompt movement observe zero journal records. This authored/transient
distinction is a hard contract.

## Sound and audio-thread ownership

`AudioRuntime` asks the shared resolver for the Sound ancestor's world transform
while building the next main/runtime-thread mix block. It does not know about
Animator, joints, palettes, or Mesh artifacts. A positional Sound beneath a
bound Attachment therefore changes attenuation and pan with the final blended
pose, freezes while the track is paused, resumes with the track, follows owner
motion, and returns to bind pose after Animator destruction.

The SDL device side still sees only copied interleaved float samples in the
existing push stream. It never touches Animator, the DataModel, Attachment, or
the resolver and takes no animation locks. Asset resolution is refreshed only
on an Audio asset change sequence rather than on every block. Headless/no-
device mode keeps Sound methods and semantic transforms safe even though no
hardware queue is opened.

## ProximityPrompt, LOS, and final validation

InteractionService also asks the same resolver for the prompt ancestor. The
resolved animated point supplies spatial-cell membership, distance eligibility,
LOS destination, hold updates, and final activation validation. It subscribes
to the Attachment's transient `WorldCFrame` signal, coalesces an already-dirty
prompt in a reused vector, and does not rebuild anchor subscriptions or cell
membership unless topology/cell changes.

Animated movement can therefore enter range, leave range, cancel an active
hold, or reject a press that was displayed against an older pose. Every press,
hold update, and trigger still resolves current range and LOS. LOS remains a
rigid `Workspace:Raycast` from the character origin to the current animated
endpoint; skinned visual triangles do not become query geometry. The ordinary
33 ms interaction cadence remains, while a transient anchor dirty signal can
force the relevant prompt's immediate semantic reevaluation.

## Lifecycle, Play, package, and renderer independence

Attachment creation/destruction, binding edits, nested reparenting, Animator
creation/destruction, owner destruction, compatible/incompatible Mesh reimport,
Play cloning, and shutdown are covered with no raw cached Instance pointers.
Engine shutdown tears down Interaction and Audio before Spatial, then Animation,
so callbacks cannot outlive authority. A new Play session or replacement
project owns a fresh resolver and generation-safe ObjectIds; no cache can cross
worlds. Ten FirstCompleteGame Play/Stop cycles preserve the authored snapshot.

The package contains only authored CFrame/JointPath and the canonical Mesh and
Animation artifacts. It does not require source glTF, Studio, or a source-tree
path. The relocated Release GPU package proof deletes its copied source project,
loads the package, compares the semantic socket against both the current joint
model transform and GPU palette oracle, advances 12 poses with zero CPU vertex
uploads, and replaces the renderer without interrupting semantic revision.

Deterministic native tests compare:

1. headless semantic joint-model transform;
2. a CPU reference-skinned bind socket;
3. forced CPU-fallback semantic output; and
4. packaged Vulkan GPU palette evidence.

They include asymmetric owner translation/rotation/nonuniform scale, animated
translation/rotation/scale, local offset, blending, bind pose, pause/resume,
and renderer restart. The semantic matrices agree within tolerance because all
paths start with the same CPU joint model transform; no GPU readback exists.

## Windows Release performance evidence

These 2026-08-29 observations are from the primary Windows machine. Each row is
a 64-joint, one-track case after five warmup frames and ten measured frames.
`bind` and `resolve` are mean resolver bookkeeping and transform time. The
slash-separated values are P50/P95/P99 microseconds. One positional Sound is
present in every nonzero-anchor scenario; one prompt is present per rig.

| Rigs | Anchors/rig | bind us | resolve us | spatial us | Sound us | Interaction us | total semantic us |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0 | 0.01 | 0.01 | 0.2/0.2/0.2 | 0/0/0 | 0/0/0 | 0.2/0.2/0.2 |
| 1 | 1 | 0.21 | 0.50 | 0.6/3.2/3.2 | 7.7/10.2/10.2 | 0.2/1.1/1.1 | 8.5/13.8/13.8 |
| 1 | 4 | 0.16 | 0.86 | 1.1/1.1/1.1 | 7.6/7.7/7.7 | 0.2/0.2/0.2 | 8.9/9.0/9.0 |
| 1 | 16 | 0.15 | 3.13 | 3.4/3.5/3.5 | 7.6/7.7/7.7 | 0.2/0.2/0.2 | 11.2/11.4/11.4 |
| 1 | 64 | 0.14 | 12.58 | 12.9/13.5/13.5 | 7.7/7.8/7.8 | 0.2/0.2/0.2 | 20.8/21.5/21.5 |
| 10 | 0 | 0.02 | 0.03 | 0.2/0.2/0.2 | 0/0/0 | 0/0.1/0.1 | 0.2/0.2/0.2 |
| 10 | 1 | 1.07 | 2.90 | 4.1/4.2/4.2 | 7.8/8.0/8.0 | 1.5/1.7/1.7 | 13.5/13.7/13.7 |
| 10 | 4 | 1.11 | 9.30 | 10.7/11.0/11.0 | 7.7/7.8/7.8 | 1.7/1.9/1.9 | 20.2/20.6/20.6 |
| 10 | 16 | 1.23 | 36.33 | 38.1/43.1/43.1 | 7.8/8.1/8.1 | 1.8/1.8/1.8 | 47.6/52.6/52.6 |
| 10 | 64 | 1.25 | 150.99 | 155.8/160.7/160.7 | 7.9/8.0/8.0 | 1.9/2.0/2.0 | 165.7/170.5/170.5 |
| 100 | 0 | 0.06 | 0.01 | 0.2/0.7/0.7 | 0/0/0 | 0/0.1/0.1 | 0.2/0.7/0.7 |
| 100 | 1 | 14.53 | 64.84 | 79.9/114.7/114.7 | 8.6/8.9/8.9 | 19.2/22.3/22.3 | 108.7/145.7/145.7 |
| 100 | 4 | 14.11 | 119.61 | 131.8/175.5/175.5 | 8.1/8.9/8.9 | 19.7/21.3/21.3 | 159.7/205.2/205.2 |
| 100 | 16 | 15.18 | 553.58 | 518.8/822.2/822.2 | 8.6/9.8/9.8 | 25.8/30.5/30.5 | 552.2/861.8/861.8 |
| 100 | 64 | 27.81 | 4377.32 | 4221.7/5308.3/5308.3 | 11.3/16.3/16.3 | 72.4/149.3/149.3 | 4300.9/5409.3/5409.3 |
| 500 | 0 | 0.17 | 0.03 | 1.3/2.4/2.4 | 0/0/0 | 0.3/1.4/1.4 | 1.6/3.8/3.8 |
| 500 | 1 | 212.78 | 1296.91 | 1523.1/1712.3/1712.3 | 14.2/21.0/21.0 | 217.8/305.9/305.9 | 1808.5/1983.9/1983.9 |
| 500 | 4 | 191.34 | 2613.38 | 2846.5/2973.6/2973.6 | 13.0/20.8/20.8 | 343.8/389.9/389.9 | 3165.2/3330.2/3330.2 |
| 500 | 16 | 303.26 | 8531.28 | 8770.0/10314.8/10314.8 | 15.7/22.7/22.7 | 620.1/893.6/893.6 | 9430.0/11121.8/11121.8 |
| 500 | 64 | 294.17 | 26280.64 | 26412.9/31845.7/31845.7 | 15.3/22.3/22.3 | 752.0/951.8/951.8 | 27223.8/32819.8/32819.8 |

The existing Animator/runtime total at zero anchors was 0.0125, 0.1124,
1.0702, and 6.4275 ms for 1/10/100/500 rigs. At 64 anchors per rig the complete
measured frame was 0.0331, 0.2762, 5.8977, and 35.3031 ms. CPU skinning remained
zero in all production-palette scenarios. Zero-anchor rows created no semantic
rig index, performed no rig visit or anchor resolution, and allocated nothing.
A sparse 256-joint case bound one anchor directly to joint 200; it visited one
rig and resolved one anchor per frame with zero steady-state allocation
(`0.6/0.6/0.6` us spatial and `8.2/9.4/9.4` us total semantic P50/P95/P99).

Every matrix row reported zero AnimationRuntime buffer growth and zero steady-
state allocations in Spatial, Sound, and Interaction. At 64 joints the
canonical fixture artifacts occupy 87,006 bytes; estimated skeleton, Animator
pose, active-track, position/normal palette, and renderer-pose storage are
10,240, 10,752, 456, 8,192, and 8,296 bytes respectively. The corresponding
128/256-joint estimates are emitted by the benchmark.

The 50,000-static-Part world plus one rig/Attachment/Sound/Prompt published one
pose and resolved one anchor, with zero static updates, dynamic vertices,
journal records, document reconciliation, full resyncs, or semantic
allocations; publisher time was 0.0283 ms. A separate world containing 50,000
ordinary Attachments plus one animated anchor resolved exactly that one anchor,
visited one rig, scanned zero static Attachments, journaled nothing, and
allocated nothing after warmup.

## Animation 2C priorities from the measurements

1. Replace per-anchor ordered-map/weak-pointer lookup with a generation-safe
   contiguous per-rig/SoA solve and group same-joint consumers. Transform
   resolution, not Sound or Interaction, dominates the 500 x 64 case at 26.28
   ms mean and 31.85 ms P95.
2. Jobify deterministic rig/anchor chunks with a bounded ordered merge. The
   zero-anchor 500-rig Animator baseline is already 6.43 ms and 32,000 semantic
   anchors raise the complete frame to 35.30 ms; the work has enough independent
   rig granularity to justify jobs.
3. Add visual Animation LOD/server pose policy around an explicit
   `has semantic consumers` contract. Zero-anchor semantic overhead is only
   1.6 us P50 at 500 rigs, so offscreen visual work may be throttled, but rigs
   feeding prompts, sounds, gameplay sockets, server logic, future XPBD cloth,
   or effects must keep the required pose authoritative.

Interaction P95 remains below 0.952 ms for 500 prompt-bearing rigs and the one-
Sound update remains below 22.7 us P95, so separate prompt/audio animation
resolvers, GPU readback, particles, cloth coupling, root motion, IK,
retargeting, animation graphs, replication, and a rig/timeline editor are not
2C performance priorities.

## Future network, effects, and cloth constraints

Future replication should transmit high-level animation state and let each
runtime derive the same semantic pose; it should not replicate every
WorldCFrame or GPU palette. A server-authoritative prompt or socket requires a
headless Animator pose even when no renderer exists. Future visual LOD must not
throttle a semantically required rig below gameplay correctness.

Particles, beams, trails, muzzle flashes, weapon sockets, and XPBD cloth
constraints should consume the same resolver transform. In particular, future
cloth attachment is `joint pose -> semantic anchor -> constraint target`, not
an XPBD dependency on a renderer palette. Foundation 2B deliberately adds none
of those systems.
