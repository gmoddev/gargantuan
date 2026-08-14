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

`InstanceProperty` metadata carries readable domains, writable domains, and
required read/write capabilities. Native Luau property dispatch enforces that
metadata. `MutationGateway` also captures and enforces the submitting security
context, including when a worker command is later drained on Main. Studio
mutation remains an EditorHost command applied by the gateway; Studio never
receives an authoritative `Instance` pointer.

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

## Deferred

Server and Client have identities but no final default grant profiles. There is
no plugin grant/consent system, persistent project trust, capability delegation,
or public Luau capability API yet. Filesystem, process, and network capabilities
are vocabulary only in this pass; Studio is not granted them.
