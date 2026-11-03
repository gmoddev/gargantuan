# Studio-compatible Input Action composition — 2026-11-03

The executable consumed by the LUI development branch is based on Studio's
qualified Engine revision `eeb760edac2dea6bf3a398f69bdd320e81ba499b`, not the older
base of `feature/input-action-foundation`. This distinction is required for
schema discovery 7, prepared property batches/defaults, PreRun, animation,
replicated services and Foundation 3L compatibility.

## Exact source composition

| Input | Identity |
| --- | --- |
| Compatible baseline / PR #11 | `eeb760edac2dea6bf3a398f69bdd320e81ba499b` |
| Original persistent Input foundation | `60baf9c61437cd23fbc957531466122013aa4e70` |
| Original bounded inspection addition | `c36e281106d9540909a0557878d2d74558c722c5` |
| Portable Input test helper | `6df10c8d844eea0eef4238a8898506fcccda6b00` |
| Composed runtime source | `f05f8d77ed0830375c2c04d931e1c0904a863b32` |
| Minimum-project test correction | `3e11031aeefd31ea77c3df324ffa598a6799e980` |
| Qualified executable/test source checkpoint | `942c4f8d9d4849da5e82f08810d4f7fda228a1d8` |

Branch: `integration/lui-input-actions`. Original branches and history are
preserved. Conflicts retained both ReplicatedStorage/custom-service establishment
and the persistent InputService root, all newer EditorHost capabilities and the
baseline's existing closed key validators. Minimum projects contain five service
children/six objects. No networking/replication/serialization implementation or
Foundation 3L policy was reverted. Independent source review found no lost
contract. The older `00A859...` executable was rejected by the adapter's schema-7
probe and was never adopted by Studio.

Engine SHA256:
`FD8AEE68B4D0B393B79BC26F03A953056F271FC71BC34EA6C151B1FB9CAF0342`.
Subsequent test-only commits do not alter that executable. Current generation
used pinned Lute 1.0.0 and all 132 generated files were hash-fenced. The two
already tracked Animator outputs retain the baseline's exact bytes.

## Executed qualification

Windows x64 MSVC 19.44 Release/Ninja on DockerPC; existing GNS OFF, NodeContent
OFF and PCH OFF settings retained, two compile jobs. Source/cache are
`C:/Sandbox/Codex/Workspaces/lui-animation-e682` and
`C:/Sandbox/Codex/Builds/lui-animation-e682/msvc-release`. Baseline preimages and
generated/vendor caches were preserved. All configured CTest executable/fixture
dependencies plus Engine were built; unused ALL targets were avoided.

| Gate | Result |
| --- | --- |
| InputAction / Foundation / PlayerRuntime / PreRunBootstrap | 4/4 PASS |
| Actual Studio schema-7 Input adapter | PASS: canonical chord creation, Undo/Redo, save/reopen, Play, Ctrl+Shift+S, owned snapshot, focus cancellation, stale Stop rejection, unchanged authoring history |
| Save As / PreRun adapter | 17 assertions PASS; original synthetic fixture unchanged and child retired |
| All configured CTests | All 82 executed; 81 without observed intermittency, one unresolved SoftBody gate |
| Initial full run | 77 PASS / 5 FAIL, 205.10 s |
| Four script failures | PASS after restoring existing controlled Python/Git to process-local PATH; no source or binary changes |
| SoftBody isolated repetition | PASS, 0.36 s; initial failure retained, KI-011 remained open at this checkpoint |

This is **selected Studio/Input contract qualification**, not a clean full-Engine
gate. Do not promote/merge on an invented 82/82 result. GNS-enabled/hosted,
physical input, IME and native Studio workflows have separate qualification.

The full build exposed MSVC's raw-literal limit in an unchanged content test
fixture. Four expression boundaries divide its two Luau payloads into portable
`std::string` chunks. Concatenated UTF-8 bytes remain exactly equal:

| Payload | Bytes | Before and after SHA256 |
| --- | ---: | --- |
| Server | 12020 | `B7B7191E188A93833B849DF1451F124A24D76225A68B51FE11910EDB0C344A28` |
| Client | 45244 | `E5442D65B289B4EA36E00FCF568A5A07B06855A3A76E8EBFC58CF76E149745F8` |

No physics code, traffic, timing threshold or correctness assertion was weakened.

## Measured inspection cost

The black-box adapter measured 128 sequential latest-state requests after eight
warmups, with 2 contexts, 9 actions and 10 bindings. Round-trip P50/P95/P99 were
**1.0981 / 1.3985 / 1.5173 ms**, maximum result 5185 bytes. Client managed allocation
was 10,176,464 bytes across the sample, including measurement serialization.
These are Engine snapshot/JSON + pipes + adapter decode costs, not physical-event
evaluation, native display latency or an optimization comparison. Engine
allocation was NOT_MEASURED. Concurrent CTest benchmark output is smoke evidence;
it is not a replacement for the isolated foundation benchmark.

## Evidence and continuation

Local receipts:
`C:/Users/aiden/.codex/artifacts/lui-primary-20261103/Qualification/`:
`Engine-Integration-03`, `Engine-Integration-04`, `Engine-Integrated-Full-04`,
`Engine-Integrated-Retry-01`, `Engine-Integrated-Retry-02`,
`Adapter-Integrated-InputActions-01`, `Adapter-Integrated-PreRun-01`, and
`FixturePortability`. Worker equivalents are under
`C:/Sandbox/Codex/Artifacts/LUI-Primary-20261103/`. Full JUnit and LastTest.log
are preserved independently from repairs/retries; JUnit truncates stdout, so the
complete LastTest.log and extracted 18-row benchmark JSON remain authoritative
for smoke output. SHA manifests cover changed sources, generated files and
binaries before/after testing.

This receipt remains bound to source `942c4f8d9`; later
[KI-011 deterministic lifecycle qualification](../Validation/ki011-20261103/README.md)
corrects the fixture premise without rewriting this original result. Retain
the cache and all historical evidence, and run applicable gates for each new
source. Studio may use the selected contract-qualified
FD8 candidate on its isolated development branch, while mainline promotion and
release remain blocked. The bounded protocol is [PlayInputInspection.md](PlayInputInspection.md).
