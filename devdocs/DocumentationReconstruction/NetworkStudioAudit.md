# Networking, Studio, and Node documentation reconstruction

Audit reconciled 2026-11-15. This is a source-pinned documentation audit, not a
new qualification run or release certificate.

## Source pins and state

| Repository/source | Exact state inspected | Interpretation |
| --- | --- | --- |
| Engine merged main | `b5df1e7b1b80da27158ea794626f90a8ebd8a6d2`, tree `7ef2a2b6b2bb34732bbb8335f30dff9588a68f39`, verified against remote main | Current Engine source for reconciled pages; the documentation worktree contains this main integration without a new documentation commit. |
| Engine active sibling | `foundation/3l-content-availability` at `eb2ce26ab1455670f4e7b53426cc3521403110c5` with tracked and untracked local work | Separate checkout; dirty files were not read as merged behavior or changed here. |
| Studio merged main | `dc960b5433b73e1d305548e0dc5a8dfc0d9cf2f3`, tree `f34f0474321b86f73416a9deb5732fa41c1ff1c9`, verified against remote main and PR #15; inspected with `git show`/`git ls-tree` | Current LUI-primary owner source. Its ten-gate native receipt binds qualified ancestor `93961e78c6ce91aa59fa297fcbd9efd57e0ac121`, tree `034dbcf500710c7d88607fb9aedb0c625d43d520`, and exact binaries, not an arbitrary rebuild. |
| Studio local checkout | `main` at `23bc08a87be25f7f84a9513fe0a9efb6402f0460`, with untracked `.validation/` | Older Avalonia checkout; read-only and not the merged source. |
| Node local checkout | `master` at `f4440423c0701ff396f589fd51b6ce41edc63539`; dirty `integration/gargantuan/CMakeLists.txt` and `internal/host/content_scale_integration_test.go` | Read-only Node source. These dirty files were not treated as a qualified release. |

## Verified boundaries

- Engine `include/gargantuan/network/{GameSessionProtocol.hpp,GameSession.hpp,Delivery.hpp,ReliableServiceProfile.hpp}` and `src/network/`, with `devdocs/CurrentArchitecture/{NetworkingContracts,NetworkSchedulerContract,RealGameTransport,BasicClientReplication,ReplicationFoundation3I,ReplicationFoundation3J,SpatialRuntimeProjectionFoundation3K,ContentAvailabilityFoundation3L,TrustedResidencyDemandFoundation3M}.md`, establish separate authoritative state, per-peer replication intent, scheduler admission, transport delivery, client projection, and trusted default-space demand. GSES, GRPL, GCHR, Remotes, 3I/3J/3K, 3L and 3M have distinct ownership and completion points. `ChangeJournal.Sequence` is not a packet sequence.
- Engine `src/editor/EditorHost.cpp`, `include/gargantuan/editor/EditorHost.hpp`, `devdocs/CurrentArchitecture/{EditorHostProtocol,ProjectRevisionAndPersistence,ProjectCreation,ScriptDocumentBindings,SourceMount}.md` establish bounded tokened IPC, schema/snapshot/journal projection, Engine revision/history/save authority, token-checked Source commits, and root-confined FileLink/asset source reads. `GetScriptSource`/`SetScriptSource` are the authoritative text path; a Studio buffer is a draft.
- Studio `dc960b:ARCHITECTURE.md`, `devdocs/AcceptedArchitecture/{LuiPrimaryMigration,StudioExtensionsV2}.md`, `devdocs/CurrentArchitecture/{LuiDocuments,PluginAuthoringAdapter,StudioPluginSourceEdits,StudioCreatorTools3}.md`, `experiments/LuiStudio/{McpGateway,PackageGame}.md`, and the `App/Src` Luau modules make LUI/Luau primary, the Host native resource/IPC owner, and Avalonia a frozen legacy reference. The retained native TextBox owns local Source Undo through docking and floating; reviewed plugin SourceWrite and explicit single-Script import edit only a local draft. Engine Commit Source remains a separate version-checked command. Studio's optional MCP gateway is a five-method subset with no Source/Save/Undo authority. Public Untrusted plugin execution is disabled. The local Studio checkout's old Avalonia files remain evidence for that earlier revision only.
- Node `docs/content-streaming-v1.md`, `protocol/proto/content/v1/content.proto`, `internal/services/content/node.go`, and `internal/services/content/provider/filesystem/provider.go` establish private manifest/content RPCs, trusted tenant and separate read capabilities, exact namespace/key provider lookup, and immutable byte delivery. Node does not create live Instances or own per-frame simulation.

## Qualification and remaining external scopes

Integrated Engine policy (`docs/adr/D03-hybrid-pooled-service-and-permit-ownership.md`, `KNOWN_ISSUES.md` KI-006, `tests/Foundation3LFinalAcceptance.md`) reports `POOLED_CORRECTNESS_V1` **core correctness qualified** at its exact source; KI-006 is closed for that core scope. The merged main retains this contract and has a separately qualified 3M trusted-demand slice. Earlier F1 and finite-grant physical failures retain their original verdicts. Physical Local32, real-TLS Node32, provider/deployment resource and performance envelopes, and release certification are deferred or not measured. No physical F1 PASS or unqualified Ready claim follows from simulator, localhost, native, or core-correctness evidence.

The Studio [creator platform receipt](https://github.com/GantriaEngine/gargantuan-studio/blob/main/devdocs/Validation/StudioCreatorPlatformNativeQualification20261114.json) reports ten PASS gates at `93961e...` with Engine `b5df1e...`: full build, Engine contracts, project plugin storage, nine creator journeys/325 actions, API 2 lifecycle/132 actions, one renderer journey/41 actions, bounded paired performance/UI checks, natural retirement, and worker CI reproduction. The final Studio merge differs from that qualified product source in launcher, evidence, audit and tests, not App/Host product source. The hosted Actions runs at final source were refused before steps by an account billing gate; they are not hosted PASS. The user reports a fresh 31-action SourceTools normal-retirement smoke and four postmerge CI blocks on dockerbox; no independently inspected receipt for those later executions is asserted here. The native receipt itself excludes universal performance, hostile-code containment, physical input, IME, gameplay, broad distribution, and clean-machine installation. Studio's older README pending-gate prose is superseded for this qualified candidate by the final ledger and source.

Remaining owner work includes physical/provider networking certification, Studio distribution and physical UI checks, public Untrusted extension containment, full legacy MCP discoverability/parity if wanted, and optional Node provider deployment qualification. No new native build or worker was run for this documentation edit.

## Stale statement inventory

Corrected here: `docs/src/content/docs/developing/editor-host.mdx` Avalonia-primary wording; `devdocs/CurrentArchitecture/PlatformInputBoundary.md` Avalonia/`WM_INPUT` current-Studio wording; `devdocs/CurrentArchitecture/EditorViewportPresentation2.md` now explicitly classifies the old receipt as pre-LUI; `docs/src/content/docs/developing/future-architecture-concerns.mdx` now treats the .NET 7/Avalonia distribution concern as historical and asks for exact-source reassessment.

Outside this audit's edit scope, report to the integration owner: `README.md:94,118` calls Studio C#/Avalonia; `devdocs/FutureArchitecture/RoadmapEvidence20260912.md:55` names Avalonia but is dated 2026-09-12 historical evidence and should retain that classification; `devdocs/CurrentArchitecture/ImplementationGaps.md:138` describes a pre-GUI-foundation sample and needs revision-scoped classification if surfaced as current. Older Studio-specific validation receipts should retain their source and dates, not be rewritten as LUI results.

The reconstruction's earlier baseline was Engine `2a4c1d2b03e40178b97334bb6f1f2eeedd51fe9f`
and Studio remote-tracking `6ae8f3dc427bc5221e978ae627eab6224e5ccea6`.
Those remain historical inspection pins, not the current main identities.
Parent reconciliation resolved the README wording, classified older audits,
and pinned historical sibling links to their recorded source. Studio's
canonical owner is `GantriaEngine/gargantuan-studio`; old gmoddev URLs redirect.
No sibling checkout was changed.
