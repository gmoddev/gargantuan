---
status: historical-evidence
authority: non-normative
review_date: 2026-09-12
---

# Cross-repository roadmap evidence review

This review reconciles the [feature checklist](../../docs/src/content/docs/developing/roadmap.mdx)
and [phase roadmap](Roadmap.md) with the five local repositories. It records
implemented slices, remaining scope, and superseded plans. It does not certify
a release or close a phase simply because code or a test exists.

## Revision scope and method

| Repository | Inspected HEAD | Local state at review start |
| --- | --- | --- |
| gargantuan-main | `5ada43a57` | Existing known-issue, 3L validation, future-architecture, and morphology work; untracked build/evidence directories. |
| gargantuan-node | `f444042` | Existing changes to `integration/gargantuan/CMakeLists.txt` and `internal/host/content_scale_integration_test.go`. |
| gargantuan-mcp | `508ea24` | Clean. |
| gargantuan-telemetry | `66841e6` | Clean. |
| gargantuan-studio | `23bc08a` | Clean. |

The review used source, test assertions, build targets, current architecture,
and existing validation reports. No runtime suites, hardware benchmarks,
sanitizers, public-hosting tests, or cross-repository release qualification were
rerun for this documentation change. Existing local evidence is identified as
such and is not presented as a newly committed or newly executed result.
Unrelated files and all four sibling repositories were left unchanged.

Checked items mean the stated slice is implemented. Partial items strike through
only the delivered portion. Superseded entries are explicitly labeled and do
not count as completed work. Unchecked items without a partial note have no
sufficient implementation evidence in this review, or retain an unmet acceptance
gate; this is not proof that no prototype exists anywhere.

## Main runtime, rendering, and tooling

| Roadmap area | Evidence inspected | Status and limits |
| --- | --- | --- |
| Build/test baseline | [CMake targets](../../CMakeLists.txt), [native CI](../../.github/workflows/native-ci.yml), [CI contract](../CurrentArchitecture/ContinuousIntegration.md) | Windows Release and Linux sanitizer/headless gates and testable core exist. Do not claim every Debug/static-analysis/formatter gate or every security finding closed. |
| Identity, schema, mutation | [foundation tests](../../tests/FoundationTests.cpp), [runtime source](../../src/runtime/), [schema lifecycle](../../include/gargantuan/reflection/RuntimeSchemaLifecycle.hpp) | Generation-safe identity, cycle rejection, transactional frozen schema, mutation gateway and journal are implemented. Broad migration/conformance coverage remains ongoing. |
| Scripting and filesystem | [capabilities](../../src/scripting/ScriptSecurity.cpp), [SourceMount](../../src/filesystem/SourceMount.cpp), [foundation tests](../../tests/FoundationTests.cpp), [security contract](../CurrentArchitecture/ScriptSecurity.md) | Domains/capabilities, checked ModuleScript resolution and source-root confinement are implemented. The old privilege enum is superseded; general API parity and two-way filesystem sync are not complete. |
| Source authoring/history | [EditorHost](../../src/editor/EditorHost.cpp), [transactions](../../src/runtime/AuthoritativeTransactions.cpp), [source contract](../CurrentArchitecture/ScriptAuthoring.md), [history contract](../CurrentArchitecture/AuthoritativeTransactions.md) | Bounded UTF-8 source read/write, expected-version rejection, source-aware undo/redo/save, and commit-only transaction grouping exist. No durable sync mapping/base/conflict store or abortable general transaction is implied. |
| Player and physics | [Player tests](../../tests/PlayerRuntimeTests.cpp), [physics tests](../../tests/PhysicsBackendTests.cpp), [neutral physics](../CurrentArchitecture/PhysicsBackend.md) | Default controller/camera, kinematic collision and weld constraints exist. Mesh collision and broader rigid constraint/material systems remain future. Deformable material parameters are a separate implemented slice. |
| Renderer/assets | [material implementation](../../src/render/passes/OpaquePass.cpp), [asset tests](../../tests/AssetFoundationTests.cpp), [Asset 2A](../CurrentArchitecture/AssetFoundation2A.md), [Renderer 2C decision](../CurrentArchitecture/RendererFoundation2C.md) | Canonical Mesh/Material/Image assets, glTF import, texture residency and renderer-neutral publication exist. SDL retention checkpoint is decided. Metallic/roughness data is preserved; the current Lambert shader does not implement metallic-roughness BRDF or normal mapping. Culling/bucketing, PBR/IBL and advanced lighting remain open. |
| Runtime GUI | [GUI runtime](../../src/gui/GuiRuntime.cpp), [GUI tests](../../tests/GuiFoundationTests.cpp), [GUI 2](../CurrentArchitecture/GuiFoundation2.md) | TextBox UTF-8 editing/focus/security, nested ScrollingFrame rendering/input and retained UI are implemented. ImageButton, general runtime drag/drop, editable images, viewport frames and broader layout/decorator classes stay open. Studio docking gestures are not runtime GUI completion. |
| Audio/animation | [audio tests](../../tests/AudioFoundationTests.cpp), [animation tests](../../tests/AnimationFoundationTests.cpp), [Audio 1](../CurrentArchitecture/AudioFoundation1.md), [root motion](../CurrentArchitecture/AnimationFoundation3RootMotion.md) | Basic audio, animation and Character root-motion foundations exist. Advanced animation tools, IK/retargeting/graphs and broader creator tooling are not complete. |
| Packaging/roles | [PackageBuilder](../../src/packaging/PackageBuilder.cpp), [host composition](../../src/host/), [package smoke](../../tests/cmake/FirstCompleteGamePackageTests.cmake), [server package test](../../tests/cmake/DedicatedServerPackageTests.cmake) | Windows standalone creator packaging and separate Player/Server roles exist. This is packaging an engine runtime with content, not an ahead-of-time Luau compiler. Linux server evidence does not complete Linux/macOS creator distribution, signing, or installed-tool delivery. |

## Studio and MCP

| Roadmap area | Evidence inspected | Status and limits |
| --- | --- | --- |
| Studio shell | [workspace model](../../../gargantuan-studio/src/GargantuanStudio/StudioWorkspaceLayout.cs), [floating windows](../../../gargantuan-studio/src/GargantuanStudio/StudioFloatingWorkspace.cs), [UX self-tests](../../../gargantuan-studio/src/GargantuanStudio/StudioUxSelfTests.cs), [current UX](../../../gargantuan-studio/devdocs/CurrentArchitecture/StudioUX.md) | New/open/save, Avalonia primitives, docking/floating windows, script documents, Explorer, Properties, Output, assets and transform gizmos exist. Menus/toolbars supersede the old ribbon proposal. Tools are first-party surfaces, not third-party plugins. Named layouts and general plugin/settings workflows remain open. |
| Studio source and play | [script editor and self-tests](../../../gargantuan-studio/src/GargantuanStudio/ScriptEditor.cs), [Studio README and smoke entrypoints](../../../gargantuan-studio/README.md) | Syntax diagnostics, version-checked commits, shared history and one isolated local Play/Stop runtime exist. Full LSP/debugging, durable authoring identity across file mappings, source mounts, two-way sync and multiplayer orchestration remain open. Paced presentation does not establish universal sustained 30/60 FPS qualification. |
| Authenticated bridge | [Studio bridge host](../../../gargantuan-studio/src/GargantuanStudio/StudioMcpBridgeHost.cs), [bridge tests](../../../gargantuan-studio/src/GargantuanStudio/StudioMcpBridgeSelfTests.cs), [MCP adapter tests](../../../gargantuan-mcp/tests/Gargantuan.Mcp.Tests/StudioGargantuanAdapterTests.cs), [tool reference](../../../gargantuan-mcp/devdocs/ToolReference.md) | Current-user Windows pipe, per-session token, bounded requests/concurrency, stale identities, independent ProjectWrite/ScriptWrite policy and source-revision conflicts are implemented. MCP shares Studio command/source routes and has no direct engine connection. Sync tools, retained three-way conflicts and watcher recovery remain future. |

The older main `ScriptAuthoring.md` deferral of MCP source tools has been
overtaken by the later Studio/MCP ScriptWrite implementation. It remains correct
about filesystem synchronization being deferred. This roadmap uses the later
implementation for MCP status without treating basic source conflicts as
three-way file-sync conflicts.

## Node, telemetry, and networking qualification

| Roadmap area | Evidence inspected | Status and limits |
| --- | --- | --- |
| Node storage/services | [DataStore implementation](../../../gargantuan-node/internal/services/datastore/), [tenant/bounds tests](../../../gargantuan-node/internal/services/datastore/node_test.go), [Node service map](../../../gargantuan-node/README.md) | Service runtime, registration/discovery, authenticated bounded calls, revisioned key/value and document/query contracts with memory provider exist. Durable production storage and game-facing DataStore integration remain open. |
| Authentication/entitlements | [player-auth source/tests](../../../gargantuan-node/internal/services/playerauth/), [entitlement source/tests](../../../gargantuan-node/internal/services/entitlements/), [Engine provider integration](../CurrentArchitecture/BackendProviderIntegration1.md) | Node player identity providers, including server-verified Steam, and Engine/Node entitlement integration exist. Purchases/commerce and short-lived GameSession admission grants are separate unfinished work. Identity-provider success is not a gameplay admission gate. |
| Content delivery | [Node content service](../../../gargantuan-node/internal/services/content/node.go), [local scale tests](../../../gargantuan-node/internal/host/content_scale_integration_test.go), [Engine content service](../../src/content/ContentAvailability.cpp), [content tests](../../tests/ContentAvailabilityTests.cpp) | Authenticated exact-index immutable package delivery and the Local/Node content lifecycle are implemented. A general CDN and qualified large-world streaming product are not. The Node scale test includes pre-existing uncommitted work. |
| Telemetry | [Rust ABI](../../../gargantuan-telemetry/src/lib.rs), [consent](../../../gargantuan-telemetry/src/consent.rs), [Engine adapter tests](../../tests/OptionalTelemetryTests.cpp), [Studio adapter tests](../../../gargantuan-studio/src/GargantuanStudio/Telemetry/TelemetrySelfTests.cs), [host integration](../CurrentArchitecture/TelemetryIntegration.md) | Optional default-off ABI/library, privacy and bounded crash/performance lifecycle, and both host integrations exist. Telemetry's README describing host integration as future is stale across repositories. A shared ABI manifest, collector/release operations and new detailed profiling schemas are not implied complete. |
| Networking 3G/3H/3K | [network implementation](../../src/network/), [3G](../CurrentArchitecture/CharacterNetworkingFoundation3G.md), [3H](../CurrentArchitecture/CharacterReplicationFoundation3H.md), [3K](../CurrentArchitecture/SpatialRuntimeProjectionFoundation3K.md) | Bounded fair publication, derived spatial indexing and generation-safe spatial projections extend the already checked 3E/3F/3I/3J work. No public SpatialRegion or portal authority was added. |
| Foundation 3L exit | [current local validation ledger](../CurrentArchitecture/ContentAvailabilityFoundation3L_3Validation.md), [known issues](../../KNOWN_ISSUES.md) | **B — partially ready; no 3M.** Planning/selection bounds, publication work, reliable-byte admission and the GNS sanitizer correction are implemented slices. Gameplay/client, sustained overload/fairness/recovery, journal margin, scale/physics and current-source security remain independent open gates. Existing successful CI does not close them. |

## New proposal integration and retained gates

The [architectural-discipline proposal](ArchitecturalDisciplineAndSubsystemBoundaries.md)
adds separate architecture-enforcement, admission, service-envelope, local
resource-attribution, Asset 2B residency, Studio provenance/trust, later
anti-entropy, and risk-weighted fuzzing tracks. Foundation 3M is trusted demand
policy only and follows 3L closure; semantic SpatialRegion/topology follows
validated default-space 3M. Immutable spatial reads and worker derivation stay
conditional on measured need.

`CMakeLists.txt` already separates `gargantuan_gns_transport` and packaged host
targets. Architecture 0D therefore has an implemented portion; the future task
is graph-backed boundary tightening and remaining neutral target extraction,
not creation of a second GNS adapter. Broad `gargantuan_core` compilation is
still present and does not itself enforce the proposed forbidden-edge graph.

The older first twelve delivery tickets contain implemented foundations, but
their broad safety/budget and clean-machine exit tests are not all requalified
here. The phase roadmap retains those gates and no phase is marked complete.
Likewise the unused `.instance.bin` name, Lute library parity, selected Roblox
compatibility, particles/trails/beams, public plugin distribution, collaboration,
mobile/VR, terrain and general economy/cloud ambitions remain open or demand-led.
