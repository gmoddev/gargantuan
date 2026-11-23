---
status: proposal
owner: serialization
baseline: e7bec187def55e79c6ca956b17feb82926e5d1fe
---

# GAS Foundation 1 inventory

Engine `origin/main` resolved to `e7bec187def55e79c6ca956b17feb82926e5d1fe` on
2026-11-23. The [native Windows and Linux run](https://github.com/gmoddev/gargantuan/actions/runs/43870187334),
[GNS sanitizer run](https://github.com/gmoddev/gargantuan/actions/runs/43870187327),
and [documentation run](https://github.com/gmoddev/gargantuan/actions/runs/43870187304)
passed at that exact revision. This is the base of `foundation/gas-1`.
[Engine hosting PR #27](https://github.com/gmoddev/gargantuan/pull/27) had head
`d51c074cfaaf973310a4d8effd192cffd85c384d` and remains outside the GAS branch.
The inventory covers Engine code at the baseline. GAS integrations below are
proposed unless their own implementation and qualification are recorded.

## Capability matrix

| Area | Baseline capability and evidence | GAS disposition |
| --- | --- | --- |
| Representations and codecs | Project Instance JSON v5, Snapshot and journal JSON v6, EditorHost JSON v1, typed `WireValue`, and private `JsonCodec`. Project `InstanceFormat::Binary` is an unimplemented placeholder. [Codec boundary](../../devdocs/CurrentArchitecture/SerializationCodecBoundary.md), [project v5](../../devdocs/CurrentArchitecture/ProjectPersistenceV5.md) | Reuse semantic types and validators. Add one bounded canonical binary codec for the two GAS profiles. Continue reading and writing existing project JSON at its current entry points. |
| DataModel persistence | `Project::CaptureGame` captures authoring state and project assets. `Project::DeserializeGame` restores the authoring graph and attaches filesystem and assets. `CaptureSnapshot` and `LoadSnapshot` carry live scope, journal cursor, and replication-selected properties. [Project](../../src/filesystem/Project.cpp), [Snapshot](../../src/runtime/Snapshot.cpp), [DataModel](../../src/classes/DataModel.cpp) | Adapt the persistence traversal and candidate-open lifecycle for GASP. Reuse service provisioning and the frozen schema. Snapshot is useful as a structural reference and an inspection aid. |
| Instance IDs and references | Project v5 assigns preorder `NodeId`s and restores references after construction and parenting. `ObjectId` is generation-scoped. `DeserializeClone` allocates fresh object and script resource identities. [Project v5](../../devdocs/CurrentArchitecture/ProjectPersistenceV5.md), [materializer](../../src/assets/InstanceSerialization.cpp) | Adapt the local table to an ordered forest with a shared reference namespace. Keep GAS IDs document-local. Parse and validate external-reference policy before any destination lookup. Remap references through newly allocated objects and preserve durable script identity on place reopen. |
| Class and property schema | Frozen `RuntimeSchemaRegistry` exposes `SchemaId`, definition versions, inheritance, declaring-class property identity, persistence flags, reference target policy, extensions, and custom classes. [`RuntimeSchema`](../../docs/src/content/docs/developing/runtime-schema.mdx), [registry](../../include/gargantuan/reflection/RuntimeSchema.hpp) | Encode versioned declaring-class property identities with canonical member names, and validate against the existing registry. Adapt legacy migrations explicitly. Native definition and property migrations beyond existing compatibility support need a defined policy. |
| Attributes, tags, and hierarchy | Project v5 stores typed attributes, tags, extension and custom overrides, and ordered children. An emitted non-archivable object omits its descendants. [Persistence](../../src/assets/InstanceSerialization.cpp), [attribute validation](../../src/runtime/AttributeValidation.cpp), [TagIndex](../../src/runtime/TagIndex.cpp) | Reuse reflection-backed attribute and tag validators, capture sibling order, and make export omissions visible. A canonical preorder table can encode both forest roots and child order. |
| Luau source and configuration | `LuaSourceContainer.Source` and `SourceId` are saved properties. `SourceVersion` is transient. Project PreRun is a distinct, bounded schema input. `FileLink` resolves project-relative mounts through `SourceMount`. [Source class](../../assets/classes/LuaSourceContainer.luau), [schema lifecycle](../../src/reflection/RuntimeSchemaLifecycle.cpp), [SourceMount](../../devdocs/CurrentArchitecture/SourceMount.md) | Carry source as inert authoring data. Use the existing authorization and activation lifecycle for imported scripts and the established trusted schema preparation path for PreRun. Document source-mount and reimport provenance requirements when moving projects. |
| Assets and CAS | `AssetService` owns `AssetId`, strict `AssetReference`, dependency records, and canonical artifact bytes keyed by SHA-256 `AssetContentId`. Project snapshots retain authoring provenance and runtime snapshots carry a source-free catalog. [Asset contracts](../../devdocs/CurrentArchitecture/AssetFoundation1.md), [AssetService](../../src/services/AssetService.cpp) | Resolve GAS dependency descriptors through the same catalog and CAS. Reuse digest verification and dependency closure. Recursively resolve explicit declarations and reflected asset-valued properties. Arbitrary script asset reachability is unproven, so retain conservative package inclusion. |
| Package construction and loading | `PackageBuilder::Capture` produces immutable `GamePayload`. Build partitions eligible content under format-2 manifests, hashes staged files, and validates before publication. Player and Server hosts share package loading. Network player code hydrates from the client-run script subset under existing host policy. [Builder](../../src/packaging/PackageBuilder.cpp), [package contract](../../devdocs/CurrentArchitecture/StandalonePackagingFoundation1.md), [content availability](../../devdocs/CurrentArchitecture/ContentAvailabilityFoundation3L.md), [host contracts](../invariants/Core.md) | First qualify explicit GASP input compiled to the existing bootstrap and content units. Measure this against direct native GAS runtime loading before changing the distribution contract. Confirm physical client artifact closure independently of client-side execution filtering. |
| Compatibility and migration | JSON readers accept project versions 0–5. Legacy reference properties fall back to defaults. Project metadata has a durable `ProjectId`; legacy directories can acquire an ID. General schema migration is absent. [Project v5](../../devdocs/CurrentArchitecture/ProjectPersistenceV5.md), [project identity](../../src/filesystem/Project.cpp), [schema compatibility](../../docs/src/content/docs/developing/runtime-schema.mdx) | Keep migrations explicit and report omissions and defaults at conversion. Require admitted schema versions and reject unknown required data before publication. Document round-trip limits for unsupported binary values, unknown legacy fields, foreign schemas, and authoring-only provenance. |
| EditorHost mutation and history | Studio structural edits use `MutationGateway` and project-scoped authority. Create, Duplicate, and internal Restore stage detached single subtrees with journal suppression, publish committed changes, and use bounded transaction/history paths. [Gateway](../../src/runtime/MutationGateway.cpp), [EditorHost](../../src/editor/EditorHost.cpp), [revision contract](../../devdocs/CurrentArchitecture/ProjectRevisionAndPersistence.md) | Add explicit GAS operations through EditorHost and provide a Studio capability handoff. Adapt adoption and Undo/Redo for a single all-or-nothing forest transaction. Preflight protected services, revision, graph, references, and asset deltas before publication. |

The project v5 specification and reachable serializer establish the current
format. Earlier v4 statements in the codec, structural authoring, and revision
documents describe older behavior. The GAS reader should retain one bounded
typed parse path. [KI-008](../../KNOWN_ISSUES.md#ki-008-json-tree-cleanup-can-allocate-during-memory-exhaustion-unwinding)
records an open allocation-failure gap in JSON cleanup, so exception handling
around a JSON conversion alone cannot qualify sustained allocation failure.

## Qualification and package decision

- Use equivalent reflected state with 100, 1,000, and 5,000 objects. Use the
  same deterministic topology and property mix in each arm, including ordered
  children, scripts, attributes, tags, reference edges, and shared asset
  dependencies. Add a depth-limit and a wide-forest boundary fixture. Compare
  saved property values, link endpoints by graph position, sibling order,
  service ownership, script source, and asset digests after load. Count admitted
  objects and references as part of each measurement.
- Compare JSON project load, clear GAS encode and decode, GAS place import
  through project materialization, direct package build, and GASP-to-package
  build. Measure cold headless startup of the standard Player and Server
  packages. Include direct GAS runtime loading as a separately versioned
  candidate only after measuring the interchange path. Use the existing
  [serialization benchmark](../../tests/SerializationBenchmark.cpp) and
  [packaging benchmark](../../tests/PackagingBenchmark.cpp) as starting points.
- Record source, compiler and configuration, OS and hardware, fixture digest,
  process isolation and cold/warm state, repetitions, file sizes, per-stage wall
  time, peak process memory, and allocation count with its coverage. Compare
  canonical clear document hashes across repeated exports and operating systems.
  Verify package repeatability against matching runtime distribution inputs,
  track per-unit changes for incremental builds, and measure compression if a
  bounded candidate is proposed. Exercise native Windows and Linux tests,
  sanitizers, docs checks, package validation, and EditorHost authorization and
  rollback on the exact GAS source.
- Relay Rescue is present at `origin/foundation/gameplay-7` revision
  `06d1a2015b26f8610a74a6b6eea39b179a4417a8`. Its merge base with Engine main is
  the GAS baseline. Use a separate worktree with the fixture and runtime from
  that revision, plus the accepted GAS commits. Verify
  `samples/GameplayFoundation7/BuildProject.py --check`, retain a source
  baseline run, and run `gargantuan_gameplay_relay_sample` and
  `gargantuan_gameplay_relay_sample_gns` against an imported project. The test
  currently takes the fixture via a compile-time root; add a test-only input
  seam in the integration worktree if needed. Verify package startup and the
  complete mission with a server and two clients, including authority, relay
  charge, inventory, landing, respawn, and disconnect. For GASM, use an authored
  subtree that exists before runtime activation and has an asset-bearing or
  reference-bearing edge. Record exact source, package and export digests, and
  include the prior mission evidence for comparison.
  [Gameplay Foundation 7 validation](https://github.com/gmoddev/gargantuan/blob/06d1a2015b26f8610a74a6b6eea39b179a4417a8/devdocs/Validation/GameplayFoundation7.md).
