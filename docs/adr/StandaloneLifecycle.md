---
status: accepted
date: 2026-11-27
supersedes: null
superseded_by: null
---

# Standalone application and gameplay lifetime

The owner accepted the [lifecycle recommendation](../../devdocs/Research/StandaloneLifecycle/README.md) at `fb3cc0065`. This decision governs the Player and Studio Play hosts.

## Ownership

A standalone application owns the platform event pump, graphics and audio devices, its presentation graph and its entry VM. It can own zero or one gameplay session. Each new session owns a fresh DataModel, VM, authority, input state and provider handles. A session can contain several spatial domains. Selecting a save, starting a session and transferring an object between domains are distinct operations.

An optional ApplicationEntry identifies a client Script by its durable source binding in the project lineage. Its direct parent is the presentation Folder. Package capture resolves the binding exactly once against the captured revision. Capture must verify the code audience, the folder's dependency closure and its references before execution. A package without the designation continues through ordinary direct startup without an application VM. A malformed designation fails admission.

Application code registers a ScreenGui in its own graph with `game:SetMainMenu(Menu)`. The host projects only the registered root. Registration and visibility changes invalidate previous input targets. The app keeps its graph, tasks, menu and device ownership when a session ends. Commands that can replace or destroy a session commit after the active callback returns.

## Time and authority

The session authority commits pause requests at complete simulation boundaries. Each live control owner can hold one pause per session generation. The last release resumes simulation. The debugger can hold an independent pause. Simulation timers and physics consume the same logical clock. Real-time deadlines for admission, networking and storage continue to use their existing clocks. Local menus and application audio continue on application time. A remote client may suppress its own inputs and display an overlay. An authoritative global pause requires a separately negotiated grant.

## Integration and release

Player and Server are produced from the Engine integration revision. The lifecycle branch owns its Engine changes. Spatial construction owns spatial admission and persistent voxel qualification. Desktop interaction owns displayed-frame and input routing. Document editor and PreRun own authoring transactions, source bindings and Play capture. Studio capability negotiation and edits require a compatible combination of those revisions. Node continues to own authenticated hosting.

The [production gates](../../devdocs/Research/StandaloneLifecycle/Roadmap.md) govern availability. Release evidence includes installed direct and opt-in Player exports, an edited and reopened voxel world, independent network clients, and Studio Play/Stop with preserved editor state. Component and research tests have narrower evidentiary scope.
