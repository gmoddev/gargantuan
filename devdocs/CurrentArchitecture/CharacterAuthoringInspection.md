---
status: current
owner: editor-host
last_verified: 2026-11-15
related_code:
  - include/gargantuan/editor/CharacterCompositionInspection.hpp
  - src/editor/CharacterCompositionInspection.cpp
  - src/editor/EditorHost.cpp
  - tests/CharacterCompositionInspectionTests.cpp
  - tests/CharacterAuthoringEditorHostTests.cpp
related_adrs: []
---

# Read-only Character authoring inspection

The optional EditorHost capability CharacterComposition and handshake field
CharacterCompositionVersion: 1 expose one bounded authoring-graph inspection.
The public envelope and protocol version remain unchanged.

GetCharacterComposition requires ReadDataModel, an open project and an
established snapshot. Params contain exactly CharacterCompositionVersion (1),
Scope (the current Snapshot.Cursor.Scope) and Object (a live Character or
KinematicCharacter identity). Engine checks complete generation/scope and type
before inspection. There is no filesystem path, provider, caller-selected
service or mutation in the request.

The result contains CharacterCompositionVersion, Context (Authoring), Scope,
AuthoritativeRevision, Character, nullable RootPart, Complete, Valid,
ObjectsVisited and bounded Meshes, Attachments and Issues arrays. Mesh entries
report current Mesh/optional Material asset identities, content revisions,
canonical skeleton compatibility, joint/Animator counts and collision facts.
Asset state describes retained canonical data, including valid prior content
during reimport; it creates no replacement service. Attachment JointPathExists
describes path existence in the current canonical rig. It does not certify an
active runtime pose binding after incompatible reimport.

Fixed public limits are 512 visited objects, 128 meshes, 256 attachments and
128 issues. Iterative traversal, references and diagnostics are bounded.
Exhaustion returns Complete=false with a LimitReached issue and never claims a
complete valid result. Incomplete is distinct from an empty successful graph.

Issues are informational, warning or source-backed structural errors.
Character's nullable RootPart remains valid. Runtime clip selection is not a
saved Animator property, and absent visual configuration is advisory.
Inspection does not add another validation registry or gate ordinary edits.
Studio still submits typed RootPart references through SetProperty; Engine
enforces BasePart, project scope and descendant constraints. References are not
prepared-batch stores.

The helper runs on Main and observes existing identities/assets only. It does
not allocate ObjectIds, create services, step simulation/physics, evaluate a
gameplay pose, dirty persistence, publish journal records or create history.
The request is exempt from legacy group expiry, so observation cannot commit
an unrelated open transaction. Context prevents treating authoring readiness
as live client/server movement evidence.

Malformed fields/identities, unsupported version, unauthorized access, absent
project/snapshot, stale project/object and non-Character targets produce
structured EditorHost errors. No native pointer or GPU handle crosses this
boundary. Studio fences replies by connection, scope, selection identity and
authoritative revision and presents limited/advisory results explicitly.

Focused tests cover logical and Part-only Characters, imported rigs,
socket/asset readiness, stale lifetimes, bounds and nonmutation, plus typed
RootPart denial, history, save/reopen and the public request contract.
Native/GUI acceptance remains a separate qualification gate. Visual-only
AttachmentMesh composition uses existing schema and render publication and
creates no gameplay authority.
