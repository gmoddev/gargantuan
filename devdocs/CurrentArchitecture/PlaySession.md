---
status: current
owner: editor-runtime
last_verified: 2026-08-24
related_code:
  - src/editor/PlaySession.cpp
  - src/Engine.cpp
  - src/services/Players.cpp
  - tests/FoundationTests.cpp
  - tests/PlayerRuntimeTests.cpp
related_adrs: []
---

# Minimal local Play session

## Accepted ownership model

EditorHost owns at most one `PlaySession`. Start captures the current authoritative
in-memory project state through the project serializer, including committed unsaved
changes, and deserializes it into a new `DataModel`. The runtime graph, `Engine`,
`ScriptEngine`, scheduler, `ProcessService`, and headless renderer are session-owned.
No authoring `Instance`, service, transaction, history entry, revision counter, or
dirty-state field is shared with the runtime graph.

This is an isolated in-process runtime, not a child process. That choice reuses the
existing Engine lifecycle and offscreen viewport while keeping a single local VM and
avoiding a premature process/game-server protocol. Consequently there is no runtime
child process to orphan or forcibly terminate. Studio bounds Start requests to 15
seconds and Stop requests to 3 seconds; EOF or host destruction synchronously stops
and destroys the owned session.

## Protocol and lifecycle

`StartPlaySession`, `StopPlaySession`, `GetPlaySessionState`,
`PollPlayDiagnostics`, and `SendPlayInput` require the token-bound EditorHost session.
Start also requires `EditorCommands` and `ReadDataModel`; input requires
`ViewportControl`. IDs are engine-issued canonical nonzero decimal strings, and every
session-scoped request must carry the exact active ID. A stale ID fails closed.

The states are `Stopped`, `Starting`, `Running`, `Stopping`, and `Failed`. Start is
rejected when no project is open, an authoring transaction is open, or another
session exists. All authoritative project/session/edit methods are rejected while a
play session is retained. Stop is idempotent at the Studio service boundary and exact
at the host boundary. Natural runtime exit and step exceptions become a controlled
`PlaySessionExited` path so Studio can perform normal cleanup.

## Viewport, input, and diagnostics

The existing EditorHost viewport renderer consumes the exact immutable
`RenderPublication` produced by the runtime Engine while Running. The initial
full publication and later deltas carry the same mesh, material, texture, GUI,
and font-atlas residency used by the session's headless renderer; EditorHost
does not re-extract the runtime through its edit publisher. Each response is
labelled `Mode: Play` with the exact `PlaySessionId`; Studio drops a mismatched
or stale frame. Start and Stop replace the viewport projections, and after Stop
capture returns the authoring Workspace as `Mode: Edit`. Runtime selection and
authoring camera mutation are intentionally unavailable during Play.

Input is a closed `HostEvent` subset: focus, bounded key transitions, finite
pointer movement, and semantic pointer-button transitions. Studio forwards input
only while the runtime viewport is focused and sends focus release on blur.
`PointerMoveEvent.Delta` is relative motion regardless of host implementation;
`PlaySession`, `Engine`, `UserInputService`, and gameplay never reconstruct it
from absolute cursor positions. EditorHost returns relative-pointer commands to
Studio, whose native host obtains physical relative motion and owns capture
cleanup on button-up, focus loss, Stop, runtime exit, or disconnect. Runtime diagnostics are sequence-ordered, timestamped,
severity/category/message records, capped at 256 records and 2,048 message bytes.
Luau `print` uses `Information / Luau`, `warn` uses `Warning / Luau`, and runtime
errors use `Error / Luau`. The bounded queue evicts its oldest record under
pressure and never waits for Studio consumption. Formatting is UTF-8 checked,
argument/message bounded, deterministic, and does not invoke user metamethods.
Luau diagnostics and lifecycle events use this sink and never become authoring
mutations.

Stop destroys the exact runtime Engine/VM/graph/renderer and discards all runtime
mutation. It never saves, changes persisted revision, advances authoritative
revision, dirties the project, or adds Undo/Redo history.

## Player runtime startup and teardown

The runtime Engine initializes the canonical Players service with exactly one
local Player, attaches ActionMap to the runtime UserInputService, and creates the
unarchivable shipped-Luau player module subtree. On the first Script step those
modules install default semantic bindings, assemble the Player character, and
connect controller/camera policy to `PreSimulation` and `PreRender`. The subtree
exists only in the deserialized runtime graph; it is never copied back to the
authoring graph.

Stop destroys the shipped modules first so their Luau cleanup releases pointer
capture, signal connections, ActionMap bindings, camera state, and character
Instances. Players then removes LocalPlayer, and Engine destroys the remaining
runtime world/VM. Focus loss and Stop both clear physical and semantic input.
Repeated Play/Stop tests verify that authoring descendants, revision, dirty
state, and history remain unchanged while runtime actions, Players, characters,
modules, and connections are recreated and discarded per session.

## Deliberately deferred

Multiplayer, client/server topology, live-edit synchronization, pause/step/debug,
breakpoints, test sessions, device emulation, runtime object selection, runtime
changes applied back to authoring, plugin execution, source mounting, GASM behavior,
simulation ownership, and asset importing remain outside this foundation.
