---
status: current
owner: editor-host-animation
last_verified: 2026-10-29
related_code:
  - src/editor/AnimationPreview.cpp
  - src/editor/EditorHost.cpp
  - tests/AnimationPreviewEditorHostTests.cpp
  - tests/AnimationFoundationTests.cpp
---

# Animation import inspection and disposable preview

Animation remains AssetService-owned. Existing glTF/GLB import/reimport creates
canonical skinned Mesh and individually named Animation records atomically.
The catalog now exposes joint count/compatibility identity for skinned meshes
and duration/channel count/compatibility identity for clips. No skeleton service,
Bone Instance, importer, asset database or authored playback state is added.

## Public EditorHost extension, version 1

Handshake advertises `AnimationRig` and `AnimationPreview`. Every operation uses
the existing authenticated, bounded EditorHost envelope. `ReadDataModel` is
required; preview operations also require `ViewportControl`.

| Method | Parameters and result |
| --- | --- |
| GetAnimationRig | Strict `Reference` to an available skinned Mesh or Animation; returns `RigVersion=1`, MeshReference, captured ContentRevision, SkeletonCompatibilityId, joints, compatible clips and mesh bounds. |
| OpenAnimationPreview | Same Reference; captures canonical assets, allocates one disposable native-only scene, returns monotonically issued string PreviewId, rig metadata and FrameTransport. A second live preview is rejected. |
| ControlAnimationPreview | Exact PreviewId and Action. Clip takes strict reference Value; Seek/Speed take finite numeric Value; Loop takes boolean Value. Play/Pause/Stop/Reset take no Value. |
| CaptureAnimationPreview | Exact PreviewId, unsigned Width/Height in 1..1024, bounded finite Position/Target camera triples. Returns RGB8 frame identity, time/playback state and current joint-model positions. |
| CloseAnimationPreview | Exact PreviewId only; releases scene, runtime, renderer and transport. Old identities never control a replacement. |

Rig inspection is bounded to 256 joints, 256 compatible clips and 4096 total
joint channels. Each joint carries canonical path, leaf name, parent index and
bind TRS (quaternion xyzw). Clip channels identify joint paths, TRS key counts
and STEP/LINEAR interpolation; key arrays and vertices do not cross IPC.
Oversized inspection is rejected rather than truncated into a false hierarchy.

Windows preview reuses the existing SharedFrameRing version 1 RGB8 contract
(three fixed slots); other platforms use the bounded Base64 capture fallback.
This is a separate mapping from the authoring viewport. Captures are requested
serially; Studio retains no frame queue. There is no new transport format.

## Ownership and execution

`AnimationPreview` constructs a fresh DataModel with native services, a unit-size
MeshPart and Animator. It captures the existing AssetProjectSnapshot into that
private world's AssetService. It never clones project Instances, constructs an
Engine gameplay runtime, boots a gameplay VM, executes project source, advances
physics or applies root motion. Loading the project retains the existing PreRun
bootstrap boundary; preview itself does not rerun it.

Native AnimationTrack and AnimationRuntime perform playback, seek and pose
evaluation. RenderPublisher accepts their immutable palette changes and existing
EditorViewportRenderer renders the actual canonical skinned mesh. No renderer
owns animation time or mutable project objects. Root motion remains disabled.

Playback time advances from a monotonic Engine-side capture delta. Play resumes
a paused track and restarts a stopped track. Stop/Reset remove the contribution
and restore bind pose. Seek after Stop samples a paused track. Speed is 0..16;
loop and speed survive clip selection. A rejected clip does not stop the current
track. No control writes the authoring journal, revision, history or dirty state.

Multiple controls before a frame coalesce into the final pose/removal before
the RenderPublisher operation, preserving its one-operation-per-object contract.
Each offscreen renderer owns an SDL video reference, so destroying the authoring
viewport cannot shut down a concurrently live preview (or vice versa).

The scene pins its captured canonical asset revisions. Reimport updates the
project catalog independently; close/reopen preview to inspect the new revision.
Compatibility continues to use canonical skeleton identity and paths, never
numeric joint index or a Studio-invented registry. Project replacement and host
shutdown release preview state. No preview ID, track or GPU resource persists.

Attachment authoring remains the ordinary schema-backed JointPath property.
Runtime Attachment semantics, package closure and source-free animation playback
remain the existing animation/semantic-anchor contracts; preview does not create
per-frame authored mutations to demonstrate them.

## Qualification scope

Focused native tests exercise canonical sampling/publication, unchanged authored
serialization/revision, controls, denial, bounds, stale IDs, replacement,
compatible reimport and save/reopen. The checked-in three-clip acceptance fixture
derives from this repository's AnimatedBeacon source, with an additional STEP clip;
it introduces no external content license. Existing animation tests retain
attachment, incompatible reimport, package/relocation and runtime coverage.
Headless tests are not a claim of native visual acceptance. Native UI/GPU evidence
and source-bound test/CI results belong in the milestone validation receipt.
