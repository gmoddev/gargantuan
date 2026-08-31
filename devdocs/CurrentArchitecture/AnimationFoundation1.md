---
status: current
owner: runtime-animation
last_verified: 2026-08-29
related_code:
  - assets/classes/Animator.luau
  - include/gargantuan/animation/AnimationRuntime.hpp
  - include/gargantuan/animation/AnimationTrack.hpp
  - include/gargantuan/assets/AssetTypes.hpp
  - include/gargantuan/render/RenderPublication.hpp
  - src/animation/AnimationRuntime.cpp
  - src/animation/AnimationTrack.cpp
  - src/assets/AssetImporter.cpp
  - src/assets/GltfImporter.cpp
  - src/classes/Animator.cpp
  - src/render/RenderPublisher.cpp
  - tests/AnimationFoundationTests.cpp
  - tests/AnimationFoundationBenchmark.cpp
---

# Animation Foundation 1

## Asset model and supported import

Animation remains part of the one public `AssetService`. `AssetKind::Animation`
has stable wire value `5`, after Audio, and there is no Animation, Rig, or
Skeleton service. A strict `asset://` reference identifies a clip by stable
`AssetId`; its deterministic canonical bytes have an `AssetContentId`, and a
content-changing reimport advances `ContentRevision` without changing the
`AssetId`.

Foundation 1 extends the existing glTF 2.0 `.gltf`/`.glb` importer. It accepts:

- node-based skin joint hierarchies with stable, unique joint names;
- inverse bind `MAT4` accessors;
- `JOINTS_0` and `WEIGHTS_0`, with exactly four bounded influences per vertex;
- translation, quaternion rotation, and scale animation channels;
- `LINEAR` and `STEP` samplers; and
- multiple skins, meshes, and named clips within the existing graph bounds.

Each source animation becomes one catalog Animation asset. A source with two
clips therefore produces two independently loadable assets, not one runtime
container that must parse clip names. Display names are retained through the
existing bounded-name path. `CUBICSPLINE` is explicitly rejected as
`UnsupportedInterpolation`; it is never treated as linear.

## Canonical artifacts and dependencies

Artifact version 3 adds self-contained skinned Mesh and Animation payloads while
the decoder retains version 1 and 2 support for old unskinned assets. A skinned
Mesh artifact owns vertices, indices, primitive ranges, joint paths and parents,
bind TRS, inverse bind matrices, the skeleton compatibility ID, and four joint
indices/weights per vertex. An Animation artifact owns duration, skeleton
compatibility ID, the dependent Mesh `AssetId`, and bounded per-joint T/R/S key
arrays. It stores no glTF JSON, source offsets, importer objects, renderer
handles, or source path dependency.

The Animation catalog record has an explicit dependency edge to its compatible
skinned Mesh. Import resolves that edge atomically with the source graph.
Runtime snapshot and package validation require the dependency to exist in the
closed catalog and require both canonical artifacts; package building never
discovers animation dependencies by scanning scripts.

Artifact decode verifies the outer SHA-256 and all internal limits. For a
skinned Mesh it also recomputes the compatibility ID from decoded hierarchy and
bind semantics instead of trusting the stored ID. Non-finite values, singular
bind scales, singular inverse binds, zero/negative/non-normalized weights,
out-of-range joints, zero quaternions, duplicate paths, and singular animated
scale keys fail closed. Linear scale segments whose signs cross zero are also
rejected.

## Skeleton representation and Bone decision

Skeleton data lives in the canonical Mesh representation. Animation clips carry
the compatibility identity and a dependency on that Mesh layout. This is the
smallest current design for one rig with many clips; the compatibility identity
also permits another Mesh with the exact same canonical skeleton to consume the
same pose in a future multi-mesh binding layer without changing track semantics.

Bones are not DataModel Instances. A joint is an immutable asset/runtime record
with:

- a stable slash-separated joint path;
- a canonical parent index used only after path-based compatibility succeeds;
- bind translation, normalized quaternion rotation, and nonsingular scale;
- an inverse bind matrix; and
- transient local/model pose values owned by `AnimationRuntime`.

The compatibility ID hashes, in canonical joint order, every path, parent,
bind-TRS component, and inverse-bind component. Numeric joint index alone is
never compatibility proof. Keeping Bones asset-owned avoids hundreds of
Instances per rig, authored/transient state ambiguity, serialized frame state,
and per-frame MutationGateway traffic. It also means Foundation 1 exposes no
scriptable per-bone Instance or authored bone Attachment target.

## Skinned Mesh representation

The source Mesh remains immutable. Every vertex has the existing position,
normal, tangent, and UV plus four `uint16` joint indices and four float weights.
The importer accepts finite nonnegative weights with a nonzero sum, normalizes
the sum deterministically, and rejects a zero sum or invalid joint. Mesh and
artifact decode revalidate the result to a `1e-4` sum tolerance.

The canonical maximum is 256 joints per rig and four influences per vertex.
Renderer-neutral mesh publication carries those static skin influences. As of
[Animation Foundation 2A](AnimationFoundation2GpuSkinning.md), a capable
graphical backend consumes them from the immutable source mesh while CPU-
skinned dynamic vertices are reserved for the reference/fallback path. No
palette is truncated.

## Animator public model

`Animator` is a constructible schema-backed Instance and must be a direct child
of a live skinned `MeshPart`. It is a per-rig semantic owner, not a global
service. Generic Studio insertion discovers it through the runtime schema. Its
public Luau entry point is:

```luau
local Track = Animator:LoadAnimation(AnimationAssetReference)
```

Loading requires a strict Animation asset, resolves the parent Mesh through
AssetService, verifies the skeleton compatibility ID, maps clip joint paths to
the canonical rig once, and captures the clip's immutable resource revision.
Each Animator admits at most 16 retained tracks. The runtime admits at most
4,096 tracked Animators and deterministically rejects a second active Animator
for the same MeshPart with one bounded diagnostic.

Foundation 1 binds one authored Animator to its direct MeshPart. The evaluated
pose and compatibility ID are distinct from that render target, so a later rig
binding component can fan one pose out to multiple compatible MeshParts rather
than changing AnimationTrack or making renderer meshes own time.

## AnimationTrack and playback state

`AnimationTrack` is shared runtime userdata, never an authored Instance. It is
not serialized, replicated, cloned as authored state, or visible in the
hierarchy. Its public surface is:

| Member | Contract |
| --- | --- |
| `Duration` | Read-only canonical clip duration. |
| `PlaybackState` | Read-only `Stopped`, `Playing`, or `Paused`. |
| `TimePosition` | Finite seek value clamped to `[0, Duration]`. |
| `Speed` | Finite `[0, 16]`; zero holds time while Playing. |
| `Weight` | Finite `[0, 1]`. |
| `Looped` | Whether natural end wraps. |
| `Ended` | Fires once only for natural non-looped completion. |
| `Play()` | Restarts from time zero, including repeated Play. |
| `Pause()` / `Resume()` | Pause preserves time; Resume continues it. |
| `Stop()` | Stops, resets time to zero, removes the contribution, and does not fire Ended. |
| `AdjustSpeed()` / `AdjustWeight()` | Validated aliases for the writable properties. |

Natural completion enters Stopped, holds the final pose, and fires `Ended` once.
A subsequent Stop or Play clears that held state. Looping uses `fmod` so a large
delta can cross many loops in one step without event storms; Looped tracks do
not fire Ended. [Foundation 2C](AnimationFoundation2UpdatePolicy.md) keeps this
lightweight time/event phase active while a visual pose is reduced or frozen.
Event callbacks run on the main runtime thread after deterministic pose merge
and may safely call Play again. Animator destruction synchronously invalidates
externally retained tracks.

Time advances from Engine's monotonic frame delta after `PreRender`; it does
not read wall-clock time or count presented frames. The same `Step(delta)` path
runs at 30, 60, 144 Hz, without a renderer, and in packaged gameplay.

## Interpolation, blending, and pose evaluation

Translation and scale use component-wise linear interpolation for `LINEAR` and
left-key hold for `STEP`. Rotation uses normalized quaternion slerp. A negative
dot product flips the right quaternion before interpolation, preventing a
long-path sign discontinuity. Euler interpolation is never used.

Foundation 1 implements deterministic weighted absolute blending without
priority or additive layers. Tracks are visited by immutable creation sequence.
For each T/R/S channel, contributing weights are accumulated; any total below
one is completed with bind-pose weight, while totals above one are normalized.
Quaternion samples are hemisphere-aligned before normalized weighted blending.
A missing channel, partial clip, stopped track, or zero-weight track cannot
reset an unrelated joint: bind state supplies only the uncovered channel.

Evaluation is synchronous on the main/runtime simulation domain:

```text
advance track time
    -> sample and blend joint-local T/R/S
    -> solve parent-before-child model transforms
    -> modelTransform * inverseBindMatrix per joint
    -> publish latest transient palette
    -> GPU skin immutable source vertices, or CPU-skin only for explicit fallback/reference use
    -> fire pending Ended callbacks
```

Workers, renderers, and Luau callbacks do not mutate this state. JobSystem
parallelism is deferred until profiling justifies the snapshot/worker/ordered-
merge complexity.

## Skinning math and production path

GLM matrices are column-major and multiply column vectors. For joint `j`:

```text
JointModel[j] = JointModel[parent] * T(local) * R(local) * S(local)
SkinMatrix[j] = JointModel[j] * InverseBindMatrix[j]
```

Positions use the four weighted homogeneous skin transforms. Normals and
tangents use each contributing skin matrix's inverse-transpose linear part,
then normalize the weighted result. Singular/non-finite palette transforms fail
the pose rather than injecting NaN. CPU skinning never overwrites the canonical
source Mesh.

CPU skinning was the Foundation 1 production path and remains the correctness
oracle. It writes a pooled transient dynamic Mesh only when an explicitly
unsupported graphical backend requests fallback or a native test/query requests
the reference result. Foundation 2A keeps the static skinned source resident,
uploads the final position/normal palette, and skins in the vertex shader
without changing Animator, clip, time, or publication semantics. The complete
current backend contract is recorded in
[Animation Foundation 2A](AnimationFoundation2GpuSkinning.md).

## Renderer-neutral publication, shadows, and lighting

Animation has its own `RenderUpdateDomain::AnimationPose`. Runtime pose changes
go directly to `RenderPublisher`; they never enter ChangeJournal or authored
property mutation. Multiple semantic changes before one publish coalesce to the
latest pose revision for that MeshPart. Publications contain immutable bounded
values and no Animator pointer, AssetId, SDL object, shader buffer, or callback.

The projection validates increasing pose revisions, finite palettes, existing
source meshes, exact skeleton identity/joint count, mode-specific object/mesh
binding, and a 256-entry limit. GPU mode forbids a posed mesh; CPU fallback
requires one. Full resync recreates source residency and the current pose plus
posed residency only when fallback is active. Stop or destruction retires the
per-rig pose state.

Both the opaque and shadow passes bind the same per-rig palette resource and
revision in GPU mode, or the same posed dynamic mesh identity in CPU fallback.
Lighting remains view/render state over the posed normals; environment-only
changes do not dirty or recompute animation pose state.

## Headless semantics and non-render consumers

`AnimationRuntime::GetPose` exposes renderer-free joint model transforms and
the palette to trusted native systems. Track advancement and compatibility
checks run without SDL video or a GPU. Foundation 2C evaluates the hierarchy and
palette headlessly only for a semantic-required rig or an explicit native pose
request; visual-only headless rigs continue logical time without GPU-oriented
pose work. Headless evaluation never deforms every source vertex by default;
CPU skinning is invoked only by an explicit reference/query consumer.

Foundation 1 did not add a semantic animated-bone anchor. [Foundation 2B](AnimationFoundation2SemanticAnchors.md)
now consumes these renderer-free joint model transforms through a canonical
`Attachment.JointPath`; Attachment, Sound, and ProximityPrompt share that one
world-transform resolver without changing Foundation 1 pose authority. Skinned
visual triangles still do not replace the BasePart collision/query shape;
Workspace raycasts do not test every skinned triangle.

Root-joint translation remains mesh-local visual pose. It does not change the
MeshPart CFrame, player root, physics body, or network authority. Root-motion
extraction is a separate future gameplay contract.

## Persistence, Play/Stop, reimport, and restart

Persistence includes the MeshPart asset reference, authored Animator Instance,
gameplay script Animation reference, and canonical catalog/artifacts. Active
tracks, playback state, time, local/model poses, palettes, GPU resources, and
fallback dynamic vertices are transient and absent from scene serialization and
replication.

Studio Play clones authored Instances and the canonical runtime asset snapshot,
then creates a new AnimationRuntime and tracks. Stop shuts the runtime down,
invalidates its tracks, removes poses, and destroys the Play clone; authoring
state is unchanged. Project replacement uses the same engine shutdown-before-
world-release ownership. A renderer restart requests a full resync from the
current semantic pose and never restarts track time.

An active track retains the immutable clip revision captured by LoadAnimation,
so an atomic asset reimport cannot mutate key arrays beneath evaluation. Tracks
loaded afterward receive the new revision. If the active Mesh is reimported to
an incompatible skeleton, evaluation removes the pose and emits a bounded
`IncompatibleSkeleton` diagnostic; it never remaps the old numeric joint index
to an unrelated joint.

## Packaging, generic Studio authoring, and sample proof

Runtime snapshots and packages contain version-3 Mesh/Animation `.gasset`
files, the explicit dependency edge, and compiled opaque/shadow skinning shader
artifacts; source glTF and shader source are unnecessary. Package validation
rejects a missing dependency, canonical artifact, or compiled shader before
standalone startup.

The generic EditorHost/Studio workflow imports animated glTF through the common
asset catalog, exposes Animation metadata/dependencies, inserts the schema-
backed Animator, saves, reopens, and starts isolated Play. Foundation 1 adds no
timeline or keyframe-authoring UI. FirstCompleteGame includes a two-bone
`AnimatedBeacon`, `BeaconAnimator`, canonical looping `BeaconPulse` clip, and a
Luau `BeaconAnimation` script using the public API. Headless Play, Stop, package
relocation, and packaged Engine execution assert that its pose runs while the
authored hierarchy remains unchanged.

## Bounds and diagnostics

The existing 8 MiB source, 64 MiB artifact, 4 MiB glTF JSON, and canonical Mesh
bounds remain in force. Animation-specific limits are:

| Resource | Limit |
| --- | ---: |
| glTF nodes | 4,096 |
| skins / clips per source | 256 / 256 |
| animation channels per source | 4,096 |
| joints per skeleton/palette | 256 |
| joint-path bytes | 256 |
| canonical tracks per clip | 256 |
| keyframes per sampled channel | 65,536 |
| total keyframes per source import | 1,048,576 |
| duration | 3,600 seconds |
| tracks retained per Animator | 16 |
| Animators per runtime | 4,096 |

Importer and runtime diagnostics are bounded by the existing AssetService
diagnostic limits and one runtime ObjectId/code entry. Expected prefixes use
`[Animation:Animator]`, `[Animation:Track]`, or `[Animation:Runtime]`. Missing
assets, incompatible skeletons, invalid rigs/poses, duplicate Animators,
capacity exhaustion, renderer palette violations, and missing package
dependencies never produce a per-frame log.

## Performance and memory evidence

The following table is the preserved Foundation 1 CPU-production baseline;
Foundation 2A measurements and direct comparisons are in
[Animation Foundation 2A](AnimationFoundation2GpuSkinning.md).
`gargantuan_animation_foundation_benchmark` uses imported 999-vertex rigs and
measures sampling/blending, hierarchy solve, skin matrices, CPU skinning, pose
construction, publication, projection, and animation-owned transient-buffer
growth. The 2026-08-28 Windows x64 Release run averaged ten measured frames:

| Scenario | Total ms/frame | Sample/blend | Hierarchy | Skin matrices | CPU skin | Publisher | Projection |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 rig, 64 bones, 1 track | 0.0616 | 0.0016 | 0.0035 | 0.0014 | 0.0334 | 0.0048 | 0.0164 |
| 10 rigs, 64 bones, 1 track | 0.6082 | 0.0173 | 0.0352 | 0.0137 | 0.3389 | 0.0421 | 0.1554 |
| 100 rigs, 64 bones, 1 track | 6.2634 | 0.1732 | 0.3587 | 0.1467 | 3.3375 | 0.4982 | 1.6426 |
| 500 rigs, 64 bones, 1 track | 32.6575 | 0.9120 | 1.8038 | 0.7630 | 16.7750 | 2.9438 | 8.6071 |

At 10 rigs, 16/64/128/256 bones measured 0.5497/0.6148/0.6848/0.8277
ms/frame. At 10 64-bone rigs, 1/2/4 tracks measured
0.6152/0.6197/0.6408 ms/frame. The CPU reference path measured 0.0564,
0.5709, 2.8208, and 5.5629 ms for 1K, 10K, 50K, and 100K vertices,
respectively. These are host observations, not cross-device budgets or GPU
shader timings.

All scenarios reported zero new animation-owned pose/palette/skinned-output
buffer allocations after warm-up; pooled capacities remain stable. This metric
does not claim that construction of the immutable per-frame RenderPublication
itself is a zero-allocation operation. A separate regression with 50,000 static
Parts and one animated 64-bone rig produced exactly one pose update, one dynamic
vertex update, zero static object updates, and 0.0284 ms publisher time.

For the benchmark's 999-vertex Mesh plus one clip, canonical artifact bytes were
87,006/98,006/120,150 at 64/128/256 bones. Structural estimates were:

| Bones | Skeleton | Animator pose buffers | Active track | Palette | Renderer pose |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 10,240 B | 10,752 B | 456 B | 4,096 B | 4,160 B |
| 128 | 20,480 B | 21,504 B | 712 B | 8,192 B | 8,256 B |
| 256 | 40,960 B | 43,008 B | 1,224 B | 16,384 B | 16,448 B |

Clip key arrays are immutable AssetService resources shared by every Animator;
they are not duplicated per track. The 500-rig scenario is the current many-rig
stress result and demonstrates why GPU skinning and animation LOD are
Foundation 2 priorities.

## Architecture decisions

| Question | Foundation 1 decision |
| --- | --- |
| Are Bones DataModel Instances? | No; skeleton joints are immutable asset/runtime records and pose is transient. |
| Where does skeleton identity live? | In the canonical skinned Mesh artifact; Animation carries its compatibility ID and Mesh dependency. |
| How are clips matched to rigs? | Stable joint paths plus a hash of hierarchy, bind TRS, and inverse binds; never index or file name alone. |
| Are multiple clips per source supported? | Yes; import creates one Animation asset per clip. |
| Is CPU or GPU skinning production? | Foundation 1 introduced CPU production/reference; Foundation 2A selects GPU skinning for capable graphical backends and retains CPU as oracle/fallback. |
| Bone palette limit? | 256, checked at import, runtime, publisher, and projection. |
| Interpolation modes? | glTF `STEP` and `LINEAR`; vector linear and shortest-path normalized quaternion slerp. |
| How does blending work? | Deterministic creation order, weighted absolute per-channel blend, normalized above weight one, bind fallback below one. |
| Is priority implemented? | No; weights and deterministic creation order are sufficient for Foundation 1. |
| What happens to root translation? | It stays mesh-local visual pose; it does not move authored/physics/world roots. |
| Can Attachments follow animated bones? | Yes. Foundation 2B resolves canonical `Attachment.JointPath` bindings from the accepted renderer-free pose. |
| Can Audio/Interaction anchors follow bones? | Yes. Positional Sound and ProximityPrompt consume that shared semantic transform. |
| Are animated shadows correct? | Yes. Opaque and shadow passes share one accepted GPU palette revision, or one CPU-fallback posed Mesh identity. |
| What happens on reimport during playback? | An active track keeps its captured immutable clip revision; new tracks use the new revision, and incompatible Mesh reimport removes the pose. |

## Replication and explicit deferrals

Foundation 1 implements no animation replication. The likely future unit is a
stable clip identity plus high-level play/time/speed/weight state, with
server-authoritative gameplay pose only where required and client visual
evaluation elsewhere. Full bone matrices are not the default network design,
and not every client should receive every distant NPC pose.

Deferred work includes IK, inverse dynamics, retargeting, humanoid abstraction,
state machines, blend trees/editor, priorities until use cases justify them,
additive layers, markers/events, root-motion authority, ragdoll, procedural
graphs, compression research, motion matching, facial/morph animation,
cloth-to-bone coupling, network animation protocol, semantic animated hitboxes,
timeline/keyframe authoring, and physical mobile performance claims.

Animation Foundation 2A now implements backend GPU skinning from the existing
source-mesh/palette seam, Foundation 2B implements semantic animated anchors,
and [Foundation 2C](AnimationFoundation2UpdatePolicy.md) implements the internal
visibility/distance policy plus measured pose jobs. Retargeting, graphs, and
authoring tools should build only on these proven authority and scheduling
contracts.
