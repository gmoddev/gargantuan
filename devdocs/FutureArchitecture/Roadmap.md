# Delivery roadmap

This roadmap optimizes for a dependable creator loop, not checkbox parity. Phases
have exit tests; dates should be assigned only after Phase 0 measures build and
defect throughput.

## Evidence updates

- Animation Foundation 3A establishes a canonical non-Humanoid `Character`,
  engine-shipped replaceable Luau assembly/locomotion, renderer-neutral root
  extraction, and collision-authoritative main-thread admission. Headless,
  offscreen semantic scheduling, loop accumulation, stale jobs, package
  closure, and 1/10/100/500 plus mixed-policy benchmarks are covered. Motion
  warping, IK/retargeting, animation graphs, and portal transfer remain future
  work.
- Character / Animation Foundation 3B completes the first dedicated realtime
  Character vertical: connection/control-epoch binding, semantic input and
  action sequences, server-derived movement and pinned root action content,
  reliable action decisions, sequenced authoritative state/acknowledgement,
  64-entry prediction/replay, collision correction, NPC/remote state, custom
  Luau policy, simulated adverse transport, localhost GNS, FirstCompleteGame
  catalog identity, malformed protocol, lifecycle, Release performance/
  bandwidth/allocation, and sanitizer gates. Character state remains separate
  from structural replication and Remote Instances.
- Character Networking Foundation 3C replaces per-Character 60 Hz state
  messages with deterministic bounded GCHR v2 absolute-state frames, a measured
  20 Hz default, compact rotation/controller encoding, unchanged suppression
  with periodic recovery, four-snapshot remote interpolation, and renderer-only
  local correction smoothing. At 500 continuously moving Characters it reduces
  the state stream from 3.36 MB/s and 30,000 state messages/s to 759,040 B/s and
  680 state frames/s. Spatial relevance, adaptive cadence, and deeper Animator
  content-readiness remain future work.
  The non-normative `SpatialRegionsAndPortalTopology.md` is unchanged.
- Character Networking Foundation 3D makes 3A-3C reachable from packaged
  `GargantuanPlayer` client/server entrypoints. One `GameSession` composes GNS,
  scheduling, structural replication, Remotes, GCHR, and Player association on
  one `ConnectionId` lifetime. GSES admits a bounded development-local session,
  creates the server Player, explicitly identifies the trusted LocalPlayer,
  and gates gameplay until client runtime readiness. Engine-shipped Luau owns
  default server assembly and shared movement policy; game Luau can replace
  movement/action policy through `CharacterControlService`. This does not claim
  external account authentication or Node integration.
- Environment / Lighting Foundation 1 establishes canonical saved Lighting and
  Sky semantics, renderer-neutral incremental publication, AssetService-owned
  coherent face residency, and the SDL shadow/Sky/opaque/GUI pipeline. Local
  lights, atmosphere, probes, HDR color management, and advanced shadows remain
  Phase 2 renderer expansion rather than implied completion.
- Standalone Packaging Foundation 1 closes the Windows x64 build-tree/CMake
  usability gap for produced games: Studio and CLI share one PackageBuilder,
  FirstCompleteGame packages into a self-contained hashed directory, and the
  relocated player passes headless and real-renderer startup from an unrelated
  working directory. This completes the package/smoke deliverable within Phase 2
  and the package portion of the Phase 4 exit; it does not claim either broader
  phase complete.
- Phase 5 remains the home for Linux/macOS distribution claims, signing,
  provenance/SBOM, installed-tool distribution, and public creator packaging.

## Priority conventions

- **P0:** blocks safe continued development or invalidates higher layers.
- **P1:** required for the minimum usable game or first multiplayer slice.
- **P2:** required before public creator/server use.
- **P3:** expansion after platform contracts are stable.

## Phase 0 — Baseline and containment (P0)

Prerequisite: none beyond an identified supported Windows toolchain and recursive
submodule checkout. Decision: treat current APIs/formats as pre-alpha and allow
breaking fixes. Tests: clean-machine build, CLI smoke, malformed input, path/link
escape, binding misuse, sanitizer regression. Likely blockers: unpinned vendor
behavior, generated-source coupling, GPU-only initialization, and defects exposed
when the disabled Vector2 tests are restored.

Deliver:

- reproducible recursive checkout/build documentation;
- CI Debug/Release build and headless test target, starting with Windows;
- engine libraries separated enough to test without application startup;
- ASan/UBSan where supported, static analysis, formatter, and crash-on-error tests;
- source-backed capability matrix and honest README status;
- fixes/tests for SEC-001 through SEC-014, hierarchy cycles/reentrancy, mouse
  state, malformed serializers, and shader/native failure handling; and
- restricted project-open mode plus canonical source-root confinement.

Exit: clean CI; all high/medium findings closed or explicitly release-blocked;
malformed/traversal corpus cannot crash, escape roots, or execute before trust.

## Phase 1 — Runtime contracts (P0/P1)

Prerequisite: Phase 0 CI and safety baseline. Decisions: Instance remains the
public model; specialized stores stay internal; IDs are never raw addresses;
mutations publish only after commit; compatibility is an adapter. Tests: property-
based hierarchy/lifecycle, schema conformance, scene round trip/migration, module
graphs, scheduler quotas and domain-denial tests. Likely blockers: generated
reflection currently mixes metadata with executable Luau, public behavior is
underspecified, and changing ownership touches every subsystem.

Deliver:

- `ObjectId`, generation-checked handles, object registry, and transactional
  hierarchy/lifecycle;
- unified schema registry for persistence, replication, access, editability,
  validation, and migration;
- ordered change journal and safe-point event publication;
- server/client/plugin/editor execution domains and capability checks;
- root-sandboxed Luau, checked native bindings, reliable ModuleScript resolution,
  bounded scheduler/signals, cancellation and diagnostics;
- versioned scene/project schema, stable references, limits, atomic save,
  migrations, and `SourceMount`; and
- documented frame phases plus physics/render command/extraction boundaries.

Exit: property-based hierarchy/round-trip tests pass; a saved scene preserves
identity/references; wrong native calls never crash; abusive tasks/signals stay
within configured frame/memory limits.

## Phase 2 — Minimum player runtime (P1)

Prerequisite: stable schemas/IDs, safe scripting, save format and extraction
boundaries from Phase 1. Decisions: one narrow desktop feature slice, a kinematic
character before general network physics, content hashes plus project aliases,
and one retained UI pipeline. Tests: visual/layout goldens, asset corruption,
physics/query correctness, input/focus devices, audio/device loss, long sample
soak and packaged-build smoke. Likely blockers: SDL GPU backend variability,
text shaping scope, physics property semantics, and temptation to expand asset/UI
breadth before the sample closes end-to-end.

Deliver:

- action mapping, corrected input state, gamepad, player camera/controller;
- physics property synchronization, raycast/overlap, collision groups, validated
  constraints and buffered contacts;
- render extraction, correct primitive meshes, culling/batching baseline,
  materials, lights, textures/meshes, and device failure diagnostics;
- content-addressed asset importer/resolver/cache;
- basic audio and transform/tween animation;
- functional Screen UI: layout, text, image, button, clipping, focus, navigation;
- structured `[System:SubSystem]` diagnostics and performance counters; and
- the collect-and-exit sample plus package/smoke-test command.

Exit: every acceptance test in `MinimumUsableGame.md` passes on a clean machine.

## Phase 3 — Authoritative multiplayer slice (P1/P2)

Prerequisite: the Phase 2 game expressed through server/client-ready domains and
schema journal. Decisions: server truth, asynchronous typed messages, local play
uses the real protocol, network ownership is only a lease, transport is replaceable
and encrypted. Tests: protocol fuzzing, golden vectors/version negotiation,
latency/loss/reorder, malicious clients, bandwidth budgets, multi-process soak and
reconnect/failure. Likely blockers: nondeterministic physics, schema churn, missing
operational auth/discovery owner, and overgeneralizing replication before the
character slice is measured.

Deliver:

- separate `GameServer` and `GameClient` roles plus loopback orchestration;
- strict protocol handshake, stable schema hash, bounded serialization, encrypted
  remote transport adapter;
- server-to-client create/update/destroy replication with baselines and recovery;
- Players/session lifecycle and semantic input-command channel;
- one predicted/reconciled character controller and remote interpolation;
- typed `NetworkEvent` and `NetworkRequest` APIs with deadlines/rate limits;
- spatial interest grid, per-client budgets, traffic profiler, and fault injection;
- malicious-client corpus and multi-process latency/loss/soak tests.

Exit: two clients complete the sample through one authoritative server under
simulated adverse network conditions; forged state and resource abuse are
rejected without crashes or unauthorized changes.

## Phase 4 — Studio foundation (P1/P2)

Prerequisite: stable engine/editor API, transactions/journal, save/assets, working
GUI and separate play roles. Decisions: command-owned documents, Studio is an API
client, edit and play worlds are isolated, plugins are brokered rather than native
in-process. Tests: undo/redo properties, recovery/conflicts, selection across
mutations, viewport/picking, script diagnostics, restricted projects/plugins, and
multi-client play cleanup. Likely blockers: trying to reproduce visual polish
before document semantics, coupling panels to native pointers, and editor GUI
performance on large hierarchies.

Deliver:

- EditorHost project/trust launcher and structured diagnostics;
- viewport/hierarchy/properties using versioned engine APIs;
- selection, picking, transform gizmos, command bus, undo/redo, dirty state,
  atomic save and recovery;
- Luau editor/language services and runtime source-map integration;
- asset browser/import queue and GUI inspector/device emulation;
- isolated local server/multi-client play orchestration; and
- capability broker architecture (third-party plugin distribution remains off).

Exit: a creator can construct, script, save, reopen, play-test, diagnose, and
package the minimum game entirely through Studio-supported workflows.

## Phase 5 — Creator alpha (P2)

Prerequisite: external users can finish the Phase 4 workflow and the multiplayer
slice has operational telemetry. Decisions: publish only measured platform/API
support; self-hosting uses replaceable providers; compatibility is versioned;
plugins require declarations/signatures. Tests: clean packages on every claimed
OS, upgrade/rollback/migration, server load/chaos, secret redaction, plugin escape,
and tutorial conformance. Likely blockers: deployment/support ownership, platform
driver differences, dependency licensing/provenance, and compatibility pressure
that conflicts with the coherent core.

Deliver:

- Linux/macOS verified packages and platform capability matrix;
- rigged character animation/tooling, improved lighting/materials/particles;
- scalable scene streaming and replication profiling;
- deployment configuration, self-hosted discovery/auth reference, server storage
  connector, secret management, admin/operations console;
- versioned compatibility adapter and migration report for selected Roblox APIs;
- sandboxed signed plugin pilot; documentation/tutorial/sample suite; and
- security policy, signed releases, SBOM/provenance, update/rollback plan.

Exit: invited external creators complete projects without routine engine-team
intervention; server operations and security response have owners and runbooks.

## Phase 6 — Platform expansion (P3)

Prerequisite: creator-alpha stability and demand/performance evidence. Decisions
and tests are feature-specific but must preserve capability boundaries, schemas,
budgets, migrations, protocol compatibility, and platform conformance. Likely
blockers are large-world scale, mobile/VR memory and input constraints, moderation/
privacy obligations, and service operating cost.

Prioritize from measured creator demand: terrain/large worlds, collaboration,
mobile, VR, deeper rendering, economy/marketplace integrations, voice/social, and
cloud services. Each new capability requires schemas, budgets, threat modeling,
cross-platform tests, and a deprecation/versioning story.

Non-Euclidean world topology is a possible Phase 6 expansion after semantic
spatial addressing, region streaming, renderer multi-view costs, and
authoritative multiplayer interest are measured. If creator demand justifies
it, follow the staged, bounded design in
[Spatial regions and portal topology](SpatialRegionsAndPortalTopology.md) rather
than introducing renderer-only portals or a second source of spatial authority.

## Cross-phase workstreams

| Workstream | Continuous requirement |
|---|---|
| Correctness | Regression test per fixed defect; property/fuzz tests for state machines and parsers. |
| Security | Threat model updated with each trust boundary; capability/abuse tests; dependency review. |
| Performance | Frame, memory, object, task, signal, GPU, physics, asset, and bandwidth budgets in CI/soak dashboards. |
| Compatibility | Small explicit target matrix, conformance tests, migration tooling; no silent behavior branches. |
| Documentation | API status generated from tested schemas; tutorials run in CI; claims tied to release gates. |
| Observability | Structured categories (`[Network:Replication]`, `[Script:Scheduler]`), trace IDs/ticks/object IDs, privacy controls. |

## First 12 concrete engineering tickets

1. Add CI bootstrap/build/headless test and dependency checkout verification.
2. Fix project/`FileLink` canonical-root confinement and add trust mode.
3. Replace unsafe loader/module casts and borrowed deserializer views.
4. Replace raw parent lifetime, reject cycles, and make destruction monotonic.
5. Centralize checked native Luau receiver/argument/error handling.
6. Introduce execution domains/capabilities and remove `ProcessService` from games.
7. Add parser/scene/scheduler/signal budgets plus fuzz/sanitizer targets.
8. Specify and prototype stable IDs, schema flags, transactions, and journal.
9. Implement bounded, source-located scene serialization and atomic round trip.
10. Finish ModuleScript identity/resolution and a bounded cancellable scheduler.
11. Fix input state and implement the first ActionMap plus player camera.
12. Implement render/physics synchronization boundary and the first visible,
    interactive Screen UI acceptance test.

Do not parallelize higher-level feature volume ahead of tickets 1–10. They are the
load-bearing contracts for networking, Studio, and long-lived creator content.
