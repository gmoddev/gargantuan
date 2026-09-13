---
status: planned
authority: non-normative
---

# Delivery roadmap

This roadmap optimizes for a dependable creator loop, not checkbox parity. Phases
have exit tests; dates should be assigned only after Phase 0 measures build and
defect throughput.

## Review status — 2026-09-12

Reviewed across main, Node, MCP, Telemetry, and Studio; see the
[revision/evidence ledger](RoadmapEvidence20260912.md) and the
[feature checklist](../../docs/src/content/docs/developing/roadmap.mdx).
Checked deliverables represent the stated implemented slice. Unchecked
**Partial** entries cross off only delivered work. No phase exit is certified by
this documentation review, and the runtime suites were not rerun.

| Phase | Status | Remaining exit scope |
| --- | --- | --- |
| 0 | Partial; build, security and test foundations exist | Full finding closure/release blocks, complete configuration/tooling matrix and clean-machine safety qualification. |
| 1 | Partial; identity/schema/journal/capabilities/persistence are implemented | Broader migrations, API conformance and end-to-end quota/lifecycle qualification. |
| 2 | Partial; input, audio/animation, retained GUI, assets, Lighting/Sky and Windows packaging exist | Mesh collision, rendering breadth/performance and every minimum-game acceptance gate. |
| 3 | Partial; packaged authoritative networking through 3K and the 3L lifecycle exist | 3L correctness/service/resource qualification and authenticated admission before public hosting. |
| 4 | Partial; native Studio shell, docking, source editing/history, local Play and MCP exist | Two-way source sync, advanced authoring/language tooling, multi-client orchestration and plugin trust. |
| 5 | Partial; Node and telemetry foundations exist | Durable backend storage, creator distribution, public operations, signing/provenance and external-creator qualification. |
| 6 | Future, demand-led | Default-space 3M precedes semantic regions/topology; other expansion requires its own measured gates. |

### Future architectural tracks

The full [Architectural Discipline and Subsystem Boundaries](ArchitecturalDisciplineAndSubsystemBoundaries.md)
proposal is non-normative design input. Priorities are scoped per track.

- [ ] Architecture 0A–0C: machine-readable dependency extraction, declared allowed/forbidden edges, reporting, then CI enforcement.
- [ ] **Partial — Architecture 0D/0E:** ~~separate GNS adapter and packaged host targets~~; graph-backed transitive-boundary tightening, neutral protocol/publication extraction, then progressive isolation.
- [ ] Close Foundation 3L before starting 3M. The [current validation ledger](../CurrentArchitecture/ContentAvailabilityFoundation3L_3Validation.md) records **partially ready; no 3M**, despite the closed GNS sanitizer gate.
- [ ] Foundation 3M: bounded trusted demand policy with semantic priority, deadlines, deterministic distance/cost ties, hysteresis, bounded lookahead and revision-stamped request/release deltas into 3L.
- [ ] **P0 before untrusted public hosting:** signed short-lived server/session-bound join grants and bounded admission before Player/Character, relevance/materialization, Remote, pin or gameplay-capability allocation; GSES retains compatibility validation.
- [ ] **Partial:** ~~existing planning/selection/transport budgets~~; finite per-stage service/resource envelopes through discovery, encoding, backend service, client application and cleanup/recovery, including overload and fairness.
- [ ] Bounded subsystem-owned CPU/allocation/residency attribution with static tags; allocator hooks only if needed. Detailed profiling remains local unless a separate telemetry contract permits it.
- [ ] Asset 2B residency accounting for canonical/decoded/GPU/physics/animation bytes, consumers/pins, last use and load/eviction counts before normative general eviction policy.
- [ ] Conditional immutable 3H/3K read epochs and later worker derivation only after measured crossover and Amdahl headroom; neither blocks 3L/3M.
- [ ] Studio engine-assigned provenance over existing history, scoped plugin grants and revocable project trust; staged atomic external-author transactions only for a demonstrated need.
- [ ] Audit canonical schema coverage for API/tooling metadata; extend the existing registry rather than introduce another.
- [ ] **P2 after reliable-service stabilization:** bounded semantic anti-entropy over receiver-normalized agreed state; mismatches use normal 3J recovery, never direct `Known` mutation.
- [ ] Default-space 3M validation, then semantic SpatialRegion, region-local physics/topology and portals with consumer-specific traversal budgets.
- [ ] **Partial:** ~~network fuzz harnesses~~; risk-weighted coverage-guided content, snapshot/journal, persistence, artifact/importer, and future admission/topology fuzzing with sanitizer runs and retained corpora.

3L owns content lifecycle, 3E relevance, and 3J accepted materialization. 3M
owns demand only. Generic prediction extraction waits for a second consumer.
Do not reintroduce an ECS semantic conversion, second schema/capability registry,
second history, or duplicate streaming lifecycle as a new foundation.

## Evidence updates

The following entries record cumulative milestone history. A milestone's
original deferrals may be implemented by a later entry; use the reviewed
checklists and evidence ledger above for present status.

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
- Character / Replication Foundation 3E makes one server-owned relevance result
  feed structural materialization and GCHR. It adds a bounded replaceable
  uniform grid, owner-required Character pinning, ancestor/hard-reference
  closure, soft-reference fixups, safe unpublish/reentry, and a GCHR v3
  materialization epoch without changing the 28-byte batch header. Player
  identity remains globally visible; remote Character descendants are spatial.
  Foundations 3F/3G subsequently add adaptive desired cadence and bounded actual
  publication; 3H canonicalizes derived SpatialCellAddress and region candidates.
  Public region APIs, content paging, and portal topology remain future work.
- Character / Replication Foundation 3E.1 makes GameSession acquisition and
  teardown transactional, uses generation-scoped CharacterControl callback
  leases, enforces terminal-peer reliable publication, and bounds transport
  event polling so admission bursts cannot outrun committed journal cursors.
  GCHR v4 widens materialization epochs to 64 bits, returns one reliable result
  per action request, and applies the final root-motion interval.
  DevelopmentLocal networking is loopback-only unless the native host
  explicitly opts into the unauthenticated development override. Adaptive
  cadence remains 3F work.
- Character Networking Foundations 3F and 3G add peer-specific 20/10/5 Hz
  desired cadence and bounded age-aware actual publication without changing
  simulation or reliable semantics. Character / Replication Foundation 3H then
  promotes 3E's private grid into canonical derived `SpatialCellAddress` semantics,
  a bounded sparse multi-region index, large-object fallback, dirty-driven
  membership, and region-assisted candidate discovery. 3E still owns relevance
  and structural dependency closure; no region protocol, persistence field,
  public Luau API, content paging, or portal topology was added.
- Replication Foundation 3I replaces repeated peer-owned structural `Publish`
  deep copies with immutable revisioned per-object descriptions and bounded
  peer-specific reference patches. GRPL v1, 3E relevance, LocalPlayer,
  materialization epochs, GCHR, and reliable scheduler commit remain unchanged.
  Replication Foundation 3J now schedules the remaining peer-specific work with
  deterministic per-peer/global limits, dependency-safe groups, critical
  bootstrap priority, fair peer rotation, compact cancellation, and exact
  scheduler-accepted peer materialization. It adds no generic QoS system or
  public spatial/materialization API.
- Spatial Runtime Projection Foundation 3K separates authoritative semantic
  pose from derived acceleration-cell addressing, adds generation-safe
  subsystem-owned spatial projections and explicit space identity, and proves
  isolated-space query and transfer behavior. Instance semantics, 3H candidate
  discovery, 3E relevance, and 3J materialization authority remain unchanged.
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

- [x] Reproducible recursive checkout/build documentation.
- [ ] **Partial:** ~~Windows Release and Linux sanitizer/headless CI~~; complete Debug/Release qualification matrix.
- [x] Core test targets and separate packaged hosts sufficient for headless testing without application startup.
- [ ] **Partial:** ~~ASan/UBSan and malformed-input regressions~~; qualify static analysis, formatting and complete crash-on-error coverage.
- [ ] **Partial:** ~~source-backed current architecture and README status~~; keep a complete release capability matrix verified.
- [ ] **Partial:** ~~hierarchy/input/parser/native-boundary regression work~~; requalify every historical SEC finding against current source and retain explicit release blocks.
- [x] Non-executing EditorHost project open and canonical SourceMount root confinement; persistent plugin/project grant policy remains separate.

Exit: clean CI; all high/medium findings closed or explicitly release-blocked;
malformed/traversal corpus cannot crash, escape roots, or execute before trust.

## Phase 1 — Runtime contracts (P0/P1)

Prerequisite: Phase 0 CI and safety baseline. Decisions: Instance remains the
public model; specialized stores stay internal; IDs are never raw addresses;
mutations publish only after commit; compatibility is an adapter. Tests: property-
based hierarchy/lifecycle, schema conformance, scene round trip/migration, module
graphs, scheduler quotas and domain-denial tests. Historical reflection coupling
is now addressed by frozen canonical schema. Remaining blockers include
remaining public behavior/migration gaps, and cross-subsystem ownership changes.

Deliver:

- [x] `ObjectId`, generation-checked registry and validated hierarchy/lifecycle with committed publication.
- [ ] **Partial:** ~~canonical frozen schema for persistence, replication, access, editability and validation~~; broader migration/tooling coverage.
- [x] Ordered committed change journal and buffered safe-point publication boundaries.
- [ ] **Partial:** ~~Core/PreRun/Studio/Server/Client domains and host-created capability checks~~; plugin grants/trust/revocation.
- [ ] **Partial:** ~~source-root sandboxing, checked bindings, ModuleScript resolution and scheduler foundations~~; full abuse/quota/cancellation exit qualification.
- [ ] **Partial:** ~~versioned project persistence, stable references, bounds, atomic save and SourceMount~~; broader migration contracts.
- [x] Documented frame phases and neutral physics/immutable render-publication boundaries.

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

- [ ] **Partial:** ~~ActionMap, corrected input boundary and default player camera/controller~~; qualify the complete device/focus matrix.
- [ ] **Partial:** ~~neutral physics synchronization/queries, weld constraints and buffered contacts~~; broader collision-group/constraint acceptance.
- [ ] **Partial:** ~~immutable render extraction, primitive/imported meshes, basic materials/textures and Lighting/Sky~~; culling/bucketing, local lights and broader device qualification.
- [x] Content-addressed asset importer/resolver/cache foundations.
- [ ] Asset Foundation 2B's mesh-collision slice: establish the separate
  backend-neutral collision boundary, MeshPart binding, physics-backend
  projection, bounded lifetime/residency behavior, and measured representation
  policy needed by imported and generated meshes; other 2B priorities remain
  separately scoped in current architecture;
- [ ] Geometry Foundation 1A ([research](SolidGeometryCompilerResearch.md)) after
  that Asset Foundation 2B slice: a bounded, deterministic headless
  solid-geometry compiler that transactionally emits ordinary canonical Mesh
  assets and produces collision data only through the established 2B contract;
  Manifold is the first prototype candidate, not yet a dependency decision, and
  runtime game-facing CSG remains deferred;
- [x] Basic Audio Foundation 1 and transform/tween/Character animation foundations.
- [x] Retained Screen UI foundation: layout, text, images, TextButtons, clipping, focus/navigation, TextBoxes and scrolling.
- [ ] **Partial:** ~~structured diagnostics and subsystem benchmark/counter surfaces~~; comprehensive local CPU/allocation/residency attribution.
- [x] FirstCompleteGame collect-and-exit sample and Windows standalone package/smoke command; broader clean-machine acceptance remains the phase exit.

Exit: every acceptance test in `MinimumUsableGame.md` passes on a clean machine.

## Phase 3 — Authoritative multiplayer slice (P1/P2)

Prerequisite: the Phase 2 game expressed through server/client-ready domains and
schema journal. Decisions: server truth, asynchronous typed messages, local play
uses the real protocol, network ownership is only a lease, transport is replaceable
and encrypted. Tests: protocol fuzzing, golden vectors/version negotiation,
latency/loss/reorder, malicious clients, bandwidth budgets, multi-process soak and
reconnect/failure. Remaining blockers include broader physics authority, schema
compatibility, public game-session admission and 3L qualification. Node now owns
service discovery and identity-provider foundations; those do not by themselves
authenticate a GameSession peer.

Deliver:

- [x] Separate packaged Server/Player roles and loopback GameSession orchestration.
- [x] GSES compatibility handshake, schema compatibility, bounded codecs and replaceable GNS transport; authenticated public join grants remain open.
- [x] Server-to-client structural replication with baselines, recovery and scheduler-accepted materialization.
- [x] Players/session lifecycle and semantic GCHR input/action channel.
- [x] Bounded predicted/reconciled Character controller and remote interpolation.
- [x] Bounded RemoteEvent/RemoteFunction semantics with deadline/rate policy; these are the implemented names for the earlier NetworkEvent/NetworkRequest intent.
- [ ] **Partial:** ~~3E/3H relevance index, per-peer budgets, traffic diagnostics and simulated fault injection~~; complete creator-facing traffic profiler.
- [ ] **Partial:** ~~malformed/adverse-transport and packaged multi-process tests~~; full current-source malicious-client, 3L overload/client/scale and public-hosting qualification.

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

- [ ] **Partial:** ~~authenticated EditorHost launch, non-executing open and structured diagnostics~~; persistent revocable project/plugin trust.
- [x] Studio viewport, virtualized Explorer and schema-driven Properties using versioned EditorHost APIs.
- [ ] **Partial:** ~~selection/picking, transform gizmos, shared commands/history, dirty state and atomic save~~; broader crash/conflict recovery.
- [ ] **Partial:** ~~Luau source tabs, syntax diagnostics and revision-checked commits~~; full language services, debugging and source-map integration.
- [ ] **Partial:** ~~Assets tool, filtered asset picker and import commands~~; expanded import queue UX and GUI inspector/device emulation.
- [ ] Geometry Foundation 1B solid-model authoring for union, intersection,
  difference, edit/separate, preview, collision inspection, diagnostics, and
  command-backed undo/redo through the headless compiler;
- [ ] **Partial:** ~~one isolated local Play/Stop runtime~~; Studio server/multi-client test orchestration.
- [ ] **Partial:** ~~explicit Studio services and independently authorized MCP bridge writes~~; third-party plugin broker/grants/distribution.

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

- [ ] **Partial:** ~~Linux headless/server evidence~~; verified Linux/macOS creator packages and complete platform matrix.
- [ ] **Partial:** ~~rigged animation, Character/root-motion runtime, Lighting/Sky and material assets~~; expanded animation tools, advanced rendering and particles.
- [ ] **Partial:** ~~3L lifecycle and replication/content profiling~~; 3L qualification, 3M trusted demand and creator-facing scalable streaming.
- [ ] **Partial:** ~~Node deployment configuration, discovery, workload/player authentication, entitlement/content services and memory-backed storage contracts~~; durable storage, gameplay join admission and production secret/admin operations.
- [ ] Versioned compatibility adapter and migration report for selected Roblox APIs.
- [ ] **Partial:** ~~documentation and samples~~; sandboxed signed plugin pilot and full tutorial-conformance suite.
- [ ] **Partial:** ~~documented capability/privacy boundaries and optional telemetry integration~~; release security qualification, signed releases, SBOM/provenance and update/rollback operations.

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

## Original first 12 engineering tickets — reviewed status

1. **Partial:** ~~CI bootstrap/build/headless tests and dependency checkout checks~~; retain the full Phase 0 configuration/release gate.
2. **Partial:** ~~FileLink/SourceMount canonical-root confinement and non-executing project open~~; persistent project/plugin trust remains open.
3. **Implemented foundation:** ~~checked module resolution and owned/bounded deserialization~~; continue parser regressions.
4. **Implemented foundation:** ~~generation-safe object identity, parent-cycle rejection and monotonic destruction~~.
5. **Partial:** ~~checked native binding/capability boundaries~~; continue auditing full API coverage rather than asserting every historical binding is closed.
6. **Implemented foundation:** ~~host-granted execution capabilities and denial of ambient game ProcessService authority~~.
7. **Partial:** ~~bounded parser/runtime surfaces and network fuzz/sanitizer targets~~; finish boundary-wide coverage and complete scheduler/signal abuse qualification.
8. **Implemented foundation:** ~~stable IDs, canonical schema, mutation journal and authoritative Studio history~~; history is commit-only grouping.
9. **Partial:** ~~versioned bounded serialization and atomic project round trip~~; broader migration/source-diagnostic qualification.
10. **Partial:** ~~ModuleScript identity/resolution and scheduler foundations~~; complete cancellable scheduler/quota exit qualification.
11. **Implemented foundation:** ~~corrected input boundary, ActionMap and default player camera/controller~~.
12. **Implemented foundation:** ~~neutral physics/immutable render boundary and interactive retained Screen UI~~.

These are continuing contracts for networking, Studio and creator content, not
twelve wholly unstarted foundations. Remaining qualification and known-issue
gates still constrain higher-level work; use the track ordering above.
