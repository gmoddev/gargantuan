Note to self: Make sure to view the preview before committing.

<div align="center">

<img src="./assets/github/banner.png" alt="Gargantuan" width="656px" /> <br/> <img src="./assets/github/demo-sphere.gif" alt="Gargantuan" width="324px" /> <img src="./assets/github/demo-waveform.gif" alt="Gargantuan" width="324px" />

<h3>

An Independently Maintained Fork of Gargantuan

</h3>

<a href="./LICENSE.md"> <img src="https://img.shields.io/github/license/gmoddev/gargantuan?style=flat-square&label=License" alt="MPL-2.0 License" /> </a> <a href="https://github.com/teamfireworks/gargantuan"> <img src="https://img.shields.io/badge/Upstream-Team%20Fireworks-informational?style=flat-square" alt="Upstream Gargantuan" /> </a>

</div>

## About This Fork

This repository is an independently maintained fork of [Gargantuan](https://github.com/teamfireworks/gargantuan), originally developed and maintained by [Team Fireworks](https://github.com/teamfireworks).

It was split from upstream for personal development and experimentation with a different architectural direction and development methodology. It is not intended to replace or represent the upstream Gargantuan project.

The goal of this fork is to retain the core idea that made Gargantuan interesting to me, a standalone game engine built around Luau, Instances, a DataModel, and a familiar Roblox-inspired development model, while allowing the runtime, tooling, security model, networking, authoring workflow, and editor to evolve in a different direction.

Development in this repository has already diverged substantially from upstream and is expected to continue doing so.

This fork remains public. Team Fireworks and other Gargantuan contributors are welcome to reference, adapt, or independently implement ideas and changes made here where permitted by the project’s license.

For the original Gargantuan project, documentation, community, and contribution process, see the upstream repository:

https://github.com/teamfireworks/gargantuan

## Current Status

Gargantuan is still experimental, but the fork now supports a functional local creator loop rather than only foundational engine architecture.

The current workflow is capable of:

``` text
Create Project
    ↓
Author Instances
    ↓
Write Luau
    ↓
Save
    ↓
Play
    ↓
Run native rendering, physics, input, signals, tasks, and scripts
    ↓
Stop
    ↓
Return to the unchanged authoring project
```

The engine and Studio have been validated together using fresh Release builds and real process integration.

Current local gameplay testing has exercised:

- project creation, opening, Save, and Save As;
- authoritative project revisions and dirty state;
- Instance creation, deletion, duplication, and reparenting;
- authoritative transactions and Undo/Redo;
- Script and ModuleScript source authoring;
- Luau source diagnostics and persistence;
- isolated local Play/Stop sessions;
- runtime Instance creation and adoption;
- semantic subtree `Clone()`;
- Attributes and Tags;
- hierarchy and property signals;
- `RunService` frame signals;
- `task.spawn`, `task.defer`, `task.delay`, and `task.wait`;
- runtime keyboard and pointer input;
- physics simulation;
- `Touched` and `TouchEnded`;
- impulses;
- rendering and editor viewport capture;
- `print(...)`, `warn(...)`, and runtime error diagnostics;
- editor orbit, pan, zoom, and focus-selected navigation.

This does not mean the engine is feature complete. The purpose of the current stage is to validate that the core architecture works coherently enough to build real gameplay and use that gameplay work to drive the next engine requirements.

## Current Direction

Development is now transitioning from foundation-first work toward building real vertical slices and fixing concrete issues discovered through use.

Major completed architecture work includes:

- Explicit Instance lifetime, ownership, detached-state, and hierarchy contracts.
- Generation-safe object identity.
- Authoritative mutation and project revision semantics.
- Bounded authoritative transactions and Undo/Redo.
- Deterministic project persistence and atomic Save/Save As.
- Custom Classes, Class Extensions, Attributes, Tags, and custom enums.
- Runtime schema identity and exact definition-version validation.
- Protocol input hardening and bounded hostile-input handling.
- Backend-neutral networking contracts.
- Deterministic simulated transport.
- Production scheduler and backpressure rules.
- Valve GameNetworkingSockets behind a replaceable transport boundary.
- Server-authoritative basic client replication.
- Bounded reliable, unreliable, sequenced, and request/response Luau remotes.
- Independent networking adversarial validation.
- Box3D isolated behind a backend-neutral physics boundary.
- Serialization libraries isolated behind Gargantuan-owned codec contracts.
- SDL input isolated behind Gargantuan host/input semantics.
- SDL GPU resources isolated behind renderer backend boundaries.
- Gargantuan Studio project authoring, script editing, viewport rendering, and Play/Stop integration.
- Fresh production-like engine and Studio build validation.

The longer-term direction remains a standalone Luau development environment that retains the productivity and familiarity of a DataModel/Instance model while providing substantially more control over the engine, runtime, networking, hosting, moderation, tooling, and platform behavior.

## Gargantuan Studio

Gargantuan Studio is a separate private repository that acts as the current authoring environment for this fork.

Studio currently supports:

- New Project;
- Open;
- Save and Save As;
- authoritative dirty-state tracking;
- Explorer hierarchy inspection;
- Properties inspection and supported property editing;
- Create, Delete, Duplicate, and Reparent;
- authoritative Undo and Redo;
- Script and ModuleScript editing;
- bounded Luau syntax diagnostics;
- editor viewport rendering and picking;
- RMB orbit;
- MMB pan;
- mouse-wheel zoom;
- `F` to focus the selected object;
- local Play and Stop;
- runtime Output;
- runtime viewport input forwarding.

Studio is intentionally a client of the engine’s authoritative EditorHost interface. It does not maintain a second authoritative scene graph or bypass the engine’s mutation rules.

## Getting Started

This project is still under active development, so the exact build paths and requirements may change. Current build details are maintained in the development documentation.

After building the engine and Studio, Studio can be launched with a matching engine executable.

Example on Windows PowerShell:

``` powershell
dotnet "C:\path\to\gargantuan-studio\src\GargantuanStudio\bin\Release\net7.0\GargantuanStudio.dll" `
  --engine "C:\path\to\gargantuan\build\gargantuan.exe"
```

Studio can also open an existing project directly:

``` powershell
dotnet "C:\path\to\GargantuanStudio.dll" `
  --engine "C:\path\to\gargantuan.exe" `
  --project "C:\path\to\MyGame"
```

A project can now be created directly from Studio through:

``` text
File
→ New Project
```

Manual fixture copying is no longer required for normal project creation.

## Minimal Runtime Example

A Script can create runtime-only objects during Play:

``` lua
local Workspace = game:GetService("Workspace")

print("runtime starting")

local Part = Instance.new("Part")
Part.Name = "RuntimePart"
Part.Size = Vector3.new(4, 1, 4)
Part.CFrame = CFrame.new(0, 3, 0)
Part.Anchored = true
Part.Color = Color3.fromRGB(80, 170, 255)
Part.Parent = Workspace

warn("RuntimePart created")
```

Runtime-created Instances belong only to the isolated Play DataModel. Pressing Stop destroys that runtime state and returns Studio to the unchanged authoring project.

This separation is intentional.

## Clone Example

`Instance:Clone()` performs semantic subtree cloning rather than shallow userdata copying.

``` lua
local Workspace = game:GetService("Workspace")
local Original = Workspace:FindFirstChild("Original")

assert(Original ~= nil)

local Copy = Original:Clone()

Copy.Name = "Copy"
Copy.CFrame = CFrame.new(5, 3, 0)
Copy.Parent = Workspace
```

Clone behavior includes:

- detached clone roots;
- fresh Instance identity;
- cloned descendants;
- Attributes;
- Tags;
- Class Extension state;
- Custom Class state;
- Script source;
- supported internal object-reference remapping.

Runtime backend state such as physics bodies, renderer resources, signals, subscribers, and coroutine state is not copied.

## Current Luau Surface

The runtime uses Luau and exposes a growing Gargantuan-owned API surface.

Current useful areas include:

### Globals

- `game`
- `Instance`
- `print`
- `warn`
- `task`
- standard Luau libraries

### Instance and hierarchy

Current support includes common operations such as:

- `Instance.new`
- `Clone`
- `Destroy`
- `ClearAllChildren`
- `GetChildren`
- `GetDescendants`
- `FindFirstChild`
- `FindFirstDescendant`
- class and ancestor lookup helpers
- `IsA`
- `GetFullName`
- `Parent`
- `Name`

Nullable Instance-returning APIs return Luau `nil` when no object exists.

### Attributes and Tags

Attributes support:

- set;
- get;
- removal through `nil`;
- enumeration;
- change signals.

Tags support:

- add;
- remove;
- membership queries;
- tagged-instance queries.

The Tags service currently has one known direct-access inconsistency:

``` lua
game.Tags
```

may return `nil` before the lazy service has been constructed, while:

``` lua
local Tags = game:GetService("Tags")
```

constructs and returns it successfully.

After construction, `game.Tags` resolves to the same service.

### Tasks and runtime signals

Current runtime testing includes:

- `task.spawn`
- `task.defer`
- `task.delay`
- `task.wait`
- `RunService.PreSimulation`
- `RunService.PostSimulation`
- `RunService.PreRender`

Task scheduling semantics are Gargantuan-defined and should not be assumed to match Roblox ordering in every case.

### Input

`UserInputService` currently receives keyboard and pointer input forwarded from the focused Play viewport.

Editor and runtime input are intentionally separated.

### Physics

Current Luau-facing runtime behavior includes:

- automatic Part simulation;
- anchored and dynamic Parts;
- collisions;
- `Touched`;
- `TouchEnded`;
- `ApplyImpulse`;
- Workspace gravity.

### Scripts

`Script` and `ModuleScript` source can be authored and persisted through Studio.

Source authoring currently supports:

- authoritative source state;
- bounded UTF-8 source;
- optimistic conflict detection;
- Undo/Redo;
- Save/reopen;
- syntax diagnostics without execution.

Broader module/runtime behavior is still incomplete.

For the current detailed runtime surface, see:

`devdocs/CurrentArchitecture/LuauRuntimeSurface.md`

## Play Isolation

Play sessions run against an isolated runtime DataModel.

The authoring project and runtime world are deliberately separate:

``` text
Studio Authoring DataModel
        ↓
coherent Play launch state
        ↓
Runtime DataModel
        ↓
gameplay simulation
        ↓
Stop
        ↓
runtime state discarded
```

Runtime changes do not:

- modify the authoritative Studio hierarchy;
- increment the authoring project revision;
- change persisted revision state;
- enter Studio Undo history;
- become permanent unless explicitly authored outside Play.

This allows scripts to create, clone, destroy, simulate, and mutate runtime Instances without contaminating the project being edited.

## Editor Viewport Controls

Current Studio editor viewport controls are:

``` text
RMB drag   Orbit
MMB drag   Pan
Wheel      Zoom
F          Focus selected object
LMB        Pick/select
```

Editor camera state is separate from the Play runtime camera.

Entering Play does not destroy the editor view, and stopping Play restores the previous editor camera state.

## Runtime Diagnostics

During Play:

``` lua
print("hello")
warn("warning")
```

are routed through the runtime diagnostic system into Studio Output.

Current semantics include:

- `print` as informational Luau output;
- `warn` as warning-level Luau output;
- runtime script failures as error-level Luau output;
- bounded argument and message sizes;
- bounded runtime diagnostic queues;
- bounded Studio Output history.

Diagnostics are not project state and do not affect project revisions or persistence.

## Architecture

Several external implementations are intentionally isolated behind Gargantuan-owned semantic boundaries.

Current examples include:

``` text
Physics semantics
    ↓
IPhysicsBackend
    ↓
Box3D

Transport semantics
    ↓
IGameTransport
    ↓
GameNetworkingSockets

Serialization semantics
    ↓
Gargantuan codec contracts
    ↓
nlohmann / optional Glaze paths

Host and input semantics
    ↓
Gargantuan HostEvent / HostCommand
    ↓
SDL platform adapter

Render extraction
    ↓
immutable RenderSnapshot
    ↓
BaseRenderer
    ↓
SDL GPU renderer
```

The intent is not to abstract every dependency. Luau, for example, is considered part of the product semantics rather than a generic replaceable scripting backend.

The general rule is that backend-specific implementation types should not define engine semantics when the implementation may reasonably change independently.

## Networking

The networking architecture currently includes:

- bounded protocol input;
- generation-safe connection identity;
- explicit reliable, unreliable, and sequenced delivery semantics;
- deterministic simulated transport;
- scheduler priorities and backpressure;
- real GameNetworkingSockets transport;
- server-authoritative basic client replication;
- schema/version compatibility;
- peer visibility rules;
- reliable structural replication;
- reliable RemoteEvent;
- UnreliableRemoteEvent;
- UnreliableSequencedRemoteEvent;
- bounded RemoteFunction request/response;
- finite deadlines;
- per-peer and aggregate resource limits.

Networking Foundations 1 through 7 have also received an independent adversarial composition pass covering scheduler, replication, remotes, reconnects, authority, resource amplification, visibility, and lifecycle behavior.

Multiplayer gameplay services and final player/session APIs are still under development.

## Current Limitations

Gargantuan is still early.

Major currently incomplete or deferred areas include:

- final Player service and character/controller model;
- broader gameplay service coverage;
- asset identity/import/cooking pipeline;
- final materials and advanced rendering;
- broader ModuleScript/runtime module behavior;
- debugger and breakpoints;
- profiler;
- live editing and hot reload;
- Studio runtime hierarchy inspection;
- multiplayer Studio test orchestration;
- final native project/container format;
- package/distribution tooling;
- supported .NET migration for Studio;
- MCP integration;
- external script synchronization;
- collaboration/Teams;
- production crash reporting/telemetry.

Some APIs are intentionally different from Roblox, and others are simply not implemented yet.

Do not assume API parity based on familiar class or method names.

## Compatibility and API Philosophy

Gargantuan uses Luau, Instances, Services, a DataModel, and several familiar API patterns, but it is not intended to be a drop-in Roblox runtime.

Familiar names are used where they provide useful developer ergonomics, but the engine maintains its own:

- authority model;
- lifetime rules;
- replication semantics;
- scheduler behavior;
- service lifecycle;
- security boundaries;
- persistence model;
- editor architecture.

Where compatibility conflicts with clear engine semantics, Gargantuan semantics take priority.

## Experimental Status

This remains experimental software.

The engine and Studio are now capable of meaningful local gameplay testing, but they should not currently be considered production replacements for established game engines or development platforms.

The current development strategy is increasingly vertical-slice driven:

``` text
build something real
→ find concrete engine friction
→ reproduce it
→ fix or define the semantic contract
→ add regression coverage
→ continue building
```

This is preferred over implementing large speculative compatibility surfaces before they are needed.

## Upstream Gargantuan

Gargantuan is a 3D game engine scriptable using Luau, independently developed by Team Fireworks.

The original project describes its goals as providing a powerful, productive, multiplatform game engine with a familiar Luau API surface while allowing developers to own their platform, assets, and core scripts.

Upstream development is maintained separately by Team Fireworks:

- Repository: https://github.com/teamfireworks/gargantuan
- Documentation: https://gargantuan.teamfireworks.org/
- Contributing: https://gargantuan.teamfireworks.org/developing/contributing-to-gargantuan

Changes in this repository should not be interpreted as changes proposed, approved, or maintained by Team Fireworks.

## Prior Art

The original Gargantuan design was informed by several other game engines and projects. These references are retained from upstream for attribution:

| Resource                                                            | Info                                                            |
|:--------------------------------------------------------------------|:----------------------------------------------------------------|
| [Kinemium Engine](https://github.com/Qquaded/Kinemium-Engine)       | Initial reference implementation for some datatypes             |
| [Phoenix Engine](https://github.com/PhoenixWhitefire/PhoenixEngine) | Initial reference implementation for Instances and the renderer |
| [Kitbash’d](https://github.com/kitbashd)                            | Previously inspired the renderer                                |
| [Flux](https://github.com/thegalaxydev/flux)                        | Inspired the architecture of Instances and userdatas            |
| [Librebox](https://github.com/StayBlue/librebox-demo/)              | Examples used to test the Gargantuan engine                     |
| [Roblox Creator Documentation](https://create.roblox.com)           | API design inspiration                                          |

## License

This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

This fork retains the licensing and applicable copyright notices of the upstream Gargantuan source from which it was derived.

## Legal Notice

This repository is an independently maintained fork of Gargantuan.

The original Gargantuan project was created and is maintained by Team Fireworks. This fork is not maintained, authorized, or endorsed by Team Fireworks, and changes made here should not be attributed to the upstream maintainers.

Gargantuan and this fork are independent projects and are NOT affiliated with, authorized by, endorsed by, or in any way officially connected with Roblox Corporation. “Roblox” is a registered trademark of Roblox Corporation.

No reverse engineering, decompilation, or extraction of proprietary binaries, source code, or assets belonging to Roblox Corporation is represented as part of this fork.

The engine implementation is based on independently implemented runtime and API concepts intended for developer familiarity and interoperability.
