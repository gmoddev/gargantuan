# Minimum usable game milestone

## Outcome

The first supported game is a small single-map “collect and exit” experience. A
player launches from a project, walks/jumps with a third-person or first-person
camera, collides with primitive scenery, collects several items through
server-ready gameplay code, sees HUD/status/menu UI, hears feedback, reaches an
exit, restarts, and can be packaged as a desktop build.

Art may be deliberately simple. The milestone proves the workflow and contracts,
not visual breadth. A developer must build it entirely through documented public
APIs and project assets—no game-specific engine C++.

Asset Foundation 1 now closes the stable image/mesh/font identity, bounded
import/reimport, canonical artifact persistence, relocation, GUI image/font, and
renderer residency groundwork. A public mesh-consuming scene object, materials,
audio, packaging, and the polished asset-browser workflow remain milestone work.

## Required player-visible slice

- one Windows desktop target first, with headless tests; Linux/macOS follow once
  CI proves them;
- project manifest, source mounts, scene document, asset imports, and atomic save;
- reliable Luau Scripts/ModuleScripts with server/client-ready domains even when
  running locally;
- action-mapped keyboard/mouse plus one gamepad path;
- player camera and kinematic character controller with ground/jump behavior;
- Parts, collisions/triggers, raycast, tags/attributes, and ordered touch events;
- basic material/color/light/shadow presentation and imported image/mesh support;
- one-shot and looping audio with volume buses;
- `ScreenLayer`, Frame, Text, Image, Button, Stack/Flex layout, focus, and click/
  action routing;
- structured console, script stack traces/source locations, frame/physics/script/
  render counters, and clear load/import errors; and
- deterministic build/package command plus a smoke test that launches the result.

Animation can begin with transform/tween tracks sufficient for pickups and UI.
Rigged character animation may be the first follow-on if it would otherwise delay
proving the engine workflow.

## Foundation acceptance tests

### Clone and build

A clean recursive checkout follows one documented command sequence. CI builds the
same targets. Missing compiler, SDK, shader compiler, submodule, or generated file
produces an actionable error. The headless test suite requires no display/GPU.

### Project and data integrity

The sample can be saved, closed, reopened, and produce equivalent scene state.
Stable IDs and references survive reorder/rename. Interrupted save preserves the
last valid document. Malformed, enormous, deeply nested, traversal, and link-
escape fixtures fail within budgets and never execute scripts before trust.

### Runtime correctness

Hierarchy cycle/reentrancy/lifetime property tests pass under ASan/UBSan. Every
native Luau binding rejects wrong receiver/type/count without crashing. Modules
resolve relative/project identities, cache once per domain, diagnose cycles, and
release cleanly. Task/signal abuse is throttled and cannot hang one frame forever.

### Game loop

The sample runs for 30 minutes under an automated input path without native
errors, unbounded object/task/signal growth, or material frame degradation. It
handles resize/focus/device input transitions, restart, and clean shutdown.
Physics edits and scene reloads synchronize bodies deterministically.

### Presentation

Reference screenshots confirm primitives, imported assets, text, clipping,
layout, focus states, and common DPI sizes. Missing assets show bounded fallback
content and diagnostics. Audio device loss is recoverable or fails gracefully.

## Public API needed for the sample

| Area | Minimum surface |
|---|---|
| Scene | `Experience`, `World`, `Instance`, `Part`, `Camera`, stable IDs, parenting, clone/destroy, tags, attributes |
| Script | `Script`, `ModuleScript`, typed Signal/Connection, bounded `Task` scheduling, domain query |
| Simulation | fixed frame phases, raycast/overlap, collision groups, constraints needed by sample, character controller |
| Input | `InputService`, `ActionMap`, binding/rebinding, handled state, mouse/gamepad |
| UI | `ScreenLayer`, `Frame`, `Text`, `Image`, `Button`, `StackLayout`/`FlexLayout`, focus/navigation |
| Assets/audio | `AssetService`, `AssetRef`, texture/mesh/font/sound import; `AudioService`, `Sound` |
| Diagnostics | structured log levels/categories such as `[Runtime:Script]`, stack/source locations, counters/profiler capture |
| Project tools | validate, import, run, test, and package commands |

Avoid adding broad APIs just for perceived parity. Every initial member needs a
game use, error semantics, schema/access flags, documentation, and tests.

## Work explicitly deferred

- public remote multiplayer transport and matchmaking;
- large-world streaming/terrain;
- advanced PBR/post-processing/particles;
- full rig authoring and cinematic animation tools;
- mobile/VR/console releases;
- marketplace/economy/voice/cloud analytics;
- collaborative editing and general third-party plugins; and
- broad Roblox compatibility beyond a small versioned migration adapter.

Networking is deferred as an implementation, not ignored as a design constraint.
Stable IDs, schemas, execution domains, change journals, and the sample’s gameplay
code must already support a later authoritative split. Immediately after this
milestone, run the same sample through a local `GameServer` and two client
processes as defined in `Roadmap.md`.

## Definition of done

The milestone is complete only when a developer unfamiliar with engine internals
can follow the docs on a clean machine, make a scene/script/UI/audio change, see
the result, diagnose an intentional error, save/reopen, run tests, and package the
sample without touching C++. Passing one developer machine or a hand-built demo
does not qualify.
