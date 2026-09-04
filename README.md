<div align="center">

<img src="./assets/github/banner.png" alt="Gargantuan" width="656px" />

# Gargantuan

**An experimental standalone game engine built around Luau, Instances, a DataModel, and server-authoritative runtime semantics.**

<a href="./LICENSE.md"><img src="https://img.shields.io/github/license/gmoddev/gargantuan?style=flat-square&label=License" alt="MPL-2.0 License" /></a>
<a href="https://github.com/teamfireworks/gargantuan"><img src="https://img.shields.io/badge/Derived%20from-Team%20Fireworks%20Gargantuan-informational?style=flat-square" alt="Derived from Team Fireworks Gargantuan" /></a>

</div>

## Overview

Gargantuan is a C++23 game engine centered on Luau scripting and an Instance/DataModel programming model familiar to Roblox developers, but with its own runtime, authority, networking, persistence, editor, security, and platform semantics.

This repository was originally forked from [Team Fireworks' Gargantuan](https://github.com/teamfireworks/gargantuan). It has since diverged substantially in architecture and scope and should no longer be treated as an alternate branch of the upstream project. The original project remains the source of the codebase's provenance; this repository now follows an independent design direction.

Gargantuan is not intended to be a drop-in Roblox runtime. Familiar names and concepts are retained where useful, but compatibility does not take precedence over engine-defined behavior.

## Current Status

Gargantuan is still experimental, but it is beyond a rendering or scripting proof of concept. The current engine and Studio support an end-to-end local creator loop:

```text
Create/Open Project
        ↓
Author Instances and Luau
        ↓
Save / Undo / Redo
        ↓
Play in an isolated runtime DataModel
        ↓
Rendering, physics, input, scripts, animation and gameplay
        ↓
Stop
        ↓
Return to the unchanged authoring document
```

Implemented systems include:

- generation-safe Instance identity and explicit lifetime/ownership rules;
- an authoritative DataModel and validated mutation gateway;
- ordered change journals, project revisions, transactions, Undo/Redo, and deterministic persistence;
- native classes plus runtime schema support for custom classes, class extensions, custom enums, Attributes, and Tags;
- Luau execution, tasks, signals, Script/ModuleScript authoring, bounded diagnostics, and capability-aware native boundaries;
- renderer-neutral immutable render publication, editor picking, lighting/environment foundations, skeletal animation, GPU skinning, and semantic animated attachments;
- backend-neutral rigid physics with Box3D plus a separate deformable/XPBD path;
- player lifecycle, default engine-shipped Luau input/camera/locomotion policy, kinematic character movement, and root-motion authority;
- server-authoritative networking with bounded protocol handling, scheduling/backpressure, reliable/unreliable/sequenced delivery, RemoteEvent/RemoteFunction semantics, GameNetworkingSockets integration, prediction/reconciliation, interpolation, spatial relevance, and bounded Character publication;
- packaged player/server execution paths and project packaging infrastructure;
- a public versioned EditorHost boundary used by the separately authored Gargantuan Studio.

The project is developed against explicit ownership and trust-boundary invariants. Renderer state, Studio state, replication projections, snapshots, and caches are intentionally non-authoritative views of the live DataModel.

## Architecture at a Glance

```text
                    Gargantuan Studio
                         │
                 versioned EditorHost IPC
                         │
                         ▼
Authoring commands ──> MutationGateway ──> Authoritative DataModel
                                             │
                      ┌──────────────────────┼──────────────────────┐
                      │                      │                      │
                      ▼                      ▼                      ▼
                ChangeJournal       Render Publication       Replication
                      │                      │                      │
                      ▼                      ▼                      ▼
             Studio projections       Renderer backend       Network peers
```

Important architectural rules include:

- the DataModel owns live Instance state and authoritative mutation;
- `ObjectId` identity is generation-safe and never pointer-, path-, or name-based;
- external mutations are bounded, validated, authorized, and committed atomically;
- rendering consumes immutable published value data rather than traversing the live graph;
- Studio receives DTOs, schema, journals, stable IDs, and pixels through EditorHost rather than linking to engine internals;
- transport identity is distinct from Player identity;
- backend implementations such as Box3D, SDL GPU, serialization libraries, and GameNetworkingSockets sit behind Gargantuan-owned semantic boundaries where replacement is expected;
- Luau is a product-level runtime choice rather than a generic replaceable scripting backend.

For the current invariant and subsystem map, start with [`AICONTEXT.md`](./AICONTEXT.md) and [`docs/architecture/README.md`](./docs/architecture/README.md).

## Gargantuan Studio

[Gargantuan Studio](https://github.com/gmoddev/gargantuan-studio) is a separate C#/Avalonia editor application. It does not link against private engine internals; it launches the public EditorHost and communicates through its versioned local protocol.

The current Studio supports, among other things:

- New/Open/Save/Save As;
- authoritative dirty state, Undo, and Redo;
- Explorer and schema-driven Properties editing;
- create/delete/duplicate/reparent operations;
- Script and ModuleScript source editing with Luau diagnostics;
- viewport rendering, picking, orbit/pan/zoom, focus, and Move/Rotate/Scale gizmos;
- dockable/floating editor workspaces and persisted per-user layouts;
- Assets and Output tools;
- isolated local Play/Stop sessions;
- an opt-in bounded local bridge for Gargantuan MCP tooling.

Studio maintains a non-authoritative document projection and submits edits back through the engine's normal mutation path.

## Related Repositories

The wider Gargantuan project is split across several repositories:

| Repository | Purpose |
| --- | --- |
| [`gargantuan`](https://github.com/gmoddev/gargantuan) | Core engine, runtime, EditorHost, networking, packaging, rendering and physics boundaries |
| [`gargantuan-studio`](https://github.com/gmoddev/gargantuan-studio) | C#/Avalonia authoring environment and Studio-side tooling boundary |
| [`gargantuan-node`](https://github.com/gmoddev/gargantuan-node) | Go backend-service foundation for Core discovery, DataStore, player authentication, entitlements and diagnostics |
| [`gargantuan-mcp`](https://github.com/gmoddev/gargantuan-mcp) | Conservative MCP server for bounded Studio-backed inspection and opt-in authoring operations |
| [`gargantuan-telemetry`](https://github.com/gmoddev/gargantuan-telemetry) | Optional replaceable Rust telemetry library and C ABI for crash/performance reporting |

These repositories are intentionally separated by explicit process and protocol boundaries rather than sharing engine internals.

## Building

Clone recursively because the engine uses Git submodules:

```bash
git clone --recursive https://github.com/gmoddev/gargantuan.git
cd gargantuan
```

The current development workflow uses CMake, Ninja, Just, Rokit/Lute-generated class sources, and `glslc` for shaders.

Typical setup:

```bash
rokit install
lute tools/classgen
just configure build_type=Release
just build
```

The exact platform prerequisites and validation commands are documented in [`devdocs/Compiling.md`](./devdocs/Compiling.md).

The build produces the engine executable as well as separate player and packaging targets. Optional GameNetworkingSockets support is controlled by CMake configuration.

## Runtime Example

```lua
local Workspace = game:GetService("Workspace")

local part = Instance.new("Part")
part.Name = "RuntimePart"
part.Size = Vector3.new(4, 1, 4)
part.CFrame = CFrame.new(0, 3, 0)
part.Anchored = true
part.Parent = Workspace

print("created", part:GetFullName())
```

Code executed during Play operates on the isolated runtime DataModel. Stopping the session destroys runtime-only state and returns Studio to the authoring project rather than merging gameplay mutations back into it.

## Networking

Networking is designed as an engine subsystem rather than a thin socket wrapper. The current implementation includes:

- bounded hostile-input handling and versioned protocol contracts;
- deterministic simulated transport for validation;
- scheduling, priorities, admission and backpressure;
- optional Valve GameNetworkingSockets transport;
- structural replication and peer-specific materialization;
- reliable, unreliable, and sequenced delivery semantics;
- bounded RemoteEvents and request/response RemoteFunctions;
- production-style session and Player/Character lifecycle handling;
- Character prediction, reconciliation, interpolation, root-motion authority and variable-rate publication;
- server-owned spatial regions, relevance, enter/leave/reentry, and owner-character pinning.

Production account identity, discovery/matchmaking orchestration, and the final online service integration are still evolving.

## Backend Services

`gargantuan-node` provides the current backend-service foundation. Its implemented contracts include Core registration/discovery, Diagnostics, revisioned key/value and document/query DataStore services, player authentication, and entitlement checks over generated protobuf/gRPC APIs.

The engine is not required to depend on a Node process for local/offline execution. Online identity and service authority are designed as explicit optional boundaries rather than assumptions embedded into the core runtime.

## Experimental Scope

Gargantuan is not feature complete and should not yet be considered a production replacement for established engines.

Major areas still under active development include broader gameplay APIs, asset import/cooking, final rendering/material systems, debugger/profiler UX, collaboration and multiplayer Studio orchestration, production account/session infrastructure, packaging/distribution polish, and wider platform validation.

Current verified defects and engineering gaps are tracked in [`KNOWN_ISSUES.md`](./KNOWN_ISSUES.md). Roadmaps and future-architecture documents should not be read as implemented behavior.

## Documentation

The repository contains both current and historical/future design material. When documents disagree, tests, enforced invariants, and verified current architecture take precedence.

Useful entry points:

- [`AICONTEXT.md`](./AICONTEXT.md) — compact architecture and subsystem routing map;
- [`docs/architecture/README.md`](./docs/architecture/README.md) — architectural authority and documentation structure;
- [`docs/invariants/Core.md`](./docs/invariants/Core.md) — core invariants;
- [`devdocs/CurrentArchitecture/`](./devdocs/CurrentArchitecture/) — detailed subsystem contracts and implementation notes;
- [`devdocs/Compiling.md`](./devdocs/Compiling.md) — build and test workflow;
- [`KNOWN_ISSUES.md`](./KNOWN_ISSUES.md) — verified current defects/gaps.

## Relationship to the Original Gargantuan

This project retains provenance from the original [Team Fireworks Gargantuan](https://github.com/teamfireworks/gargantuan) codebase and remains distributed under its applicable open-source licensing terms.

The current `gmoddev/gargantuan` codebase, however, has accumulated substantial independent architecture and implementation across editor integration, mutation/persistence semantics, runtime schema, networking, Character systems, security boundaries, physics/render isolation, packaging, and related services. New users should evaluate this repository as its own evolving engine rather than assuming behavior, compatibility, roadmap, or contribution practices from upstream.

## License

Gargantuan is licensed under the [Mozilla Public License 2.0](./LICENSE.md).
