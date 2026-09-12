---
status: validation-in-progress
owner: runtime
last_verified: 2026-09-09
related_code:
  - src/network/ReplicationCoordinator.cpp
  - src/network/ReplicaApplier.cpp
  - src/network/GameSession.cpp
  - tests/GameSessionBenchmark.cpp
---

# Foundation 3L.2 final-correctness closure

The current measured verdict is **B**. This report covers the expanded
134-item/44-question closure request, not the older first-pass acceptance list.
No automatic residency, generic QoS/scheduler, transport protocol or content
limit change is introduced. Foundation 3M must not begin.

## Source and evidence boundaries

Published starting baselines: Engine
`b54256805b1e4af8f8ad60a8672507ce26bb0be2`; Node
`4e6bc0a8ff37c58a8a122262d746dd8934b71835`.
Current local HEADs remain Engine
`a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1` and Node
`f4440423c0701ff396f589fd51b6ce41edc63539`, plus the scoped uncommitted changes.
There is no new final publication SHA yet. Local working trees remain canonical;
`dockerbox` is only a disposable validation worker under `C:\Sandbox\Codex`.

The latest production/performance snapshot is identified by
`enter-history-service-combined-source.json`, with 28 Engine native/fixture
file hashes and both changed Node file hashes. Its aggregate Engine digest is
`BF90D8A77842A8BFDB98012FFFA6BC5724FF1FBA5769EF84763BAFC7F4E2DE18`.
Two subsequent test-only corrections send actual Remote replies before the
GameSession receive assertions, and drain both redirected host streams before
waiting in `PackagedGameSession.ps1`. Production and benchmark code are unchanged.
`closure-final-native-source.json` records the resulting 29-file native/fixture
digest `28B20D8AED2039804D96998722619EB55FEC2640F0A16D966F3D91784D9CB6F6`;
all 29 Engine and two Node hashes match the worker on 2026-09-09 03:03:57 UTC.
The later ordinary Node-cycle harness has the same pipe-drain correction, without
changing its Churn branch or any native code. `closure-drained-native-source.json`
records digest `6A7DA8A1ED27930C27AAC3195C93F0235938D9CBAA649F43FA84B12199447DC6`;
its 29 Engine hashes match the worker at 03:25:38 UTC. Node source is unchanged
from the preceding two-file verification. Native sanitizer binaries and measured
production performance therefore remain exact; this final delta is PowerShell
test orchestration only.

Retained evidence is under `build-3l2-worker/evidence/` in the local Engine tree;
worker raw logs are under `C:\Sandbox\Codex\Logs\gargantuan-3l2`.
Historical long RSS/scale tests are deliberately not represented as runs of the
new Player/journal fixes. Their exact artifacts and platform caveats remain in
[the measured validation report](ContentAvailabilityFoundation3L_2Validation.md).

## Correctness fixes and remaining boundaries

Three independent pre-fix sanitizer reproductions establish the lifetime issue:

| Path | Destroyed owner and stale dependent | Fix and relevance |
| --- | --- | --- |
| Native scale fixture | `RawPeer` reverse member destruction freed its Engine before its GameSession; GameSession teardown then read `Runtime->Players`. Allocation originated in `PollRealPeer`; exception unwind skipped explicit successful-path Stop. | Declare Renderer, Engine, then Session so reverse destruction preserves dependencies. This exact ownership graph was fixture-invalid. |
| Official Server exception | Renderer inside `try` died before the outer Engine's catch-handler teardown called `Renderer->Destroy`. | Outer RAII renderer owner is declared before Engine. This is production-reachable exception cleanup, not benchmark-only. |
| Failed Engine constructor | Constructor registered world callbacks and then threw during corrupt FullyResident admission; `~Engine` never ran, leaving a callback to freed Engine storage on the retained world. | Constructor-body guard invokes normal cleanup before initialized members die. Production constructor failures are relevant. |

Overall classification: **D — the originally reported fixture ordering plus two
independently reproduced production lifetime errors**. It would be misleading to
label the whole investigation fixture-only. Complete original stacks are retained
in `ctest-linux-node-final.log`, `linux-unwind-prefix.log`,
`linux-server-unwind-prefix-full.log` and `linux-constructor-prefix.log`.
No leak, skipped exception path or new sanitizer suppression hides these defects.

The 14-stage unwind suite covers provider/Engine/Session/observer/client/peer
construction, demand, active acquisition, Resident state, provider/admission
failure, peer Stop and teardown. Earlier corrected runs passed 100 Local
iterations (40.48 s) and 100 real-TLS Node iterations (50.07 s CTest / 42.79 s
native fixture), plus 24 constructor failures and official Server exceptions.
Those runs use `lifetime-source-manifest.json`, before Player changes.

Two downstream lifetime regressions were also corrected: querying a replication
scope during nonreplicated LocalPlayer teardown could throw `bad_weak_ptr`, and
recursive Character destruction followed by an ordinary Leave could attempt to
retire the same remote materialization twice. Native checks remain strict; the
caller now retires only a still-known lifetime. Legal regression fixtures preserve
the existing indivisible structural cap.

The new 3J regression proves that a complete Enter could be followed by older
Attribute journal records, temporarily rolling state backward. Accepted complete
publications now carry an exclusive **prepare-time** per-object journal boundary.
Only older records already represented by that snapshot are skipped; mutations
after preparation still replicate. Acceptance remains scheduler-owned. The sparse
map is bounded to 65,536 entries per peer and cleared by Leave/destroy/peer teardown.
There is no new relevance authority or wire field. Pre-fix regression fails two
assertions; the corrected replication-relevance suite passes ASan/UBSan/LSan.

## Official Player: CPU work improved, critical service still unhealthy

The legal unit remains 512 objects / 1 MiB. The near-limit fixture is 512 objects /
1,048,197 bytes; the property-heavy fixture is 512 / 1,024,009 bytes.
Latest isolated runs use relocated official Windows hosts, real GNS and real TLS
for Node. They are **headless official Player runs**, not GPU-present measurements.
All four fully materialize the expected hierarchy/properties, evict it, reload a
fresh lifetime and finish 100 paced RPCs with zero timeouts/errors/crashes.

| Shape/provider | Frame mean / p50 / p95 / p99 / max ms | Event-loop max ms | Decode / apply / render-extract max ms | Character / Remote service max gap ms |
| --- | --- | ---: | --- | --- |
| Near-limit Local | 16.649 / 16.655 / 17.719 / 18.677 / 46.783 | 57.888 | 2.971 / 45.810 / 0.206 | 3606.059 / 3796.147 |
| Near-limit Node | 16.649 / 16.655 / 17.687 / 18.158 / 46.567 | 60.417 | 5.993 / 40.289 / 0.205 | 3642.559 / 3809.294 |
| Property-heavy Local | 16.648 / 16.656 / 17.696 / 18.532 / 95.473 | 108.894 | 7.552 / 84.094 / 0.236 | 3487.632 / 3582.197 |
| Property-heavy Node | 16.648 / 16.640 / 17.733 / 23.696 / 88.960 | 101.981 | 7.646 / 77.532 / 0.203 | 3572.700 / 3576.208 |

| Shape/provider | RPC mean / p50 / p95 / p99 / max ms | RemoteEvent maximum ACK gap ms | Full content / eviction / fresh reload observed after bootstrap ms |
| --- | --- | ---: | --- |
| Near-limit Local | 124.554 / 49.963 / 51.321 / 3657.912 / 3802.216 | 3820.055 | 3720.856 / 3747.115 / 7390.256 |
| Near-limit Node | 122.461 / 49.941 / 50.970 / 3677.916 / 3787.662 | 3804.566 | 3724.599 / 3749.157 / 7391.817 |
| Property-heavy Local | 121.329 / 50.032 / 51.432 / 3558.622 / 3586.984 | 3612.926 | 3622.493 / 3648.740 / 7084.658 |
| Property-heavy Node | 121.578 / 50.035 / 51.450 / 3568.543 / 3589.703 | 3590.432 | 3599.900 / 3631.512 / 7091.346 |

Evidence: `player-enter-history.log` (all four pass, 94.58 s) and
`player-enter-history/measurements.json` plus raw Player/Server CSV and logs.
Handler gap metrics include any legitimate sender idle, so they are interpreted
here only with the fixture's continuously offered gameplay/event work. They are
not a generic latency estimator or proof of deadlock.

The original near-limit 1,706.896 ms client interval was apply/preflight dominated,
not decode or render. One worst poll processed 96 small frames / 192 operations:
decode 0.503 ms, candidate copy 53.490 ms, semantic validation 272.797 ms, temporary
native `LoadSnapshot` 1,083.495 ms, live application 6.191 ms. The fixes discard
validation-world journal payload that cannot be observed, remove duplicate
semantic validation, and reuse native preflight only for exact repetitions of
already validated native properties. Live native setters still run, including
prediction correction. Changed state/custom properties/baselines still receive
full native validation. No client structural queue or generic scheduler was added.

Earlier direct same-clock traces also show prompt Server RPC handlers whose
replies wait behind large reliable structural sends; queued reliable bytes reach
over 1.5 MiB. That is transport/service head-of-line delay, not provider delay.
The newest run has no timeout but still has multi-second critical-service gaps.
Measured finite completion is not an acceptable non-starvation bound, nor does
it prove indefinite starvation impossible. LocalPlayer and Character initially
materialize in roughly 11–12 ms, but movement can then wait seconds for service.
No final separate GPU-present time or complete phase-by-phase transport percentile
distribution is available. `encode_ns` in the Server CSV is baseline encode time,
not total incremental GRPL encoding; it must not be mislabeled.

The maximum-content fixture does exercise eviction before Player convergence:
the Local Server commits at 2,418 ms, removes the lifetime at 3,158 ms and commits
the replacement at 3,969 ms relative to its startup clock. Correlating its Unix
clock anchor with the Player trace places Server removal more than 2.6 seconds
before the Player's first full-content observation, even allowing the entire
227-ms Player startup uncertainty. The client observes the old complete lifetime,
then its ordered removal 26.259 ms later, then the replacement. This proves
evict/reload with previously accepted reliable delivery still pending, not the
separate case of an old Leave still unselected inside 3J. No client resurrection
after consumption of the old Leave is observed.

### Final-source object-count and shape comparison

With heavy worker builds/tests finished, the unchanged production binaries also
pass four Local official Player cases in 69.58 s. Each performs eight content
lifetimes and 100 RPCs, with zero timeouts/errors/crashes. The `lightweight` fixture
means Part-only region composition (plus its root), not stripped serialization
of native Part properties; its 294,488 bytes are similar to the representative
512-object unit's 292,485 bytes.

| Objects / shape / payload bytes | Frame mean / p50 / p95 / p99 / max ms | Event-loop max ms | Decode / apply / render-extract max ms | Character / Remote gap ms | First full content observed ms |
| --- | --- | ---: | --- | --- | ---: |
| 100 representative / 55107 | 16.631 / 16.692 / 17.472 / 17.757 / 18.197 | 33.625 | 0.433 / 15.367 / 0.082 | 160.943 / 149.377 | 194.951 |
| 256 representative / 144917 | 16.625 / 16.647 / 17.638 / 17.930 / 21.070 | 36.217 | 0.813 / 18.449 / 0.182 | 335.278 / 335.281 | 382.013 |
| 512 representative / 292485 | 16.631 / 16.653 / 17.663 / 18.130 / 42.856 | 56.430 | 1.566 / 37.812 / 0.384 | 677.314 / 674.553 | 719.618 |
| 512 Part-only / 294488 | 16.637 / 16.646 / 17.772 / 18.564 / 34.555 | 49.362 | 1.503 / 29.637 / 0.355 | 661.335 / 658.829 | 690.714 |

| Profile | RPC mean / p50 / p95 / p99 / max ms | Event ACK mean / p50 / p95 / p99 / max ms | Event max ACK gap ms | Structural messages / bytes / operations |
| --- | --- | --- | ---: | --- |
| 100 | 51.606 / 49.877 / 51.603 / 133.629 / 140.620 | 51.334 / 50.047 / 51.565 / 116.909 / 123.582 | 150.788 | 429 / 132373 / 1157 |
| 256 | 41.679 / 33.635 / 50.505 / 281.363 / 321.172 | 42.741 / 33.748 / 50.649 / 304.558 / 314.233 | 347.824 | 368 / 215827 / 1503 |
| 512 representative | 53.478 / 35.227 / 52.045 / 650.598 / 675.805 | 54.871 / 34.733 / 52.093 / 642.641 / 650.684 | 692.344 | 434 / 382369 / 2413 |
| 512 Part-only | 62.068 / 50.001 / 51.670 / 628.417 / 684.538 | 62.007 / 50.086 / 51.702 / 637.784 / 651.586 | 671.711 | 488 / 393411 / 2521 |

These message/operation totals cover the whole Player trace, including bootstrap
and ordinary updates; they are not create-only counts. Full mean/tail decode,
apply and event-loop distributions are retained in
`player-closure-sizes/measurements.json`. No 4,096-row trace cap is reached.
The corrected CPU cost is roughly proportional over the tested 256-to-512 range,
instead of the original catastrophic repeated-world-validation curve; four runs
are not an asymptotic complexity proof. At the same 512 objects, near-limit bytes
increase service gaps to about 3.8 s despite near-limit apply max only 45.8 ms.
Property-heavy apply reaches 84.1 ms. Thus serialized volume/property shape still
matters independently of object count, especially to reliable service delay.
Neither the smaller CPU peaks nor eventual convergence close critical-service
acceptance. Raw output: `player-closure-sizes.log` and its per-host traces.

## Healthy differential: actual streaming degradation, not the 500-peer baseline

The chosen feasible fixture has 200 connected/materialized peers, 50 active
Characters, groups of five spatial neighbors, 12 Hz staggered owner input and ten
Animator-driven root-motion Characters. It retains real actions, events, RPCs,
ordinary 3E/3J and one shared 512-object region. One gameplay peer is a real client
Engine; the other peers are protocol consumers, not 200 official Player processes.
This is a useful healthy differential but does not establish the requested
500-connected healthy workload.

Windows ordinary relative sleep overshot by p50 14.474 / p95 15.480 / max 16.409 ms
per frame in the first attempted control. The fixture now uses existing SDL precise
delay to pace remaining frame time, without catch-up ticks or global timer changes.
Root-motion crossings stay on the ground inside the same neighborhood; all final
phases retain minimum Y=2.495. Every no-streaming phase and both provider baseline
phases pass the explicit health gate. Streaming load/evict/reload fail it.

| Equivalent load phase | No streaming | Local OnDemand | TLS Node OnDemand |
| --- | ---: | ---: | ---: |
| Server tick mean ms | 7.655 | 35.506 | 36.211 |
| Tick p50 / p95 / p99 / max ms | 6.420 / 13.596 / 16.503 / 33.210 | 20.216 / 90.795 / 98.078 / 270.961 | 20.414 / 92.035 / 98.916 / 280.166 |
| Character recipient states/wall-second | 8218.01 | 2777.62 | 2706.83 |
| Character bytes/simulation-second | 695767 | 695026 | 695026 |
| Maximum Character/root publication gap ms | 205.696 | 1075.46 | 1133.61 |
| RPC mean / p50 / p95 / p99 / max ms | 47.788 / 49.648 / 51.568 / 52.780 / 53.055 | 160.889 / 169.712 / 197.324 / 424.622 / 458.841 | 165.259 / 175.253 / 218.644 / 430.907 / 458.639 |
| Event ACK mean / p50 / p95 / p99 / max ms | 50.403 / 50.094 / 57.822 / 60.115 / 77.285 | 147.844 / 163.365 / 194.893 / 427.934 / 550.404 | 151.748 / 164.319 / 202.538 / 431.429 / 573.309 |
| Action result mean / p50 / p95 / p99 / max ms | 48.598 / 50.545 / 51.543 / 51.543 / 51.920 | 153.762 / 164.051 / 191.805 / 191.805 / 196.133 | 160.034 / 174.453 / 194.234 / 194.234 / 217.976 |
| Root produced / authoritative commits / GCHR states | 1920 / 1920 / 33033 | 2080 / 2080 / 32968 | 2030 / 2030 / 32968 |
| Root spatial crossings | 11 | 11 | 11 |
| Phase wall duration ms / ticks | 6732.65 / 401 | 19896.1 / 401 | 20416.5 / 401 |
| Accepted 3J selected/committed operations | 2440 | 104840 | 104840 |
| 3J pending HWM / journal lag / failures | 30126 / 3548 / 0 (cumulative bootstrap HWM) | 94208 / 6135 / 0 | 94208 / 6135 / 0 |
| Last-peer content convergence | N/A | 1910.29 ms / 14 ticks | 1981.56 ms / 14 ticks |

Local reduces Character wall throughput by about 66.2%, Node by 67.1% compared
with the identical no-streaming phase. Tick p99 grows about sixfold. All phases
complete 100 RPCs without timeout/error/crash and no action rejection/submission
failure, but latency is not healthy. Eviction action max is 341.618/343.095 ms;
eviction Character gap is 827.337/823.055 ms. Reload Character max gaps are
1080.84/1079.78 ms, with tick p99/max 104.150/279.699 and 97.205/279.579 ms.
Root commits match produced updates and safe-point 3K/3H consistency checks pass;
correct spatial identity is not proof of healthy wall-time cadence.

The 3J history fix reduces load work from approximately 1.26 million operations
to 104,840 without changing the 8,192 cap. Resident gameplay is still expensive.
Copying peer views is an inspection lead, not an independently timed dominant
cause. No speculative optimization is claimed as a fix.

Evidence: `differential-enter-history-200p-50a.log` (control PASS, Local health
FAIL, 85.80 s) and `differential-enter-history-node-200p-50a.log` (Node health
FAIL, 57.76 s). The former completion-only fixture's Go PASS is historical and
must not be confused with the current strict phase health result.

The dense 500-Character historical workload remains useful capacity evidence:
baseline RPC p99 was already about 1.16–1.34 seconds. Its streaming comparison
cannot isolate starvation. That limitation no longer explains away the healthy
200-peer differential failure above.

## Final validation gates

Full Windows MSVC Release build and CTest pass: **53/53 in 39.50 s**, including
the new unwind regression, repeated GameSession tests, GNS and packaged hosts.
The first run was 52/53: the packaged fixture blocked on redirected smoke output
because it waited for process exit before reading either stream. With the same
executables/assertions and concurrent stream drainage, that test passes in 4.71 s
and the full rerun is green. This harness pipe stall is separate from the real
large-content client/service failures above; the main Node vertical already used
asynchronous drainage during those measurements.

The full Linux Clang 19 build completes. CTest passes 47/49 in 174.15 s; two
packaging cases exhaust the 3-GiB temporary filesystem, with no sanitizer finding.
The packaging group then passes 3/3 in 150.12 s using the disposable container's
disk-backed executable scratch space, with unchanged tests/binaries: packaging
127.19 s, editor-host 6.25 s, packaging benchmark 16.62 s. Thus all 49 supported
Engine tests have passing current-source results, without calling the initial
47/49 run wholly green. No ASan/UBSan/LSan finding appears in either completed
passing set. The final
Node native sanitizer/TLS suite passes 4/4 in 76.68 s, including 100 TLS exception
iterations (34.09 s), direct real-TLS vertical (24.36 s), configuration and the
official relocated headless Server (17.84 s). The rebuilt packager is included.
Node Go tests, vet and Linux CGO-enabled race pass on
the latest changed fixture. Final documentation builds 19 pages in 1.79 s with bundled
Node 24.19.0 and existing local dependencies. The worker documentation-install
attempt used the wrong lockfile workflow (`npm ci`, while this repository has
`bun.lock`) and failed before build; no lockfile/package change was made. The
successful quick cached local build is not a remote native workload or a hosted
Pages deployment. The security preflight passes and TAC
advisory is granted (`tac1`), but final security inventory generation is blocked
by the helper's Git subprocess not recognizing the local checkout. No completed
new scan or zero-findings claim is made. This is not a cybersecurity-classification
block and no Daybreak fallback was invoked. No final commit or CI deployment is
claimed while these gates remain unresolved.

The latest ordinary official Windows TLS/GNS host suite also passes in 140.59 s:
ten same-port Node/Server/Player cycles, all four provider/policy semantic-world
comparisons, Stop during a delayed Node call, fresh Server success, resident
survival after Node outage, corrupt payload and package/auth/TLS/unavailable
bootstrap rejection, offline Player isolation, and an additional 100-call RPC
vertical. That stress case reports mean/p50/p95/p99/max 38.868/33.740/50.528/52.000/
52.818 ms, zero timeouts/errors/crashes; Event ACK max 50.486 ms, gap 85.421 ms.
The ten Node starts average 21 ms, max 29 ms. The first attempt failed because
ordinary Node-cycle Server output was not drained until after Player exit; it
blocked at a full diagnostic pipe before servicing the handshake. Immediate
drainage fixes the harness with identical runtime binaries and assertions.
The earlier large-content Churn mode already drained both streams, so this does
not explain away its measured critical-service delay. Retained logs:
`official-closure-pipe-prefix.log` and `official-closure-drain-fixed.log`.

An additional invocation of the existing graphical packaged fixture establishes
a session and presents/stops two actions, but exits 26 (smoke step 6: final remote
Character/relevance-cycle condition unmet). Startup is 1,002 ms. The fixture's
fixed server-tick relevance sequence is a timing investigation lead, not a proven
root cause; this attempt is **not green graphical coverage**. Its full log is
`windows-packaged-graphical-closure.log`. No renderer/environment failure or new
access violation is asserted from that nonzero semantic smoke result.

The retained Windows 1,000-cycle and Linux 10,000-cycle RSS plateaus, decoded
16-MiB exact-bound tests, 64-unit backpressure, and 32/100/500 structural convergence
remain valid for their recorded earlier sources. The latest 3J change justifies
targeted structural/lifecycle confirmation; prior conclusive long RSS runs are
not automatically repeated.

### Journal-fix structural confirmation

The latest real-TLS Node 512-object / 273,032-byte rerun passes 32/100/500 peer cases in
959.55 s total (59.14 / 138.50 / 761.75 s). Source is the performance digest above;
later test-only changes do not affect this benchmark. It overlaps builds, so no
wall-latency number from it is used as an isolated performance conclusion.

| Peers / phase | Acquisitions / admissions cumulative | Pending HWM | Selected cap | Selected = committed | Last-peer convergence ticks | Journal lag HWM / failures |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| 32 / load | 1 / 1 | 8192 | 8192 | 29760 | 14 | 6135 / 0 |
| 32 / evict | 1 / 1 | 8192 | 8192 | 28892 | 2 | 6135 / 0 |
| 32 / reload | 1 / 2 | 8192 | 8192 | 29825 | 3 | 6138 / 0 |
| 100 / load | 1 / 1 | 43008 | 8192 | 93300 | 8 | 6143 / 0 |
| 100 / evict | 1 / 1 | 43008 | 8192 | 93696 | 7 | 6143 / 0 |
| 100 / reload | 1 / 2 | 43008 | 8192 | 93201 | 8 | 6143 / 0 |
| 500 / load | 1 / 1 | 249321 | 8192 | 536957 | 35 | 10416 / 0 |
| 500 / evict | 1 / 1 | 261281 | 8192 | 528474 | 32 | 10416 / 0 |
| 500 / reload | 1 / 2 | 261281 | 8192 | 532965 | 33 | 10416 / 0 |

All twelve phases finish 100 RPCs without timeout/error/crash. This is still not
a gameplay health pass: 500 baseline RPC p99 is 1,607.707 ms, action submission
failures occur, and the 500 reload phase records a root-motion Character falling
to Y=-590.336 (earlier phases remain at 2.495). Thus that reload is not valid
healthy root-motion cadence evidence. The separate feasible differential above
has stable ground/crossings and explicit health gates. The 32-peer baseline also
hits the unchanged Remote rate limit while simulated time runs faster than wall
time; the associated diagnostic is retained, not interpreted as a production
authorization bypass. Evidence: `structural-enter-history-node-512.log`.

## Retained RSS and decoded memory proof

These are physical process measurements, not logical counters. Windows is MSVC
Release; Linux is Clang 19 with ASan/UBSan/LSan. Values are bytes, slope is bytes/s.
Start is the first process sample, not an idle fully initialized baseline. Final
is the final-drain sample mean. Windows historical milestone alignment is coarse;
Linux drain has only two/three samples. The sources predate the new Player/3J
follow-up and are not relabeled as final-revision tests.

| Platform / unit / provider | Cycles | Start RSS | Warm HWM | Steady RSS min–max | Final-drain RSS | Slope |
| --- | ---: | ---: | ---: | --- | ---: | ---: |
| Windows / 20 / Local | 1000 | 3702784 | 49131520 | 49131520–49131520 | 49131520 | 0 |
| Windows / 20 / Node | 1000 | 3051520 | 53661696 | 53661696–53661696 | 53661696 | 0 |
| Windows / 100 / Local | 1000 | 3473408 | 50778112 | 48934912–50778112 | 50778112 | 1112.486 |
| Windows / 100 / Node | 1000 | 3469312 | 55648256 | 55603200–55672832 | 55672832 | 138.600 |
| Windows / 256 / Local | 1000 | 3366912 | 54538240 | 51486720–54743040 | 54489088 | 1270.994 |
| Windows / 256 / Node | 1000 | 2711552 | 59494400 | 54075392–59494400 | 58331136 | -207.115 |
| Windows / 512 / Local | 1000 | 4878336 | 59756544 | 53612544–61054976 | 58297754 | 1604.305 |
| Windows / 512 / Node | 1000 | 2678784 | 64348160 | 58335232–65597440 | 63680512 | 769.278 |
| Windows / near-limit / Local | 1000 | 3596288 | 73420800 | 64446464–72577024 | 71263027 | -629.222 |
| Windows / near-limit / Node | 1000 | 3420160 | 76357632 | 70004736–77901824 | 75980369 | 3956.908 |
| Linux / 20 / Local | 10000 | 60223488 | 529862656 | 510734336–530538496 | 521146368 | 2313.684 |
| Linux / 20 / Node | 10000 | 60223488 | 544243712 | 530849792–544493568 | 536050347 | -521.946 |

Every retained profile plateaus within a bounded observed range. Positive fitted
slopes and finite duration are explicitly retained; this is not proof of zero
growth for arbitrarily long runs. Linux Local first/last steady-quarter means are
529,371,114/529,782,230 bytes; Node 543,742,511/543,585,170 bytes. Windows private
memory distributions and all raw samples remain in the companion report/artifacts.
Churn uses cached verified content: 1,000/10,000 lifetimes are not that many RPCs.

The global/session retained decoded-document ceiling remains 16,777,216 bytes.
A shared RAII charge follows a document across completion/Available/preparation
owners; copies of that owner do not evade or double-charge the document. The
measured per-document charge in the exact-bound test is 2,172,554 bytes. At
4,345,107 bytes the second document defers; at 4,345,108 and 4,345,109 both fit.
Cancellation, renewed admission, eviction and Stop release the charges. Transient
bounded parser storage and allocator overhead are not falsely included as decoded
retained bytes or confused with total RSS.

The retained 64-unit stress admits 64 independent 512-object units, one acquisition
each, with decoded HWM 16,151,680 bytes, 59 deferrals, pending HWM 48, in-flight HWM
16, raw completions 3,154,824 bytes and encoded cache 16,825,728 bytes. Drain takes
70 ticks / 3,057.12 ms; maximum content Main step is 69,823 microseconds. RSS peaks
at 341,024,768 bytes, drops to 81,420,288 after content Stop and 33,312,768 after
world teardown. A simultaneous official Player/gameplay 64-unit overload and its
catch-up bound remain unproven; server-only memory success does not close that gap.

### Current-source targeted memory confirmation

After the lifetime, Player and 3J fixes, another 1,000-cycle near-limit run per
provider completes using the current official Windows Server: Local 268.987 s /
2,350 samples, TLS Node 268.066 s / 2,349 samples, total harness 542.00 s. The unit
is 512 objects / 1,048,197 bytes. This is Server-only cached lifetime churn, not
1,000 network acquisitions or a Player soak. Linux builds overlap; these are
physical-memory observations, not isolated gameplay timing measurements.

| Provider | Start RSS | Warm HWM | Steady RSS min–max | Final-drain RSS | Steady slope B/s | First / last steady-quarter mean RSS |
| --- | ---: | ---: | --- | ---: | ---: | --- |
| Local | 5177344 | 71880704 | 63545344–72777728 | 72216576 | 15048.521 | 69619113 / 71167486 |
| TLS Node | 5312512 | 77324288 | 69083136–78135296 | 76369920 | 3065.057 | 75950592 / 76166400 |

Both remain inside the earlier near-limit memory envelope. Node is nearly flat;
Local's steady-quarter means rise by 1,548,374 bytes (about 2.2%). The latter is
bounded over this run but not a literal flat plateau or proof that the residual
growth would stop forever. Do not discard the longer retained evidence or hide
this newer drift. PrivateUsage steady ranges are 52,068,352–61,333,504 Local and
55,627,776–65,511,424 Node, with slopes 14,642.527 / 3,597.355 B/s. Handle maxima
are 154/171, thread maxima 18/22; final-drain windows contain sixteen samples each.
Raw data and aligned calculations: `rss-closure-targeted/` and
`rss-closure-targeted-summary.json`, native production digest recorded above.

### Explicit concurrent memory categories

These limits are per ContentAvailability service/session, not a new process-wide
limit across independently constructed Engines. Defaults can be reduced by trusted
configuration but cannot exceed the hard ceiling.

| Category | Default / hard ceiling | Measured HWM in retained 64-unit run | Released when |
| --- | --- | --- | --- |
| Pending content requests | 1,024 / 1,024 keys | 48 | scheduled, cancelled or Stop |
| In-flight acquisitions | 4 / 16; workers 2 / 8 | 16 | completion/cancellation joined |
| Raw completed payload | 16 MiB / 16 MiB | 3,154,824 bytes | drained, cancelled or Stop |
| Retained decoded documents | 16 MiB / 16 MiB | 16,151,680 bytes | final shared document owner releases on admission, deferral, cancellation or Stop |
| Immutable encoded cache | 32 MiB / 32 MiB | 16,825,728 bytes | eviction or Stop |
| Detached preparation graph | serial Main admission, at most 512 objects / 1 MiB encoded per step | 512 objects; native graph bytes not separately measured | authoritative adoption or rollback |
| Client structural pending state | no new queue; existing negotiated message, snapshot and Poll limits | new queue HWM not applicable; reliable send backlog above 1.5 MiB observed separately | ordinary application or session teardown |

Raw bytes, decoded documents and detached/live graphs can overlap. The shared
document charge is counted once across completion/Available/Main owners, but it
does not include an independent encoded-buffer charge, transient parser storage,
allocator overhead or live Instances. Per-input limits and at most eight workers
bound transient parses separately. The simultaneous 341,024,768-byte RSS peak
above measures actual combined ownership; summing unrelated category maxima would
not establish a physical process-memory ceiling. Resident worlds and independent
sessions are not subject to a newly invented global 16-MiB RAM cap.

## Exact acceptance report: 134 items

Entries explicitly distinguish historical proof, current proof and incomplete
gates. Where a distribution is already tabulated above, the entry identifies that
table rather than inventing a second metric.

| # | Result |
| ---: | --- |
| 1 | Starting Engine `b54256805b1e4af8f8ad60a8672507ce26bb0be2`. |
| 2 | Starting Node `4e6bc0a8ff37c58a8a122262d746dd8934b71835`. |
| 3 | Current Engine HEAD `a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1` plus uncommitted scoped fixes; no final commit yet. |
| 4 | Current Node HEAD `f4440423c0701ff396f589fd51b6ce41edc63539` plus uncommitted fixture changes; no final commit yet. |
| 5 | Engine versus starting baseline: 55 scoped files, +5,130/-147 (51 tracked plus four new); versus current HEAD: 43 files, +3,212/-111. Node versus starting baseline: four files, +469/-5; versus HEAD: two files, +113/-2. Build/evidence trees and unrelated morphology/old logs excluded. |
| 6 | Performance digest `BF90D8A77842A8BFDB98012FFFA6BC5724FF1FBA5769EF84763BAFC7F4E2DE18`; native test snapshot `28B20D8AED2039804D96998722619EB55FEC2640F0A16D966F3D91784D9CB6F6`; last PowerShell drainage snapshot `6A7DA8A1ED27930C27AAC3195C93F0235938D9CBAA649F43FA84B12199447DC6`. Worker hashes match, deltas test-only. |
| 7 | Original ASan: GameSession destructor → TearDownPeer → access through freed Engine; complete reports retained in the four named logs above. |
| 8 | Original destroyed allocation is RawPeer's Engine. Two production reproductions additionally involve the dead renderer and a failed Engine allocation with surviving world callback. |
| 9 | Original stale reference is GameSession's non-owning Runtime pointer. Production references are Engine's renderer pointer and callback capture of Engine. |
| 10 | Exception bypasses successful explicit Stop; reverse declarations destroy Engine before Session. Production renderer leaves try scope before catch cleanup; failed constructor never gets a destructor. |
| 11 | Classification D: original fixture-invalid order plus independently reproduced production lifetime bugs; not a blanket fixture-only disposition. |
| 12 | RAII declaration order makes renderer outlive Engine and Engine outlive Session; failed constructor guard runs normal cleanup before members die. |
| 13 | Current-source 100 Local 14-stage unwind iterations pass in 48.42 s; 100 real-TLS Node iterations pass in 34.09 s. Constructor/admission failure regressions are included in the passing content suite. |
| 14 | ASan clean across all 49 supported current Engine tests (47 initial passes plus corrected-storage packaging group) and four native Node/TLS tests. |
| 15 | UBSan halt-on-error clean across the same complete supported test coverage. |
| 16 | LSan detect_leaks=1 clean on those completed exits; no new suppression. Failed/killed runs are not substituted for leak-clean exits. |
| 17 | Legal maximum remains 512 objects / 1,048,576 bytes; measured near-limit package is 1,048,197 bytes. |
| 18 | Repeated whole-world native preflight and discarded journal serialization dominate original CPU stall; reliable structural backlog separately delays critical service. |
| 19 | Earlier isolated candidate Server tick p99/max 18.264/45.184 ms, Engine step max 26.181 ms; these are not the full final near-limit admission/encode distribution. Final differential Server times are tabulated above. |
| 20 | Direct trace: RPC handler runs ~67 ms after request but completion takes 1,891 ms behind reliable backlog; other promptly handled replies time out at five seconds. Latest full-content observation is 3.60–3.72 s after bootstrap, not a pure transport timer. |
| 21 | Latest decode maxima near Local/Node 2.971/5.993 ms; property-heavy 7.552/7.646 ms. Full distributions retained in measurements.json. |
| 22 | Latest apply maxima near Local/Node 45.810/40.289 ms; property-heavy 84.094/77.532 ms. |
| 23 | Maximum-content extraction ≤0.237 ms/publication ≤0.123 ms; final size-sweep extraction reaches 0.384 ms. GPU present remains unmeasured. |
| 24 | Latest worst frame interval 95.473 ms among four legal-max profiles; near-limit worst 46.783 ms. Not an all-input theoretical bound. |
| 25 | Latest worst event-loop gap 108.894 ms; near-limit worst 60.417 ms. |
| 26 | Latest maximum Character handler gap 3,642.559 ms under continuous fixture traffic. |
| 27 | Latest maximum Remote handler gap 3,809.294 ms; RemoteEvent ACK gap reaches 3,820.055 ms. |
| 28 | Final 100-object Local official profile passes, frame max 18.197 ms, apply max 15.367 ms, Character gap 160.943 ms. |
| 29 | Final 256-object Local official profile passes, frame max 21.070 ms, apply max 18.449 ms, Character gap 335.278 ms. |
| 30 | Final representative 512 Local profile passes, frame max 42.856 ms, apply max 37.812 ms, Character gap 677.314 ms. Part-only comparison is tabulated above. |
| 31 | Latest near-limit Local/Node load/evict/reload + 100 RPC pass, but service gaps remain unacceptable; not readiness A. |
| 32 | Original repeated-world-validation curve was pathological/superlinear. Final observed CPU peaks scale roughly proportionally from 256 to 512; byte-driven service gaps remain. Full final size/shape table above, not an asymptotic guarantee. |
| 33 | Property volume materially matters: final property-heavy max apply 77.5–84.1 ms vs long-name near-limit 40.3–45.8 ms. Persisted non-null Instance references are not supported by the current package value domain; no fake reference-heavy package test. |
| 34 | Narrow preflight/render-journal changes only; no new queued/incremental client structural architecture. |
| 35 | No new client apply-time budget. Existing message/operation and Poll limits remain; these are not an accepted wall-time bound. |
| 36 | No new pending structural queue. Existing negotiated wire, snapshot/object and transport limits remain; temporary preflight worlds remain bounded by existing snapshot limits. |
| 37 | No new priority policy; critical service can suffer measured seconds-long reliable backlog. This acceptance condition remains open. |
| 38 | No new queued creation to cancel. Existing epoch/sequence/ObjectId/materialization denial tests remain; latest full hierarchy reload uses a fresh client lifetime. |
| 39 | Latest large load → authoritative eviction → client removal passes for all four profiles. |
| 40 | Latest large load → eviction → reload yields fresh server and retained client references, passes all four profiles. |
| 41 | Latest large-content vertical disconnect/Stop completes; immediate mid-application disconnect variant is not independently covered by these after-convergence runs. |
| 42 | Retained Windows ten 1,000-cycle Local/Node profiles plus current near-limit 1,000 cycles/provider confirmation. |
| 43 | Historical first RSS 2,678,784–4,878,336 bytes; current near-limit Local 5,177,344 / Node 5,312,512, not idle-running baselines. |
| 44 | Per-profile historical table retained; current Local 63,545,344–72,777,728 / Node 69,083,136–78,135,296 bytes. |
| 45 | Current near-limit final-drain Local 72,216,576 / Node 76,369,920 bytes, sixteen samples each; historical values separately tabulated. |
| 46 | Historical fitted slopes -629.222 to +3956.908 B/s; current Local +15048.521 / Node +3065.057. Local's 1.55-MB steady-quarter drift is explicitly not a perfectly flat plateau claim. |
| 47 | Retained Linux 10,000 cycles per Local/TLS Node small-entry profile. |
| 48 | Both Linux first RSS samples 60,223,488 bytes. |
| 49 | Linux Local steady 510,734,336–530,538,496; Node 530,849,792–544,493,568 bytes. |
| 50 | Linux final-drain means 521,146,368 / 536,050,347 bytes, only two/three samples. |
| 51 | Linux slopes +2313.684 / -521.946 bytes/s; instrumentation/quarantine prevents direct Windows absolute comparison. |
| 52 | Retained decoded-document hard ceiling 16 MiB (16,777,216 bytes). |
| 53 | 64-unit decoded HWM 16,151,680 bytes; before accounting the same category reached about 87.7 MB. |
| 54 | Two-document charge minus one: ceiling 4,345,107 retains one charge, second defers. |
| 55 | Exact two-charge ceiling 4,345,108 retains both, no deferral. |
| 56 | Ceiling 4,345,109 retains both; the first operation that would exceed the active ceiling defers without unbounded retention. |
| 57 | Cancellation releases charges; retry/admission and eviction regressions pass. Transient bounded parse allocations are separately accounted conceptually, not included in retained bytes. |
| 58 | Stop cancels/joins workers and releases retained decoded owners; content Stop/world teardown RSS measurements are above. |
| 59 | Latest journal-fix TLS Node 32-peer run: one shared acquisition including cached reload. Historical Local proof retained. |
| 60 | Latest Node 32-peer: one initial authoritative admission, second new lifetime on reload. |
| 61 | Latest 32-peer pending HWM 8,192; selected cap 8,192, journal lag HWM 6,138, zero failures. |
| 62 | Latest Node 32-peer load/evict/reload convergence 14/2/3 ticks; concurrent-build wall times are not isolated performance evidence. Historical Local/Node timing remains in the companion report. |
| 63 | Latest Node 100-peer: one shared acquisition. |
| 64 | Latest Node 100-peer: one initial admission, second on reload. |
| 65 | Latest Node 100-peer load/evict/reload convergence 8/7/8 ticks; pending HWM 43,008 and zero journal failures. |
| 66 | Latest Node 500-peer: one shared acquisition, not one per peer. |
| 67 | Latest Node 500-peer: one authoritative region lifetime, second admission after eviction/reload. |
| 68 | Latest Node 500-peer pending HWM 261,281 across phases (load 249,321); historical earlier-source HWM 247,808 retained separately. |
| 69 | Global selected cap 8,192, no increase/bypass. Current 200-peer differential also stays within it. |
| 70 | Latest Node 512-object 500-peer load/evict/reload converges in 35/32/33 ticks; all peers converge. |
| 71 | Latest 500-peer journal lag HWM 10,416 against 16,384 retention; historical maximum 13,961 on earlier source. |
| 72 | Latest 32/100/500 structural confirmation and feasible 200-peer differential journal failures all zero. |
| 73 | Dense 500 baseline RPC p99 ~1.16–1.34 s: already unhealthy before streamed content. |
| 74 | Dense 500 streaming RPC p99 ~1.88–1.94 s, action failures occur in the matrix; structural convergence does not imply gameplay health. |
| 75 | Dense 500 is a baseline capacity boundary, unsuitable as primary streaming attribution. Healthy 200-peer results independently demonstrate streaming degradation. |
| 76 | Feasible control: 200 connected, 50 active, five per neighborhood, 12 Hz input, ten root-motion Characters; no healthy 500-connected final gate yet. |
| 77 | Equivalent no-stream load tick p99/max 16.503/33.210 ms; initial control baseline 15.624/52.340 ms. |
| 78 | Local load tick p99/max 98.078/270.961 ms; reload 104.150/279.699 ms. |
| 79 | Node load tick p99/max 98.916/280.166 ms; reload 97.205/279.579 ms. |
| 80 | Equivalent no-stream load 8,218.01 Character recipient states/wall-second. |
| 81 | Local load 2,777.62 states/wall-second, about 66.2% lower. |
| 82 | Node load 2,706.83 states/wall-second, about 67.1% lower. |
| 83 | Equivalent no-stream Character max gap 205.696 ms. |
| 84 | Local load 1,075.46 ms; reload 1,080.84 ms. |
| 85 | Node load 1,133.61 ms; reload 1,079.78 ms. |
| 86 | No-stream load Event ACK mean/p50/p95/p99/max 50.403/50.094/57.822/60.115/77.285 ms. |
| 87 | Local load Event ACK 147.844/163.365/194.893/427.934/550.404 ms; continuous progress, unhealthy latency. |
| 88 | Node load Event ACK 151.748/164.319/202.538/431.429/573.309 ms; continuous progress, unhealthy latency. |
| 89 | No-stream load RPC 47.788/49.648/51.568/52.780/53.055 ms. |
| 90 | Local load RPC 160.889/169.712/197.324/424.622/458.841 ms. |
| 91 | Node load RPC 165.259/175.253/218.644/430.907/458.639 ms. |
| 92 | Zero timeouts/errors in 100 calls per current differential phase and each latest maximum-content official profile; earlier candidate failures retained separately. |
| 93 | Zero access violations/crashes in those current stress cases. This is not a claim about unlimited concurrency. |
| 94 | No-stream load action mean/p50/p95/p99/max 48.598/50.545/51.543/51.543/51.920 ms. |
| 95 | Local load action 153.762/164.051/191.805/191.805/196.133 ms; eviction max 341.618 ms. |
| 96 | Node load action 160.034/174.453/194.234/194.234/217.976 ms; eviction max 343.095 ms. |
| 97 | Current feasible differential zero action submission failures/rejections; result/end observed. Separate timeout counter is not reported; small action sample counts must not be treated as a large tail study. |
| 98 | No-stream load root produced/committed 1,920/1,920, 33,033 GCHR observations in 6.733 s. |
| 99 | Local load 2,080/2,080, 32,968 GCHR observations in 19.896 s. Simulation consistency survives; wall cadence degrades. |
| 100 | Node load 2,030/2,030, 32,968 GCHR observations in 20.417 s. |
| 101 | Load root publication gaps match 205.696 / 1,075.46 / 1,133.61 ms control/Local/Node. |
| 102 | Eleven load crossings per configuration; 3K/3H safe-point consistency passes, no stale old content projection or region identity contamination observed. |
| 103 | Retained 64-unit overload RSS HWM 341,024,768 bytes, decoded HWM 16,151,680. Not a simultaneous final Player overload proof. |
| 104 | Pending HWM 48, in-flight 16, raw completions 3,154,824 bytes, encoded cache 16,825,728. |
| 105 | 64-unit content backlog drains in 70 ticks / 3,057.12 ms. |
| 106 | Maximum official Player catch-up for that simultaneous 64-unit overload is unmeasured. |
| 107 | Character under one shared region is measured and unhealthy; combined 64-unit overload Character envelope not established. |
| 108 | Remotes under one region continue but degrade; combined 64-unit overload Remote envelope not established. |
| 109 | Retained 100 official normal cycles pass in 665.251 s; current follow-up adds ten same-port normal cycles, focused failure/Stop cases and four maximum-content verticals, not 100 mixed pending-failure cycles. |
| 110 | Retained 100 same-process legal-max lifetimes; 100 Local + 100 TLS Node unwind iterations. Latest Windows full GameSession repetition and unwind CTest pass. |
| 111 | Latest official TLS suite passes Stop during the delayed active call, zero stopped-world admission/stale projection, observed provider cancellation and fresh Server success. |
| 112 | Existing generation/peer-removal tests cover pending work; combined large official Player disconnect before full 3J convergence is not newly proven. |
| 113 | Maximum-content fixture removes the Server lifetime before Player convergence; accepted reliable Enter completes, then ordered Leave removes it. |
| 114 | Fresh Server lifetime commits while old reliable delivery remains pending; client receives fresh identity. Old Leave still unselected inside 3J remains a separate gap. |
| 115 | RemoteFunction handler lifetime regression survives normal stress/Stop; an explicitly active handler at each of 100 shutdowns is not established. |
| 116 | Retained 100 official cycles reuse Node TCP and Server UDP endpoints; explicit same-port Server restart passes. |
| 117 | Final worker audit at 03:31:04 UTC finds zero task-owned official runtime processes and zero task containers; six unrelated containers remain running. Same-port restart/cycle assertions pass; no stale per-process worker can survive those exited processes. |
| 118 | Fresh provider/channel per official process; cancellation/join and weak provider-owner checks pass in focused unwind suites. |
| 119 | Destroyed old ObjectIds fail resolution; reload retains distinct fresh server/client lifetime; no old-generation attachment observed. |
| 120 | Old content 3K projections absent after eviction/teardown in focused and differential assertions. |
| 121 | Final-source Windows MSVC Release build and full CTest 53/53 in 39.50 s, after correcting the packaged fixture's output-pipe deadlock. |
| 122 | All 49 supported Linux Engine tests have passing exact-native-source results: initial 47/49 in 174.15 s, corrected-storage packaging group 3/3 in 150.12 s. Native Node sanitizer/TLS 4/4 in 76.68 s. |
| 123 | Latest Node Go test ./..., vet ./..., Linux CGO-enabled race ./... pass; native opt-in tests are separate. |
| 124 | Earlier sealed scans found zero; latest final scan incomplete at Git inventory helper. No claim of zero reportable issues for the exact final diff. |
| 125 | Final documentation build PASS: 19 pages in 1.79 s, Node 24.19.0, cached local dependencies; docs-closure-final-build.log retained. |
| 126 | Earlier a68 Pages deployment green; no final uncommitted-document deployment. |
| 127 | Earlier Engine native CI a68 green; no terminal CI for the uncommitted fixes. |
| 128 | Earlier Node f444 CI green, including generated bindings; latest fixture diff has worker test/vet/race proof but no final hosted run. |
| 129 | No additional Engine commit published in this final-correctness continuation. |
| 130 | No additional Node commit published in this continuation; no sync-only commits. |
| 131 | Engine 39 tracked modifications plus four scoped new files; Node two tracked modifications. Both HEADs equal their tracked remote tips. No continuation commit/push; unrelated builds, old logs and morphology work preserved. |
| 132 | B — foundation remains partially ready: critical service and healthy-baseline streaming degradation remain measured failures. |
| 133 | No, Foundation 3M may not begin. |
| 134 | Narrowly stabilize reliable structural/gameplay service and attribute remaining Resident/per-peer replication cost; keep dense Character capacity separate. Then close mixed pending-lifecycle/overload and graphical gaps, rerun affected memory/sanitizer gates and exact-diff security/CI before reconsidering 3M. |

## Direct answers: 44 questions

1. Reverse member destruction freed the benchmark Engine before its Session used
   the Engine during peer teardown. Two production exception-lifetime errors were
   also independently reproduced; see the ownership table.
2. Yes, production had equivalent lifetime hazards, although not the exact
   benchmark member layout: Server renderer scope and failed Engine construction.
3. Owners now outlive dependents; constructor failure disconnects callbacks using
   normal teardown before initialized members are destroyed.
4. Yes in the exercised paths: current 100-iteration 14-stage Local/TLS Node
   exception suites are clean, with production-equivalent partial construction.
5. The original Player CPU stall was repeated full-world native preflight and
   discarded journal serialization. Reliable structural backlog is a separate
   remaining critical-service delay.
6. Original CPU stall was apply/preflight dominated, not decode/render. Remaining
   multi-second service gaps are consistent with directly traced reliable backlog.
7. Latest measured worst event-loop gap is 108.894 ms, frame 95.473 ms; Character
   handler gap is 3,642.559 ms and Remote handler gap 3,809.294 ms. There is no
   accepted global legal-input responsiveness guarantee yet.
8. It fully applies without the old seconds-long CPU loop, but not safely within
   the requested complete critical-service budget. The maximum-unit gate stays open.
9. Same answer for the near-1-MiB unit: eventual full application is proven;
   acceptable Character/Remote responsiveness is not.
10. No, the 1-MiB/512-object limit is unchanged.
11. Yes, narrow native-preflight and render/journal work changed; no new structural
    queue, relevance authority or scheduler was introduced.
12. Existing message/snapshot/Poll bounds remain. Exact-repeat reuse preserves
    validation and live setters; no new wall-time apply budget is claimed.
13. Indefinite critical starvation is not proved, but seconds-long delay exists;
    the required non-starvation guarantee is unresolved.
14. No new unbounded queue was added; existing network/semantic bounds remain.
    Finite limits alone do not guarantee acceptable service latency.
15. No new queued creation exists. Current epoch/sequence/identity checks and
    fresh reload pass, including eviction before client convergence with accepted
    reliable delivery pending. The separate unselected-old-Leave stress gap remains.
16. Yes for the retained ten Windows 1,000-cycle Server-only profiles.
17. Yes for the retained Local/Node Linux 10,000-cycle small-entry profiles.
18. Historical slopes: Windows up to +3,956.908 B/s; Linux +2,313.684/-521.946.
    Current near-limit confirmation: Local +15,048.521, Node +3,065.057 B/s.
    Local's 1.55-MB steady-quarter drift is reported, not relabeled perfectly flat.
    These are measured finite envelopes, not an arbitrary-duration proof.
19. Yes: retained decoded documents have an explicit 16-MiB session/global ceiling.
20. No bypass was observed in exact boundary/cancel/Stop tests. Bounded transient
    parser allocations and total RSS are distinct from retained-document charges.
21. Yes in retained 500-peer proof: one shared acquisition, with cached reload.
22. Yes: one authoritative region, a new lifetime on reload, not one per peer.
23. Yes in completed evidence: the configured 8,192 selected cap is preserved.
24. Yes in retained cases, the latest TLS Node 32/100/500 confirmation, and the
    feasible 200-peer Local/Node differential. Convergence is not latency health.
25. Yes in completed structural/differential cases. This does not imply low latency.
26. Yes, the dense 500-Character baseline is already overloaded.
27. 200 connected peers, 50 active Characters, five-neighbor groups, 12 Hz owner
    input and ten root-motion Characters. A healthy 500-connected fixture is not
    established by reducing the count to 200.
28. Yes, all four corrected no-stream phases and both provider baseline phases
    satisfy the explicit health gate.
29. Local load reduces Character wall throughput about 66.2%; max publication gap
    grows from 205.696 to 1,075.46 ms, reload reaches 1,080.84 ms.
30. Node load reduces throughput about 67.1%; max gap reaches 1,133.61 ms.
31. Feasible differential max 1,133.61 ms; official Player Character handler max
    3,642.559 ms is a separate downstream service measure.
32. No, action requests/results/endings continue without current submission
    failures, but streaming/eviction result latency is unhealthy.
33. No in wall time. Produced and committed updates agree, but publication gaps
    exceed a second with streaming.
34. Yes in exercised safe-point/cell-crossing checks; no stale spatial membership
    or content identity contamination is observed.
35. No at the required latency envelope. It continues, but feasible differential
    ACK max reaches 573.309 ms and official near-limit ACK gap 3,820.055 ms.
36. Yes: current 100-call-per-phase and latest official maximum-content profiles
    finish without timeouts/errors/access violations; latency is independently bad.
37. No `0xc0000005` occurs in those completed current cases. This is scoped evidence,
    not an unlimited-load guarantee.
38. The measured 64-unit server overload is bounded, including RSS. Combined final
    Player/gameplay overload remains unproven.
39. Server backlog drains in 70 ticks. A giant official Player catch-up under that
    simultaneous overload has not been ruled out.
40. No stale resources were observed in completed focused/normal lifecycle proofs;
    the full requested 100-cycle mixture of pending-fan-out/RPC-active exits remains
    incomplete. Do not infer it from 100 normal cycles.
41. Yes for completed final supported native coverage: ASan/UBSan/LSan clean in
    all 49 Engine tests and four Node/TLS tests. The initial packaging failures
    were disk exhaustion; unchanged binaries/tests pass with disk-backed scratch.
42. Yes, latest Linux CGO-enabled Node Go race passes.
43. No. The measured healthy differential and critical-service gaps require B.
44. No. Do not start automatic residency on this unresolved substrate.

## Recommendation

Do not create another broad numbered networking foundation automatically. The
next work should be narrow structural/gameplay-service stabilization: attribute
and bound accepted reliable structural work's interference with critical messages,
and the remaining per-peer Resident replication cost. Keep the dense Character
capacity boundary a separate capacity task. Do not mask these with lower fidelity,
raised queue limits, disabled validation or automatic residency.

B — FOUNDATION 3L REMAINS PARTIALLY READY
