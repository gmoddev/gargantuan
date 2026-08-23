# Current subsystem audit

Status reflects reachable behavior at commit `e4fca3575cc84c0d5fa4a946b88bf528aac2223b`.
"Usable" means a developer can rely on the feature without routinely changing
C++; it does not mean API-complete.

## Capability matrix

| Area | Status | What exists | Main blockers |
|---|---|---|---|
| Build and packaging | Partial | CMake C++23 build, vendored dependencies, GLSL compilation, SPIR-V and Apple Metal shader paths. | No engine CI, release artifacts, installer, dependency lock verification, or tested platform matrix. Generated headers and submodules make a fresh build sensitive to checkout setup. |
| CLI and lifecycle | Partial | Load a project, script, or Instance JSON; windowed or headless loop; process-controlled exit. | Parsed compatibility/log-level options are not applied; no edit/play/pause/reload states or graceful fault boundary. |
| Instance model | Partial | Hierarchy, properties, signals, cloning/serialization hooks, native/Luau reflection. | Raw parent pointer, cycles allowed, reentrant partial mutations, writable lifecycle state, no stable IDs, transactions, schema migrations, or undo journal. |
| Luau execution | Partial, unsafe for untrusted code | Scripts compile and run, native datatypes/classes bind into Luau, signals and `task` primitives exist. | One shared VM, incomplete module resolution, unchecked native conversions, no instruction/memory budgets, weak capability isolation, and several lifetime defects. |
| Modules | Scaffold | Luau require callbacks and `ModuleScript` type exist. | Instance path lookup is unfinished; unchecked static casts can reinterpret arbitrary Instances; cache/lifetime behavior is not robust. Do not count `require` as implemented. |
| Scheduling | Partial | Scheduled, delayed, spawned, and deferred work queues. | No quotas, cancellation, ownership, fairness, observability, or shutdown semantics; self-defer can monopolize a frame and recursive spawn consumes native stack. |
| Serialization | Partial | Versioned JSON tree format with tagged property values and `Archivable` filtering. | Binary enum is unimplemented; no stable object/reference IDs, limits, unknown-field preservation, atomic save, robust diagnostics, or network schema. Malformed input can throw or read invalid array elements. |
| Projects and source sync | Partial | `.gargantuan/project.instance.json`; canonical root-confined, bounded `SourceMount`; transactional `FileLink` compatibility import for scripts, directories, and nested Instance JSON. | No live watch, trust prompt, conflict handling, deterministic source map, import database, or working editor save loop. |
| Rendering | Prototype | SDL GPU device, primitive mesh upload, opaque pass, directional shadow pass, free camera. | No asset/material pipeline, lighting service, textures, text, batching, culling, transparency model, post-processing, device recovery, or useful diagnostics. Several primitive visuals are placeholders. |
| Physics | Prototype | Box3D world, fixed-step accumulator, primitive bodies, anchoring, sensors, contacts mapped to touch signals, basic constraints. | Property edits do not reliably rebuild/synchronize bodies; geometry and constraint validation are weak; no queries, characters, collision groups, streaming, deterministic network state, or ownership model. |
| Input | Prototype | SDL keyboard, mouse, focus/window events and Luau signals. | Incorrect mouse collections/location, exceptions for unknown buttons, no text/gamepad/touch/action layer, consumption, focus routing, remapping, or accessibility. |
| Runtime GUI | Scaffold/disabled | `GuiBase`, `GuiBase2d`, `GuiObject`, and `Frame`; UI vertex/pass scaffolding. | Layout is unfinished, GUI render-pass construction is disabled, drawing is commented out, and there are no screens, text, images, buttons, clipping, focus, input routing, or layout primitives. |
| Studio | Concept demo | A Luau Studio project creates one red `Frame` through Fluid. | The engine cannot render that UI. There is no editor host, viewport tooling, hierarchy/property panels, selection, gizmos, undo, save, play isolation, script editor, diagnostics, assets, plugins, or packaging. |
| Networking | Absent | An unregistered container scaffold named `ReplicatedStorage`. | No transport, sessions, players, authority, replication, RPC, interest management, prediction, rate limits, authentication, or server executable role. |
| Assets | Absent as a platform service | Built-in primitive meshes; SDL image/font libraries are linked. | No import database, content-addressed identity, dependency graph, cache, texture/font/model loaders exposed to games, hot reload, packaging, remote retrieval, or licensing metadata. |
| Audio | Absent | No effective audio runtime was found. | Device, mixer, sound objects, spatial audio, streaming, buses, lifecycle, and asset integration are all needed. |
| Animation | Absent | No effective animation system was found. | No clips, tracks, rigs, interpolation, state machine, events, replication, or editor tooling. |
| Configuration | Scaffold | CLI flags, a Studio config JSON, documented compatibility ideas. | Config is not consumed consistently. Documentation, CLI flag names, and sample shape disagree; compatibility does not alter behavior. |
| Logging and diagnostics | Partial | SDL log callback with pretty/JSON presentation and basic errors. | Requested log levels are ignored, project-controlled control characters are not neutralized, and there are no source locations, structured engine codes, profiler surfaces, crash reports, or in-editor console. |
| Testing | Very low coverage | Four Luau specs for `Axes`, `CFrame`, `Vector2`, and `Vector3`. | A Vector2 arithmetic test is disabled after a segfault. No C++ unit, integration, serialization, renderer, physics, scheduler, security, fuzz, sanitizer, or sample-game tests. |
| Platform abstraction | Partial | SDL and filesystem give a desktop portability base; shaders have Vulkan/Metal build paths. | No verified platform CI, platform service boundary, mobile/VR input/window lifecycle, console strategy, or capability discovery. Desktop support claims exceed evidence. |
| Concurrency | Absent | The engine is deliberately single-threaded today. | No job model, thread ownership declarations, immutable snapshots, command buffers, async I/O, or safe VM/subsystem handoff. |

## What a developer can demonstrate now

A controlled demo can load a trusted local source tree, create Instances, run
small Luau scripts, update primitive properties, move a free camera, simulate and
render a few primitive Parts, and receive basic touch events. Headless mode can
exercise non-rendering paths. This is enough for engine experiments and datatype
work, but not for a dependable game loop or content-production workflow.

## Key implementation observations

### Object model and reflection

Parents hold children through `shared_ptr`, while children hold a raw parent
pointer. There is no cycle rejection in `SetParent`; synchronous ancestry events
can observe or mutate intermediate state. `Destroy()` raises `Destroying` before
the object is irreversibly marked destroyed, so re-entry is possible. The
generated property metadata contains security concepts, but native dispatch does
not enforce those levels. Reflection therefore describes more policy than the
runtime supplies.

The public Instance hierarchy remains a productive API idea. The implementation
needs handles, stable IDs, transactional mutations, and explicit schema flags
before it can safely drive networking, editor history, or parallel subsystems.

### Scripting, signals, and tasks

The main Luau state opens standard libraries and sources receive sandboxed
threads, but custom globals, native services, and engine bindings remain shared.
Security cannot be modeled as “the thread was sandboxed”; authority must be
assigned per execution domain and checked at every native boundary. Native
dispatch currently has paths that accept the wrong receiver or argument type,
allow C++ exceptions to cross a Luau C callback, or retain borrowed string
storage beyond its lifetime.

Signals resume callbacks synchronously. That makes ordinary property and
hierarchy operations reentrant. Signal connections and waiters also lack strong
ownership and quota semantics. Tasks have no per-script budget, cancellation
token, or admission control. A production scheduler needs bounded phase queues,
script attribution, deadlines, and deterministic shutdown.

### Renderer, world, and physics

The renderer walks the world and submits primitive meshes through a small pass
set. A fixed directional light and 2048 shadow map demonstrate the shader path;
they are not a lighting system. Ball and Cylinder currently reuse block visuals,
and CornerWedge is also a placeholder, even though physics has more specific
shape creation.

Physics runs from a fixed-step accumulator and sends contact events into
Instances. This is a useful base, but edits to dimensions, transform, anchoring,
and related properties are not backed by a complete body-update protocol. The
renderer and physics both reach directly into mutable Instance state, making
future threading or replication unsafe without an extraction/command boundary.

### Projects and serialization

Project loading is filesystem-oriented: `FileLink` performs one initial
compatibility import through the canonical, bounded `SourceMount` owned by the
project filesystem. Absolute paths, root traversal, symlinks, and Windows
reparse points are rejected. Candidate scripts, directories, and nested Instance
JSON remain detached until the complete candidate validates, so failure retains
last-known-good linked content. Client and server filename suffixes still create
Script objects without a complete multiplayer execution-domain separation.

The JSON format is useful for bootstrapping, but represents a recursive tree
rather than a graph with durable identity. It cannot safely express shared
references needed by constraints, editor selection/history, or replication.
Malformed or oversized documents are insufficiently bounded. Binary
serialization is named but not implemented.

### GUI and Studio

GUI code is a type scaffold. Absolute-bound calculation is unfinished, GUI pass
registration is disabled, and its draw invocation is commented. The Studio demo
therefore tests Luau-side declarative construction, not an editor or even visible
runtime UI. Building panels on that scaffold now would create disposable work;
the future GUI contract and editor prerequisites are defined in the future docs.

## Source map, dependencies, and usefulness gates

| Subsystem | Important source/symbols | Direct dependencies | Gate for real-game use |
|---|---|---|---|
| Application/CLI | `src/Main.cpp`: `main`, target constructors; `src/Engine.cpp`: `Engine::Engine`, `Engine::Step` | argparse, SDL, loaders, all runtime services | Apply options, formalize lifecycle/fault handling, and test every target/headless path. |
| DataModel/services | `src/classes/DataModel.cpp`, `src/classes/ServiceProvider.cpp`, `src/services/*` | Instance registry/generated reflection | Stable service discovery, execution-domain access policy, explicit startup/shutdown. |
| Instance/lifetime | `src/classes/Instance.cpp`, `include/gargantuan/classes/Instance.hpp`, `InstanceClassRegistry` | generated property/class definitions, Signal | Safe handles, acyclic transactions, idempotent lifecycle, durable IDs, invariant tests. |
| Luau runtime | `src/scripting/ScriptEngine.cpp`, `LibBase.cpp`, `LibInstance.cpp`, `StackGuard.cpp`, `Userdata*` headers | Luau VM/compiler, reflection, Instances/services | Root/domain sandboxing, checked bindings, budgets, source-mapped diagnostics, clean teardown. |
| Tasks/signals | `src/scripting/ThreadEngine.cpp`, `src/scripting/LibTask.cpp`, `src/datatypes/Signal.cpp`, `LibSignal.cpp` | Luau coroutine API, frame loop, Instance events | Bounded phase queues, cancellation/ownership, safe reentrancy, connection cleanup and profiling. |
| Modules/require | `src/scripting/LibRequire.cpp`, `ScriptEngine` require callbacks, `src/classes/ModuleScript.cpp` | Luau require library, Instance paths, filesystem/source identity | Complete resolver, checked targets, per-domain graph/cache, cycles/errors, trust policy. |
| Serialization | `src/assets/InstanceSerialization.cpp`, `include/gargantuan/assets/InstanceSerialization.hpp` | nlohmann/json, reflection, datatype codecs | Stable IDs/references, strict limits/schema/migrations, atomic round trip and fuzzing. |
| Project/filesystem | `src/filesystem/Project.cpp`, `DiskFilesystem.cpp`, `BaseFilesystem.cpp`, `SourceMount.cpp`, `src/classes/FileLink.cpp` | `std::filesystem`, JSON/TOML/Glaze surfaces, Script creation | Project trust, watcher/conflicts, deterministic mappings, and broader persistence diagnostics. |
| Renderer | `src/render/Renderer.cpp`, `RenderPass.cpp`, `GpuMesh.cpp`, `PipelineBuilder.cpp`, `Shader.cpp` | SDL3 GPU, GLM, compiled shaders | Resource handles/lifetimes, extract/submit boundary, culling/batching, device recovery and render tests. |
| Camera | `src/classes/Camera.cpp`, `assets/classes/Camera.luau`, `Engine::Step` | input events, GLM transforms/projection, renderer | Player camera modes/subject, resize/aspect correctness, constraints and scriptable controls. |
| Lighting/shadows | `src/render/passes/ShadowPass.cpp`, `OpaquePass.cpp`, shadow/opaque shaders | renderer, fixed directional light, camera/world geometry | Lighting schema/service, configurable lights/shadows, materials, culling and quality budgets. |
| Geometry/materials | `src/render/Mesh.cpp`, `MeshProvider.cpp`, `PrimitiveMeshes.cpp`, `src/classes/Part.cpp` | SDL GPU, Part properties | Correct primitive meshes, imported meshes/textures, material/alpha model, cache and asset IDs. |
| Physics/collisions | `src/classes/WorldRoot.cpp`, `BasePart.cpp`, `Part.cpp`, `Constraint.cpp`, `WeldConstraint.cpp`, `physics/Conversions.hpp` | Box3D, Instance properties/signals, fixed frame phase | Complete property/body synchronization, validation, queries/groups, buffered events and character controller. |
| Input | `src/services/UserInputService.cpp`, `src/classes/InputObject.cpp`, Camera SDL handling | SDL events, Signals | Correct device state plus actions, routing/focus, keyboard/mouse/gamepad/touch/text and remapping. |
| GUI | `GuiObject.cpp`, GUI class headers/metadata, `src/render/passes/GuiPass.cpp` | Instance hierarchy, UDim/UDim2, renderer, input, SDL_ttf/image | Deterministic layout, display list/pass, text/images/clipping/layers, focus/routing and accessibility. |
| Studio | External private application through public `EditorHost` | versioned local IPC, snapshot/journal wire contracts | Viewport transport, picking, camera commands, and explicit editor services without private engine headers. |
| Logging/profiling | `src/Log.cpp`, `include/gargantuan/Log.hpp`, `Profiler.hpp` | SDL logging, Tracy | Applied levels, structured codes/context, source stacks, safe fields, counters and editor/CI capture. |
| Tests | `src/datatypes/*.spec.luau`, CMake test target | Lest/Luau and engine executable build | Native/integration/fuzz/sanitizer suites and sample-game/package smoke tests. |

## Build and dependency structure

`CMakeLists.txt` requires C++23 and `glslc`, generates shaders and generated class
bindings, and currently produces one `gargantuan` executable. On Apple it also
looks for `spirv-cross`/Metal tooling. The repository vendors dependencies as Git
submodules:

| Dependency | Present use |
|---|---|
| Luau | VM, compiler, standard libraries, require/task integration, class-generation tool scripts |
| SDL3 | platform events/window, logging, GPU API and executable runtime dependency |
| SDL_image / SDL_ttf | linked, but no complete public runtime image/text asset/UI path |
| Box3D | rigid-body world, primitive shapes, contacts, constraints |
| GLM | transforms, vectors, camera/render math |
| nlohmann/json | Instance/project JSON work |
| Glaze | configuration/serialization support surfaces |
| argparse | command-line parsing |
| Tracy | optional profiling instrumentation |
| magic_enum | enum reflection/conversion support |

The build adds most vendor trees directly with `add_subdirectory`; dependency
security, warnings, build time, and feature options therefore affect the single
top-level target. Generated headers under `include/gargantuan/**/generated` are
build products rather than committed inputs. A non-recursive checkout is not
buildable. Split libraries/roles, verify exact submodule revisions in CI, and
make the code-generation dependency graph explicit before packaging releases.

## Configuration and public surface details

- The CLI accepts exactly one of `--project`, `--script`, or `--instance`, plus
  headless/log/compatibility-looking flags in `src/Main.cpp`.
- `--enable_roblox_compat` is parsed but no behavior consumes it. Documentation
  also uses a different flag spelling. Log-level flags likewise do not control
  the priorities hard-coded during logger setup.
- The Studio project includes project configuration data, but an effective
  runtime configuration loader/precedence path was not found.
- `.client.luau` and `.server.luau` suffixes are recognized by `FileLink`, but the
  working execution-domain separation their names imply is absent.
- Platform abstraction is primarily SDL plus `std::filesystem`; no explicit
  mobile/VR/platform-capability layer or platform CI exists.
- The frame loop is single-threaded. `thread_local` script bookkeeping must not be
  mistaken for a worker/job architecture.

## Immediate stop-ship defects

Before distributing builds that open third-party projects, at minimum:

1. Preserve SourceMount confinement regression coverage and disable automatic
   project script execution until an explicit trust decision is given.
2. Remove unsafe Instance/ModuleScript/DataModel static casts and borrowed string
   lifetimes at the native boundary.
3. Replace raw parent lifetime assumptions and reject hierarchy cycles.
4. Enforce execution-domain capabilities; experience scripts must never receive
   host process termination or arbitrary host filesystem access.
5. Add document, hierarchy, task, signal, and per-frame resource limits.
6. Establish reproducible builds plus CI with unit, integration, sanitizer, and
   malformed-input tests.
