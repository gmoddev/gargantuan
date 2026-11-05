# Durable Script binding qualification — 2026-11-03

The Engine foundation is qualified by a source-bound Windows worker build and
five focused test suites. Studio restoration through the native UI remains a
separate gate. This receipt does not claim unsaved-draft recovery or full-suite
qualification.

The contract is [Script document bindings](ScriptDocumentBindings.md); its
ownership and persistence decision is [ADR D04](../../docs/adr/D04-durable-script-resource-bindings.md).

## Source and binary identity

The implementation composes onto `6ee7bfd03f231cc6cda8d638a446a5770b7f7119`,
including the previously qualified disjoint SoftBody work. The initial overlay
uses `624135ed10f4915c1cadc7c3ab1fe30b3ad1e883` for its 14-file preimage fence;
this is not a claim that the final binary excludes the later SoftBody changes.
All three peer physics file hashes were retained and checked before and after
qualification.

The worker generated 132 class files with the pinned Lute 1.0.0 tool. It built
Release with MSVC 14.44, two build jobs, GNS disabled and NodeContent disabled.
The final installed manifest verifies all 14 overlay files, all 132 generated
files and the three peer files. The controller independently matched all 14
source files against that manifest before adding this prose receipt. No
functional source changed after the passing run.

| Artifact | SHA-256 |
| --- | --- |
| Final `InstalledManifest.json` | `A01AA34E0FC58DC97854EC59970D6B52ABB49D4991BD98B78737A29685C3B85A` |
| Final JUnit `tests.xml` | `B0EBD2C0A412C4985156844B0198E0254E58EAAB74DA1EDC5AB68E4E07F9D788` |
| `gargantuan.exe` | `B66F92CC48BF2145FDED969B72835EE34587EB17C5C6DC5173E116A5E4276FF2` |
| `gargantuan_script_document_binding_tests.exe` | `638B7EFE919D470C74B7DE437C5AE1B51F55759282A85EE4F10617A3DC1D14C2` |
| Final binding test source | `11DD98C73391EC30BBB42DEB8F69FBCE5ACB94C3F2DB9EF295ACB42BECE420A8` |
| Pinned Lute executable | `F770F4E595872EDBF393158F9E5B94C3D2AE03E0A86AE09A4ED65FC60D976709` |

The executable requires its ordinary SDL3, runtime and shader closure. An
exe-plus-SDL-only export is insufficient for Play and is not a qualified bundle.

## Tests

The final worker result is `PASS`, completed at
`2026-11-03T11:40:24.5665553Z`. CTest ran with two jobs and dummy SDL video/audio:

```text
ctest --test-dir C:/Sandbox/Codex/Builds/lui-animation-e682/msvc-release
  -C Release --parallel 2 --timeout 300 --output-on-failure --no-tests=error
  -R ^gargantuan_(script_document_binding|foundation|input_action|player_runtime|prerun_bootstrap)$
```

| Suite | Result | Seconds |
| --- | --- | ---: |
| `gargantuan_script_document_binding` | PASS, 465 checks | 0.26 |
| `gargantuan_foundation` | PASS | 23.85 |
| `gargantuan_input_action` | PASS | 1.84 |
| `gargantuan_player_runtime` | PASS | 0.40 |
| `gargantuan_prerun_bootstrap` | PASS | 1.07 |

Total wall time was 23.87 seconds. This was a concurrent qualification run;
unrelated LUI work was permitted in its separate cache.

The new fixture verifies saved/native-only/nonreplicated schema ownership,
constructor and Clone identities, malformed and duplicate transactional-load
rejection, legacy read-without-rewrite, explicit save/reopen, stale live IDs,
rename/reparent/source editing, Duplicate and Undo/Redo identity, delete/recreate,
Save As lineage, cross-project rejection, strict token fields and permissions.
It also preserves an ordinary non-Script custom property named `SourceId` through
Clone.

The actual Studio property mutation returned `Ok=false`, `Error.Code=ReadOnly`
and `Property is not exposed for Studio editing by the frozen schema`. The test
requires that exact rejection and independently checks unchanged resource
identity/revision, complete project/history state and zero emitted journal
records. Direct ordinary native property mutation is separately required to
return `Unauthorized`.

Earlier attempt receipts are retained: the PowerShell 5 native-stderr wrapper
stop, a portable test-comparison compile fix, a guard refusal before staging,
and the initially incorrect expected denial category. The final correction
strengthened the state and journal assertions. These retries changed no
production source or generated metadata and did not rerun class generation.

## Bounded resolution samples

Each row records eight warmups followed by 64 individual requests. The boundary
includes in-process EditorHost request JSON, bounded authoritative tree lookup,
response encoding and test JSON decoding. It excludes pipes, Studio scheduling,
source reads, display and physical input. Allocations were not measured.

| Scripts | P50 μs | P95 μs | P99 μs | Maximum reply bytes |
| ---: | ---: | ---: | ---: | ---: |
| 2 | 66.2 | 75.9 | 81.3 | 438 |
| 32 | 73.4 | 82.7 | 93.4 | 438 |
| 128 | 93.7 | 101.6 | 113.5 | 439 |

These are concurrent smoke measurements, not isolated performance results or
an improvement comparison. Resolution remains bounded by the existing 65,536
persistence-object limit and retains no secondary identity registry.

## Evidence and continuation

Worker evidence is under
`C:/Sandbox/Codex/Artifacts/LUI-Primary-20261103/DurableBindings/Attempt04`.
The controller copy is under
`C:/Users/aiden/.codex/artifacts/lui-primary-20261103/DurableBindings/Attempt04/WorkerReceipt`.
The public wire remains version 1 with exactly `ProjectId`, `SourceId`,
`ClassSchemaId`, `ClassDefinitionVersion` and `Version`. Studio must resolve
these through Engine, fence the returned project/scope/revision against its
current session, and open the fresh live ID through its existing source-buffer
authority. Never restore a persisted ObjectId or fall back to a name/path.

`IdentityOrigin=Generated` remains provenance after Save; successful explicit
Save establishes persistence without changing that field. Native UI restoration
and the complete exported runtime closure must be qualified separately before
claiming the Studio blocker is removed.
