---
status: validation-evidence
owner: runtime-networking-and-runtime-host
last_verified: 2026-09-14
---

# Foundation 3L reliable transport feedback proof validation

## Retirement attribution qualification (2026-09-14)

Implementation: `62c663335cfd36498869fde3d8e85406f102e281`. Corrected executable
source: `a5a182ff94ae7373e0cdcdcc6232b434696926fe`, published on
`foundation/3l-content-availability`. The pin remains
`2cb93a06350bb065db53abdb0d87cf297e0bfd34`. No production pooled admission,
wire, ordering, grant/profile, or dependency change was made.

**READY FOR POOLED-SERVICE INTEGRATION. Local and required hosted qualification
pass on the corrected executable source.** The
[contract](ReliableTransportFeedbackProof3L.md#exact-retirement-attribution)
and [integration stop](PooledReliableServiceIntegration3L.md) supersede the
earlier conclusion that aggregate connection retirement alone suffices for
structural debt. The older evidence below remains valid in its recorded scope.

### Failure classification and correction

Both original exact-source workflows failed during compilation, before tests:
[Native engine CI 34829807610](https://github.com/gmoddev/gargantuan/actions/runs/34829807610)
and [GNS sanitizer CI 34829807689](https://github.com/gmoddev/gargantuan/actions/runs/34829807689).
`ReliableServiceFeedback.hpp` had moved the global public GNS interface forward
declaration into `SteamNetworkingSocketsLib` and removed the native-read and
close-capture declarations. MSVC reported C3861/C2027; Clang reported undeclared
native read and incomplete interface use. Restoring the original declarations
is a source compilation correction, not a protocol or architecture change.

The focused Clang build then exposed a fixture portability defect:
`std::int64_t*` is not GNS's `int64*` on Linux. The test now uses the API's exact
output-pointer type. No attribution behavior was changed to make tests pass.
There was no sanitizer finding or dependency/infrastructure failure at these
two failure points.

### Final executable-source evidence

| Evidence | MSVC Release | Clang 19 ASan/UBSan/LSan |
| --- | --- | --- |
| Feedback fixture, including mixed matrix and real native identity | **9/9 PASS** | **9/9 PASS** |
| Deterministic mixed-retirement attribution matrix | **12/12 PASS** | **12/12 PASS** |
| Affected CTests: native fairness/retirement, networking contracts, real transport | **3/3 PASS**, 14.85 s | **3/3 PASS**, 14.85 s |
| Accepted feedback model | **19/19 PASS** | **19/19 PASS** |
| Unchanged Option C model/hardening | **42/42 PASS** | **42/42 PASS** |

The mixed matrix covers both opposite retirement orders, alternating structural/
gameplay/control messages, successive grants separated by gameplay, structural
and gameplay retransmission, delayed/duplicate ACK, multi-segment final retirement,
co-processed message isolation, terminal mixed outstanding debt and fresh-sender
isolation. It invokes the native counter implementation deterministically;
co-packetization and reconnect in that matrix are modeled ownership scenarios,
not additional physical packet/reconnect measurements.

The real-GNS `RealGnsAttributedRetirement` fixture uses a network-loopback native
socket pair. A sender-local token binds to the message number returned by the
real send API; final retirement reports exactly 49,152 payload bytes once, and
later unattributed reliable traffic cannot overwrite that receipt. Existing real
loss/retry, delayed/duplicate traffic and adapter teardown/reconnect cases remain
green. The native reference-count fixture separately proves that partial ACKs
and retry references cannot trigger premature message retirement.

Checked overflow covers first-send/retry/payload arithmetic, impossible ACK
progress, negative ranges, token reuse, concurrent attributed obligations,
sequence exhaustion and nested attribution scope rejection. Purge and verified
retirement remain distinct. The deterministic attribution lifecycle proof and
real adapter generation/reset proof are separate layers; production propagation
of exact receipts through the adapter remains part of the next integration task.

### Commands and worker provenance

Worker: trusted `dockerbox`. MSVC reused
`C:\Sandbox\Codex\Builds\gargantuan\runtime-host-f1-1-baseline2-msvc-gns-vs`
with sources in `C:\Sandbox\Codex\Workspaces\gargantuan-runtime-host-f1-1\engine`.
Clang reused `/build-engine` and `/workspace-engine` in the task-owned
`codex-gargantuan-native-feedback` container (four CPUs, 12 GiB).
Both builds used four jobs; dependencies and incremental caches were retained.
Nine native integration/adapter/build/test files match the local executable
source by SHA-256 after CRLF normalization; all six transferred files also match
byte-for-byte. The final container stopped with exit 0.

```powershell
$TaskBuild = 'C:\Sandbox\Codex\Builds\gargantuan\runtime-host-f1-1-baseline2-msvc-gns-vs'
$TaskTools = 'C:\Sandbox\Codex\Tools\cmake-3.31.10\cmake-3.31.10-windows-x86_64\bin'
& "$TaskTools\cmake.exe" --build $TaskBuild --config Release --parallel 4 --target gargantuan_real_transport_tests gargantuan_networking_contract_tests gargantuan_gns_service_fairness_tests
& "$TaskBuild\Release\gargantuan_real_transport_tests.exe" --reliable-feedback
& "$TaskBuild\Release\gargantuan_networking_contract_tests.exe"
& "$TaskTools\ctest.exe" --test-dir $TaskBuild -C Release -R '^(gargantuan_real_transport|gargantuan_networking_contracts|gargantuan_gns_service_fairness)$' --parallel 1 --timeout 300 --output-on-failure --no-tests=error
```

Linux commands:

```sh
cmake --build /build-engine --parallel 4 --target gargantuan_real_transport_tests gargantuan_networking_contract_tests gargantuan_gns_service_fairness_tests
/build-engine/gargantuan_real_transport_tests --reliable-feedback
/build-engine/gargantuan_networking_contract_tests
ctest --test-dir /build-engine -R '^(gargantuan_real_transport|gargantuan_networking_contracts|gargantuan_gns_service_fairness)$' --parallel 1 --timeout 300 --output-on-failure --no-tests=error
```

Environment: `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`,
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`,
`ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer-19`, `SDL_VIDEODRIVER=dummy`.
Logs are `attribution-{build,msvc-feedback,msvc-models,msvc-ctest,linux-build,linux-feedback,linux-models,linux-ctest}.log`
under `C:\Sandbox\Codex\Logs\gargantuan-3l-native-feedback`, copied to the
isolated worktree's ignored `build/attribution-closure/` directory.

### Hosted CI and sanitizer scope

Both corrective runs target exact executable source `a5a182ff94ae7373e0cdcdcc6232b434696926fe`:

| Workflow | Result |
| --- | --- |
| [Native engine CI 34831487114](https://github.com/gmoddev/gargantuan/actions/runs/34831487114) | **Terminal PASS**: MSVC **58/58**, 60.04 s; Linux Clang ASan/UBSan/LSan **50/50**, 283.26 s |
| [GNS sanitizer CI 34831486990](https://github.com/gmoddev/gargantuan/actions/runs/34831486990) | **Terminal PASS**: **5/5** transport CTests, 15.48 s, and full established workflow scope |

The required GNS workflow includes its five transport CTests, repeated incremental
build, profiled GameSession, production-admission matrix, ordinary reliable
workload, 32-peer workload and 32-peer structural workload. These remain the
existing localhost workflow scope, not physical pooled-service qualification.
The workflow checks full instrumentation of owned counters, snapshot bridge and
adapter. Existing GNS-only function/alignment exclusions must not leak into them.
That scope check passed. No sanitizer finding occurred. Complete hosted logs and
terminal JSON are retained beside the focused worker receipts as `ci-native.log`,
`ci-gns.log` and `ci-<run-id>.json`. This closure changes documentation only;
`a5a182ff94ae7373e0cdcdcc6232b434696926fe` remains the qualified native/test/build source.

### Resource measurements and limitations

The existing size fixture reports **88 B native counter state**, including
**48 B** for six attribution scalars: 1,536 B at 32 sender states or 196,608 B
at 4,096 states. These are scalar multiplication examples, not an application
memory ceiling. One active attribution and one last receipt per native sender
are the hard cardinality limits. No packet/message map or allocation is added.
Thread-local submission state adds one `uint64_t` per participating thread.

The unchanged aggregate adapter value remains **72 B**, its optional terminal
slot **80 B**, and vector control object **24 B** on tested x64 builds. It does
not yet store exact attributed terminal receipts. Native snapshots are temporary
copies. Snapshot mean over 10,000 idle samples: **241 ns MSVC**, **469 ns Clang
sanitizers**. End-to-end throughput, worst-case contention and allocator/RSS delta
are **not measured**; these microbenchmarks are not throughput guarantees.

Documentation validation: Node 24.19.0 with the existing locked dependency cache
builds **19 pages**; all **60 relative link/anchor checks** across the
five changed documents pass, as does `git diff --check`. No new instrumentation
was created for memory/size measurements.

No tracked active 3L checkpoint exists at this branch revision; this receipt and
the canonical 3L.3 ledger own the checkpoint. Production `POOLED_SERVICE` remains
**NOT IMPLEMENTED**, physical 32-client pooled qualification **NOT MEASURED**,
KI-006 **OPEN**, Foundation 3L **B — PARTIALLY READY**, and 3M **BLOCKED / NOT STARTED**.
The retirement-attribution blocker is closed at the native boundary. The exact
next task is to resume production `POOLED_SERVICE` admission integration using
exact attributed structural retirement. Production integration and physical
qualification remain separate uncompleted gates.

## Native implementation checkpoint (2026-09-14)

Starting source: `009fc4f9b87faa473a463965ae75925517cda4dd`, branch
`foundation/3l-content-availability`. The GNS pin remains
`2cb93a06350bb065db53abdb0d87cf297e0bfd34`. Only the primary repository's native
feedback integration, tests and documentation change. `ReliableByteAdmission`,
`GameSession`, pooled policy and wire behavior are unchanged.

Implementation commits:

- native instrumentation: `2e15c66573b2b169ae89e62a767697e23cb7da0d`;
- private wrapper and tests: `0f64d562dcdc7bbe5f6dbe1181570b38d727bd8e`;
- build dependency correction: `ac7a27d5bf2c463796e7dd5db2f6435d00dd3fc9`;
- locked observation timestamp: `4f8da450ad8ed6bc34cd1ef5dc4bd9c995f1eb13`.

The [contract](ReliableTransportFeedbackProof3L.md#implemented-private-boundary)
records exact native hook locations and ownership. Private
`src/network/ReliableServiceFeedback.hpp` supplies all accepted fields, generation
identity and a monotonic observation timestamp from one locked native snapshot.
No per-packet allocation or history is added. A terminal sample is retained only
until its adapter slot is reused; old identity then fails lookup. Overflow is
sticky invalid/unavailable and cannot manufacture service.

The final timestamp correction captures steady-clock time in the owned native
copy while the existing connection lock protects the counters and queue values.
The adapter preserves that time, so a pause before returning the sample cannot
make old service appear fresh. The native-copy fixture checks the captured time
against the surrounding clock interval and preserves sticky invalidity.

### Real transport and native fixture coverage

`gargantuan_real_transport_tests --reliable-feedback` runs seven cases and the
ordinary real-transport CTest runs them too. The private GNS fairness executable
also exercises the exact `RemoveRefCountReliableSegment` retirement function.

| Case | Required evidence |
| --- | --- |
| First send/message boundaries | 97 B, 64 KiB, maximum 524,256 B application payload, then three queued 1,024/1,025/1,026 B messages; 593,156 complete-message payload bytes ACK-retired, 593,171 unique stream bytes, no no-loss retry |
| Repeated retransmission | 100% receive loss forces repeated physical retries; zero ACK progress during loss; recovery ACKs each unique range and retires payload once |
| Delayed ACK | 200-ms receive lag with configured 16 MiB/s; positive outstanding state and no early payload retirement, then complete drain |
| Duplicate traffic | 100% receive duplication; no additional unique ACK or payload retirement after completion, one application delivery |
| Teardown/reconnect | Outstanding data purged without ACK credit; old generation terminal, reused slot has new generation/zero counters and rejects old identity |
| Checked exhaustion | Actual native counter type seeded near `UINT64_MAX`; no wrap, sticky invalid; duplicate ACK idempotence and impossible ACK/negative range rejection |
| Snapshot overhead | 10,000 coherent observations on an idle connected pair, monotonic timestamps and counters |
| Native partial/final/duplicate retirement | Partial segment retirement and an ACKed retry reference do not retire payload; final reference retires exactly 129 B from a 132 B stream, once; purge retains that exact cumulative value |

### Model correspondence

The real first-send/drain case matches `UniqueFirstSendDrain`. Injected repeated
loss/recovery matches `RepeatedRetransmissions`, `AckAfterRetransmission` and
`LogicalDebtPhysicalCostSeparated`: first-send and unique ACK stay logical, while
retries accumulate independently. Delayed/duplicate traffic matches `DelayedAck`
and `DuplicateAck`; native segment-reference tests refine the model's complete-
message retirement boundary. Teardown/reuse matches `ConnectionTeardown` and
`ReconnectNewGeneration`. Counter invalidation corresponds to conservative
`CounterResetWrap`/`TransportFailure`. Qualification/grant behavior remains model
coverage only; it is not consumed by production admission.

### Validation and overhead

MSVC Release on trusted `dockerbox` passes the seven feedback cases, native
retirement/fairness test and affected networking contracts. The registered model
rerun passes **19/19 feedback** and **42/42 Option C** (33 + 9 hardening) cases.
The three affected CTests pass in **14.77 s**. Final Linux Clang 19
ASan/UBSan/LSan validation passes all seven feedback cases and **6/6 CTests in
15.47 s**, including native retirement, networking contracts, real transport,
Remote, Character and GameSession transport. No sanitizer finding occurred.

Measured x64 layout: **40 B native counters/state**, **72 B feedback value**, and
**80 B optional terminal snapshot per allocated adapter slot**, plus one vector
control object per adapter (24 B on tested x64 platforms), existing vector spare
capacity and a pointer-sized thread-local close capture. Snapshot stack copies
are temporary. There is no unbounded per-message/packet telemetry cardinality.
Hot paths add a fixed checked increment at an existing transition, with no heap
allocation, trace or history. End-to-end throughput delta is **not measured**.
Final MSVC snapshot mean is **230 ns** over 10,000 samples on the trusted worker.
Final Linux sanitizer snapshot mean is **452 ns** over 10,000 samples. Both
platforms observe 49,257 retransmitted stream bytes in the forced-retry fixture,
without duplicate unique first-send or payload retirement. These idle-pair
microbenchmarks are observations, not a
worst-case lock-contention or per-packet throughput guarantee.

Owned arithmetic and native bridge compile separately under full ASan/UBSan;
CI checks both commands and the adapter for exclusion leakage. Existing GNS-only
function/alignment exclusions are unchanged. LSan is enabled through ASan's leak
detection. The existing GNS workflow scope remains required, including its
already established gameplay/overload fixtures; no physical pooled qualification
is run.

The full existing GNS sanitizer scope passed during implementation: six CTests,
profiled GameSession, production-admission capacity matrix, ordinary reliable
workload, 32-peer workload and 32-peer structural workload. The final native
snapshot/build refinement was then rebuilt and revalidated with the seven
feedback fixtures, six CTests and 19/42 models above. Exact-source hosted GNS CI
replays the complete workflow scope after publication.

Incremental Ninja validation exposed a generated-header cycle in an intermediate
bridge object that included private GNS headers. The final object accepts plain
values from the connection-locked native ownership hook and includes only the
public sockets interface. Checked arithmetic, copying and close-capture state
remain fully instrumented. Reconfigure plus a second incremental build passes
with `ninja: no work to do`; CI now repeats the build to catch discovered-header
dependency regressions. The exact pinned-source patch also passes repeat
application without unknown changes or timestamp churn.

Documentation: locked dependency reuse with Node 24.19.0/Astro passes **19 pages
in 1.72 s**;
the three implementation/validation documents pass **28 relative link/anchor
checks**, and `git diff --check` passes. Temporary build artifacts stay untracked.

Worker receipts live under
`C:\Sandbox\Codex\Logs\gargantuan-3l-native-feedback`. Cached worker validation
matches all **13 committed implementation/test files by SHA-256** with zero
mismatches; logs/XML are copied to the isolated worktree's ignored
`build/native-feedback/` directory. Cached worker validation
is followed by exact published-source Native engine CI and GNS sanitizer CI;
terminal results and commit identities are recorded in the publication handoff.
An older green workflow does not qualify a later source revision.

Local native feedback is **READY FOR POOLED-SERVICE INTEGRATION**, subject to
terminal-green Native engine CI and GNS sanitizer CI at the published checkpoint.

Production pooled service remains **NOT IMPLEMENTED**, KI-006 **OPEN**,
Foundation 3L **B — PARTIALLY READY**, Foundation 3M **BLOCKED / NOT STARTED**.
Once native feedback validation and exact-source CI are green, the next task is
a separately authorized implementation of accepted Option C `POOLED_SERVICE`
admission consuming this feedback, followed by its own real-GNS qualification.

## Historical architecture/proof checkpoint

The sections below record the pre-implementation proof at `e2912895` and its
follow-up `009fc4f9`. Their NOT IMPLEMENTED and pending-CI labels are historical;
the native implementation checkpoint above owns the current feedback status.

### Source and scope

Starting published source: `e29128950d51d669057129c5408dd2ea0c558a10` on
`foundation/3l-content-availability`. The accepted 42-case Option C pooled-service
model is treated as PASS. This task changes only test/reference-model and
architecture/validation documentation. Production `ReliableServiceProfile`,
`ReliableByteAdmission`, `GameSession`, scheduler, GNS behavior, wire protocol and
reliable ordering remain unchanged.

Implementation decision: **B — NARROW GNS ADAPTER EXTENSION REQUIRED**.
Production pooled service remains **NOT IMPLEMENTED**.

## Source-level GNS probe

Pinned GameNetworkingSockets revision:
`2cb93a06350bb065db53abdb0d87cf297e0bfd34`.

Direct inspection of pinned `steamnetworkingsockets_snp.cpp/.h` establishes:

- unique reliable stream ranges exist in pending, in-flight/sent-unacked, retry,
  or Acked state;
- first send moves a range from pending to sent-unacked;
- loss/NACK moves that same range from sent-unacked back to pending retry;
- retransmission moves it back to sent-unacked;
- ACK removes it from whichever outstanding state owns it and marks it Acked;
- the reliable message holds `m_cbHdr` and a sent-segment reference count;
- after the final Acked segment reference retires, the reliable message is
  unlinked/released;
- shutdown purges sender state and zeros pending/unacked counters.

Therefore pending alone is nonmonotonic and cannot be integrated as delivered
bytes. Pending + sent-unacked is a truthful instantaneous unique reliable-stream
outstanding quantity, but GNS private reliable headers mean that stream quantity
cannot exactly retire Gargantuan complete-message debt without a narrow message-
retirement signal.

Current upstream master was inspected at
`a424b7db649438acafb60c99cae6667587c42732`. It retains the same internal sender
state/message-retirement design; no sufficient public cumulative application-
payload ACK statistic was found. A dependency upgrade is not selected.

## Reference model execution

`tests/ReliableTransportFeedbackModelFixture.hpp` is deterministic, bounded,
network-free and registered in `gargantuan_networking_contract_tests` beside the
accepted Option C model/hardening tests.

The standalone development form was compiled as C++23 with warnings enabled and
passes **19/19** cases:

1. `UniqueFirstSendDrain`
2. `OneRetransmission`
3. `RepeatedRetransmissions`
4. `DelayedAck`
5. `DuplicateAck`
6. `AckAfterRetransmission`
7. `QueueBytesReenterPending`
8. `PeerDrainsAtFloor`
9. `PeerDrainsBelowFloor`
10. `IdlePeer`
11. `FirstGrantBootstrap`
12. `DrainedSlowPeerRequalification`
13. `StaleFeedback`
14. `MissingFeedback`
15. `ConnectionTeardown`
16. `ReconnectNewGeneration`
17. `CounterResetWrap`
18. `TransportFailure`
19. `LogicalDebtPhysicalCostSeparated`

The repeated-retransmission cases prove that physical bytes may exceed logical
created bytes while verified logical drain never exceeds the one admitted
payload. Terminal release remains separate from ACKed drain.

## Proof mapping back to Option C

The native feedback refinement does not change N=32, G=524,288 B, the 96 MiB/s
usable model envelope, 64 MiB/s structural pool, 8 MiB/s gameplay reserve,
four structural grants, 16 MiB/s active-grant floor, or 50-ms freshness bound.
It replaces only the abstract meaning of `verified service` with implementable
native observations:

- exact logical debt retirement: delta `ReliablePayloadBytesAcked`;
- sender queue-service qualification: delta `UniqueReliableStreamBytesFirstSent`
  at >=16 MiB/s over a fresh <=50-ms sample window;
- remote transport progress: positive delta `UniqueReliableStreamBytesAcked`;
- physical retransmission cost: delta `ReliableStreamBytesRetransmitted`;
- queue occupancy: current pending + sent-unacked reliable stream bytes;
- terminal reconciliation: explicit generation teardown only.

First service uses one bounded qualification grant. A drained peer that previously
failed the floor cannot regain ordinary-grant eligibility merely because debt
reaches zero; it may obtain a new qualification grant only after a one-second
per-generation cooldown. These grants consume the same existing grant/debt
bounds, so the feedback mechanism does not introduce an unbounded probe channel.

## Validation disposition

| Gate | Result |
| --- | --- |
| Option C abstract service model | **PASS**, retained 42/42 evidence |
| Transport feedback reference model | **PASS**, standalone 19/19 |
| Pinned GNS source-level semantics probe | **PASS**, exact lifecycle identified |
| Current upstream comparison | **B**, same internal capability; no sufficient public signal |
| Registered networking-contract CTest at final source | hosted CI pending/record separately |
| Real GNS implementation of new counters | **NOT IMPLEMENTED** |
| Real-GNS loss/retransmission counter probe after extension | **NOT MEASURED** |
| Physical 32-client pooled qualification | **NOT MEASURED** |
| KI-006 | **OPEN** |
| Foundation 3L | **B — PARTIALLY READY** |
| Foundation 3M | **BLOCKED / NOT STARTED** |

## Exact next task

Implement only the narrow GNS telemetry boundary first. Add checked sender
counters at existing first-send, ACK, complete-message retirement and retry-send
transitions, expose them through one private lock-safe Gargantuan bridge, wrap the
sample with generation-safe `ConnectionId` and monotonic observation time, and
prove the counters under real loss/retransmission/delayed/duplicate ACK plus
teardown/reset on Windows Release and Linux sanitizers.

Do **not** connect the counters to production pooled admission in that telemetry
task. Once the native signal itself is green, a subsequent separately authorized
task may implement `POOLED_SERVICE` admission against this contract and re-run
Option C plus actual GNS qualification.
