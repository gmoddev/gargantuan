---
status: verified-test-contract-attribution
owner: networking
last_verified: 2026-09-12
---

# Profiled GameSession reliable-admission assertion attribution

## Result and revisions

**Test-contract failure; no production accounting defect established.** The
normal profile can fund this bootstrap before the first reservation. The
published contract requires bounded deferral when resources are insufficient;
it does not require every successful bootstrap to encounter insufficient credit.

Branch: `foundation/3l-content-availability`. Failing source:
`632b28ca495d6ecb0c1c6e3ba61f7f3c523fe614`. The test-only correction
`8fa3324164fc8e767227cda3b5da361c824c72f5` was already published when this
investigation fetched the branch. It removes only the mandatory nonzero-deferral
assertion and documents the separate deterministic coverage. Production sources
are identical between those revisions. This follow-up records independent
attribution and validation; it adds no production correction.

**GNS ASan/UBSan/LSan gate: PASS, including the downstream production matrix.**
**Foundation 3L: B — PARTIALLY READY. Foundation 3M remains gated.** No merge,
rate/share/reserve change, admission-policy change, lane/order/wire change,
timeout change, sanitizer weakening, or secondary-repository change.

## Reproduction and provenance

The independent worker used Ubuntu 24.04, Clang 19.1.1, GCC 14 C++ headers/runtime,
RelWithDebInfo, Ninja, and the configuration in
`.github/workflows/gns-sanitizers.yml`. Pinned vcpkg dependencies were Protobuf
3.21.8 and OpenSSL 3.0.7. Runtime environment:

```text
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer-19
SDL_VIDEODRIVER=dummy
```

Compile-command verification checked ASan/UBSan on upstream GNS, the Gargantuan
adapter, GameSession, and the fixture. Existing upstream-only function/alignment
compatibility exclusions did not appear on Gargantuan compilation commands.
No exclusion or leak-detection setting was changed.

The worker's **692 tracked source/build/test/asset inputs matched the failing
revision's Git blobs byte-for-byte**, including Linux LF line endings. Preparation
first encountered missing container tools, a copied CMake configuration that did
not load the newly supplied toolchain, and Windows archive newline conversion.
Those preparation attempts are not counted as reproductions. Dependencies were
restored through the canonical toolchain and affected LF inputs rebuilt before
the counted invocations. No trial changed the fixture profile or workload.

```text
build-gns-sanitizers/gargantuan_game_session_real_transport_tests --reliable-profile
```

| Invocation set | Count | Result |
| --- | ---: | --- |
| Unmodified failing revision | 20 | 20 exits 1, each with exactly the reported mandatory-deferral assertion; no sanitizer diagnostic |
| Temporary bounded trace on failing revision | 5 | Same assertion in all five; every observed bootstrap reservation accepted |
| Uninstrumented published correction | 20 | 20 exits 0 |
| Separate corrected profiled stage after base CTest | 1 | Exit 0 |

The failure was repeatable in this environment: **no alternating pass/fail result
was observed in the twenty baseline runs**. Its predicate nevertheless depends
on wall-clock timing, rather than a deterministic contract requirement. Do not
describe the measurements as an observed intermittent failure. The source and
trace prove that sufficient elapsed-time credit is a valid zero-deferral path.

## Measured bootstrap reservations

All five traces contain **one structural reservation before the Ready sample**:
one 3,737-byte encoded GRPL frame plus the 32-byte adapter envelope, for an exact
charge of **3,769 bytes**. This is the complete reservation/frame cost, not a
measurement of each dependency subgroup inside that frame.

R=A=8,388,608 B/s; backend ceiling=16,777,216 B/s. Structural share is 750 permille,
so peer/global refill rates are both **6,291,456 B/s**. Both burst caps are
524,288 B. Gameplay reserve is 25%, with a 262,176-byte gameplay burst; peer/global
backlog thresholds are 1,048,640 B and structural exposure limits are 786,464 B.

The accountant's first global initialization has zero credit. Connection `{1,1}`
is observed at step 3 with **zero peer credit**; the already-running global bucket
has earned credit by then. The first reservation is at step 8. Backend pending
reliable bytes, scheduler queued reliable bytes, and scheduler queued reliable
messages are all zero at that reservation's feedback snapshot.

| Trace | Peer creation time, monotonic us | Peer refill elapsed, us | Global credit at peer creation, B | Peer credit before reserve, B | Global credit before reserve, B | Peer/global credit after reserve, B |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| 1 | 483846361870 | 7411 | 18440 | 46625 | 65066 | 42856 / 61297 |
| 2 | 483852242326 | 7497 | 19025 | 47167 | 66192 | 43398 / 62423 |
| 3 | 483858089928 | 7440 | 18282 | 46808 | 65091 | 43039 / 61322 |
| 4 | 483863647043 | 7398 | 18943 | 46544 | 65487 | 42775 / 61718 |
| 5 | 483869461153 | 7395 | 19176 | 46525 | 65701 | 42756 / 61932 |

Trace 1's global refill intervals through the first quote are
`0, 1713, 1218, 1213, 1198, 1172, 1161, 2585, 82` us. Its first peer refill uses
7,411 us. Reserve rechecks allowance at the same accountant time, so both
reserve-time refill intervals are zero. The other traces' complete refill events
are retained in the evidence JSON and logs.

Every reservation and commit succeeds. In every trace:

- `CreditDeferrals`: **0 -> 0**; `SizeDeferrals`: **0 -> 0**.
- `ReservedBytes`: **0 -> 3769** at reservation.
- `AcceptedBytes`: **0 -> 3769** at commit; never incremented before commit.
- Peer and global credit each decrease by exactly 3,769 B.
- Feedback, backlog, and fairness deferrals remain zero through bootstrap.

At the Ready sample, peer-credit high-water values are
`46625, 47167, 46808, 46544, 46525` B; global-credit high-water values are
`296874, 294433, 287902, 289777, 290312` B. Global credit continues accruing while
the handshake reaches Ready; the sample does not erase earlier counters.

## Source-level explanation and denial coverage

`ReliableByteAdmission::Observe` creates a zero-initialized peer bucket and sets
its update time to the accountant's `Now`. `Advance` initializes the global bucket
at zero on the first step. `Allowance` advances global time and refills peer
credit **before** returning the bounded structural allowance. Refills use integer
fractional remainder and finite caps. No time before the accountant/peer's own
initialization earns that bucket credit: process or sanitizer startup is not a
free initial balance. Elapsed connection/bootstrap/planning work after
initialization can fund the first actual request, as measured here.

GameSession observes connections before they necessarily have READY structural
work. Its production path is `BeginStep/Observe -> ProcessPlanning -> Allowance
-> exact preparation -> Reserve -> scheduler acceptance -> Commit`. In these
traces the peer exists for roughly 7.4 ms before its first reservation. This
3,769-byte charge needs only 600 us at the unchanged structural rate. Deferral
is therefore unnecessary; it would be wrong to force it by changing the profile.

`GameSession::GetMetrics` copies the live accountant totals. Credit/size/accepted
counters accumulate for the accountant/session lifetime; they are not transient
gauges or per-generation counters. Peer credit/state is generation-scoped.
`Remove` erases peer state without clearing these totals, and no accountant
replacement/reset occurs before the fixture's Ready sample.

`Allowance` increments `CreditDeferrals` when the peer or global bucket cannot
fund its required-cost hint. Exact preparation exceeding the quoted allowance
returns `DeferredForBytes`; GameSession calls `DeferSize`, which increments
`SizeDeferrals` and preserves the scalar required-cost hint. Preparation does
not commit Known, an emitting cursor, or sequence on this path.

Not every empty allowance/reservation belongs to those two categories: missing
feedback, backlog pressure, and fairness have separate counters. Invalid IDs,
backward time, an active reservation, invalid sizes, and token exhaustion can
also fail without incrementing credit/size counters. `Reserve` alone can reject
bytes larger than its allowance without a size increment when its smaller
required-cost hint is already funded. GameSession's legal exact-size denial path
is the preceding preparation/`DeferSize` path; an unexpected failure between its
quote and synchronous reservation is terminal. None of these alternative paths
explains this successful bootstrap, whose reservation never failed.

A separate test-only deterministic program uses the same production accountant
and unchanged 8 MiB/s profile. At time zero, allowance is zero and credit
deferrals become 1. At 1,000 us, 6,291 B is available; an 8,000-B size deferral
sets size deferrals to 1 and credit deferrals to 2. At 2,000 us, 12,582 B funds
the exact 8,000-B charge, leaving 4,582 B, with counters retained after peer
removal. A separate funded-first case accepts the same charge with both deferral
counters zero. Both cases pass ASan/UBSan/LSan. Existing
`ReliableByteAdmissionFixture.hpp` also retains deterministic accumulation,
denial, exact debit/refund, fairness, bounds, and lifecycle coverage.

## Final validation and remaining gates

| Gate | Independent worker result |
| --- | --- |
| `gargantuan_real_transport` | PASS |
| `gargantuan_remote_real_transport` | PASS |
| `gargantuan_character_real_transport` | PASS |
| `gargantuan_game_session_real_transport` | PASS |
| Four-fixture CTest | 4/4, 13.03 s |
| Corrected profiled GameSession | 20/20 repeated plus separate stage PASS |
| Deterministic counter/charged-byte checks | PASS |
| Production byte-admission matrix | Executed, exit 0, 12/12 analyzer PASS |
| ASan / UBSan / LSan | No diagnostic in the counted baseline, trace, or final validation runs |

Each downstream case accepts exactly 1,450,000 structural bytes, delivers every
submitted reliable message, and completes all 60 RPC, Event and action replies.
The repository analyzer verifies effective profiles, finite credit/exposure
bounds, and the candidate profile's probe targets. Low-rate cases remain
unqualified; their slower service is not a new blocker or a changed contract.

Independent published evidence at the corrected code revision also passes:
[GNS sanitizer CI 34719712475](https://github.com/gmoddev/gargantuan/actions/runs/34719712475)
includes all four base fixtures, profiled GameSession, exclusion-scope verification,
and the downstream twelve-case matrix; its matrix also passes the local analyzer.
[Native CI 34719712455](https://github.com/gmoddev/gargantuan/actions/runs/34719712455)
is terminal success. The original
[failing CI 34715498735](https://github.com/gmoddev/gargantuan/actions/runs/34715498735)
is retained as baseline evidence.

Temporary instrumentation used a fixed 512-record memory buffer and printed at
teardown, with a separate test sample. It was removed before corrected validation.
Restored production blobs are `cab72c6459346747b24a50203edb026d56049ce3`
(`GameSession.cpp`) and `4e38e08781a488e97eb0e9ed5e0c53d10a15fba1`
(`ReliableByteAdmission.hpp`); the corrected fixture blob is
`6165d697d4ec8c1dfceae780097404b776f76145`.

Evidence and reproduction helpers are under
`build-3l3-worker/gns-attribution/` locally and
`C:\Sandbox\Codex\Artifacts\gns-attribution-632b28c` on `dockerbox`.
`attribution-summary.json` includes all baseline/corrected results and bootstrap
events; raw `instrumented-*.log` files retain the bounded capture.
`verified-configuration.json`, `baseline-source-verification.json`,
`production-matrix.log`, and `production-matrix.json` retain configuration,
source, and downstream acceptance evidence. The isolated container uses
`/workspace-engine` and `/build-engine`, four CPUs/build jobs, and 12 GiB; original
worker sources/build caches were mounted read-only. Its reusable copied build
and pinned dependency caches are retained separately from unrelated workloads.
The completed `codex-gns-attribution-632b28c` container is stopped and retained
for those caches; no attribution test/build process remains running.

This closes only the formerly blocked GNS sanitizer progression. Full gameplay
burst/request-path and client qualification, overload/recovery, production
journal-retention margin, real 500-peer byte-profile convergence, fresh required
physics/scale qualification, and current-source security review remain distinct
Foundation 3L gates in the main validation ledger. No new production fix implies
a new MSVC or late-handoff regression requirement here; their existing evidence
is not relabeled as newly rerun by this attribution task.
