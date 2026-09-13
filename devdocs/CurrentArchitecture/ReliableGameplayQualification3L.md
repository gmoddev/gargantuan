---
status: partial-qualification
owner: runtime-networking
last_verified: 2026-09-13
---

# Foundation 3L reliable gameplay qualification

**B — FOUNDATION 3L PARTIALLY READY; no 3M.** The
[engine-default workload](ReliableGameplayWorkloadContract3L.md) replaces the
missing-product-number stop. Qualification applies to the measured cases below,
not every legal message or every workload satisfying the selected upper bounds.
Independent current-source security, client/scale and final validation remain
separate gates.

## Source and execution

Work begins from published `b866a89e741b4b4df3419c74856d293b650bf475` on
`foundation/3l-content-availability`. Local, upstream and remote agreed; Native
CI `34734069657` and GNS sanitizer CI `34734069623` were terminal success before
changes. Earlier approved documentation commits were already published.

The trusted Windows `dockerbox` runs MSVC Release and a bounded four-CPU,
12-GiB Linux Clang 19 Docker worker with ASan, UBSan and leak detection. A
normalized source-hash manifest compares 558 native/runtime/test/CMake files;
two stale worker-cache files were reconciled before final admission validation.
No sibling repository is modified. Official Node tests use its existing worker
checkout read-only. Raw development measurements are retained under
`build-3l3-worker/evidence`; worker artifacts are under
`C:\Sandbox\Codex\Artifacts\reliable-workload-20260913`.

## Single-peer GameSession/GNS results

All qualified cases use actual bootstrap, materialized Remote identities,
production request dispatch and reply/echo payload comparison, and matched
Luau action results. Gates remain RPC p95/p99/max 150/250/500 ms and Event echo
RTT/action-result max 250 ms. Each row is a fixed 480-opportunity phase.

| Case | MSVC RPC p95 / p99 / max, ms | Linux sanitizer RPC p95 / p99 / max, ms | Worst Event / action max across both, ms |
| --- | --- | --- | --- |
| Small, 60 RPCs | 37.43 / 37.83 / 37.83 | 34.57 / 35.77 / 35.77 | 37.82 / 36.73 |
| Upper 16-KiB frame, six RPCs | 91.62 / 91.62 / 91.62 | 85.09 / 85.09 / 85.09 | 90.42 / 90.51 |
| Four concurrent RPCs, 48 total | 73.75 / 74.68 / 74.68 | 68.17 / 68.52 / 68.52 | 92.48 / 90.18 |
| Mixed structural streaming, 48 RPCs | 73.61 / 74.29 / 74.29 | 69.90 / 70.46 / 70.46 | 97.73 / 95.81 |
| Ordinary recovery, 60 RPCs | 36.75 / 37.71 / 37.71 | 34.56 / 34.89 / 34.89 | 37.70 / 36.80 |

All these cases complete without RPC terminal errors, rejected calls, payload
mismatch or missing Event/action completion. Eight actions complete in each
qualified phase. The six upper-size native samples are a functional boundary
check, not a statistically strong tail estimate; the official upper runs below
retain 100 RPCs per provider.

The mixed case admits 44,612 structural bytes, creates 120 anchored Parts and
produces 1,440 journal records. Its final client state converges. The ordinary
Remote offered rate is approximately 21–25 kB/s in the upper/burst/mixed cases. These
counters include the adapter envelope but exclude engine control/action fanout;
they do not prove complete sliding-window workload accounting. Event echo RTT
is measured separately from the official ACK-to-ACK service-gap probe.

## Single-peer overload and recovery

| Case | MSVC / Linux duration, s | Worst RPC / Event / action max, ms | MSVC / Linux drain and convergence, ms |
| --- | --- | --- | --- |
| Gameplay: 16 concurrent upper-size calls | 8.63 / 8.08 | 1064.71 / 789.60 / 1063.42 | 1064.84 / 1056.13 |
| Structural: 16 x 24-KiB name mutations/opportunity | 33.61 / 56.32 | 623.96 / 623.95 / 592.62 | 244.16 / 889.13 |
| Mixed structural and gameplay | 36.65 / 58.52 | 1600.94 / 612.38 / 1539.51 | 1087.74 / 1091.64 |

There are zero RPC terminal errors or payload mismatches in these runs. Ordinary
latency gates are deliberately not applied to overload. All accepted probes
finish and final structural values converge inside the unchanged 20-second
deadline; the subsequent ordinary phase passes the original gates.

Gameplay overload admits 1,937,088 Remote bytes; mixed admits 3,611,520.
Each structural-heavy phase admits approximately 189.06 MB of complete structural
messages. Host pacing reduces actual structural throughput below the nominal
60-Hz offered rate: these runs exercise actual credit deferrals, but do not prove
sustained application delivery above 6 MiB/s. The separate production-admission
matrix exercises service/backlog limits independently of this CPU-heavy scene.

Observed client reliable backlog is at most 262,705 B; server backlog is at most
776,634 B. Peer and global credit remain at or below the unchanged 524,288-B
caps. Planning stays within 65,536 work and structural selection within 8,192.
MSVC sampled whole-process RSS peaks at 458,158,080 B for the eight-phase run;
recovery-only peak RSS and Linux peak RSS are **not measured**. No buffer or
journal capacity is increased.

## Journal ownership and client service

The actual raw-history readers are the catalog cursor and each live peer's
journal cursor. Revision/publication stamps that do not reread raw entries are
not counted as retention owners. Single-peer structural and mixed overload each
produce 8,064 records, at 137.80–239.91 records/s over the measured whole phases.
The retained ring reaches its unchanged 16,384-entry capacity, but the worst
live requirement is only 174 entries: **minimum margin 16,210/16,384**, owned by
peer slot 1 at sequence 17,839 in the Linux mixed run. The oldest observed
required-entry age reaches 1,252.38 ms. That age starts at first post-step
observation and can underestimate true retention by one service interval.

Every single-peer phase ends with zero journal backlog and zero pending
enter/leave work. The ordinary recovery phase has no old required history;
the output reports `journal_owner=none` when no old entry remains required.
Transport deferral temporarily holds live peer history and releases it on
recovery. These observations replace the historical 170-entry startup-margin
claim only for this dedicated-server scope. Concurrent EditorHost consumers,
broader aggregate journal production and a worst-case production guarantee are
**not measured**.

Structural convergence means the expected replicated value exists in the client
DataModel. Cumulative decode/application counters separate client work: after
the final recovery they are about 2.717/4.780 s on MSVC and 3.107/12.654 s under
sanitizers. They are totals over the run, not per-object latency or frame budgets.
Rendered visibility and exact commit-to-observer correlation are **not measured**.
The client whole-world preflight concern remains open.

## Official Local and Node

Both providers pass the near-max 512-object, 1,048,197-byte content fixture with
eight load/evict/reacquire cycles and 100 RPCs per run. The upper mode sends exact
16,384-B request and response frames and waits 1.25 seconds between completions;
the small mode retains the existing probe behavior.

| Provider / mode | RPC p95 / p99 / max, ms | Timeout / error |
| --- | --- | --- |
| Local small | 40.466 / 60.985 / 67.313 | 0 / 0 |
| Node small | 39.933 / 59.268 / 73.164 | 0 / 0 |
| Local upper | 84.874 / 86.351 / 100.004 | 0 / 0 |
| Node upper | 84.740 / 86.153 / 100.356 | 0 / 0 |

Upper runs retain the existing small Event/action probes; upper Event frames are
covered by the native fixture, not claimed as official-provider measurements.
Official Event max RTT / ACK gap is 67.413/127.536 ms for small Local and
59.374/83.642 ms for small Node; upper-RPC Local is 80.490/146.079 ms and
Node is 68.500/129.313 ms. Official action acceptance/completion is retained,
but a separate official action-result latency distribution is **not measured**.
Packaged hosts were rebuilt after the final private diagnostic change and both
small cases rerun. Upper-RPC evidence predates only that terminal observation
addition; its gameplay policy and official payload fixture are unchanged.
First-visible and reload-visible timeline fields describe client application
observations, not a rendered frame guarantee. Local/Node upper first visibility
is approximately 357/343 ms and reload visibility 1823/1808 ms from fixture start.

## Demonstrated defect

Sustained action testing exposed accumulated negative vertical velocity while
grounded: velocity reached roughly -735 to -958 even though the Character stood
on its floor. The compact Character state could no longer encode that velocity;
state publication stopped and prediction history overflowed, rejecting later
actions. The shared default Luau locomotion policy now clears downward velocity
when grounded before applying gravity. Positive jump and airborne gravity remain
unchanged. A 600-step grounded regression plus jump/airborne checks passes in
GameSessionTests. No native physics, wire format or velocity encoding is changed.

## Aggregate, validation and remaining scope

`--reliable-workload-32` retains the RPC-only aggregate profile: 32 connected,
eight active with four simultaneous 3-KiB requests each, then all 32 active with
16 outstanding per peer, followed by ordinary recovery. This deliberately
exceeds qualified concurrency and aggregate demand without claiming simultaneous
maximum bursts from every peer as ordinary service.

Final MSVC and Linux canonical aggregate runs each complete 48 RPCs per active
peer in both ordinary phases and 208 per peer across all 32 during overload,
with zero errors and no unserved peer. Worst ordinary RPC maxima are
76.54/81.84 ms and overload maxima 263.93/317.51 ms (MSVC/Linux). MSVC whole-run
RSS peaks at 130,977,792 B. This proves service in the RPC-only workload, not
structural admission fairness under the separate failing combined stress.

The deterministic production accountant serves all 200/500 eligible peers
within 283/583 ms, with 17,936/44,336 logical state bytes, exact full-group byte
accounting and credit at or below 524,288 B. These are controlled admission
service-gap bounds, not physical multi-client latency measurements.

The separate `--reliable-workload-32-structural` mode preserves the additional
combined stress unchanged: six anchored Part names of 24 KiB mutate every one
of 480 overload opportunities. Both initial uninstrumented MSVC and Linux runs
fail after the qualified phase. A temporary terminal-only native diagnostic
captures `Pkt number lurch by 32578; 06a0->85e2` from pinned GNS
`2cb93a06350bb065db53abdb0d87cf297e0bfd34`. Its packet-number guard rejects gaps
greater than `0x4000` in `steamnetworkingsockets_connections.cpp`; the ultimate
cause of the gap is not established. The server reports zero protocol,
structural-backlog-limit and journal-lag failures. Accepted RPCs terminate with
errors and structural convergence is not established.

The final Linux terminal-only reproducer also exits 1, reporting packet-number
gaps 28,144 and 25,986. It has no ASan/UBSan/LSan diagnostic. This confirms the
same rejecting guard on both platforms while keeping the backend failure
distinct from the corrected fixture lifetime defect.

Per-message backend tracing made one Linux run pass, with slower service;
that timing-sensitive result is not used to close the failure. The retained
diagnostic callback observes terminal close reasons only, through the existing
private borrowed GNS seam. It does not poll per-message backend counters.
The fixture also fixes its own stale RemoteManager pointer use after disconnect
and stops sessions before callback captures expire. The original ASan failure
was in the fixture, not proof of a production manager lifetime defect.

This is [KI-008](../../KNOWN_ISSUES.md#ki-008-gns-packet-sequence-close-during-aggregate-structural-overload)
and an explicit stop at the backend wire-safety boundary. The known failing
combined diagnostic is not a required green CI step. CI covers the separate
single-peer matrix and RPC-only aggregate modes; its green result must not be
presented as combined aggregate overload closure.

Aggregate qualification and final publication results are recorded in the
[current ledger](ContentAvailabilityFoundation3L_3Validation.md). The 200/500-peer
deterministic admission cases measure finite rotating full-group service, not
200/500 complete game clients or physical network capacity. Event/action fanout,
maximum admission gap in a fully instrumented aggregate game, worst-case journal
retention, independent security and complete client/scale qualification remain
separate obligations. Foundation 3L must not be marked Ready from this subset.

## Validation receipt

Final canonical MSVC validation exits 0: **11/11** affected CTests, the full
single-peer workload and 32-peer RPC-only mode, and **12/12** production-admission
cases. Linux Clang 19 with ASan/UBSan/LSan exits 0: **9/9** affected CTests, both
canonical workload modes, and **12/12** admission cases. The canonical analyzer
confirms complete delivery and unchanged profile/credit/probe gates on both.
The separately recorded KI-008 diagnostic exits 1; it is not counted among those
passes. No sanitizer suppression is added; the existing GNS-only compatibility
exclusions remain confined to the pinned third-party target.

The affected foundation/replication/GameSession suites retain KI-007,
dependency/bootstrap failure, late-handoff, disconnect/reacquisition and stale
generation coverage. The byte accountant tests retain failed/rolled-back
reservations, backlog feedback and peer removal during deferral. These are
controlled lifecycle regressions, not claims that every failure was injected
into each real overload phase. Complete-group destruction during the aggregate
backend failure and recovery peak memory are **not measured**.

The documentation build passes with Node 24.19.0: 19 pages, retaining the
pre-existing missing-404-entry warning. All 110 checked relative Markdown links
resolve and patch whitespace validation passes. The default Node 20 runtime is
unsupported by this Astro version; that attempted build was replaced by the
supported bundled runtime. Published-source CI must be read to terminal state
after the normal push; its eventual success cannot close KI-008 or independent
security/client qualification.
