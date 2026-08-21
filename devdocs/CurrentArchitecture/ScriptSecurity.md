---
status: current
owner: runtime
last_verified: 2026-08-21
related_code:
  - include/gargantuan/scripting/ScriptSecurity.hpp
  - src/classes/Script.cpp
  - src/datatypes/Signal.cpp
  - src/scripting/ScriptSecurity.cpp
  - src/scripting/ThreadEngine.cpp
  - src/services/ProcessService.cpp
related_adrs: []
---

# Script execution domains and capabilities

## Implemented now

Gargantuan represents five script execution contexts: `Core`, `PreRun`,
`Studio`, `Server`, and `Client`. A domain identifies where code is executing; it is not
a numeric trust level. Authorization is an explicit capability-set test, so a
Client context granted one operation can perform it while a Core context
without that capability cannot.

The initial capabilities are `ReadDataModel`, `MutateDataModel`,
`EditorCommands`, `SelectionAccess`, `ViewportControl`, `FilesystemRead`,
`FilesystemWrite`, `ProcessControl`, `NetworkSend`, `NetworkReceive`, and
`DefineSchema`.
`CoreTrusted()` currently grants the complete set for existing engine-internal
execution. EditorHost uses `StudioCoreUi()`, which grants only
`ReadDataModel`, `MutateDataModel`, `EditorCommands`, `SelectionAccess`, and
`ViewportControl`. Every viewport transport, configuration, camera, capture,
and picking request enforces `ViewportControl` at EditorHost dispatch. The
grant does not expose renderer or shared-memory implementation handles to Luau.

Regular Scripts enter `ServerRuntime()` or `ClientRuntime()` according to their
`RunContext`. Both profiles grant gameplay DataModel read/mutation and the
existing bounded network send/receive capabilities. They do not grant Studio,
viewport, editor, selection, filesystem, process, or schema-definition
authority. Engine-shipped player modules run as an ordinary client-context
Script and receive no privilege solely from their shipped location.

`InstanceProperty` metadata carries readable domains, writable domains, and
required read/write capabilities. Native Luau property dispatch enforces that
metadata. `MutationGateway` also captures and enforces the submitting security
context, including when a worker command is later drained on Main. Studio
mutation remains an EditorHost command applied by the gateway; Studio never
receives an authoritative `Instance` pointer.

Dedicated script Source reads require EditorCommands plus ReadDataModel;
Source writes require EditorCommands plus MutateDataModel and pass through the
MutationGateway with project scope, stable identity, UTF-8/size validation, and
the expected SourceVersion. `Source` itself is Engine-only reflection state and
generic `SetProperty` rejects it. Server/Client code and ordinary project
scripts do not receive the EditorHost session token or source operations, so
Studio authoring does not create a gameplay source-disclosure path.

Minimal Play startup also requires EditorCommands plus ReadDataModel. The host
serializes the current authoritative state into a distinct runtime DataModel and
constructs the normal Script VM against that graph. Runtime code receives no
EditorHost token, project transaction/history object, Studio-domain grant,
filesystem, process, viewport, or schema capability. `SendPlayInput` is a native closed HostEvent adapter
guarded by ViewportControl; it is not a general IPC or capability delegation surface.

Task scheduling and Luau signal connections capture the current context and
restore it whenever a continuation or callback resumes. A native event fired
later from ambient Core therefore cannot upgrade the callback that an ordinary
runtime Script registered. Signal Wait uses the same rule. `ProcessService`
checks `ProcessControl` for exit and stdout operations; ordinary player/runtime
profiles do not receive it.

The current context is thread-local and defaults to trusted Core for backwards
compatibility with engine-owned call paths. New untrusted script entry points
must establish a `ScriptSecurityScope` before dispatch. Treating the default as
an ambient permission for new code is prohibited.

`PreRunRegistration()` grants only `DefineSchema`. The native bootstrap selects
the project registration source and installs this context; being in the
`PreRun` domain, using a `Game` namespace, or having `Game` provenance does not
authorize registration. The callback also requires the lifecycle to be in its
hidden-candidate `PreRunRegistration` phase. Freeze remains authoritative even
if a context retains the capability. PreRun receives no DataModel mutation,
filesystem, process, network, Studio, or viewport capability.

`Schema:RegisterClass` follows the same rule as enum and extension registration:
it requires both the hidden `PreRunRegistration` lifecycle phase and the native
`DefineSchema` capability. The resulting class grants no construction or
mutation authority. Runtime construction and custom property writes continue
to require the ordinary DataModel mutation path after the registry is frozen.

## Deferred

There is no plugin grant/consent system, persistent project trust, capability
delegation, public Luau capability API, or final per-experience/per-player grant
policy yet. Server/client network grants support the existing bounded Remote
surface but do not imply a final multiplayer authority or Player ownership
model. Studio is not granted filesystem, process, or network authority.
