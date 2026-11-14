# Networking, Studio, and Node documentation reconstruction

Audit date: 2026-11-14. This is a source-pinned documentation audit, not a build,
new qualification run, or release certificate.

## Source pins and state

| Repository/source | Exact state inspected | Interpretation |
| --- | --- | --- |
| Engine isolated worktree | `docs/reconstruction-20261114`, clean at `2a4c1d2b03e40178b97334bb6f1f2eeedd51fe9f`, identical to its `origin/main` before this documentation work | Integrated main baseline for the new pages. |
| Engine active sibling | `foundation/3l-content-availability` at `eb2ce26ab1455670f4e7b53426cc3521403110c5`, behind its remote by 8 at inspection, with tracked and untracked local work | Separate development state; no dirty files were read as integrated behavior or changed here. |
| Studio local checkout | `main` at `23bc08a87be25f7f84a9513fe0a9efb6402f0460`, 29 behind `origin/main`, with untracked `.validation/` | Older Avalonia checkout; read-only. |
| Studio remote-tracking object | Local Git object `origin/main` at `6ae8f3dc427bc5221e978ae627eab6224e5ccea6` (2026-11-07 merge), inspected with `git show`/`git ls-tree`, without checkout or fetch | LUI-primary Studio source and current owner documentation. Its `experiments/LuiStudio/README.md` cites qualified native checkpoint `effc10447337aff4641fb4731aee651c1be4113d`; physical input, IME, gameplay, and display latency are outside that receipt. |
| Node local checkout | `master` at `f4440423c0701ff396f589fd51b6ce41edc63539`; dirty `integration/gargantuan/CMakeLists.txt` and `internal/host/content_scale_integration_test.go` | Read-only Node source. These dirty files were not treated as a qualified release. |

## Verified boundaries

- Engine `include/gargantuan/network/{GameSessionProtocol.hpp,GameSession.hpp,Delivery.hpp,ReliableServiceProfile.hpp}` and `src/network/`, with `devdocs/CurrentArchitecture/{NetworkingContracts,NetworkSchedulerContract,RealGameTransport,BasicClientReplication,ReplicationFoundation3I,ReplicationFoundation3J,SpatialRuntimeProjectionFoundation3K,ContentAvailabilityFoundation3L}.md`, establish separate authoritative state, per-peer replication intent, scheduler admission, transport delivery, and client projection. GSES, GRPL, GCHR, Remotes, 3I/3J/3K and 3L have distinct ownership and completion points. `ChangeJournal.Sequence` is not a packet sequence.
- Engine `src/editor/EditorHost.cpp`, `include/gargantuan/editor/EditorHost.hpp`, `devdocs/CurrentArchitecture/{EditorHostProtocol,ProjectRevisionAndPersistence,ProjectCreation,ScriptDocumentBindings,SourceMount}.md` establish bounded tokened IPC, schema/snapshot/journal projection, Engine revision/history/save authority, token-checked Source commits, and root-confined FileLink/asset source reads. `GetScriptSource`/`SetScriptSource` are the authoritative text path; a Studio buffer is a draft.
- Studio remote-tracking `origin/main:README.md`, `ARCHITECTURE.md`, `devdocs/AcceptedArchitecture/LuiPrimaryMigration.md`, `devdocs/CurrentArchitecture/LuiDocuments.md`, and `experiments/LuiStudio/README.md` make LUI/Luau primary, the Host native resource/IPC owner, and Avalonia a frozen legacy reference. The local Studio checkout's older `README.md`, `src/GargantuanStudio/StudioWorkspaceShell.cs`, and `devdocs/CurrentArchitecture/StudioUX.md` remain evidence for that older revision only. Studio-owned documents hold the detailed feature and qualification ledger; the Engine site links there.
- Node `docs/content-streaming-v1.md`, `protocol/proto/content/v1/content.proto`, `internal/services/content/node.go`, and `internal/services/content/provider/filesystem/provider.go` establish private manifest/content RPCs, trusted tenant and separate read capabilities, exact namespace/key provider lookup, and immutable byte delivery. Node does not create live Instances or own per-frame simulation.

## Qualification and pending integration

Integrated Engine policy (`docs/adr/D03-hybrid-pooled-service-and-permit-ownership.md`, `KNOWN_ISSUES.md` KI-006, `tests/Foundation3LFinalAcceptance.md`) reports `POOLED_CORRECTNESS_V1` **core correctness qualified** at its exact source; KI-006 is closed for that core scope. Earlier F1 and finite-grant physical failures retain their original verdicts. Physical Local32, real-TLS Node32, provider/deployment resource and performance envelopes, and release certification are deferred or not measured. No physical F1 PASS or unqualified Ready claim follows from simulator, localhost, native, or core-correctness evidence. The active 3L branch and dirty work are not silently merged into the integrated main description.

Remaining work belongs to its owning repository and exact source: physical/provider certification and current-source integration for networking; Studio's source-bound LUI distribution/physical UI checks and separately tracked feature limits; optional Node provider deployment qualification. No native build or worker was run for this documentation change.

## Stale statement inventory

Corrected here: `docs/src/content/docs/developing/editor-host.mdx` Avalonia-primary wording; `devdocs/CurrentArchitecture/PlatformInputBoundary.md` Avalonia/`WM_INPUT` current-Studio wording; `devdocs/CurrentArchitecture/EditorViewportPresentation2.md` now explicitly classifies the old receipt as pre-LUI; `docs/src/content/docs/developing/future-architecture-concerns.mdx` now treats the .NET 7/Avalonia distribution concern as historical and asks for exact-source reassessment.

Outside this audit's edit scope, report to the integration owner: `README.md:94,118` calls Studio C#/Avalonia; `devdocs/FutureArchitecture/RoadmapEvidence20260912.md:55` names Avalonia but is dated 2026-09-12 historical evidence and should retain that classification; `devdocs/CurrentArchitecture/ImplementationGaps.md:138` describes a pre-GUI-foundation sample and needs revision-scoped classification if surfaced as current. Older Studio-specific validation receipts should retain their source and dates, not be rewritten as LUI results.

Parent reconciliation resolved the README wording, explicitly classified the
older audits, and pinned historical sibling links to their original recorded
HEADs. Studio's canonical owner is `GantriaEngine/gargantuan-studio`, confirmed
by live Git remote and repository metadata; old gmoddev URLs redirect. A final
read-only remote check confirmed Engine, Studio and Node mainline pins still
match the source identities above. No sibling campaign was changed.
