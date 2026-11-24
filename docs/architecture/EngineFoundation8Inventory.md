---
status: proposal
owner: engine
baseline: df3c8bb052439e768db9b301fc85d64049b159d1
---

# Engine Foundation 8 capability inventory

## Qualified starting point

Engine main `df3c8bb052439e768db9b301fc85d64049b159d1` has the same tree
`be355e6d1863708bfa4550e3e6262964d2812b15` as the reviewed GAS and Gameplay
integration. Windows Release passed 183 tests and five Release workloads,
Linux headless ASan and UBSan passed 128 tests, GNS ASan, UBSan, and leak
detection passed 13 tests and the production admission checks. Relay Rescue
ran from GASP on simulated transport on both platforms and real GNS on
Windows. The integration also retains scoped offscreen Vulkan and instrumented
packaged host receipts. The physical UDP preflight timed out.
[Promotion and exact-source evidence](https://github.com/gmoddev/gargantuan/pull/28)

The independently closed source lines are GAS `5daa31bb7` and runtime maturity
`2c7cab0d2`. The existing Studio workflow review is pinned to Studio
`9e0589144720cce2a6951a32e585fd4a53c6582c`. It already supports Character
composition, input actions, and Play. The native GAS transfer and document
scope changes are in the Engine-to-Studio handoff.
[Integration evidence and Studio handoff](../../devdocs/Validation/GasGameplayIntegration.md)

## Public capability at the baseline

The inventory combines generated declarations with native bindings, subsystem
implementations, tests, and playable samples. Reachability, default settings
and qualification are recorded separately where they differ.

| Family | Capability | Observed status | Evidence and developer consequence |
| --- | --- | --- | --- |
| World | Mesh scenery, static mesh collision, dynamics and constraints | Complete and usable | MeshPart exposes StaticMesh for anchored asset-backed triangle collision. PhysicsBackend documents Box3D confinement, filtering, collision queries, contact events and lifecycle. PhysicsImportedMeshTests covers the production path. |
| World | Shape inspection and movement | Complete and usable | WorldRoot exposes raycast, sphere/capsule/box overlap and sweep, and bounded kinematic capsule movement. A game can author sensors and clearance checks through public APIs and rigid simulation. |
| World | Rigid surface response authoring | Functional but incomplete | The neutral physics body descriptor and Box3D default response exist. BasePart exposes no general friction/restitution authoring or separate rigid material asset. Deformable parameters belong to a separate provider. |
| World | Terrain, vegetation and water | Missing | Terrain is mentioned in package partition policy and future architecture. The public class schema has no terrain service, editable heightmap/voxel model, foliage, water surface or water simulation. A developer can use meshes for static scenery now. |
| World | Lighting, Sky and fog | Complete and usable in the current scope | Lighting, local lights, Sky and bounded environment controls already support composed scenes. Layered shadows, probes, HDR residency and native instancing keep their individually qualified or opt-in scopes. |
| World | Large-world game content | Functional but incomplete | Server relevance, structural replication, package content residency and lifecycle are delivered. Ordinary Luau does not choose providers or pin arbitrary package units. Large-world editor tooling and terrain paging remain future work. |
| Visual effects | Immutable scene publication, transparent meshes, visual-only attachments | Complete and usable | RenderPublisher resolves immutable values and assets for renderer work. AttachmentMesh supplies a replaceable visual-only binding precedent. |
| Visual effects | Particles, beams, trails, decals, billboards and local effect controls | Missing | The generated catalog has no corresponding authorable effect types. Renderer and asset seams can support a later narrow effect class after draw, culling and asset-lifetime contracts are defined. |
| Visual effects | Post-processing | Functional but incomplete | Existing environment and tone mapping controls cover the current renderer. A general authorable post-process stack, volumetric environment and local effects authoring are absent. |
| Audio | Spatial playback and asset ownership | Complete and usable in the current scope | Sound plays canonical 16-bit mono/stereo PCM WAV assets up to 30 seconds, supports seek/loop/state, uses the active camera listener, and resolves BasePart/Attachment parents including animated anchors. |
| Audio | Device and cross-role lifecycle | Complete and usable | The bounded 48 kHz software mixer supports up to 256 active voices, four blocks per step and provider failure without gameplay termination. Headless/server roles open no audio device. AudioFoundationTests and FirstCompleteGame prove the existing path. |
| Audio | Local player mix controls and authored categories | Missing | Per-Sound volume exists. There is no master or category API and no independent preferences at the world mixer. Authored category fields and local gains can preserve canonical PCM and package compatibility. |
| Audio | Streaming and environmental audio | Missing | Long-form compression/decode, custom listeners, acoustic occlusion, reverb and live device features are separate from resident PCM and bounded stereo panning. |
| Gameplay | Players, Character and possession | Complete and usable | Public player lifecycle, Character assembly, ActionMap, CharacterControlService, overlap queries, real-GNS GameSession and Relay Rescue demonstrate authority with replaceable project policy. |
| Gameplay | Interactions | Exposed but poorly integrated for multiplayer | ProximityPrompt authoring, targeting, hold, visibility, local activation and HUD are implemented. Dedicated Server has no LocalPlayer and no incoming prompt activation route, so stock Triggered is local. |
| Gameplay | Teams, spawn and gameplay state composition | Intentionally project-owned in this slice | Player ID, attributes, CharacterSpawnRequested, CharacterAdded, RunService, Remotes and possession support project-owned policy. Relay already implements teams and spawn/respawn. There are no built-in Teams or SpawnLocation classes. |
| Gameplay | Scheduled Luau work | Functional but incomplete | `task.spawn`, `task.defer`, `task.delay`, `task.wait`, and RunService phases are real. Scheduler keeps originating security context and bounds pending entries. Cancel, synchronize and desynchronize are currently stubs. |
| Gameplay | Asset lookup and admission | Complete and usable for packaged content | AssetService lookup, availability and metadata are public. Mesh, Audio, Animation and Image use typed references. Trusted import and content-provider keys stay outside ordinary Luau. |
| Runtime UI | Retained screen UI | Complete and usable in the current scope | ScreenGui, scale and offsets, safe areas, clip, layouts, scroll containers, UTF-8 text boxes, keyboard focus, TextButton and semantic accessibility are delivered. GuiFoundation2 and GuiFoundationTests supersede stale introductory prose. |
| Runtime UI | Responsive interactive images | Functional but incomplete | ImageLabel uses strict Image assets and stretch. Button activation is bound to TextButton. Developers must currently assemble image controls from multiple objects and manually align hit/focus geometry. |
| Runtime UI | World-space UI and platform accessibility | Missing | Engine GUI has no Billboard/Surface root and no general OS accessibility bridge. The Studio LUI workspace uses a separate runtime. |
| Developer experience | Public discovery and diagnostics | Complete and usable in the current scope | Generated catalog covers class and service declarations, native runtime globals are separately documented, and EditorHost supplies schema, snapshot, Play diagnostics, input inspection and streaming status. Opt-in work trace exports bounded phase data. |
| Developer experience | Breakpoints, runtime inspection and Studio multi-client | Missing or architecture-gated | General game debugger, runtime mutation inspector and Studio multi-client TestSession require separate accepted contracts. Standalone Player and Server are available for multiplayer. |
| Developer experience | GAS authoring in Studio | Implemented in Engine, poorly integrated in Studio | Engine exposes project/model methods, exact catalog pins, transactional import and scope replacement. Studio needs native transfer, capability gating and fresh snapshot reconciliation. |

## Foundation 8 scope

| Rank | Workstream | Why it fits this campaign | Qualification |
| --- | --- | --- | --- |
| 1 | Interactive image HUD | High everyday authoring value. Reuse the existing asset, hit, layout and focus paths and preserve Stretch as the ImageLabel default. | Pointer and keyboard activation, accessibility and disabled state, clipping and resizing, legacy and new saved projects, GASM pins and replay. |
| 2 | Client audio mixing | High developer value with a fixed, small mixer model. Keep authored categories separate from each client's output controls. | Unity compatibility, mute and independent sessions, resume and completion, retained asset pins, headless safety, a matched mixer profile. |
| 3 | Rigid contact surfaces | High level-authoring value without a new physics owner. Project two authored values into the existing body and shape safe point. | Real friction and rebound, unchanged defaults, live edits and mesh lifetime, invalid bounds, JSON and GAS. |
| 4 | Scheduler cancellation | Small API gap encountered by scripts with overlapping timers and UI feedback. Keep existing coroutine ownership and scheduling rules. | Delayed and deferred callbacks, cancellation during dispatch, early release, precise false outcomes, scripting context and shutdown. |

Run the extended [Relay Rescue](../../samples/GameplayFoundation7/README.md)
with two clients. It combines existing stock and custom control with local
image UI and audio, rapid feedback timer cancellation, and server-owned cargo
released onto an authored contact surface. Run gameplay from GASP, compare
both package roles, and retain graphical and diagnostic evidence at their
actual execution sources.

## Follow-up opportunities

- The highest remaining gameplay gap is server-authoritative ProximityPrompt.
  Keep the current local targeting and presentation. Specify server identity,
  range, visibility, hold lifecycle and transport compatibility before adding
  network ingress. Reusable game remotes and spatial queries are already public.
- The first visual effect candidate is an asset-backed face decal for box
  Parts, using immutable publication and existing depth-tested alpha
  compositing. It needs clipping, reimport, lifetime, streaming and GPU
  qualification. Terrain, foliage, water and general particles need separate
  authoring and performance contracts.
- Keep Audio streaming, editable bus graphs, scene acoustics and world-space
  UI as distinct feature work. Studio GAS authoring and a native multi-client
  workflow remain paired Studio work.
- The measured crossing spent about 30 ms of 41 ms in client snapshot
  preflight. The smaller structural changes showed no repeatable whole-frame
  improvement. Evaluate direct typed materialization and structural scratch
  allocations with matched tests, while preserving the distinct GAS and
  replication transactions and the normal uninstrumented Release baseline.

## Source references

- [Generated public declaration catalog](../src/data/PublicApi.json)
- [Rigid backend and static mesh collision](../../devdocs/CurrentArchitecture/PhysicsBackend.md)
- [Package content and streaming ownership](../../devdocs/CurrentArchitecture/ContentAvailabilityFoundation3L.md)
- [Audio ownership and bounded mixer](../../devdocs/CurrentArchitecture/AudioFoundation1.md)
- [Interaction and multiplayer authority boundary](../../devdocs/CurrentArchitecture/InteractionFoundation1.md)
- [Retained GUI and semantic focus](../../devdocs/CurrentArchitecture/GuiFoundation2.md)
- [EditorHost and Studio boundary](../src/content/docs/developing/editor-host.mdx)
