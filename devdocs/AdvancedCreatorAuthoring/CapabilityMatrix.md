---
status: current
owner: creator-authoring
last_verified: 2026-11-15
---

# Advanced creator authoring: integrated baseline and gaps

The source assessment uses Engine `66711af2fe9e03a8d5f82611626932b785f2b837`
(tree `edf71150913a312439d1defe97b91b1ac39864de`) and Studio
`dc960b5433b73e1d305548e0dc5a8dfc0d9cf2f3`.
Source, tests and retained native evidence establish the bounded capabilities
below. New feature work has separate qualification; model checks do not establish GUI acceptance.

Engine documentation PR #25 merged normally. Its actual main-push
[Documentation](https://github.com/gmoddev/gargantuan/actions/runs/43756931407),
[Native](https://github.com/gmoddev/gargantuan/actions/runs/43756931339) and
[GNS](https://github.com/gmoddev/gargantuan/actions/runs/43756931290) passed:
Windows 162, Linux 109, five Release workload gates, and GNS 11 plus profiles.

Studio's existing pin is separately recorded: source
`b5df1e7b1b80da27158ea794626f90a8ebd8a6d2`, build
`7bd9f54866427aecba93fb76d2a65d2a536d5bcd`, equal tree
`7ef2a2b6b2bb34732bbb8335f30dff9588a68f39`, and 32 hashed runtime files.
It does not prove a combined run with the new feature Engine.
Qualify Engine, pin its exact qualified revision/bytes, then qualify the pair.

## Capability matrix

Complete refers to the supported profile. Partial identifies the missing creator workflow.

| Capability | Baseline | Existing implementation/evidence | Missing workflow or explicit limit |
| --- | --- | --- | --- |
| Character creation/configuration | Partial | Character/KinematicCharacter, nullable descendant RootPart, generic EditorHost create/property/history; Character.cpp and FirstCompleteGameTests | Guided assembly, typed root selection, imported rig root and controller/collision checklist |
| Rig creation/inspection | Partial | glTF/GLB import, GetAnimationRig, RigBrowser, AnimationPreviewEditorHostTests | Topology authored externally; LUI selected-joint overlay and socket guidance unported |
| Skeleton hierarchy | Complete for imported immutable rigs | Canonical paths, compatibility identity, 256 joints; GltfImporter and AnimationFoundationTests | No in-Engine skeleton topology editor |
| Animation import/playback | Partial | Per-clip assets and disposable play/pause/seek/speed/loop preview | Host drops pose/dimension metadata needed by overlay; complete joint selection and native acceptance |
| Blending/transitions | Partial | Up to 16 Animator tracks and weighted absolute TRS blend | Single-clip preview; no guided transitions/state authoring or additive/priority system |
| Root-motion authoring | Partial | Canonical extraction, Character admission/prediction and action tests | No requested-versus-collision-accepted creator view; visual preview deliberately applies no root motion |
| Events/markers | Missing beyond Ended | AnimationTrack.Ended; ImportedAnimation has no marker collection | Separate canonical marker persistence/import/event milestone |
| Attachments/sockets | Partial | Saved/replicated CFrame/JointPath and semantic WorldCFrame; animation/reimport tests | Discoverable joint selection and rigid visual socket followers |
| Movement/action configuration | Partial | Kinematic capsule, CharacterControlService, replaceable Luau policy | Guided policy/animation-driven action setup |
| Appearance composition | Partial | MeshPart Mesh/Material/color/transparency, canonical assets, save/load and structural replication | Explicit visual-only accessories, variants/presets and readiness presentation |
| Collision/physics configuration | Partial | BasePart properties, capsule, Box and anchored static-mesh profiles | Character-specific guidance; skinned static-triangle collision unsupported |
| Camera authoring | Partial | Editor gestures/bookmarks/framing, Camera schema, default camera policy | Guided authored Camera/policy setup; CurrentCamera is not saved writable state |
| Input/actions | Partial | InputContext/Action/Binding tools and Play input inspection | Connect existing tools to gameplay setup; runtime ActionMap is not persistent authoring data |
| Scene editing | Partial | Explorer multi-select/structure, single-Part gizmos/snapping/framing, prepared property batch API | Multi-object transform/precision workflow and Character-aware assembly |
| Materials/lighting | Complete for current bounded profile | Existing asset/Properties UI and Renderer Foundations 3–5 | Existing lights/probes/shadows/residency/hardware limits remain |
| Asset workflows | Partial | Atomic import/reimport/catalog, stable source-group child IDs, strict references | Guided compatibility/reimport consequences, socket composition and animation editing |
| Script editing/diagnostics | Complete for bounded source workflow | Retained native editor/Undo, diagnostics, drafts and separate SourceVersion commit | New generated policies must use these same ownership boundaries |
| Plugins/extensions | Complete for supported reviewed surface | Extension Platforms 1/2 and narrower MCP subset | Public Untrusted remains disabled; no ambient authority |
| Save/reopen and Play/Stop | Partial creator journey | Engine history/persistence and isolated Play; existing native/source journeys | Fresh combined animated Character/accessory project and invalid/lifecycle acceptance |
| Standalone packaging | Partial creator journey | Engine package job and bounded Studio launch | Package/run the new authored project with exact paired binaries |

Primary Engine source paths:
`src/classes/Character.cpp`, `src/animation/AnimationRuntime.cpp`,
`src/animation/AnimationTrack.cpp`, `src/editor/EditorHost.cpp`,
`src/runtime/SemanticSpatialResolver.cpp`, `src/render/RenderPublisher.cpp`,
and their focused tests. Studio evidence is pinned to
[Main](https://github.com/GantriaEngine/gargantuan-studio/blob/dc960b5433b73e1d305548e0dc5a8dfc0d9cf2f3/experiments/LuiStudio/App/Src/Main.luau),
RigBrowser, PreviewSession, property/source/Host code and native creator receipts.
The native Host metadata omission was verified in its frame proxy; a Luau model
receiving synthetic JointPositions alone cannot establish the actual path.

## Historical animation PR and bounded implementation order

Studio [PR #2](https://github.com/GantriaEngine/gargantuan-studio/pull/2)
is open at `e27fec00aeb790f0d3463b9ed1ffe836e7815ceb` on the retained
Avalonia branch. It provided inspection/preview and selectable joint overlay
behavior, not a keyframe timeline editor. Current LUI already has rig inspection
and playback. Its overlay and Attachment.JointPath guidance need a port and fresh
native qualification; the old branch/evidence remain preserved.

The selected implementation extends existing workflows: two-clip preview,
read-only composition inspection, joint overlays, guided Character/root/rig
assembly, gameplay setup, project-owned animation transitions and explicit
visual-only AttachmentMesh socket variants. Main/physics/Character authority,
semantic pose, prediction, native source buffers, EditorHost/history and immutable
render publication remain. No mandatory Humanoid, second controller, asset catalog
or network protocol is introduced.

Timeline/keyframe editing, canonical markers, IK, cross-rig retargeting and motion
warping have separate representation, cost and acceptance needs. They are not
inferred from blend preview or mock policy tests. Exact skeleton compatibility
continues to reject incompatible clips.

## Ownership and qualification

Animation, appearance, Studio and EditorHost use isolated worktrees. Root owns
shared registration/build/catalog integration and paired pins. Independent
correctness/security/performance review follows concrete patches. The concurrent
Node hosting campaign owns GameAdmission/GameSession/protocol and PlayerHost/
ServerHost admission seams; its unqualified changes are excluded from this baseline.

New work requires supported Windows/Linux Engine gates and actual LUI Studio/
Native Host journeys: valid/invalid workflows, source Undo, import/reimport,
save/reopen, Play/Stop, packaging, resource lifetime and performance.
Linux framework GTK checks alone do not qualify Linux Studio GUI.
Physical/provider F1 remains unqualified under its recorded scope.
Unavailable mandatory gates retain draft PRs with exact blockers; thresholds and
branch protections remain.
