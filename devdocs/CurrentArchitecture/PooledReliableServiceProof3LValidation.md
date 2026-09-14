---
status: validation-evidence
owner: runtime-networking-and-runtime-host
last_verified: 2026-09-14
---

# Foundation 3L pooled-service proof completion receipt

## Source and gate scope

Target/published branch: `foundation/3l-content-availability`.
Starting published HEAD: `fffaed16e246039ac2931be989341847d48ff343`.
Registered/corrected test source: `0512aac60a7649d1c0498c423c5327aa16f6c540`.
The subsequent reconciliation commit changes documentation only; its hash and
exact-current CI checks are available from this file's history and the task
handoff. An isolated checkout preserves the original morphology edits and
`docs/public-portal`; no sibling or production networking source changes.

The [numeric proof](PooledReliableServiceProof3L.md) and
[decision](../FutureArchitecture/Foundation3LServiceCoverageDecision.md) retain
the selected N=32, G=524,288 B, 96-MiB/s envelope, 64/8/8/4-MiB/s pools,
four grants at 16 MiB/s and 50-ms feedback limit. No candidate value changed.
This receipt supplies execution evidence, not another architecture definition.

| Gate | Result |
| --- | --- |
| OPTION C MODEL PROOF | **PASS**, 33 original + nine hardening = 42/42 cases |
| PRODUCTION OPTION C IMPLEMENTATION | **NOT IMPLEMENTED** |
| PHYSICAL 32-CLIENT QUALIFICATION | **NOT MEASURED** |
| KI-006 / Foundation 3L / 3M | OPEN / B — PARTIALLY READY / BLOCKED |
| Implementation readiness | Requires matching docs/link/build checks and terminal-green required CI at the consuming commit; model PASS alone is insufficient |

## Initial failure and bounded correction

Registering the unchanged hardening header compiled successfully. The first
complete run passed 33 model and eight hardening cases; the ninth failed
`AggregateDebtInvariantIncludesTransportReserve` with
`credit/service separation or pending accounting mismatch`.

This was a **test expectation defect**, not a profile or debt-conservation
failure. `Accept` moves each group out of `pending` into `committed`. Five G
offers followed by four acceptances leave pending G and committed 4G. The
test incorrectly expected pending 5G. The correction asserts pending G **and**
pending + committed = 5G while retaining peer-five credit G, four-grant/4G
limits, aggregate funding and rejection of a fifth grant. No assertion was
relaxed, model algorithm changed or production code touched.

## Final executed results

The existing networking-contract executable/CTest passes; all **42 registered
pooled cases pass, zero fail**. These are subcases of one CTest, not 42 separate
CTest registrations. Other existing networking-contract assertions also pass.

| Obligation | Executed result |
| --- | --- |
| Debt conservation | 577,536 B created = 167,772 B verified drained + 409,764 B terminally released + zero outstanding; time/disconnect alone release none |
| Gameplay after admitted G | 32.5 ms model FIFO delay, 132.5 ms including 100-ms nonqueue allowance; accepted targets unchanged |
| Feedback | Fresh and delayed-valid 40 ms admit; stale/missing/error defer; candidate limit stays 50 ms |
| Slow-peer regrant | Existing debt remains charged; even after draining it, fresh below-floor feedback cannot grant another group |
| 32xG | p50 376 ms, p95/p99/max 502 ms from zero credit; max first grant 220.5 ms and first verified service 221 ms from credit eligibility |
| Fairness | Both tiny/large competition directions and repeated mixed rounds complete; no observed starvation in registered cases |
| Overload/recovery | Pending <=32G, committed <=4G, finite credits; recovery 226.5 ms |
| Memory | Fixed 32-peer model, no retained payload queue; MSVC object 7,816 B, distinct from historical Clang/GCC layout |
| Invalid profile | Underfunding/incompatible bounds and tested checked-arithmetic overflow reject; full-reservation mode remains distinct |

## Commands, environment and provenance

Windows x64 worker, 24 logical processors / 32 GiB, existing Visual Studio 2022
MSVC Release cache and CMake 3.31.10; four build jobs. No compiler workload
overlapped this run. The existing LNK4098 CRT-link warning remains; this is not
a warning-free build claim. Hosted CI owns the newer MSVC and Linux sanitizer
platform contracts. No worker/toolchain installation or cache purge occurred.

```powershell
$Build = 'C:/Sandbox/Codex/Builds/gargantuan/runtime-host-f1-1-baseline2-msvc-gns-vs'
$Logs = 'C:/Sandbox/Codex/Logs/gargantuan-3l-proof-completion'
$Tools = 'C:/Sandbox/Codex/Tools/cmake-3.31.10/cmake-3.31.10-windows-x86_64/bin'
& "$Tools/cmake.exe" --build $Build --config Release --parallel 4 --target gargantuan_networking_contract_tests
& "$Tools/ctest.exe" --test-dir $Build -C Release --verbose --no-tests=error --timeout 300 -R '^gargantuan_networking_contracts$' --output-junit "$Logs/final-ctest.xml"
```

Final compile exits 0 in 11.47 s; CTest exits 0. The 679 primary-engine
source/configuration inputs in `source-manifest.json` match the worker after
normalizing CRLF/LF. Five unrelated repository images are excluded from that
text comparison. The three proof/harness files also match byte-for-byte:

| File / artifact | SHA-256 |
| --- | --- |
| `tests/NetworkingContractsTests.cpp` | `FA9DAAA8DE9F0DEBA8A36D69454CC810871089909BBCBA3D7FB40D0D62957344` |
| `tests/PooledReliableServiceModelFixture.hpp` | `57083CFCAA04F7D0D6444A1EC4CD51C25F9B18A08743076C9C6625EBCB950302` |
| `tests/PooledReliableServiceProofHardening.hpp` | `72B71EF903D4504EAEBA2188372D3744EEC3DE599C5602D72DA5D550B3E0AD6A` |
| Release networking-contract executable | `E53AC99E215EB70F325E2CBAE2058FD0777E63C92107778E2ED6102022E0BBBF` |

Logs preserve `registration-build.log`, `registration-ctest.log/.xml`,
`final-build.log`, `final-ctest.log/.xml`, `source-manifest.json` and
`source-receipt.json` in the worker log directory above, copied back to the
isolated primary worktree's `build/proof-completion/`. Existing generated and
vendor caches were reused; this is focused model execution, not requalification
of the whole runtime/dependency stack or actual network service.

## Documentation and hosted validation

The source branch does not contain the separate AI-operability branch's active
checkpoint/context/verify tools. No tooling or stale checkpoint was imported.
Changed-document relative links/anchors and diff whitespace are checked, and
the existing locked Astro build is run against the final reconciliation source.
Exact final commands/results and published hashes belong in the task handoff;
an older docs build or in-progress CI is not a final-source PASS.

`Native engine CI` runs the complete registered CTest set on push, so its
Windows and Linux headless jobs include this model and hardening registration.
`GNS sanitizer CI` also runs on push; its specialized GNS matrix does not
replace the networking-contract entry. Wait for both workflows to become
terminal green at the final published commit. Documentation deployment only
runs on main push/manual dispatch; this task validates the docs build locally
and does not deploy Pages. No standalone physical/GNS qualification is invoked
locally merely for this deterministic proof.

## Native transport-feedback follow-up

The later [reliable transport feedback proof](ReliableTransportFeedbackProof3L.md)
refines only the abstract model's `verified drain` observation boundary. The
42/42 Option C service-model result above remains accepted, but production must
not interpret configured GNS rate or positive pending-byte decreases as that
verified service.

Pinned GNS source inspection establishes an exact internal distinction between
pending and sent-unacked reliable-stream ownership, retry re-entry, ACK retirement
and final reliable-message retirement. The selected production boundary therefore
requires a narrow GNS/adapter telemetry extension exposing monotonic unique
first-send stream bytes, unique ACKed stream bytes, complete payload bytes ACKed,
and retransmitted stream bytes. No wire/application ACK is required.

`tests/ReliableTransportFeedbackModelFixture.hpp` is registered in the existing
networking-contract CTest and adds 19 deterministic feedback/lifecycle cases.
It preserves the original N=32/G/96-MiB/s Option C candidate and its 50-ms
freshness limit. A bounded one-G qualification grant breaks the first-service
circularity; a previously slow drained peer may receive another qualification
grant only after a one-second per-generation cooldown. Ordinary grants still
require fresh measured serialization at the 16-MiB/s floor plus positive ACK
progress.

Decision: **B — NARROW GNS ADAPTER EXTENSION REQUIRED**. Production pooled
service remains **NOT IMPLEMENTED**, KI-006 stays **OPEN**, Foundation 3L remains
**B — PARTIALLY READY**, and no 3M work is authorized by this follow-up.
