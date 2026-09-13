---
status: partial-validation
owner: runtime-networking
last_verified: 2026-09-12
---

# Foundation 3L.3 diagnostic validation ledger

## Overload qualification stopped at Part A (2026-09-12)

At published HEAD `5ada43a5773a96b1f0a97e9e6299baa6f420b762`, the
[payload-envelope review](ReliableOverloadQualification3L.md) finds legal codec
ceilings and a passing small-gameplay official fixture, but no sufficiently
defined ordinary qualified request/response/Event size and arrival envelope.
The task's explicit Part A stop applies; sustained overload, aggregate fairness,
recovery and production journal margin are **not measured** in this checkpoint.
No production or fixture change is retained. Existing 170-entry startup margin
does not qualify journal sufficiency under overload.

Native CI `34724984881` and complete GNS sanitizer CI `34724984872` are both
verified terminal **success** at this HEAD. This supersedes pending CI statements
at earlier checkpoints without closing the independent gameplay/client/scale,
overload, journal or current-source security gates. **B — FOUNDATION 3L PARTIALLY
READY; no 3M.** The linked report lists the contract decisions required to resume.

## Profiled GNS sanitizer closure attribution (2026-09-12)

The [independent attribution report](GameSessionReliableAdmissionAttribution.md)
reproduces `632b28ca4`'s profiled assertion in **20/20 unchanged runs**. Five
bounded traces show a successful 3,769-byte bootstrap reservation with
46,525–47,167 B peer credit already earned. No reservation was denied and neither
deferral counter needed to increment. The assertion over-constrained the
elapsed-time service contract; no production accounting defect was established.

The already-published test-only correction `8fa332416` passes **20/20** repeated
profiled runs, all **four** base GNS sanitizer fixtures, a separate profiled stage,
and the executed **12/12 production byte-admission matrix** with its analyzer.
ASan, UBSan and LSan remain enabled. Published GNS CI `34719712475` and Native CI
`34719712455` also passed at that code revision. **This GNS sanitizer gate is
closed**, superseding the historical not-run limitation below. Full gameplay/
client qualification, overload, journal margin, scale/physics and security gates
remain independent: **B — FOUNDATION 3L PARTIALLY READY; no 3M.**

## Hierarchical byte-admission validation (2026-09-12)

**B — FOUNDATION 3L PARTIALLY READY.** Policy and implementation are one coherent
slice on `foundation/3l-content-availability`, starting at
`e4739af176ce9c3c588343988893b72cfd46d8ac`. No merge, 3M, lane, ordering-domain,
wire-version, content-limit, GNS-buffer or secondary-repository change. Unrelated
morphology edits and sealed security evidence are excluded and preserved.

The approved [service contract](NetworkingReliableDeploymentContract.md) is RPC
p95/p99/max **150/250/500 ms**, Event ACK max gap **250 ms**, action-result max
**250 ms**, structural share at most **75%**, gameplay reserve at least **25%**,
with **no large-group exception**. Numeric compatibility is a necessary check,
not path certification. Incompatible low rates remain explicitly unqualified;
requesting compatibility on such a profile fails before runtime acquisition.

### Source and verification method

Local source is canonical. Scoped, timestamp-preserving archives went only to
`dockerbox` / `192.168.0.108`, under `C:\Sandbox\Codex`. The final native receipt
`build-3l3-worker/evidence/reliable-admission-v4-source.json` checks **69** scoped
native/test/build-input files against worker SHA-256. Official v3 and v4 have
identical production source; the final v4 test-only addition records Event ACK
gaps in the capacity matrix and includes its analyzer. No commit was used to
synchronize source. Native builds use four jobs, CTest two; the existing Clang 19
container is limited to four CPUs and 12 GiB. Incremental build/dependency caches
and unrelated worker workloads remain intact.

The actual boundary is Main-owned `ReliableByteAdmission` -> exact pre-acceptance
encoded group -> synchronous peer/global reservation -> existing scheduler
acceptance -> existing 3J Known/journal commit -> normal transport publication.
Deferral keeps only a scalar cost hint, not encoded payload. Sizing attempts still
consume selection work; no PreparedCommit or emitting cursor advances. Tests
cover fresh property values after a deferred encode, destroy/recreate, strict
replica application, rollback exactly once and disconnect generation cleanup.
The existing 8,192 selection / 65,536 planning limits and KI-007 remain unchanged.

Node remains at `f4440423c0701ff396f589fd51b6ce41edc63539` with its two pre-existing
uncommitted integration files preserved, not committed or changed by this slice.
Their local/worker SHA-256 values match: `integration/gargantuan/CMakeLists.txt`
`A27EB8CDC7C9868342F68C3A420FFFB33BE8712B489317128AD64A9591E31EDD` and
`internal/host/content_scale_integration_test.go`
`878F041125BB8AC7B5A68BDB4AA5677FFF115A2015EDED6BF1E2F74D11EDC4B6`.

### Production-accountant capacity matrix

**MEASURED:** `gargantuan_gns_capacity_benchmark --production-admission` calls the
same production integer accountant, with one IP-loopback GNS connection, 1,450,000
structural bytes and 60 small RPC, Event and action probes of each type. Backend
min=max is **2R**, structural credit **0.75R**. Whole opaque messages model group
cost but are not GRPL or RemoteManager; official correctness evidence is separate.
Both 2,000-byte and 524,288-byte maximum groups drain at every tested rate, with
905/905 and 183/183 reliable messages delivered and all 60 replies per type. Each
case accepts exactly 1,450,000 structural bytes. No oversize exception or splitting.

| R | 2,000-B RPC p50 / p95 / p99 / max ms | Max-group RPC p50 / p95 / p99 / max ms | Max-group Event ACK gap / action RTT max ms | Capacity classification |
| --- | --- | --- | --- | --- |
| 256 KiB/s | 11.047 / 14.657 / 15.029 / 15.060 | 14.075 / 867.813 / 969.075 / 1,012.720 | 1,101.320 / 1,012.720 | Unqualified |
| 512 KiB/s | 12.762 / 15.942 / 16.288 / 16.608 | 14.333 / 357.724 / 440.181 / 462.996 | 550.618 / 462.994 | Unqualified |
| 1 MiB/s | 12.641 / 15.581 / 16.616 / 17.558 | 13.749 / 119.244 / 180.724 / 219.985 | 306.372 / 219.982 | Unqualified |
| 2 MiB/s | 13.578 / 14.836 / 15.125 / 15.656 | 13.556 / 15.747 / 61.561 / 92.852 | 179.905 / 92.850 | Unqualified |
| 4 MiB/s | 13.131 / 15.078 / 15.545 / 16.131 | 13.666 / 15.638 / 17.665 / 35.680 | 120.777 / 35.678 | Unqualified |
| 8 MiB/s | 13.544 / 15.583 / 16.112 / 16.158 | 13.153 / 15.669 / 16.005 / 16.203 | 105.301 / 16.197 | Capacity-compatible candidate |

The 2/4 MiB/s probe samples passing is not grounds to override the worst-profile
compatibility arithmetic. Likewise a 2,000-byte probe does not qualify low rates
for the unchanged maximum group. ACK-gap values include the 100-ms probe cadence.

| R | Pending reliable high-water: small / max group B | Convergence: small / max group s | Max-group admission wait ms | Max-group size / backlog deferral attempts | Max-group deferred byte attempts |
| --- | ---: | ---: | ---: | ---: | ---: |
| 256 KiB/s | 2,354 / 524,292 | 7.38367 / 8.16885 | 2,668.516 | 4,000 / 862 | 1,959,790,048 |
| 512 KiB/s | 3,424 / 524,292 | 3.69476 / 4.08910 | 1,335.052 | 1,993 / 450 | 977,207,920 |
| 1 MiB/s | 3,508 / 524,292 | 1.85108 / 2.04602 | 667.887 | 966 / 208 | 473,043,200 |
| 2 MiB/s | 8,255 / 524,292 | 0.93027 / 1.02275 | 333.828 | 512 / 110 | 250,865,904 |
| 4 MiB/s | 13,831 / 524,292 | 0.467702 / 0.513593 | 168.346 | 248 / 54 | 121,668,672 |
| 8 MiB/s | 34,449 / 524,292 | 0.239158 / 0.261796 | 85.490 | 125 / 28 | 61,481,488 |

Deferred bytes count repeated **attempts**, not retained memory. The laboratory
driver asks every poll; GameSession instead retains a scalar required-cost hint
and does not repeatedly encode while credit is short. Every max-group case stays
at accountant exposure and global-credit high-water **524,288 B**; GNS may report
524,292 B including its own protocol overhead. No exact GNS memory-cap claim.
Credit high-water is <=524,288 in all twelve cases. Full data, including small-
group Event/action and credit/size attempts, is retained in
`byte-admission-v4-final-analysis.json`, reproducible with
`tests/AnalyzeProductionByteAdmission.ps1 -CapacityLog <log> -OutputFile <json>`.

Historical backend comparisons at 256 KiB/s: no admission RPC max **5,708 ms**;
small-group experimental 75% **52.82 ms / 7.316 s** convergence, 50%
**52.94 ms / 10.970 s**. New small-group production accounting is
**15.060 ms / 7.384 s**, but backend=512 KiB/s, not 256 KiB/s. Finite accumulated
maximum-group credit intentionally trades larger structural wait for intact
atomic groups; it does not promise small-message latency during impossible
low-rate serialization.

### Fairness, denial and validation

**MEASURED deterministic accounting:** 500 continuously eligible peer identities,
8 MiB/s R, A=500R, zero initial credit, full 512 KiB group per peer, rotating
consideration and 1-ms simulated accounting clock. All 500 serviced by **583 ms**;
44,336 logical bytes / 500 peer records; Qg=262,700,288. This is not 500 actual
network connections or production peer convergence. A separate adversarial
small-first peer cannot steal the refill earmarked by a locally eligible large
peer. Six profile accumulation tests, 13 invalid resource profiles, unavailable/
missing feedback, high-water recovery, stale generation, repeated same-time
passes, double rollback/commit denial and lifecycle cleanup pass. Pure full-group
credit waits at the six R values are 2,666.667 / 1,333.334 / 666.667 / 333.334 /
166.667 / 83.334 ms. These are calculated from a deterministic clock and tested,
not observed Internet service times.

| Validation | Exact result / scope |
| --- | --- |
| MSVC Release current-source targeted suite | **10/10**, 18.69 s; Foundation, replication, relevance/KI-007/dependency/bootstrap, GameSession, scheduler, physics and four GNS suites |
| Clang 19 ASan / UBSan / LSan | **7/7**, 58.76 s, `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`, `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`; separate late-handoff also passes |
| Profile-enabled lifecycle | Failed structural acceptance refunds; full action/Remote/root-motion/Character lifecycle and disconnect pass; late same-step sends preserved |
| GNS production-accountant matrix | **12/12**, all reliable messages/replies delivered; analyzer validates effective profile, credit/exposure bounds and candidate probe targets |
| ServerHost early rejection | **4/4**: partial CLI, zero R, unfunded N*R, incompatible requested latency class; exit 2 before runtime acquisition |
| Official Local / Node | **2/2**, eight cycles and 100 RPCs each, details below |
| Documentation | Isolated unchanged Astro site **19 pages / 3.86 s**, existing missing-404-entry warning; morphology excluded. 44 local Markdown link targets resolve; developer Markdown is outside Astro |
| Source review | Complete **32-path** explicit diff plus new-file inventory; ordinary source/contract review, **not** a completed current-source Security Diff Scan |
| Pre-change branch CI | `e4739af17`, run `34676794324`: terminal **success**; not evidence for the new implementation |
| New publication CI | Must be checked after push; not presumed green from worker validation |

Evidence is under `build-3l3-worker/evidence/`: v4 source receipt, v4 CTest and
Linux sanitizer logs, v4-final matrix log/analysis, official v3 logs/analysis, and
`reliable-admission-review-inventory.{json,patch}`. Source inventory enumerates
all 32 scoped changed/new paths and excluded unrelated morphology; the sealed
prior security artifact is not modified or relabeled. Final full security scan,
full supported CTest, fresh 90-case physics matrix, real 500-peer byte-profile
convergence, overload/recovery and production journal-retention margin are
**not run/measured** in this slice. Earlier physics/scale evidence is preserved,
not relabeled as a current profile run. GNS observer/matrix sanitizer execution
is **not run**: the existing Clang image lacks pkg-config, OpenSSL/protobuf headers
and protoc. The production accountant/Coordinator/GameSession are covered by the
seven sanitizer cases; backend-only native evidence is not sanitizer evidence.

One instrumentation provenance correction is retained: timestamp-preserving v4
sync placed the changed matrix source older than an object built during v3 work.
The first v4 matrix therefore lacked the new Event-gap field. Only that test file
differed (plus its new analyzer); production hashes were unchanged. Its worker
timestamp was refreshed, the target actually recompiled, and all twelve cases
reran into **v4-final**. The final analyzer requires the field. No old result is
claimed as final Event-gap evidence. A mixed-CRT LNK4098 warning remains in the
native GNS capacity target's link log; no linker suppression was added. Early
CLI probing first targeted an incomplete executable directory (missing DLLs);
the four reported checks used the complete `ServerRuntimeDistribution` instead.

### Official product-path measurements

**MEASURED:** `TestOfficialContentMemorySoak/near-max`, Local and Node, eight
cycles each, 512 representative Parts, name padding 1,536, payload 1,048,197 bytes,
100 RPCs each. First residency includes a real official Player, eviction and fresh
reload. Remaining churn runs after that Player disconnects; this is **not** eight
continuous connected-client pressure cycles. Both cases passed, 13.18/13.56 s
(29.32 s combined including setup). Effective Server profile R=A=8,388,608 B/s,
N=1, backend min=max=16,777,216 B/s, S=750/1000, Bp=Bg=524,288, E=262,176,
Qp=Qg=1,048,640. The official Player's outgoing backend remains 262,144 B/s.

| Metric | Local | Node |
| --- | ---: | ---: |
| RPC samples | 100 | 100 |
| RPC p50 / p95 / p99 / max ms (Luau observation) | 33.127 / 35.307 / 72.058 / 73.316 | 33.492 / 43.713 / 74.723 / 75.801 |
| RPC timeout / error / crash | 0 / 0 / 0 | 0 / 0 / 0 |
| GNS pending reliable peak B | 469,320 | 469,320 |
| GNS queue estimate at peak ms | 27.933 | 27.933 |
| Response queue estimate p99 / max ms | 25.976 / 27.596 | 25.976 / 27.798 |
| Handler -> produced response max ms | 0.059 | 0.207 |
| Response produced -> scheduler acceptance max ms | 0.009 | 0.022 |
| Acceptance -> GNS call begins max ms | 4.961 | 0.119 |
| Player receive -> callback max ms | 23.202 | 22.900 |
| Player callback -> RPC completion max ms | 2.434 | 2.782 |
| Reliable Event ACK samples | 71 | 76 |
| Event ACK RTT p95 / p99 / max ms | 50.506 / 68.048 / 72.111 | 37.467 / 49.882 / 59.479 |
| Event ACK max service gap ms (Luau) | 100.787 | 83.469 |
| RemoteEvent max receive gap ms (GNS poll) | 100.288 | 82.970 |
| RPC response max receive gap ms (GNS poll) | 66.645 | 66.702 |
| One action request-accepted -> client result-handled ms | 47.667 | 41.270 |
| Player frame interval p50 / p95 / p99 / max ms | 16.624 / 17.893 / 18.214 / 25.895 | 16.619 / 17.624 / 18.560 / 25.899 |
| Player event-loop max service gap ms | 39.504 | 39.998 |
| Player Character max service gap ms | 72.118 | 74.026 |
| Player combined Remote max service gap ms | 83.069 | 74.024 |
| Client structural apply total max ms | 20.018 | 19.766 |
| First full 512-object visibility from fixture start ms | 351.357 | 337.666 |
| Eviction / fresh full reload observation from fixture start ms | 966.330 / 1,803.848 | 949.952 / 1,786.232 |
| Structural accepted bytes, including adapter | 1,881,755 | 1,885,057 |
| Credit / size / backlog / feedback deferral attempts | 1 / 1 / 0 / 0 | 1 / 1 / 0 / 0 |
| Deferred byte attempts (not unique backlog bytes) | 11,942 | 11,942 |
| Maximum sampled byte-admission wait ms | 14.359 | 16.754 |
| Accountant peer/global exposure high-water B | 469,316 | 469,316 |
| Peer/global credit high-water B | 524,288 | 524,288 |
| Retained accounting peers after disconnect | 0 | 0 |

Action has **one** matched request/result, not a p99 distribution or sustained
worst-backlog action qualification. Character/root-motion recipient p95/p99 are
**not measured** here: the existing Player counters retain maxima. Do not compute
quantiles of the running-max columns. First/full/reload times use one client-local
fixture clock, not server-commit-to-client convergence. Exact commit-relative
convergence and current 200/500-peer production distributions are **not measured**.

Reproduce RPC/queue analysis using `tests/AnalyzeTransportService.ps1` on each
`near-max-{local,node}-{server,player}.log` in
`build-3l3-worker/evidence/byte-admission-v3-official/`. All four service traces
have zero invalid/dropped records. Player frame summaries use complete
`[Runtime:PlayerFrame]` CSV rows, milliseconds = ns/1e6, quantile index
floor((count-1)*fraction); service gaps use the maximum, not quantiles of maxima.
Action pairs are the single player-local `SchedulerAccepted kind=4` and
`ClientHandled kind=7`; no inter-host timestamp subtraction is used.

**Historical comparison, not a single-variable counterfactual:** the inherited
256 KiB/s official run had about 1.45 MB pending, 5.3 s queue residence, 5.5 s RPC
delay and a timeout. The new runs remove that symptom in this fixture, but change
both admission and explicit backend capacity. They do not prove that admission
alone provides the same result at 256 KiB/s.

### Bounds, qualification and remaining owners

The accountant owns at most N fixed peer records, one global refill earmark and
one synchronous reservation; no peer*object frontier or deferred frame queue.
Credit is finite elapsed-time S*R/S*A, not credit per tick/flush. Admission cannot
raise exposure above Qp-E / Qg-NE, including same-step reservations. Provider
protocol bytes, already-unacknowledged storage and additional gameplay are not
misrepresented as that exact threshold. Existing provider/scheduler resource
ceilings and terminal backpressure still apply. Unlimited gameplay is unqualified.

Low-rate maximum-group results must not be replaced by small-message success.
With this E/Q/nonqueue allowance, 256/512 KiB/s and 1/2/4 MiB/s fail necessary
compatibility; 8 MiB/s is only a **candidate**. Request path, actual service,
RTT/loss, host pauses and sustained offered gameplay still require qualification.
In particular, the unchanged 256 KiB/s Player request path is not qualified for
arbitrary codec-ceiling RPC requests within a 100-ms nonqueue allowance. Existing
Remote payload/concurrency limits are not silently reduced to hide that fact.

No lane is implemented or demonstrated necessary by the qualified-candidate
small-gameplay fixture. Mandatory low-rate plus maximum-group plus these latency
targets would require a separate ordering-domain assessment; those profiles are
unqualified here. Next: qualify the full declared gameplay burst/request path and
client service under near-max complete groups, then sustained overload/recovery,
500-peer production fairness/convergence and journal retention. Do not infer
current production journal margin from synthetic byte-accounting fairness.
Current-source security and publication CI remain explicit gates, not inherited
from a sealed prior checkpoint or a passing pre-change workflow.

## Historical first implementation checkpoint (2026-09-12)

The user approved RPC p95/p99/max 150/250/500 ms, Event ACK/action max 250 ms,
75% structural share with >=25% gameplay reserve, no large-group exception, and
unqualified low-rate compatibility. The preflight below is historical. Work
continues on `foundation/3l-content-availability` over published `e4739af17`.

Implementation now in validation: trusted startup R/A/N profile, finite
elapsed-time peer/global structural credit, backlog feedback, fair global refill
earmark, exact pre-acceptance sizing and encoded-byte reuse, byte deferral without
Known/sequence advancement, failed-admission refund and generation-safe cleanup.
No new payload queue, lane, wire version, public priority, content bound, increased
3J/planning limit, merge or 3M. Unrelated morphology edits remain excluded.

First exact worker receipt: 67 scoped native files matched local SHA-256.
The first MSVC build regenerated its CMake glob but the in-flight MSBuild link
did not yet load the new profile translation unit; restarting the incremental
build resolved that source-list issue without a source workaround. Four targeted
MSVC Release tests then passed **4/4 in 14.38 s**: scheduler contracts, replication
relevance/dependencies, GameSession, and real GNS transport. This includes pure
elapsed-time/fairness/profile tests and exact pre-acceptance mutation/journal/
recreation tests, but precedes the added profile-enabled lifecycle/official matrix.

Final profile-enabled MSVC/sanitizer, official Local/Node, backend matrix, docs
and current-source CI results are still pending at this checkpoint. No security,
overload, journal-retention or latency qualification is inferred from 4/4.
**B — FOUNDATION 3L PARTIALLY READY.**

## Reliable service-class preflight (2026-09-12)

**B — FOUNDATION 3L PARTIALLY READY.** Local and published branch HEAD both remain
`e4739af176ce9c3c588343988893b72cfd46d8ac`. The follow-up implementation request's
policy stop condition was reached before production edits. The
[deployment contract](NetworkingReliableDeploymentContract.md#historical-service-class-selection-preflight-2026-09-12)
records the candidate and the required decision, not a newly installed policy.
The existing 250/500-ms RPC and 250-ms Event/action thresholds are explicitly
fixture health gates in 3L.2 validation. Current networking/host contracts do not
independently define the requested ordinary-gameplay latency or burst workload.

Recomputed arithmetic below assumes application service equals the displayed
rate. It is **calculated**, not a new backend measurement or an Internet guarantee.
G=524,288 includes adapter framing; E=262,176 conservatively covers one Remote
codec-ceiling message plus framing, not arbitrary concurrent gameplay bursts.

| Rate | G/R ideal serialization ms | (G+E)/R ideal drain ms |
| --- | ---: | ---: |
| 256 KiB/s | 2,000 | 3,000.122 |
| 512 KiB/s | 1,000 | 1,500.061 |
| 1 MiB/s | 500 | 750.031 |
| 2 MiB/s | 250 | 375.015 |
| 4 MiB/s | 125 | 187.508 |

These omit request-path service, RTT, packet/retransmission overhead, host pauses
and additional gameplay bursts. They cannot by themselves qualify even the
4 MiB/s profile. Nor do they make the five requested test rates mandatory
production latency-qualified deployments. The ceiling fixture is eight ancestry
operations; the large Player/Character sample is separately 123,183 bytes.

No implementation, new backend matrix, official Local/Node differential, byte
admission fairness, overload or recovery result exists in this preflight:
**not measured**. Prior 10/10 MSVC, 7/7 Clang ASan/UBSan/LSan plus late-handoff,
18-case model and 19-page docs results below remain evidence for unchanged code,
not rerun or relabeled as admission validation. No new security closure is claimed.
CI reads for `e4739af17` (`34676794324`) and `108200d07` (`34675486038`) remain
in progress at this checkpoint; neither is terminal green. Morphology edits and
sealed evidence remain untouched. No merge, production commit, push or 3M.
Preflight documentation verification: eight local link targets in the two edited
documents resolve, `git diff --check` passes, and the isolated unchanged docs site
builds 19 pages in 2.67 s with the existing missing-404-entry warning. Developer
Markdown is outside that site build; this is not native/runtime validation.

## Diagnostic publication and deployment contract (2026-09-12)

**B — FOUNDATION 3L PARTIALLY READY.** The completed 12-file diagnostic slice was
committed and normally pushed as `108200d077a8830ba2fd5fc96e518bdde6baf0c2` on
`foundation/3l-content-availability`. Remote HEAD was verified equal. Only the
explicit diagnostic/documentation paths were staged; morphology and untracked
build/evidence files were excluded. No production source/config changes or
secondary repository edits. No force push, merge or 3M.

Before that push, 59 affected committed native/test/script files were checked
against their Git blobs and exact worker SHA-256 values. Two previously local-only
analysis scripts were missing on the worker and copied explicitly; all 59 then
matched. Committed-source MSVC targeted native/network tests passed **10/10 in
14.45 s**. The complete ten-case rate/admission matrix passed again, checked by
the committed analyzer; all 905/905 messages and 60 replies of each probe type
arrived in every case. Clang 19 ASan/UBSan/LSan core passed **7/7 in 57.03 s** with
leak detection, plus the separate late-handoff test. These are not sanitizer
results for the unchanged GNS capacity executable or scale byte observer.
The isolated committed docs-site content built **19 pages in 3.34 s**; morphology
was not included. Developer Markdown is reviewed separately, not an Astro route.

Artifacts remain local under `build-3l3-worker/evidence/`:
`envelope-publish-source.json`, `envelope-publish-native.log`,
`envelope-publish-matrix.log`, `envelope-publish-analysis.json`, and
`envelope-publish-linux-sanitizers.log`. The earlier `service-envelope/` evidence
and sealed security artifact are preserved unchanged.

The [new design contract](NetworkingReliableDeploymentContract.md) is based on
source inspection of ServerHost, GameSession, network limits, scheduler and GNS
configuration, plus the actual group tests. It does not install a production
profile or byte controller. There is no current aggregate-rate option; the
proposed reservation contract requires `N*R <= A`, hierarchical finite peer/global
credit, separately bounded gameplay reserve, and queue-window feasibility.
Omitted operator configuration remains explicitly unqualified legacy behavior,
not a universal rate guarantee. No cross-repository ownership change is needed
for a single Server process; machine-wide multi-process allocation stays with
the operator/orchestrator.

### Actual group observations

`ReliableEnvelopeContractFixture.hpp` records a frame only when existing
`PlanningCompletedGroups == 1`; every measured case checks strict application,
unchanged Known before acceptance, exact scheduler commit and bounded disposal.
No production observer, callbacks, queue, budget or group formation was added.
The fixture is isolated on purpose: full 200/500-peer **group frequency** and
official-network group partition remain **not measured**. Prior frame data is
not upgraded into group evidence.

Native `deployment-contract-reserve` final results, GRPL bytes including
36-byte header (add 32 adapter bytes for admission):

| Case | Samples | p50 / p95 / p99 / max bytes | Operations |
| --- | ---: | --- | ---: |
| Ordinary Part | 128 | 338 / 339 / 339 / 339 | 1 |
| Ordinary authoritative destruction | 128 | 45 / 45 / 45 / 45 | 1 |
| Player/Character | 1 | 334 / 334 / 334 / 334 | 2 |
| 32-node ancestry group | 1 | 2,788 / 2,788 / 2,788 / 2,788 | 32 |
| KI-007 replace/remove | 1 | 157 / 157 / 157 / 157 | 5 |
| Fresh target/restore | 1 | 440 / 440 / 440 / 440 | 5 |
| KI-007 clear/remove | 1 | 125 / 125 / 125 / 125 | 5 |
| 32-node subtree eviction | 1 | 324 / 324 / 324 / 324 | 32 |

Legal worst-payload cases are separate, not ordinary production sizing authority:
123,183 B/2 ops for 60 KiB Player/Character names; 459,292 B/7 ops for seven long-
named ancestors; **524,256 B/8 ops** at the exact GNS complete-message ceiling;
524,900 B/8 ops for eight full-length names. The last group is rejected by the
524,256-byte planner allowance without Known advance, yet remains valid under
the larger bounded GRPL codec and strict replica. This proves a finite supported
transport ceiling distinct from legal object/schema state and the 8 MiB codec
ceiling. No group was split to produce a passing observation.

The additional official-style normal baseline is 1,867 B/5 ops with **five**
completed groups in the measured native run. It is not an atomic-group sample.
Identity order and existing Known prerequisites can make Player/Character enter
together or in separate dependency-safe ordered groups. The dedicated isolated
case proves the possible 123,183-byte group, without assuming all baselines use it.
Quantiles on one sample describe only that observation, not population tails.

### Contract/test follow-up validation

- Test-only numeric profile model: **18/18** positive/negative cases, including
  aggregate underfunding, overflow, queue/burst bounds, impossible low-rate class
  and full Remote codec-ceiling reserve. `IsValid()` is not deployment qualification.
- Final MSVC Release targeted native/network CTest: **10/10, 14.27 s**; independent
  relevance executable also passed and retained the complete group observations.
- Final Clang 19 ASan/UBSan/LSan core CTest: **7/7, 57.31 s**, with
  `detect_leaks=1`, sanitizer halt-on-error, followed by a passing late-handoff
  case. The new profile/group code is included. GNS matrix/scale-observer code
  was not changed in this follow-up and has no new sanitizer claim.
- Docs-site build: **19 pages, 2.50 s**, same isolated committed site content;
  the existing missing-404-entry warning remains. The five developer/issue
  documents' **30 local Markdown link targets** were separately verified.
- `git diff --check` passes. Production `src`, `include`, assets and CMake have
  no changes relative to the published diagnostic source. Prior physics matrix,
  provider/scale, official Local/Node and memory evidence is retained, not rerun
  or represented as new production improvement. No current-source security scan
  or full CI result is implied by this targeted pass.

Final worker logs are retained under `build-3l3-worker/evidence/` as
`deployment-contract-reserve-{build,groups,native,linux-sanitizers}.log`.
The published-source receipt records final committed blobs and worker SHA-256
verification separately from the prior diagnostic receipt. Local working trees
remain canonical; the worker supplies build/test results, never replacement source.

### Remaining gate

Full 512 KiB group compatibility at 256 KiB/s requires at least 2.000 s of ideal
serialization; 4 KiB bounded gameplay burst makes the illustrative Q/R 2.015625 s.
That is a small-probe reserve, not full Remote compatibility. The existing
256 KiB Remote codec ceiling plus adapter requires a conservative 262,176-byte
single-message reserve. With a maximum structural group, Q/R becomes 3.000122 s;
the model rejects 2.1 s and accepts 3.1 s numerically. Exact maximum attainable
Remote payload and overlapping production burst distribution are **not measured**.
No Remote payload, request or rate semantics were reduced.
The proposed policy waits for finite credit and rejects an impossible claimed
profile, not legal content silently. A slower compatibility class is not a
passing 3L interactive envelope. If full compatibility, low bandwidth and a
subsecond service class are all mandatory, a separate ordering-domain assessment
is justified. No lanes are implemented here.

Production byte admission, actual rate/overhead mapping, aggregate fairness,
strict backend accounting slack, official Local/Node correction, client service,
overload/recovery, production journal margin and current-source security remain
open. Prior physics, shared acquisition/admission, selection/planning-cap evidence
is unchanged; no broad workload rerun is represented as a production improvement.
The `1bbcd9489` Native CI run `34672410169` has now reached terminal success for
both Windows and Linux. The published diagnostic run `34675486038` is still in
progress at this checkpoint; this is not current-source CI closure. Docs CI/Pages
is main-only, so this branch push does not deploy Pages.

## Reliable service envelope assessment (2026-09-12)

**B — FOUNDATION 3L PARTIALLY READY.** Diagnostic/design slice over published
`1bbcd948991a398c7a0613dcfea90668e10b68db`, branch
`foundation/3l-content-availability`. Production source is unchanged. No new
rate, buffer, lane, wire, client queue, semantic authority or 3J/planning policy.
No secondary repository edits, merge or 3M. Morphology files remain excluded.
This slice is local/uncommitted for the explicit contract decision described in
[NetworkingReliableServiceEnvelope.md](NetworkingReliableServiceEnvelope.md).

### Reproducibility and measurement limits

Worker is verified `dockerbox` / HostPC, 24 logical processors, 32 GiB RAM;
four build jobs and two test jobs, retained incremental caches. Runs do not
overlap native builds or sanitizer compilation. Canonical 200-peer/50-Character,
neighborhood-5, input-period-5 workload is unchanged; Local 500-peer uses the same
50 active Characters. Node is the existing TLS provider fixture, not an Engine
transport policy change. Each scale phase runs 301 ticks and all four phases
(baseline/load/evict/reload) are retained. The source remains canonical locally.

`GARGANTUAN_STRUCTURAL_BYTES=1` enables a **test-only** scoped observer. Frame
size/operation distributions cover all recipients; detailed re-encoding and
payload breakdown sample the first, real-client recipient. Five vectors cap at
32,768 doubles each; three arrays hold 1,201 tick counters each. Maximum sample
payload is 1,339,544 bytes plus container/object overhead; transient codec scratch
is bounded by the existing legal frame. Bounds fail the fixture rather than
silently dropping samples. No production diagnostic, public control or queue
was introduced. Codecs re-encode individual operations and assert their sizes
sum exactly to the original frame. This observer adds test work: these timings
are **not** a clean optimization counterfactual. Production code did not change.

Quantiles use the existing deterministic floor-rank convention. Frames are
received accepted frames on the lossless simulator; source structural encoded
bytes/tick are measured separately from received total reliable bytes/tick.
Those boundaries are not conflated. All measured selected operations equal
committed operations and no peer failed. Total reliable *admission* bytes/tick,
actual generic planner-group percentiles, official-group decomposition and
packet-level bandwidth overhead remain **not measured**. Console floating-point
quantiles round large byte values; exact integer per-phase totals are retained.

The diagnostic `flatEnterGroup` label denotes a single encoded publication
conditional on its parent already being accepted, **not** the planner's actual
group partition. The first child may bring its unknown parent. Canonical decoded
Folder/Part costs imply a conservative 429-byte complete Enter group for this
flat graph; this is **INFERRED**, not a measured generic group maximum. Removal
samples retain the whole removal frame as a conservative complete envelope.

### Canonical load byte distribution — MEASURED

Entries below are p50 / p95 / p99 / max unless stated. No byte admission is active.

| Metric | Control 200 | Local 200 | Node 200 | Local 500 |
| --- | --- | --- | --- | --- |
| Observed GRPL frames | 200 | 400 | 400 | 1,000 |
| Operations/frame | 12/13/14/14 | 14/512/512/512 | same | 12/512/512/512 |
| Bytes/frame | 864/933/1,002/1,002 | 1,002/154,851/154,851/154,851 | same | 864/154,851/154,851/154,851 |
| Frame bytes/operation | 72/72/72/72 | 72/302.443/302.443/302.443 | same | 72.273/302.443/302.443/302.443 |
| Single-Part-after-parent bytes | no content samples | 339/339/339/339 | same | same |
| Global encoded GRPL bytes/tick | 0/0/0/175,560 | 0/0/~2,477,620/~2,477,620 | same | 0/~2,477,620/~2,477,620/~2,477,620 |
| Global received reliable bytes/tick | 66/21,666/36,243/237,550 | 66/36,243/~2,477,740/~2,477,740 | same | 66/~2,477,680/~2,477,740/~2,567,860 |
| Consecutive nonzero structural receive ticks | 1 | 13 | 13 | 32 |
| Total GRPL bytes, load | 175,560 | 31,145,760 | 31,145,760 | 77,826,105 |
| Total operations, load | 2,440 | 104,840 | 104,840 | 261,545 |
| Total GRPL bytes, eviction | 175,560 | 1,104,396 | 1,104,396 | 2,767,067 |
| Total GRPL bytes, reload | 192,674 | 31,162,874 | 31,162,874 | 77,791,674 |

The 512-content-operation frame contributes 109,354 reflected-property bytes,
33,794 identity/class/parent/count bytes, 11,667 name bytes and 36 framing bytes.
Attributes/extensions/custom state/tags contribute zero entry payload bytes in
this package. Known-object ordinary updates are separate (828 bytes in the first
200-peer recipient's accompanying 12-op frame; 759 in the 500-peer case).
The complete flat removal frame is 4,644 bytes / 512 operations.

Official source inspection explains the different ~450 kB frames: 492 new Parts
each carry 1,536 padding characters in Name. Two publications add 1,511,424 bytes
of padding, **83.35%** of the retained 1,813,406-byte four-frame total. An all-padded
256-op frame has 393,216 padding bytes and 76,068 other bytes. The codec emits
Name once. This is authored fixture payload, not a redundant serialization claim.
The predicted padded Part frame is 1,869 bytes; that prediction is not a measured
official-group quantile. The unrelated property-heavy fixture is not used here.

Extended native hard-reference tests measure a 123,183-byte/two-op planned group
and a 123,347-byte/four-op reference-path frame with 60 KiB Player/Character names.
The 40 KiB attempt does not advance Known; the larger valid allowance preserves
strict replica validation and explicit scheduler acceptance. All pending cleanup
then drains. The initial diagnostic harness asserted before planned disposal
finished and failed its new final assertion; it was corrected to service existing
bounded cleanup. No production fix or weakened invariant was needed. Final
`service-atomic-v3` passes both paths, including the 24 KiB cases and KI-007 suite.

### Rate × admission — MEASURED, backend-only

`gargantuan_gns_capacity_benchmark --envelope`, `service-envelope-v3`: same pinned
GNS localhost/ordinary flags/FIFO; 1,450,000 structural bytes in 725 whole opaque
2,000-byte messages, then ongoing tiny RPC/event/action probes. Not real GRPL or
RemoteManager semantics. All ten cases receive **905/905 reliable messages,
1,470,700/1,470,700 server reliable bytes, 60/60 RPC, 60/60 event ACKs and 60/60
action results**. No reliable drop, timeout or crash. Each case is deadline-bounded.

Admission is 50% or 75% of nominal `S`, with credit capped at 50 ms of `S` and
pending reliable+unreliable threshold at 100 ms of `S`. Credit is elapsed-time
shared, not renewed per flush; no source candidate/payload queue. Status and
latency sample capacities remain fixed. These are test hypotheses, not selected
product thresholds. A global server egress contract is not exercised.

| Nominal KiB/s | Structural % | Pending reliable peak B | RPC p99/max ms | Structural receive complete s | Structural B/s (finite burst average) | Max admitted structural B/10ms | Drain after last structural admission ms |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 256 | 50 | 10,876 | 16.08 / 52.94 | 10.9708 | 132,169 | 14,000 | 7.03 |
| 256 | 75 | 12,012 | 32.17 / 52.82 | 7.3160 | 198,197 | 14,000 | 6.01 |
| 512 | 50 | 26,026 | 17.07 / 49.98 | 5.4388 | 266,602 | 28,000 | 6.49 |
| 512 | 75 | 26,026 | 33.85 / 52.53 | 3.6295 | 399,508 | 28,000 | 7.50 |
| 1,024 | 50 | 49,699 | 19.11 / 53.92 | 2.6735 | 542,367 | 56,000 | 7.61 |
| 1,024 | 75 | 49,699 | 33.75 / 57.89 | 1.7857 | 811,990 | 58,000 | 8.12 |
| 2,048 | 50 | 102,617 | 16.07 / 56.41 | 1.2907 | ~1,123,440 | 114,000 | 7.50 |
| 2,048 | 75 | 102,266 | 35.88 / 59.72 | 0.8644 | ~1,677,540 | 118,000 | 8.61 |
| 4,096 | 50 | 205,855 | 16.62 / 56.75 | 0.5999 | ~2,417,190 | 228,000 | 6.50 |
| 4,096 | 75 | 208,372 | 38.00 / 58.78 | 0.4014 | ~3,612,280 | 238,000 | 6.37 |

Per-case RPC p50/p95 range 11.02–14.05 / 15.06–16.19 ms; the full values and
event/action percentiles are preserved in `analysis.json`. Event ACK max is
49.98–59.72 ms; action-result max 49.98–59.72 ms. These are backend round trips,
not owner-action validation latency. Unreliable probes keep their best-effort
semantics; observed max latency stays below 55 ms. Fixture average B/s over its
entire six-second gameplay period must not be mistaken for the separate finite
structural drain throughput. Initial credit explains finite-burst throughput
above the sustained structural token rate.

At unchanged 256 KiB/s, retained unpaced 1.45 MB burst -> RPC was 5.708 s.
The 75% case reduces it to 52.82 ms while structure takes 7.316 s rather than
5.709 s (about 28% longer). The 50% case takes 10.971 s (about 92% longer).
This is a targeted causal admission experiment, not an official correction.

`--envelope-shortfall`: nominal admission profile 1 MiB/s at 75%, actual fixed
backend 256 KiB/s. Pending high 104,915 B against 104,857 B admission threshold
(58 B backend accounting/framing difference); 2,860 guard deferrals; RPC
p50/p95/p99/max **416.73/421.54/422.98/423.29 ms**. Event max 423.29, action max
426.76 ms. All 905 messages/180 gameplay responses arrive. Structural convergence
5.813 s, last-admission-to-receive drain 414.34 ms. This validates exercised
backpressure but disproves a nominal-rate-only latency guarantee. The threshold
is not reported as an exact backend hard byte cap.

### Canonical convergence and unchanged blockers

No candidate byte admission is installed in these scale runs. Load maxima:
Control/Local/Node p99 **3.735/33.279/34.561 ms**, Character accepted states/s
**8,263.54/7,498.38/7,424.28**, raw recipient maxima **202.38/660.20/698.53 ms**.
The diagnostic observer adds re-encoding work; do not attribute these differences
to a production optimization. Control exits 0; streaming exits 1 because existing
gameplay health gates still fail, despite successful structural convergence and
zero Remote errors. Node integration's FAIL is that propagated health failure,
not a provider acquisition error. Official Local/Node was not rerun: its unchanged
production result remains the retained failure (one timeout each, ~5.53 s response
availability). Backend probes do not supersede it.

Local 500 load/evict/reload all converge: **158/212/158 ticks**, **4,406.06 /
3,873.18 /4,316.56 ms**. Prior equivalent values were 4,399.78/3,868.62/4,320.81 ms;
this is no material convergence correction. Character/root raw load max 814.61 ms,
eviction 305.93, reload 430.17. Exactly one acquisition and initial admission,
fresh reload identity/admission 2, exact 8,192 selected maximum and 65,536 planning
maximum remain intact. Journal failures zero; maximum lag 16,214 /16,384 retains
only **170 entries** margin. No new journal or transport guarantee is claimed.

### Validation, artifacts and implementation gate

MSVC Release targeted CTest passes **7/7** in 14.29 s; the three precisely named
Remote/Character/GameSession real-transport entries additionally pass **3/3** in
0.27 s. Full reference/planned relevance tests, including KI-007, bootstrap,
byte rejection and the enlarged hard group, pass. Ten matrix cases and one
shortfall case pass. Four canonical byte fixtures complete their observations;
streaming health remains FAIL as above. No production physics change invalidates
the retained 90-case matrix.

Current-source Clang 19 ASan/UBSan/LSan core CTest passes **7/7** in **57.04 s**,
with `detect_leaks=1:halt_on_error=1`, plus the separate late-handoff fixture.
This includes the enlarged reference/planned hard-group regression. The backend
capacity executable and scale byte observer were not run under sanitizers; no
GNS-on sanitizer closure is claimed (the pinned upstream UBSan issue remains as
documented). No new production mechanism was introduced. The isolated published
documentation-site snapshot builds **19 pages in 2.20 s** with bundled Node 24;
the new developer Markdown assessment/ledger are manually reviewed, not falsely
counted as Astro routes. Unrelated morphology edits are excluded from that build.

The published `1bbcd9489` [Native CI run](https://github.com/gmoddev/gargantuan/actions/runs/34672410169)
has terminal-success Windows and in-progress Linux at this checkpoint; the
workflow is **not green**. This uncommitted diagnostic slice has no new CI run or
Pages deployment. Final-source security/CI are not claimed. No normal rate policy
or official Local/Node correction was deployed.

Node fixture provenance is existing local HEAD
`f4440423c0701ff396f589fd51b6ce41edc63539` plus its preserved uncommitted
`integration/gargantuan/CMakeLists.txt` and
`internal/host/content_scale_integration_test.go`. This slice edits neither file
and does not treat Node published HEAD alone as their source state.

Artifacts: `build-3l3-worker/evidence/service-envelope/`, `service-envelope-v3.log`,
`service-shortfall-v3.log`, `service-atomic-v3.log`, both CTest logs, four
`service-bytes-v2-*` logs and `analysis.json`. `tests/AnalyzeReliableEnvelope.ps1`
requires all ten unique cases, exact reliable totals and all four phases/eight
byte distributions per fixture, and emits source-log hashes. The analysis is not
a security scan. The obsolete sealed security artifact is untouched.

Proposed boundary: bounded backend admission experiments, canonical byte observer,
the enlarged reference/planned hard-group regression, the reproducible analyzer
and this design/ledger update. No production networking policy belongs in that
commit yet. Before implementation, approve a trusted deployment/aggregate rate
profile and explicit policy for valid groups above the service quantum. No lane
is demonstrated necessary for small-group workloads; full same-FIFO guarantees
remain unproved. Client, overload/recovery, journal margin, current-source security
and CI remain open. No push/merge or 3M is implied by this local checkpoint.

## Official reliable service attribution (2026-09-12)

**B — FOUNDATION 3L PARTIALLY READY.** Attribution-only delta over
`630a1522069567cb834862be6dcaa1fcab4369fe` on
`foundation/3l-content-availability`. No rate, buffer, lane, reliable ordering,
wire, replication budget, semantic authority or client transaction change.
No secondary source changes, branch merge or 3M work. Morphology edits remain
outside this slice. The source/evidence receipt is
`build-3l3-worker/evidence/transport-v1/source-and-evidence.json`.

### Method and actual ownership

Official fixture: the existing `TestOfficialContentMemorySoak/near-max`, same
packaged 512-object content, Local then Node OnDemand, headless official Windows
Server/Player processes over GNS IP loopback on `dockerbox` / HostPC. Eight churn
cycles are **requested, not completed**. Both Players exit 22 at 1,800 frames
with one RPC timeout; the harness then terminates the still-running server.
This is a failed acceptance run, not a crash or a successful eight-cycle soak.
Native builds use four jobs; tests use two; capacity runs do not overlap builds.

The source path is `NetworkScheduler::Submit` (engine queue admission), shared
same-step `GameSession` flush allowance, `NetworkScheduler::Flush`, adapter
`Send`, private 32-byte envelope construction, GNS `SendMessageToConnection`,
backend send/token-bucket/ordered queue, receiver backend complete-message queue,
adapter `DrainMessages`, `GameSession::HandleReceived`, `RemoteManager` dispatch,
and completion/Luau continuation. Scheduler admission is not delivery/ACK, and
GNS acceptance does not grant additional authority or modify Known. TrafficClass
controls **engine** queue selection; the adapter carries it as metadata but uses
one backend lane/ordered stream, not a GNS priority. GNS owns service threads,
packetization, retransmission, reliable reassembly and ordering. The engine owns
Main-thread polling and semantic callbacks.

`TransportServiceSmoke.hpp` installs borrowed private publication/GNS sinks only
for `--session-smoke` plus `GARGANTUAN_TRANSPORT_SERVICE_TRACE=1`. No payload copy,
graph decode or per-message heap allocation is added by the collector. It emits
at most 131,072 fixed-size diagnostic lines, using a 1,024-byte stack buffer and
a saturating dropped counter; it retains no history or native pointer/handle.
Records include full peer/object generations, existing RequestId/frame sequence,
and backend message number. Missing status fields are -1, never invented zero.

Observed server records: 20,038 / 20,063; Player: 18,576 / 18,610, zero Player
drops and zero malformed parsed records. Server forced termination means its
final end receipt is unavailable. Counts below describe the retained window,
not hypothetical later churn. Immediate bounded output preserves failure traces
but adds test overhead: Player event-loop max is 55.24/50.46 ms, compared with
39.03/53.95 ms in the preceding unsampled official run. No isolated logging-cost
counterfactual is claimed. It remains far below the multi-second receive gap.

All subtractions stay within one process's clock. The GNS receive age subtracts
GNS local timestamps only. Matching backend message numbers proves identity,
not clock synchronization. `m_usecQueueTime` is an **estimate**, not actual send
time. It includes pending unreliable bytes and ignores precise framing details.
Receiver age starts at completed-message allocation, not first fragment arrival.
Exact per-packet transmission, sender retransmission residence, receiver fragment/
ordered reassembly residence, and one-way cross-process transit are **not measured**.

### Reliable traffic and burst shape — MEASURED

Application bytes exclude the adapter's additional 32 bytes/message and GNS
packet/framing/encryption overhead. Size columns are p50/p95/p99/max bytes.

| Producer | Server Local messages / bytes | Server Node messages / bytes | Size distribution |
| --- | ---: | ---: | --- |
| GRPL | 1,769 / 2,141,545 | 1,770 / 2,141,719 | 174 / 174 / 174 / 469,284 |
| Reliable Event ACK | 338 / 28,054 | 338 / 28,054 | 83 / 83 / 83 / 83 |
| RPC response | 100 / 8,300 | 100 / 8,300 | 83 / 83 / 83 / 83 |
| Character bind | 1 / 40 | 1 / 40 | 40 / 40 / 40 / 40 |
| Reliable Character state/control | 2 / 288 | 2 / 288 | 108 / 108 / 108 / 180 |
| Owner-action result | 1 / 116 | 1 / 116 | 116 / 116 / 116 / 116 |
| GSES bootstrap | 1 / 80 | 1 / 80 | 80 / 80 / 80 / 80 |
| Total | 2,212 / 2,178,423 | 2,213 / 2,178,597 | — |

No additional unclassified reliable sends appear. Server RPC requests are zero
in this client-originated workload. Each Player submits 342 reliable events
(28,376 bytes, each 83), 100 requests (7,400 bytes, each 74), one cancellation
(52 bytes), one action request (48 bytes), and two GSES messages (80 bytes total,
32/48). Unbind/other unexercised producer counts are zero, not validated service
guarantees. All recorded backend send results succeed; no observed rejection.

GRPL contributes 98.31% of server reliable application bytes and about 80% of
messages. Four frames alone contain 1,813,406 bytes (83.24% of all reliable
payload). Local relative submit times are 0.318, 0.385, 1.768 and 1.834 seconds;
sizes are 437,419 / 469,284 / 467,539 / 439,164 bytes. Node has the same sizes at
0.302 / 0.369 / 1.751 / 1.817 seconds. Each frame has 256 operations. The two
frames of each pair are about four ticks apart, not a continuous saturated-tick
run. Maximum selected/committed work in an official tick is only 258; maximum
GRPL bytes/tick is 469,284. p50/p95/p99 GRPL bytes/tick are 174. The existing
8,192 limit is not approached, yet a single accepted frame requires roughly
1.8 seconds of service at the inherited backend rate. This is the measured
operation-count versus encoded-byte mismatch, not permission to change 3J.

Backend pending reliable peak is 1,449,887 / 1,449,885 bytes, replacing the
earlier capped 1,420,323-byte **sample** high-water. Pending exceeds 1,000,000
bytes for approximately 1.917/1.934 seconds by poll-interval integration.
Local backlog reaches 0.889 MB after the first pair and 1.450 MB after the second.
It falls from 1.399 MB at t=2.05 s to 0.053 MB at t=7.67 s (about 240 kB/s net
drain despite ongoing small submissions), and is zero by the t=8.17 s sample.
Node is zero by t=8.13 s. Recent backend outbound rate is 248,846/249,727 bytes/s
in the active drain window. Sent-unacked bytes are measured separately; they
are not the megabyte pending queue. Unreliable pending work also accumulates.
Class-specific *pending* bytes are not directly exposed by this single GNS queue;
submitted bytes identify the dominant producer, not exact per-class queue residence.

### RPC/Remote path — MEASURED

| Metric (ms) | Local | Node |
| --- | ---: | ---: |
| RPC p50 / p95 / p99 / max, including timeout sample | 32.168 / 35.662 / 1,736.313 / 5,013.167 | 33.407 / 34.646 / 1,737.034 / 5,016.815 |
| RPC timeouts / errors / crashes | 1 / 1 / 0 | 1 / 1 / 0 |
| Event ACK max gap | 5,549.584 | 5,547.756 |
| Player Remote callback max gap | 5,543.347 | 5,542.214 |
| Player Character callback max gap | 1,864.97 | 1,852.51 |
| Player event-loop max gap | 55.241 | 50.462 |
| Player Poll max | 43.522 | 45.756 |
| Player structural apply max | 42.585 | 45.121 |
| Response acceptance → GNS call p99 / max | 0.0662 / 0.0860 | 0.0457 / 0.0475 |
| Instrumented GNS send call max | 0.0100 | 0.0106 |
| Handler → response produced max | 0.0821 | 0.0523 |
| Player GNS receive → RPC callback max | 18.788 | 5.176 |
| RPC callback → completion max, 99 completed responses only | 6.886 | 4.318 |

RPC 6 provides the exact failing correlation: object 23/generation 1, RequestId 6,
backend server-send/player-receive message **139/139 Local, 142/142 Node**.

| RPC 6 stage (ms unless bytes) | Local | Node |
| --- | ---: | ---: |
| Client request acceptance → GNS accepted | 0.0180 | 0.0251 |
| Client request estimated backend queue wait | 0 | 0 |
| Server GNS receive → engine callback | 0.0131 | 0.0117 |
| Server callback → handler | 0.3495 | 0.2927 |
| Handler → response produced | 0.0438 | 0.0361 |
| Response produced → scheduler accepted | 0.0071 | 0.0075 |
| Scheduler accepted → GNS call | 0.0067 | 0.0065 |
| GNS call → accepted | 0.0057 | 0.0057 |
| Reliable pending bytes before response | 1,394,418 | 1,391,411 |
| Estimated backend queue wait before response | 5,337.341 | 5,324.386 |
| Client-local request acceptance → response polled | 5,539.164 | 5,523.265 |
| Completed-message GNS receive age at Player poll | 18.339 | 13.271 |
| Player poll observation → engine callback | 2.951 | 5.176 |
| Callback → gameplay completion | not completed: already timed out | not completed: already timed out |

RPC 5 is also delayed: backend estimate 1,649.566/1,645.248 ms, completed RTT
1,736.313/1,737.034 ms. The response deadline does not create the backend wait;
RPC 6 eventually arrives after its deadline and cannot be counted as a successful
completion. The reliable action result is submitted with 3,623 ms estimated queue
wait. Exact owner-action end-to-end latency is **not measured** in this capture.
Reliable Character state/control also enters a queue with 3,174–3,623 ms estimated
wait. Luau Event ACK RTT/gaps and backend receive gaps are distinct observations.
No renderer/presentation or root-motion-specific new guarantee is inferred.

### Capacity versus ordering discriminator — MEASURED

`gargantuan_gns_capacity_benchmark`, final `transport-capacity-v2`, uses the same
pinned GNS IP localhost backend and ordinary reliable/unreliable flags. It is
not a fake successful official game: no GameSession, GRPL decode, RemoteManager,
DataModel or Player application runs. Tiny 115-byte request/response probes and
opaque structural-sized payloads isolate backend service. A 115-byte unreliable
probe every 100 ms makes contention visible. Each case has a 25-second deadline;
histories cap at 128 RTTs, 8,192 receiver-age samples and 512 unreliable samples.
Status is printed at 10 Hz. All submitted reliable bytes/messages and every RPC
probe are required to arrive before success. Unreliable delivery is not guaranteed.
One preceding v1 run establishes repeatability; v2 extends RPC probing throughout
the full six-second paced interval and submits the exact final due chunk.

| Case | Reliable bytes / messages | Reliable drain s | RPC max ms | Effective delivered bytes/s |
| --- | ---: | ---: | ---: | ---: |
| Gameplay only | 2,300 / 20 | 1.936 (paced) | 16.525 | 1,188 |
| Structure only | 1,450,000 / 4 | 5.708 | not applicable | 254,029 |
| Structure burst → RPC | 1,450,115 / 5 | 5.709 | 5,708.43 | 254,013 |
| RPC response queued → same burst | 1,450,115 / 5 | 5.718 | 11.575 | 253,602 |
| Same burst, 16 kB messages | 1,450,115 / 92 | 5.710 | 5,708.85 | 253,980 |
| Same burst, test-only fixed 1 MiB/s | 1,450,115 / 5 | 1.429 | 1,428.55 | 1,014,720 |
| Same burst, test-only fixed 4 MiB/s | 1,450,115 / 5 | 0.359 | 358.208 | 4,042,290 |

| Six-second paced structural offer, default backend rate | Total reliable bytes delivered (includes 60 RPC responses) | Pending just before offer end | Peak pending | RPC p99 / max ms | Drain complete s from start |
| --- | ---: | ---: | ---: | ---: | ---: |
| 128 KiB/s | 793,332 | 0 | 16,505 | 64.099 / 64.245 | 6.062 |
| 256 KiB/s | 1,579,764 | 62,298 | 78,685 | 294.275 / 297.114 | 6.314 |
| 512 KiB/s | 3,152,628 | 1,624,087 | 1,639,337 | 6,300.87 / 6,393.96 | 12.473 |

Approximate accumulation is zero at 128 KiB/s after each small burst, about
10 kB/s at the nominal 256 KiB/s application offer (additional framing/probes
consume capacity), and about 271 kB/s at 512 KiB/s. These are six-second fixture
observations, not universal sustainable WAN rates. At 512 KiB/s the finite offer
drains about 6.47 seconds after demand stops. Its trailing unreliable probes also
wait about 6.39 seconds. Burst unreliable results count arrivals by reliable drain,
not all eventual datagrams; stopping the fixture can discard remaining best-effort
work. Do not infer guaranteed delivery or pure network loss from that count.

**Inference supported by counterfactuals:** configured capacity plus FIFO queued
byte order dominates the official seconds-long delay. The backend is not physically
limited to 256 KiB/s on this host: changing only test rate moves delivered throughput
by approximately 4x/16x. Same bytes with smaller messages leave the trailing RPC
unchanged; changing submission order changes RPC latency without improving total
drain. Reliable stream ordering and shared sender queue service matter, but an
independent lane is **not yet proven necessary**. No claim assigns every microsecond
to sender versus receiver ordered reassembly; packet-level residence is unmeasured.

### Decision, validation and next boundary

No production rate increase, buffer increase, message reordering or speculative
byte scheduler is retained. Raising a forced minimum using loopback data is not a
safe capacity policy for slower paths. Deferring arbitrary bytes would split
dependency-complete or KI-007 groups; limiting the number of already-enqueued
messages leaves the 469 kB atomic-message problem intact. The next task is
**Networking Reliable Service Envelope**: define the supported capacity/rate and
service-latency contract, derive a maximum atomic encoded group plus finite
transport-facing backlog/byte admission, and prove overload/convergence. Reuse
current 3J acceptance and engine ordering. Only then assess independent lanes if
necessary, with reference/lifecycle barriers, reconnect, version compatibility,
and reverse-starvation tests. GNS has lane facilities, but implementing them is
explicitly outside this attribution slice.

Retained code is bounded diagnostic taps, a smoke collector, the backend-only
benchmark and offline parser. Local/Node official capture still **FAILS** as
expected. MSVC Release official hosts build; seven targeted CTests pass in
14.22 seconds (four GNS adapter/Remote/Character/GameSession tests plus session,
replication relevance/KI-007 and scheduler). Ten capacity cases pass. Diagnostics
do not modify planning, 3E, 3J, KI-007, or late-send allowance. Established exact
8,192-cap, 500-peer convergence and acquisition/admission evidence is preserved,
not falsely described as rerun. The ordinary-runtime diagnostic path does no
status query, timestamp, allocation or logging without a borrowed sink.

The previous 7/7 Clang 19 ASan/UBSan/LSan core result is retained for unchanged
core code; it is **not** a fresh sanitizer claim for these GNS-only taps/hosts.
The already-documented upstream GNS-on UBSan signature issue remains distinct.
No complete current-source security scan, overload/recovery, production journal
retention, official service or client preflight closure is claimed. Known/journal
progress is engine acceptance-based; this slice does not establish that backend
ACKs pin journal retention. That causal relationship remains **not measured**.

Reproduction: enable the smoke environment variable for the existing official
near-max fixture; analyze each server/player pair with
`tests/AnalyzeTransportService.ps1`; run the optional
`gargantuan_gns_capacity_benchmark` executable. Raw captures, parsed JSON,
capacity samples and build/test receipts are retained in
`build-3l3-worker/evidence/transport-v1`, with worker originals under
`C:\Sandbox\Codex\Logs\gargantuan-3l3\transport-v1-official` and
`transport-capacity-v2.log`. They are untracked investigation artifacts, not
production per-tick logging. The offline analyzer rejects multi-peer/generation
captures rather than guessing a cross-process peer mapping.

Documentation build: **19 pages PASS, 3.32 s**, Node 24.19, an isolated committed
public-docs snapshot (public pages unchanged; unrelated morphology edits excluded).
The developer Markdown ledger/contract updates are separately reviewed, not claimed
to be Astro-rendered pages. Branch CI checked: previous `47d0b5a94` run
`34667804305` is terminal green for Windows and Linux; starting `630a15220` run
`34670385751` has Windows terminal green, Linux sanitizer build in progress at
the publication checkpoint. This is not current-delta CI closure. No incomplete
gate is promoted to green.

## Publication checkpoint (2026-09-11)

Base: `a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1`; dedicated branch:
`foundation/3l-content-availability`. Local source remains canonical. No secondary
repository is modified or published, no merge/PR is requested, and 3M remains gated.

### Retained scope and preservation

The initial tracked inventory contains 58 modified files. The two morphology-only
index changes (`devdocs/FutureArchitecture/README.md` and the public future-architecture
page), and the untracked humanoid proposal, are excluded and preserved locally.
The retained scope comprises the other 56 tracked files plus five current-architecture
documents, four private runtime/network headers, two benchmark/analysis files and
the content-unwind CTest driver. All `build-*` directories, worker receipts, generated
documentation and six `docs/3i-docs-*` logs are excluded. No abandoned iterator-frontier,
job-system or transaction redesign is restored from historical experiments.

`build-3l3-worker/evidence/publication-start-source.json` and its binary tracked
patch preserve the pre-staging state. All 49 retained native/fixture hashes match
`derive-lookup-final-source.json` and the disposable worker. This establishes source
continuity, **not** a new security or validation pass. The obsolete sealed security
artifact is neither edited nor treated as green.

The series separates settled-static physics/diagnostics, runtime and client lifetime/
journal groundwork, the coupled replication planning/lifecycle/service implementation
with its fixtures, and the consolidated historical architecture/validation record.
The bounded continuation, KI-007 ordering, reference index, typed Character inspection,
retirement and same-step allowance share current coordinator/session contracts; they
are not split into historically broken intermediate implementations.

### Executed committed-source gate and publication

Published HEAD: `47d0b5a9406449ce7c4ae1f2341e0889e4366ee2`, tracking
`origin/foundation/3l-content-availability`. Normal push; no force, merge or PR.
The coherent series is `801a2a6ab` (physics), `2728f097e` (lifetime/preflight),
`36d4eb668` (coupled planning/lifecycle/service), `47d0b5a9` (validation history).
All 49 changed native/build/fixture files match the pre-staging source. The complete
522-file native inventory also matches the worker's Git-normalized source: 157 raw
hash matches, 365 line-ending-only differences, zero semantic source differences.
`publication-all-native-source.json` and `publication-final-source.json` supersede
the incomplete 38-file dirty-tree-helper inventory `publication-committed-source.json`.

**Measured on committed source:** MSVC Release 6/6 (8.09 s), Clang 19 ASan/UBSan/LSan
7/7 (57.23 s, leak detection enabled), same-step late-handoff, 200-peer differential,
500-peer load/eviction/reload convergence, and 19 documentation pages from a clean
committed archive. The benchmark hash remains
`6CFD2439AD1F56FCADC17D12A5C623642637465E36530B432795372B84B796F7`.
Control load p99/max 4.118/8.988 ms; Local 32.859/48.454; Node 34.070/47.750.
The streaming cases converge but fail their unchanged health gates. This is a
partially-ready development publication, not a non-starvation acceptance.

## Post-publication: due-to-recipient attribution (2026-09-11)

**B — FOUNDATION 3L PARTIALLY READY.** No new production scheduling correction is
justified by this slice. Retain bounded, opt-in test diagnostics and their regression
test, not a jobs, Character priority, transaction, transport or wire redesign.

### Exact source and method

`latency-v3-source.json` preserves the diagnostic delta over the published source;
the complete publication inventory establishes its base. Current native benchmark:
`DC2AF141FFAC8A5CAEA4FF873926CA83825E529EE9A864745083B96779ECCB35`.
Evidence: `latency-v3-{none,local}-200-50.out.log`, `latency-v3-node-200-50.log`,
`latency-v3-local-500-50.out.log`, and `latency-final-{control,local,node,500}.json`
plus `latency-final-reviewed.json`, all under `build-3l3-worker/evidence` (untracked).
The analyzer is `tests/AnalyzePublicationLatency.ps1`. Its later offline-only
surrounding-tick analysis does not change the measured native source.

Unchanged deterministic workload: 200 connected peers, 50 Characters, 10 root-motion
Characters, five-tick input cadence, 512-object package version 17, 120 warmup ticks,
301 ticks per phase, 100 RPCs per phase, the existing event/action schedule; one real
headless GameSession client plus raw simulated recipients. Node changes only the
content provider. The official network fixture is separate below.

All stage intervals use one-process steady-clock nanoseconds and complete stable
peer/object generation + sequence/epoch or RequestId correlations. First/last peers
are sampled; all recipients contribute to fixed 1-ms histograms. The 200-peer capture
has 29,759 records of a hard 131,072 maximum, 11,534,336 reserved record bytes, about
320 KiB fixed histogram storage, zero drops, zero decode exceptions, zero histogram
overflow. The 500-peer capture has 25,084 records. No production record history or
client-selectable enable/priority is introduced. The scope restores its borrowed sink.

**Method correction:** `Observe` runs before real `GameSession` packet dispatch;
raw recipients never apply a DataModel. Histograms therefore measure pre-application
packet observation, not rendered presentation. `ClientHandled` means packet handler
return/interpolation input, not pose visibility. Exact authoritative-change birth,
render-observer completion and application-to-visible-observer latency are **not
measured**. Do not manufacture those intervals from the observer timestamps.

### Healthy differential, load phase

All times in milliseconds. Histogram percentiles marked `upper` are deterministic
1-ms bucket upper bounds; maxima remain exact measurements, not inferred quantiles.

| Metric | Control | Local | Node |
| --- | ---: | ---: | ---: |
| Tick p50 / p95 | 2.468 / 3.229 | 6.169 / 20.713 | 6.412 / 21.540 |
| Tick p99 / max | 3.828 / 9.198 | 31.000 / 48.523 | 33.885 / 53.348 |
| Accepted Character states/wall second | 8,263.35 | 7,517.65 | 7,473.59 |
| Throughput loss versus control | baseline | 9.02% | 9.56% |
| Character gap p50 / p95 / p99 upper | 52 / 152 / 201 | 56 / 200 / 321 | 56 / 200 / 318 |
| Root gap p50 / p95 / p99 upper | 100 / 200 / 201 | 100 / 201 / 353 | 100 / 201 / 352 |
| Character/root exact max gap | 201.532 | 645.851 | 658.239 |
| Reliable Event ACK max gap | 19.130 | 86.199 | 82.165 |
| Event RTT p99 / max (297 samples) | 35.732 / 36.113 | 104.082 / 137.175 | 108.168 / 134.281 |
| RPC RTT p50 / p95 / p99 / max | 33.680 / 35.302 / 35.539 / 35.882 | 37.996 / 62.774 / 100.967 / 103.280 | 38.241 / 46.030 / 106.360 / 134.142 |
| RPC timeouts/errors/crashes | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| Action p99 / max (7 samples) | 34.713 / 36.050 | 39.069 / 39.293 | 39.008 / 39.896 |
| Peer convergence max ms / ticks | 16.848 / 1 | 1,348.55 / 51 | 1,397.97 / 52 |

There are 44,480 Character-gap and 24,619 root-gap observations per load case.
Scheduler rejections remain zero. Streaming health remains FAIL, not an accepted
envelope inferred from a lower p99 or an unchanged number of accepted states.

### Character/root stage decomposition

Entries are p50 / p95 / p99 / max milliseconds; stage quantiles are **not additive**.

| Stage | Control | Local | Node |
| --- | --- | --- | --- |
| Due-tick Engine complete to produced | 2.110 / 2.668 / 3.016 / 3.174 | 4.066 / 20.145 / 28.032 / 29.089 | 2.139 / 24.239 / 28.511 / 33.256 |
| Produced to scheduler accepted | .0009 / .0014 / .0020 / .0088 | .0009 / .0015 / .0021 / .0112 | .0009 / .0017 / .0032 / .0103 |
| Accepted to handoff | .213 / .752 / .866 / 1.028 | .226 / .758 / .878 / 1.078 | .222 / .776 / .961 / 1.258 |
| Handoff to simulator delivery | .135 / .215 / .268 / .394 | .144 / .299 / 1.511 / 1.881 | .146 / .354 / 1.228 / 1.852 |
| Simulator delivery to raw observer | .413 / .662 / .768 / 4.018 | .419 / .696 / 20.343 / 53.220 | .419 / .728 / 20.564 / 52.009 |
| Real-client observer to callback | .0029 / .0038 / .0051 / 2.602 | .0029 / .0046 / .0075 / 2.688 | .0030 / .0049 / 3.144 / 23.148 |
| GCHR callback to handler return | .0015 / .0019 / .0024 / .0068 | .0015 / .0021 / .0024 / .0040 | .0016 / .0025 / .0060 / .0074 |

All 340 sampled ordinary productions per load case occur on the due tick: lateness
0 ticks at every retained quantile/max. The due-time anchor is after `Engine::Step`
and before `GameSession::Step`, not an exact state mutation timestamp.

The worst Local sample is root-motion Character 63 generation 1, peer 200 generation
1, materialization epoch 11, sequence 478/tick 504 to sequence 490/tick 516. Effective
tier is Low (12 ticks), ordinary rather than forced. Its measured 645.851-ms gap is:
596.8999 before the due-tick Engine-complete anchor; 28.0321 to production; .0015 to
acceptance; .2747 to handoff; .2994 to simulator delivery; 20.3434 to raw observation.
It has no real-client application stage because peer 200 is a raw observer.

Surrounding ticks 505–516 take 46.09–84.85 ms per **whole shared fixture iteration**;
server Session steps take 24.61–33.96 ms, raw observer draining 18.76–20.11 ms each.
Tick 509 additionally contains the 24.03-ms real-client structural callback below.
The complete per-tick trace is retained in `WorstGapSurroundingTicks`. On due tick
516, frame construction is 3.353 ms, validation encoding 4.344 ms, submission encoding
4.264 ms and acceptance/commit 3.536 ms. Selection's inclusive scope is 10.045 ms;
do not add child encode scopes to it. These bounded bursts materially dilate ticks
even though cumulative post-selection CPU is smaller than relevance. This is a
combination of runtime work and serialized fixture work, not a pure server-host
counterfactual or an accepted packet waiting 650 ms in the scheduler.

### RPC and reliable Event attribution

For 100 Local load RPCs, p50 / p99 / max milliseconds:

| Stage | Measured interval |
| --- | --- |
| Request accepted to handoff | .053 / .098 / .109 |
| Request handoff to simulator delivery | 20.476 / 50.069 / 52.891 |
| Request delivery to server callback | 10.531 / 20.387 / 20.497 |
| Server callback to native handler dispatch | 6.559 / 30.888 / 34.034 |
| Handler dispatch to response produced | .0021 / .0093 / .0121 |
| Response produced to acceptance | .0015 / .0020 / .0035 |
| Response accepted to handoff | .0032 / .0064 / .0065 |
| Response handoff to simulator delivery | .188 / 1.410 / 1.881 |
| Response delivery to client callback | .0055 / .0813 / .0821 |
| Client callback to native completion return | .167 / .409 / .433 |

The same Node response-acceptance/handoff maximum is .013 ms and callback/completion
maximum .401 ms. Native completion-return RTT is slightly longer than the Lua RTT
table because Lua records before callback return. Loopback's remaining RPC time is
predominantly before response production; the retained late-send correction is not
contradicted. This does **not** establish official reliable-backend service.

Reliable Event ACK sequences/RTT remain measured by the Luau fixture. Their native
packet-stage correlation is **not measured**: the existing reliable Event wire field
has sequence zero, so repeated packet taps cannot uniquely identify those events.
The analyzer explicitly reports duplicate `Remote100` keys and does not derive event
stage distributions from them. No wire field is added to repair diagnostics.

### Real-client structural application

Local frame sequence 8, epoch 1: 154,851 bytes / 512 create operations; 274 existing
candidate identities copied and 786 identities natively preflighted, zero removals.
Callback total 24.033 ms; `ReplicaApplier` 22.589 ms: copy .111, semantic 1.325,
preflight 8.888, live apply 9.175, residual 3.090 ms. Preflight contains validation
.582, construction 1.857, parenting 4.267 and properties 2.182 ms. Residual includes
temporary-world destruction and metadata, not a separately timed cleanup-only scope.
Node's equivalent frame is 23.140 ms. The Node GCHR callback is measurably delayed
23.148 ms after pre-application observation by synchronous packet processing in the
batch; its own handler remains below .008 ms.

This verifies actual contribution from client native preflight/live creation without
claiming it alone owns the approximately 650-ms scale gap. The prior 8,193-replica,
one-property 89–101-ms scaling result remains applicable to the unchanged transaction
path, but is historical, not a fresh run of that size. A whole-world transactional
preflight redesign is outside this task. Renderer-visible completion remains unmeasured.

### Bounds, overhead and gates

500 peers still load in 158 ticks (4,399.78 ms), evict in 212 (3,868.62 ms), and reload
in 158 (4,320.81 ms). Initial acquisition/admission is exactly 1/1; reload admissions
become 2, not a duplicated initial lifetime. Planning remains 65,536 charged steps,
2,048 peer slice, maximum measured service interval 16 ticks; 3J remains 8,192.
Known is acceptance-only; targeted dependency/bootstrap/KI-007 tests pass. Load gap
812.921 ms, eviction gap 304.180 ms and reload gap 427.784 ms do not close health gates. Journal lag high-water
16,214 versus retention 16,384 leaves 170 entries, not a defensible final margin.
There are zero journal failures or RPC errors; convergence does not close overload.

Paired same-binary v2 probe-off/on load runs: Control wall 5,019.16/5,020.65 ms,
Local 5,520.23/5,521.72 ms; Local throughput 7,514.18/7,512.16 states/s. This is about
.03% wall/throughput difference, not a statistically established zero-overhead bound.
Local p99 varies 30.965/32.388 ms; do not interpret one pair as exact probe CPU cost.
The v3 addition only samples existing real-client replica counters and root metadata.

### Official GNS path: reproduced failure, not closure

Freshly rebuilt official Player/Server/Packager, existing Node fixture only as the
test driver; no secondary source changes. `TestOfficialContentMemorySoak/near-max`
requests eight churn cycles and 100 RPCs using one real headless Player, Local then
Node provider. Both runs FAIL (Player exit 22) after 1,800 frames; no successful
ClientTimeline marker. This is not a crash: exit 22 is the existing smoke proof's
incomplete Step 2, with `FunctionComplete=false` after one RPC timeout. The fixture
does not finish its requested eight cycles, so do not report eight-cycle soak success.

Raw logs and `player-analysis.json` are in `build-3l3-worker/evidence/latency-final-official`;
the driver receipt is `latency-v3-official-near-max.log`. Both traces retain all 1,800
Player frame rows, below the 4,096 cap; `Complete=false` correctly reports failed proof.

| Official near-max metric | Local | Node |
| --- | ---: | ---: |
| RPC p50 / p95 / p99 / max ms | 33.224 / 34.640 / 1,734.685 / 5,000.479 | 33.332 / 34.792 / 1,749.883 / 5,000.113 |
| RPC samples / timeouts / errors / crashes | 100 / 1 / 1 / 0 | 100 / 1 / 1 / 0 |
| Reliable Event ACK max gap ms | 5,549.552 | 5,531.744 |
| Player Remote receive-service max gap ms | 5,544.188 | 5,529.299 |
| Player Character receive-service max gap ms | 1,854.763 | 1,865.923 |
| Player event-loop max gap ms | 39.027 | 53.949 |
| Player Poll max ms | 29.642 | 36.378 |
| Player replica apply max ms | 29.371 | 33.022 |
| Native preflight max ms | 18.041 | 17.698 |
| Sampled backend pending reliable bytes high-water | 1,420,323 | 1,420,323 |

The reliable-byte trace reaches its bounded 256 structural + 256 application samples;
the high-water is a **sampled lower bound**, not a complete run maximum. It measures
GNS pending reliable bytes, not a separately measured RPC queue, acknowledged history,
queue age or end-to-end backlog. All captured sends report acceptance. Local RPC 6's
handler runs; a same-server-clock application send follows 34 microseconds later with
1,390,274 pending reliable bytes. That tap does not carry RequestId, so this is temporal
association, not a falsely exact response-ID handoff match. Client-clock RPC 6 times
out at 5,000.479 ms. Server-clock response/body timing is never subtracted from an
unsynchronized client timestamp. Precise official response-handoff-to-receive duration
is **not measured**.

Source finding: `GameNetworkingSocketsTransport::Send` sends both GRPL and reliable
application traffic through `SendMessageToConnection` on the same connection with
the Reliable flag. It does not configure independent lanes; native semantic order
tags do not create backend lanes. Pending bytes are queried before admission. Client
event service continues in tens of milliseconds while reliable service gaps reach
seconds. Local server-frame interval max is approximately 23.76 ms, not a giant
multi-second synchronous tick. Therefore the observed official failure cannot be
explained by the Player's measured preflight spike or the raw scale harness alone.

**Inferred owning boundary:** reliable backend stream/head-of-line and service/capacity
policy. Exact congestion, configured rate, effective send rate, unacknowledged bytes,
per-response queue age and backend packet receive timestamps remain unmeasured. Do
not distinguish inadequate configured capacity from arbitration starvation without
those measurements. Do not raise bandwidth limits or add lanes on this evidence
alone. Historical approximately 3.8-second official delay is not fixed; this run
demonstrates approximately 5.5-second Remote gaps and an RPC timeout instead.

The next dedicated task is **Official Reliable Transport Service/Capacity Attribution**:
correlate RequestId through the actual GNS send/receive boundary with interval-local
clocks; capture bounded pending/unacknowledged bytes, rate policy, observed drain rate,
queue age and packet/callback service; separate physical/configured saturation from
same-stream head-of-line delay. Preserve ordering/lifecycle dependencies, GCHR semantics
and reverse-direction structural progress. Any new ordering lane requires explicit
architecture/wire review. Client transactional preflight remains the next independent
client owner; neither it nor general jobs is a workaround for the official failure.

### Final diagnostic validation and publication boundary

Final diagnostic native source: MSVC Release affected tests **6/6 PASS (8.29 s)**;
Clang 19 ASan/UBSan/LSan **7/7 PASS (56.78 s, detect_leaks=1)** plus late-handoff.
The committed-source rerun is MSVC **6/6 (8.35 s)** and sanitizers **7/7 (57.06 s)**,
with 13 GCHR and two reliable Event messages handed off after production in the
same step. All 53 changed native/build/fixture worker hashes match committed source.
The final documentation archive builds **19 pages (2.13 s)**. The final pre-push
amendment only updates this result ledger and known-issue publication/status text;
native/build/fixture blobs are unchanged from those committed-source reruns.
The previous v2 sanitizer pass was also 7/7; it is not substituted for final v3.
No physics behavior changed: targeted physics regression passes; the prior 90-case
matrix is preserved, not falsely rerun. Planning/3J, KI-007, dependency/bootstrap,
acceptance-only Known and 500-peer convergence remain green. Control health passes;
Local/Node/500 health and official near-max proof remain failed as recorded above.
No production correction is retained after publication, only attribution diagnostics,
bounded histogram/storage tests, offline analysis and this ledger.

Current-source security inventory/review, startup retention margin, overload/recovery,
final client/official transport service contracts and terminal current-source CI remain
open. The sealed obsolete security checkpoint is untouched. No Foundation 3M work.

## Previous slice: derivation versus serial commit (2026-09-11)

**B — FOUNDATION 3L PARTIALLY READY.** Attribution is complete; one narrow
serial 3E lookup correction is retained. No jobified phase, JobSystem change,
wire change, budget increase, commit, push or Foundation 3M work occurred.
The [derivation assessment](ReplicationDerivationAssessment3L.md) records the
source-traced phase map, existing pool capability and proposed (unimplemented)
read-epoch boundary. The previous dependency-complete planner remains intact.

### Source, method and artifacts

HEAD/origin remain `a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1`.
`derive-attribution-before-source.json` verifies all 48 prior native hashes match
`planning-boundary-final`. Diagnostic baseline `derive-attribution-v1` adds
fine scopes/counters without changing policy. Its executable SHA256 is
`720321B07E80603A949967407A81285320E850EF51CFA8AB1B38E0F2033DB4CF`.
The final `derive-lookup-v3-source.json` captures 49 native hashes at
2026-09-12 01:42:29 UTC (September 11 locally); all match dockerbox. Its executable is
`6CFD2439AD1F56FCADC17D12A5C623642637465E36530B432795372B84B796F7`.
V2 is intermediate evidence; v3 uses an explicit ordered loop rather than a
stateful algorithm predicate. Local source is canonical; secondary source is
unmodified and only the retained Node integration fixture is used.

Artifacts under `build-3l3-worker/evidence/` retain both prefixes' raw
`none-200-50.out.log`, `local-200-50.out.log`, `node-200-50.log`,
`local-500-50.out.log`, `*-derivation.json`, `*-reviewed.json`, source manifests
and tracked patches. Final client scopes are in
`derive-lookup-v3-client-phases.json`. The reproducible analyzers are
`AnalyzeDerivation.ps1` and `AnalyzeReviewedOwners.ps1`; raw-log SHA256s are
recorded in their outputs. These inventories are **not a security review**.

The previous 200/500-peer, 50-Character, five-neighbour fixture is unchanged:
512 objects / 273,032 bytes, ten root-motion tracks, input every five ticks,
actions every 40 callbacks, reliable Event each callback, and 100 sequential
RPCs per phase. Each phase has 301 ticks. Performance cases ran sequentially,
without overlapping builds/sanitizers. Node acquisition uses real TLS; gameplay
uses loopback, not official GNS. All 200-peer load cases accept exactly 41,480
Character states before and after; scheduler rejections remain zero.

**MEASURED:** opt-in Main elapsed busy scopes, loop counts and scoped allocation
traffic. These are **not OS thread CPU samples**. Percentiles use
floor((N-1)*p), with inactive phases zero-filled; rare spikes can therefore have
zero p99 and a nonzero maximum. Nested scopes overlap and are not additive.
Independent copy time, isolated instrumentation overhead, worker utilization,
queue/barrier waits, concurrent discard rate and actual worker scaling are
**not measured**. No worker execution was added. Query time also falls despite
unchanged query code; improved locality is an inference, not a separately proved
cause. Busy-time shares are not causal percentages of throughput loss.

### Attributed serial cleanup

Before, the same-root check searched the retained-root tree for every sorted
query candidate and checked a second spatial-root map before projection lookup.
After, it joins the ordered identities with a call-local cursor and performs one
projection lookup. Every current-position predicate still runs; there is no
new no-change skip, cache, frontier, thread or semantic projection. Transactional
root/projection membership and existing unhealthy-owner failure handling justify
the removed lookup. The independent 128-object/48-step reference test covers
three focus volumes, mixed membership, hysteresis, movement and destroy/recreate.

| Cumulative load work, 301 ticks | Before Local | After Local | Before Node | After Node |
| --- | ---: | ---: | ---: | ---: |
| 3E total ms | 1542.16 | 853.19 | 1531.52 | 878.09 |
| Spatial query ms | 630.28 | 489.10 | 630.18 | 502.65 |
| Hysteresis ms | 878.89 | 332.04 | 868.51 | 343.92 |
| Selection construction/install ms | 16.79 | 17.52 | 16.83 | 16.86 |

Local old-root/candidate visits remain exactly **5,060,921 / 5,553,168**;
9,864/10,064 evaluations retain the same roots. The 500 load retains exactly
9,624,483/11,817,405 visits and 18,764/19,264 unchanged-root results.
The optimization makes legitimate examinations cheaper; it does not lower the
counter by discarding work. Query allocation traffic is zero in the diagnostic
Local phase. Its 103,435 hysteresis allocations occur in changed-root rebuilds,
not every no-op evaluation. Planner-resume allocation traffic there is 954,520
allocations / 41,625,960 bytes; encoder traffic is 30,000 / 375,054,640 bytes.
These are cumulative allocation bytes, not retained memory high-water.

### Final server decomposition

Cumulative milliseconds over all 301 ticks. These rows are non-overlapping;
selection construction uses its exclusive scope, while the acceptance row
includes Known and accepted-reference mutation. Remainder is **mixed/unclassified**,
not a claim that it is all irreducibly serial or freely parallelizable.

| Work | Control 200 load | Local 200 load | Node 200 load | Local 500 reload |
| --- | ---: | ---: | ---: | ---: |
| Spatial query (derived, mutable scratch today) | 187.83 | 489.10 | 502.65 | 1067.04 |
| Hysteresis derivation | 12.33 | 332.04 | 343.92 | 803.65 |
| Selection construction exclusive | 0.00 | 16.38 | 15.75 | 46.02 |
| 3E installation/bookkeeping | 0.62 | 1.97 | 1.87 | 5.79 |
| Dependency/fixup derivation + resume dispatch | 0.00 | 196.55 | 192.51 | 693.56 |
| Planner scratch cleanup | 0.00 | 9.10 | 9.24 | 38.82 |
| Planner service revision validation | 0.00 | 0.18 | 0.19 | 0.69 |
| Complete planner installation | 0.00 | 0.09 | 0.08 | 0.22 |
| Planned frame construction/fixup preparation | 0.00 | 43.38 | 43.60 | 119.03 |
| Encoding, both passes | 1.31 | 115.09 | 114.57 | 359.67 |
| Serial scheduler acceptance/commit | 0.09 | 47.08 | 45.65 | 153.72 |
| Intent creation | 0.01 | 0.02 | 0.02 | 0.06 |
| Structural scheduler submission | 0.05 | 0.17 | 0.14 | 0.46 |
| Mixed/unclassified remainder | 528.39 | 683.63 | 694.27 | 1330.25 |
| Captured server busy total | 730.63 | 1934.77 | 1964.47 | 4618.97 |

Planner-resume scope includes coroutine dispatch and shared credit/metric updates:
it is a **candidate derivation upper bound**, not already worker-safe code.
Dependency versus fixup versus raw coroutine-dispatch CPU within that scope is
**not measured separately**. Known and accepted metadata are timed below as
children of acceptance. Journal cursor assignment is included in commit;
no separate nanosecond claim is made for that scalar assignment. Incremental
preparation includes bounded journal reads/coalescing, preparation and validation
encoding. Pure selector decision time and fixup-only frame construction time are
**not separately measured**; the named StructuralSelection parent is mixed.
The trace and source make that ownership explicit rather than calling the entire
parent scope serialized 3J acceptance.

### Final per-tick phase distributions

Each cell is **p50 / p95 / p99 / max / cumulative ms** over 301 ticks.
Parents and children overlap; do not add these rows.

| Scope | Local 200 load | Local 500 reload |
| --- | --- | --- |
| Relevance | 1.762 / 5.777 / 6.951 / 13.436 / 853.193 | 6.210 / 7.048 / 14.741 / 14.998 / 1943.955 |
| RelevancePeer | 1.725 / 5.733 / 6.865 / 13.400 / 841.563 | 6.150 / 6.923 / 14.694 / 14.949 / 1926.524 |
| RelevanceQuery | 1.009 / 3.345 / 3.787 / 4.106 / 489.102 | 3.511 / 3.962 / 4.134 / 4.558 / 1067.043 |
| RelevanceHysteresis | 0.527 / 2.337 / 2.731 / 3.945 / 332.036 | 2.662 / 2.960 / 4.131 / 4.225 / 803.651 |
| RelevanceSelection | 0.000 / 0.000 / 0.691 / 5.667 / 17.524 | 0.000 / 0.000 / 6.420 / 6.592 / 50.106 |
| RelevanceCommit | 0.004 / 0.006 / 0.049 / 0.401 / 1.969 | 0.005 / 0.008 / 0.531 / 0.620 / 5.787 |
| StructuralPlanning | 0.000 / 4.676 / 7.244 / 7.900 / 211.190 | 1.981 / 8.095 / 9.133 / 9.395 / 760.824 |
| PlanningResume | 0.000 / 4.644 / 7.230 / 7.884 / 196.552 | 0.000 / 8.080 / 9.118 / 9.380 / 693.559 |
| PlanningCleanup | 0.000 / 0.000 / 0.726 / 2.413 / 9.104 | 0.000 / 1.227 / 1.289 / 3.007 / 38.820 |
| PlanningValidation | 0.000 / 0.004 / 0.007 / 0.009 / 0.181 | 0.001 / 0.006 / 0.017 / 0.031 / 0.687 |
| PlanningInstall | 0.000 / 0.000 / 0.014 / 0.027 / 0.085 | 0.000 / 0.000 / 0.042 / 0.060 / 0.222 |
| StructuralSelection | 0.000 / 0.084 / 10.737 / 11.994 / 136.324 | 0.168 / 12.296 / 13.066 / 13.787 / 409.523 |
| StructuralFrameBuild | 0.000 / 0.000 / 3.570 / 3.766 / 43.383 | 0.000 / 3.789 / 3.923 / 4.226 / 119.031 |
| StructuralEncode | 0.000 / 0.000 / 9.171 / 9.939 / 115.086 | 0.000 / 11.322 / 12.595 / 12.981 / 359.665 |
| StructuralValidationEncode | 0.000 / 0.000 / 4.679 / 5.524 / 59.158 | 0.000 / 5.798 / 6.388 / 6.569 / 182.948 |
| StructuralSubmissionEncode | 0.000 / 0.000 / 4.431 / 4.605 / 55.981 | 0.000 / 5.564 / 6.209 / 6.414 / 176.849 |
| StructuralAcceptance | 0.000 / 0.000 / 3.804 / 4.218 / 47.077 | 0.000 / 4.923 / 5.074 / 5.415 / 153.722 |
| KnownCommit | 0.000 / 0.000 / 2.800 / 3.104 / 34.476 | 0.000 / 3.758 / 3.875 / 4.134 / 117.253 |
| AcceptedMetadataCommit | 0.000 / 0.000 / 0.844 / 0.939 / 10.491 | 0.000 / 0.990 / 1.022 / 1.083 / 31.028 |
| IncrementalPreparation | 0.065 / 2.269 / 2.410 / 2.536 / 107.644 | 0.171 / 4.171 / 4.323 / 5.358 / 432.917 |
| StructuralIntent | 0.000 / 0.000 / 0.001 / 0.007 / 0.018 | 0.000 / 0.001 / 0.001 / 0.024 / 0.056 |
| StructuralSubmit | 0.000 / 0.000 / 0.009 / 0.062 / 0.170 | 0.000 / 0.009 / 0.012 / 0.156 / 0.465 |
| GraphSynchronization | 0.302 / 0.867 / 1.032 / 1.224 / 114.177 | 0.408 / 0.911 / 1.008 / 1.080 / 152.102 |
| RemoteMaterialization | 0.000 / 0.000 / 2.120 / 3.010 / 28.372 | 0.000 / 2.658 / 2.789 / 13.761 / 97.043 |
| SchedulerFlush | 0.059 / 0.141 / 0.183 / 0.338 / 21.545 | 0.073 / 0.360 / 0.491 / 0.577 / 32.810 |

The Control/Node distributions and allocation/call counters remain in the full
JSON summaries. Backend TransportSend is **not measured on this loopback path**;
a zero GNS-only scope is not zero handoff cost. SchedulerFlush includes loopback
handoff. No official-backend arbitration conclusion is drawn.

### Healthy differential: before / final

Before means diagnostic v1, not a reconstructed published-main baseline. The
previous preserved v13 reference was Control/Local/Node p99 4.03/35.71/39.07 ms,
with Local/Node recipient maxima 686.79/703.88 ms. Single-run noise and diagnostic
overhead are visible; the controlled local correction comparison is v1 versus v3.

| Metric | Before Control | Before Local | Before Node | Final Control | Final Local | Final Node |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Tick p50 ms | 2.58 | 10.52 | 10.51 | 2.49 | 6.10 | 6.30 |
| Tick p95 ms | 3.35 | 22.50 | 22.39 | 3.82 | 21.03 | 20.57 |
| Tick p99 ms | 4.08 | 37.46 | 36.08 | 4.07 | 30.82 | 30.74 |
| Tick max ms | 10.34 | 49.28 | 48.87 | 9.65 | 47.98 | 47.67 |
| Character accepted states/s | 8262.78 | 7373.05 | 7433.52 | 8262.20 | 7506.19 | 7514.10 |
| Character/root recipient max gap ms | 201.36 | 713.57 | 680.30 | 201.95 | 652.59 | 649.24 |
| Reliable Event ACK max gap ms | 18.96 | 94.32 | 84.12 | 18.80 | 84.03 | 87.38 |
| RPC p50 ms | 34.02 | 41.59 | 41.60 | 33.00 | 37.92 | 37.03 |
| RPC p95 ms | 35.15 | 67.99 | 66.64 | 35.01 | 65.23 | 63.72 |
| RPC p99 ms | 35.42 | 114.75 | 104.39 | 35.18 | 100.33 | 101.32 |
| RPC max ms | 35.50 | 118.31 | 113.63 | 35.87 | 102.41 | 103.19 |
| Action p99 ms | 35.00 | 43.97 | 43.42 | 35.05 | 39.06 | 38.78 |
| Action max ms | 35.25 | 46.41 | 43.73 | 35.28 | 39.42 | 39.09 |
| Throughput loss vs same-run control | 0.00% | 10.77% | 10.04% | 0.00% | 9.15% | 9.05% |

All 100-call load RPC sequences complete with zero timeouts/errors; no process
crashes occur. Action p99 has only seven observations in load, not a large-tail
sample. Root requests equal authoritative commits and eleven spatial crossings
remain in load; authority is unchanged. Character/root recipient p95/p99 remain
**not measured (max-only sampler)**. Reliable Event ACK service gaps are distinct
from Event request/ACK latency distributions. No unreliable-event guarantee is
inferred. Control passes all phases; Local/Node load and reload still fail the
existing health gates (exit 1), despite successful convergence.

### 500-peer comparison and resource invariants

| Metric | Before load | Final load | Before eviction | Final eviction | Before reload | Final reload |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Tick p99 ms | 49.52 | 48.12 | 28.32 | 26.75 | 48.33 | 43.27 |
| Tick max ms | 54.25 | 51.70 | 30.79 | 28.51 | 55.14 | 52.41 |
| Converge max ms | 4820.33 | 4353.78 | 3917.20 | 3866.83 | 4679.38 | 4257.47 |
| Converge ticks | 158.00 | 158.00 | 212.00 | 212.00 | 158.00 | 158.00 |
| Character/root recipient max gap ms | 881.43 | 846.56 | 320.74 | 311.95 | 454.97 | 425.41 |
| Character states/s | 6018.76 | 6614.47 | 6066.67 | 6124.10 | 3047.04 | 3309.52 |

All 500 peers converge. Logical load/reload convergence remains **158 ticks**,
eviction **212 ticks**: the lookup correction does not trade work capacity for
extra ticks. Acquisition/initial admission remain exactly **1/1**, with two total
admissions only after reload. Exact selection saturation stays **8,192**; planner
work stays **65,536**, peer slices **2,048**, fairness max service interval **16**
ticks (seven at 200 peers). No worker-count multiplier or new queue exists.
Reservation high-water remains **4,574,325** global / **8,635** per continuation.
Load Pending high-water remains 218,112; cumulative reload high-water is 225,792.
Journal failures remain zero and phase-end backlog drains to zero. Capacity stays
16,384; startup cumulative lag 16,214 leaves only **170 entries**, still open.
Load/reload measured lag 6,135 leaves 10,249; eviction lag 1,044 leaves 15,340.
None of those phase-local margins proves the startup or overload envelope.

### Tail attribution and Amdahl decision

The final Local maximum Character/root gap is 652.587 ms, ticks 504→516.
All twelve intervening ticks select/accept 8,192 operations. Their captured
whole-iteration busy time totals 646.695 ms, including 234.543 ms simulated-recipient
observer drain; 3E derivation is only 33.111 ms. The real client's tick 509
spends 23.568 ms in structural apply and 26.316 ms in poll, plus an 8.482-ms Engine
step. These are nested scopes, not additional mutually exclusive totals. Client
apply max is 26.327 ms in the 500 reload; its 301-tick p99 is zero because apply
occurs rarely, not because the spike disappeared. Independent whole-world
preflight remains the historical 89–101-ms legal-world blocker; this slice did
not rerun or redesign that fixture.

**INFERRED counterfactual:** even ideal eight-worker 3E execution removes only
28.972 ms from the recorded Local gap window, before overhead or changed scheduling.
In the final 500 reload gap, derivation is 36.394 ms of 424.795-ms whole-iteration
busy time, giving an analogous ideal saving of 31.845 ms. A 3E optimization alone
cannot be called non-starvation closure on these measurements.

The server-busy-time Amdahl model is T' = T - P + P/N + H + W, with P consisting
only of query+hysteresis+selection construction. This is an optimistic bound,
not measured scaling. W (stale/retry work) is unmeasured; H=0 is ideal. The second
number below assumes **0.5 ms overhead every tick**, solely a sensitivity example,
not a measured barrier cost. Work imbalance and memory contention are omitted.

| Workers | Local 200 ideal / illustrative overhead speedup | Local 500 reload ideal / illustrative overhead speedup |
| --- | --- | --- |
| 2 | 1.28× / 1.16× | 1.26× / 1.21× |
| 4 | 1.48× / 1.33× | 1.45× / 1.39× |
| 8 | 1.61× / 1.43× | 1.57× / 1.49× |
| 16 | 1.68× / 1.49× | 1.64× / 1.55× |

There is material **average server** parallelism potential (3E candidate share
43.29% Local load / 41.50% 500 reload), but it is not the dominant work inside the
recipient-tail windows. The implementation gate is **not yet established**:
today's Query mutates shared dedup state and exposes no pinned read contract.
The existing pool supports bounded batches and a Main barrier; it does not supply
that input contract. Do not submit current UpdatePeer/Run to workers. The
assessment defines the narrow 3H/3K read epoch/private scratch prerequisite,
serial validation, finite result sizing and unchanged logical budgets. No
workerized phase or unmeasured concurrency claim is retained.

### Validation and proposed boundary

- Final MSVC Release: **6/6 PASS**, 8.42 s, including relevance/planner/KI-007,
  dependency/bootstrap, scheduler, GameSession, physics and JobSystem foundation
  coverage. The new ordered-hysteresis reference is included.
- Final Clang 19 ASan/UBSan/LSan: **7/7 PASS**, 57.99 s, leak detection enabled;
  content/lifecycle coverage is included. Late-handoff fixture also passes.
- Documentation: **19 pages built**, 3.74 s, exit 0. The existing missing custom
  404-entry warning remains; no deployment/CI claim is made. Output is isolated
  under `build-3l3-worker/docs-derive-lookup-v3`.
- Prior 90-case physics matrix is preserved; physics implementation is unchanged.
  This slice reran the physics regression test, not the 90-case benchmark matrix.
- All healthy/500-peer fixtures converge, but streaming health returns failure as
  recorded above. These are not all-green performance results.
- New parallel-equivalence/TSAN/worker teardown tests: **not run / not applicable**
  to this no-job change. Existing JobSystem tests are not proof of the proposed
  future read boundary.
- Full CTest, official transport/Player guarantee, overload/recovery, new long soak,
  current-source security and CI: **not executed/closed in this slice**.

Proposed commit boundary, on top of the preserved prior Foundation work: the 3E
ordered/projection lookup correction and independent reference test; bounded
attribution scopes; this assessment and checkpoint ledger. Scope by **hunks**, not
whole inherited dirty files. No inherited planner/KI-007/physics work is silently
folded into this slice, and no commit or push is performed.

Remaining owners: 3E queries/hysteresis dominate average server derivation;
post-selection frame/encoding/commit plus observer/client service dominate the
recorded tail windows; independent client whole-world preflight and official
reliable delivery remain product-path blockers. Startup journal, overload,
security and CI remain open. **B — FOUNDATION 3L PARTIALLY READY.**

## Previous slice: dependency-complete bounded planning (2026-09-11)

**B — FOUNDATION 3L PARTIALLY READY.** The private planning boundary is implemented
and the final native checkpoint is `planning-boundary-v13`. It materially reduces
the 500-peer reload tail and enforces a finite planning-work limit. It also slows
convergence and worsens healthy load recipient gaps. Those regressions remain
visible failures, not an accepted non-starvation envelope. No commit, push, reset,
secondary-repository modification or Foundation 3M work occurred.

### Source state and method

Local HEAD/origin remain `a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1`. The starting
`referrer-join-final` checkpoint and its inherited uncommitted changes are retained.
The final manifest, `build-3l3-worker/evidence/planning-boundary-v13-source.json`,
contains 48 native file hashes; it was captured at 2026-09-12 00:29:46 UTC
(September 11 locally). The benchmark SHA256 is
`08968F59733728E815DC22CF669B37E7E3697692B3BD34AB9D9C1B3B9CD8E8FF`.
The complete tracked patch inventory includes inherited work, not just this slice.
This inventory is **not** a current-source security review.

Final raw artifacts have prefix `planning-boundary-v13` under
`build-3l3-worker/evidence/`: `local-500-50.out.log`, `none-200-50.out.log`,
`local-200-50.out.log`, `node-200-50.log`, their `*-planning.json` summaries,
and `reviewed-owners-timeline.json`. Checkpoints v1–v12 are intermediate evidence,
not final validation. Native source remains canonical locally; dockerbox is only
the scoped build/test worker. No source is copied back from it.

The unchanged fixture uses 200/500 peers, 50 Characters, five neighbours,
512 objects / 273,032 encoded bytes, ten root-motion tracks, owner input every
five ticks, actions every 40 callbacks, reliable Event each callback and 100
sequential RPCs per phase. Each measured phase has 301 ticks. Node content uses
real TLS; gameplay uses the established loopback path, **not official GNS**.
Performance runs are serialized with each other and do not overlap worker builds
or sanitizers. Quantiles use floor((N-1)*p). Recipient p95/p99 are **not measured**;
the recipient sampler remains max-only. Instrumentation overhead in isolation
is **not measured**. The fixture is unchanged, but longer bootstrap/planning
changes lifecycle phase alignment: before-load accepted Character states were
41,573; all three final cases accept exactly 41,480. Compare same-checkpoint
Control/Local/Node for throughput attribution, not raw cross-checkpoint totals.

### Design and exact boundary

The source path now is `GameSession::Step` → immutable 3E selection lease →
`RequestPlanning` → bounded `ProcessPlanning` → private closure/reconciliation/
reverse-Leave/referrer/cost construction → complete READY batch → existing 3J
publication passes → prepared frame → scheduler acceptance → Known/accepted
metadata commit. `ReplicationPlanning.hpp` is a private coordinator implementation,
not another semantic relevance or acceptance subsystem.

Incomplete state is not installed in Peer Desired/Pending or offered to 3J.
Only after the full input is reconciled, reference dependencies are inspected,
and the selected batch is completely costed does the planner swap in its complete
projection and mark READY. Deferral leaves READY unaccepted. Acceptance consumes
the batch and advances accepted metadata; scratch disposal and subsequent planning
remain budgeted. Cost includes required ancestors, hard references, reverse Leaves,
parent fixups, KI-007 clear/replace/removal and same-frame reference restores.
Current publication templates supply scalar payload values at submission.

The retained model reads Known/accepted ancestry in place under a revision guard;
it does not duplicate a second accepted graph. It preserves the previous reference
index and the synchronous reference/oracle path. Coroutine suspension is private
Main-thread execution, not a worker-thread mutation mechanism.

| Resource | Enforced contract |
| --- | --- |
| Planning work/tick | 65,536 default; native validated range 2–262,144 |
| Work before peer rotation | At most 2,048 resumes plus one charged peer visit |
| Private reservation credits/continuation | 262,144 |
| Aggregate reservation credits | 8,388,608, including current/retained selection leases |
| Live plus detached continuation peers | 1,024 |
| READY batch / existing 3J peer quantum | At most 512 complete operations by default |
| Existing global 3J limit | 8,192, unchanged |
| Existing Pending limits | 65,536 per peer / 1,048,576 session, unchanged |

A unit covers a peer visit or one resumed identity, edge, referrer, fixup, group
or disposal step. Inner loops yield, including nested accepted-reference and
reverse-dependency vectors. Repeated/older calls cannot refill the tick allowance.
At least two units are required to select a peer and advance work. The rotating
cursor does not advance with only one leftover unit, avoiding visit-without-work
starvation at odd/small budgets. Empty planning returns before timing/allocation.
Tree lookup, fixed-schema property and group operations retain existing finite
cardinality bounds: this is a work limit, not wall-clock preemption.

Credits are conservative reservations, **not exact live entries or bytes**. Reused
or released records remain charged until disposal completes. Growth is charged
before allocation; finite element types and existing selection/property bounds
bound storage. No maximum-peers × maximum-objects future candidate matrix is
preallocated. Per-object allocator/container overhead and exact retained planning
bytes are **not measured**. On exhaustion the existing structured peer resource
failure applies; no silent lifecycle drop or indefinite wait holding all capacity
is permitted. The explicit limit-denial test leaves Known unchanged and drains
all detached reservation accounting to zero.

### Invalidation and implementation corrections

Catalog identity, ancestry and hard-reference changes advance `DependencyCursor`;
those changes plus native soft-reference changes advance `PlanningCursor`.
Ordinary scalar/CFrame changes invalidate neither. Known/accepted ancestry/native
reference changes advance saturating `AcceptedRevision`; exhaustion fails closed.
Every resumed slice and READY submission validates the selection lease and both
relevant catalog/accepted revisions. A lease is input versioning, not ObjectId.

Disconnect cancels the coroutine borrowing a peer before erasing that peer.
Only owned scratch is detached for bounded reclamation. Catalog/reference values
are reacquired by generation-safe ObjectId after suspension. Accepted map nodes
are guarded by revision; accepted reference vectors are indexed afresh even when
a same-value journal update replaces vector storage without a semantic revision.
No Instance, Lua state or provider callback pointer is retained by the continuation.

Three measured integration corrections belong to this boundary:

- Preserve initial ordinary objects when the complete baseline fits the existing
  bootstrap quantum; incomplete mandatory bootstrap cannot be published.
- A destroyed required Character may remain in the previous 3E snapshot while
  that peer awaits relevance service. Discard private work and park until a new
  selection lease arrives, rather than disconnecting or repeatedly rebuilding
  the stale input. The benchmark now fails explicitly on initial-drain disconnect.
- When READY output consumes the 3J operation cap, remaining journal allowance
  may consume only a leading non-replicated property prefix with zero operations.
  It stops before replicated/lifecycle work. This fixes the measured journal
  progress regression without raising caps, truncating history or advancing Known.

Intermediate failures are retained: v1 bootstrap omissions; v2 journal progress;
v3 test mutation at disposal rather than incomplete planning; v4–v6 apparent
500-peer convergence failure actually caused by stale-required-input disconnect;
v12 a tick-zero service-gap counter underflow. The corresponding final regressions
pass. v8's redundant accepted-state copy was removed in v9; its 585.80-MB process
RSS high-water versus final 490.53 MB is a whole-process comparison, not an exact
planner byte attribution. v13 retains the same lean model plus lifecycle/limit
and small-budget fairness checks.

### MEASURED: primary 500-peer reload

Before is `referrer-join-v1` (repeat maxima in the previous section). After is v13.
All peers converge; one acquisition and one initial admission remain exact,
with a second authoritative admission on cached reload, not a second acquisition.

| Metric | Before | After |
| --- | ---: | ---: |
| Tick p95 / p99 / max ms | 84.342 / 95.949 / 178.456 | 44.187 / 48.886 / 56.293 |
| Character/root recipient max gap ms | 1,147.77 | 459.612 |
| Convergence max ms / ticks | 3,233.36 / 33 | 4,743.20 / 158 |
| Convergence p50 / p95 / p99 ms | 1,614.21 / 2,977.93 / 3,047.46 | 3,653.32 / 4,634.64 / 4,707.49 |
| Desired+Known+Pending examinations, max/tick | 138,752 | 56,007 |
| Desired referrer visits, max/tick | 536,510 | 31,562 |
| Accepted referrer visits, max/tick | 535,950 | 18,259 |
| Restore referrer visits, max/tick | 536,510 | 44,456 |
| Dependency edges, max/tick | 72,766 | 19,333 |
| Fixup planning max ms (old synchronous scope) | 128.261 | Superseded scope; not a zero-cost claim |
| New complete planner CPU max ms | Not measured under this scope | 9.7445 |
| 3J Pending high-water, cumulative | 194,239 | 225,792 |
| Whole-process RSS high-water bytes | 426,377,216 | 490,528,768 |
| Maximum selected/accepted operations per tick | 8,192 | 8,192 |
| Journal failures | 0 | 0 |

New planner CPU includes closure, reconciliation, referrer/group work and scratch
cleanup. It is broader than the old fixup-only scope; zeros in bypassed legacy
scopes do not mean those semantics disappeared. Counts can move between phases
when lifecycle work completes at different ticks. KI-007 correctness is established
by strict-application tests, not by requiring identical phase referrer totals.

| Reload per-tick quantity | p50 | p95 | p99 | max | Cumulative |
| --- | ---: | ---: | ---: | ---: | ---: |
| Charged planning steps | 28,167 | 65,536 | 65,536 | 65,536 | 9,319,343 |
| Desired+Known+Pending examinations | 0 | 41,993 | 56,001 | 56,007 | 1,081,666 |
| Desired referrer visits | 0 | 17,325 | 31,561 | 31,562 | 540,833 |
| Accepted referrer visits | 0 | 9,112 | 18,229 | 18,259 | 284,833 |
| Restore referrer visits | 0 | 23,925 | 33,288 | 44,456 | 540,833 |
| Dependency edges | 0 | 17,634 | 19,303 | 19,333 | 566,553 |
| Current reference-cost examinations | 0 | 1,605 | 3,119 | 3,119 | 50,000 |
| Accepted reference examinations | 0 | 808 | 1,617 | 1,617 | 25,246 |
| Restore property examinations | 0 | 2,448 | 2,456 | 3,110 | 50,000 |
| Complete groups | 0 | 5,955 | 5,969 | 5,974 | 255,500 |
| Peer continuation service slices | 16 | 32 | 110 | 134 | 5,005 |
| Complete READY batches | 0 | 0 | 100 | 133 | 500 |
| Complete planner CPU ms | 2.0811 | 8.4910 | 9.3740 | 9.7445 | 786.6563 |

Group interruption count is **not separately measured**; service-slice counts are
not unique interrupted groups. The complete-group and READY counters denote
planning completion, not acceptance. Accepted reload operations total 261,046.
Aggregate reconciliation/accepted-operation ratio is 4.1436, and all three
referrer passes total 1,366,499 visits / 261,046 = 5.2347 visits/accepted operation.
These whole-phase ratios are not comparable to the previous exceptional-tick
400.64 visits/operation without that denominator qualification.

Planning allocation traffic across the 158 ticks where its scope ran is
3,206,430 allocations / 141,482,484 bytes; maximum 66,595 allocations /
3,241,384 bytes. These are scope-active-tick statistics and temporary allocation
traffic, not retained memory or all-301-tick percentiles.

### MEASURED: fairness, finite drain, memory and journal

500-peer planning service max interval is 16 ticks (at most 15 intervening ticks
without service), maximum peer slice 2,048. Final 200-peer max interval is seven
ticks. Reload has 5,005 actual service slices and 500 READY batches. Per-tick
maximum-service-gap p95/p99 are both 16; these are **not per-peer distribution
percentiles**. Unit tests exercise five peers at 31/7 work and two peers at 3/1,
including the final one-unit remainder and tick zero. No-change ticks do no
planning work. READY waiting at 3J is excluded from planning-service delay.

Global planning reservation high-water is 4,574,325 / 8,388,608 credits; maximum
one continuation 8,635 / 262,144. In the 200-peer fixtures those are 1,417,005 and
6,400. Current input leases remain after drain, explaining nonzero aggregate
credits with no active work. Disconnect/limit-denial tests reclaim all such
accounting to zero. Final 500-peer process RSS high-water is 490,528,768 bytes,
up 64,151,552 bytes from the before checkpoint. No new long lifecycle soak or
leak classification follows from that single run; the finite continuation is
not a claim of memory-free scheduling.

| 500-peer phase | Before convergence ms / ticks | After convergence ms / ticks | Before → after Character/root max gap ms | After tick p99 / max ms | Planning CPU p99 / max ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Load | 2,646.49 / 33 | 4,866.05 / 158 | 682.477 → 905.915 | 53.040 / 54.962 | 8.830 / 9.709 |
| Evict | 1,295.15 / 32 | 3,931.25 / 212 | 596.435 → 330.409 | 28.585 / 30.753 | 11.474 / 12.310 |
| Reload | 3,233.36 / 33 | 4,743.20 / 158 | 1,147.77 → 459.612 | 48.886 / 56.293 | 9.374 / 9.745 |

Finite workload capacity/liveness is demonstrated by all phases draining, not by
instant materialization. Sustained overload capacity remains **unproved**. These
convergence slowdowns are material. The limit is retained because it supplies
the required upstream resource boundary and improves the primary reload tail,
not because every responsiveness or capacity metric improved.

Journal capacity stays 16,384. Startup-cumulative maximum lag is 16,214, giving
170 entries of observed minimum margin versus the previous 26. During measured
load/reload maximum lag is 6,135 (margin 10,249); eviction maximum is 1,044
(margin 15,340). Reload lag p50/p95/p99/max is 6,135; eviction is 0/1,044/1,044/
1,044. No retention failures, and all phase-end backlogs drain to zero. The
oldest pin remains pre-acceptance consumer progress, not an official transport
ACK measurement. Aggregate journal-backlog high-water is 3,492,980 records across
peers, longest backlog episode 240 ticks. The improved startup sample is not a
defensible production worst-case margin; startup/overload remain open. No journal
capacity increase or silent truncation was made.

### MEASURED: healthy 200-peer load differential

| Metric | Before Control | Before Local | Before Node | Final Control | Final Local | Final Node |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Tick p50 ms | 2.4482 | 10.1411 | 10.1849 | 2.5226 | 10.3011 | 11.6764 |
| Tick p95 ms | 3.3926 | 13.0260 | 14.5022 | 3.6096 | 22.5137 | 24.6593 |
| Tick p99 ms | 3.7286 | 41.4510 | 39.5345 | 4.0303 | 35.7087 | 39.0700 |
| Tick max ms | 9.3990 | 92.7522 | 94.3282 | 9.1105 | 51.3881 | 55.4808 |
| Character recipient states/wall second | 8,281.80 | 7,338.80 | 7,330.89 | 8,263.68 | 7,424.52 | 7,335.97 |
| Throughput loss vs same-run Control | baseline | 11.3864% | 11.4819% | baseline | 10.1548% | 11.2264% |
| Character/root recipient max gap ms | 201.649 | 395.763 | 402.018 | 201.873 | 686.789 | 703.876 |
| Reliable Event ACK max gap ms | 19.7726 | 89.8830 | 96.8259 | 19.8752 | 87.3472 | 87.9287 |
| RPC p50 ms | 33.313 | 41.332 | 41.058 | 33.022 | 41.776 | 42.337 |
| RPC p95 ms | 34.969 | 44.098 | 44.795 | 35.358 | 66.977 | 66.766 |
| RPC p99 ms | 35.633 | 141.078 | 149.295 | 35.718 | 106.235 | 107.938 |
| RPC max ms | 35.986 | 185.971 | 186.800 | 36.509 | 114.037 | 118.936 |
| Action p99 / max ms | 35.302 / 35.632 | 42.982 / 43.312 | 43.797 / 44.741 | 35.794 / 35.880 | 43.329 / 43.458 | 44.674 / 48.970 |
| Load convergence max ms / ticks | n/a | 878.154 / 14 | 882.492 / 14 | n/a | 1,416.88 / 51 | 1,471.19 / 51 |

All final cases have 100 RPC completions, zero timeouts/errors/crashes, zero
Character scheduler rejections and zero action submission failures/rejections.
Action percentiles use only seven completed samples per load phase; do not claim
a high-confidence tail distribution. Root-motion requests/commits match and
eleven cell crossings remain correct. Control passes every health phase. Local/
Node converge but fail unchanged load/reload health gates (their test exit 1 is
retained, not reclassified as success).

| Final load scope, p50 / p95 / p99 / max / cumulative ms | Control | Local | Node |
| --- | ---: | ---: | ---: |
| Complete planning | 0 / 0 / 0 / 0 / 0 | 0 / 4.80 / 7.35 / 7.74 / 215.76 | 0 / 5.42 / 7.99 / 8.41 / 231.98 |
| 3E relevance, inclusive | 1.20 / 1.56 / 1.80 / 1.82 / 222.93 | 1.95 / 9.67 / 10.61 / 15.13 / 1,485.53 | 2.13 / 10.84 / 12.90 / 16.68 / 1,603.48 |
| Character graph synchronization | 0.30 / 0.61 / 0.72 / 0.74 / 100.12 | 0.36 / 0.86 / 0.98 / 1.03 / 129.67 | 0.87 / 1.06 / 1.21 / 1.29 / 256.69 |

No-content load planning executes zero units and zero planning allocations. Control
tick p99 rises 0.302 ms in this comparison; max and throughput remain close. This
is an observed control result, not a separate statistically repeated overhead
experiment. Character graph synchronization retains the previous derived typed
inspection correction; no broad graph polling or scheduler-policy change was
introduced by this slice.

### Remaining causal owners and non-starvation status

The Local worst gap is 686.789 ms, from fixture trace time 7,743.14 to 8,429.93 ms,
recipient ticks 504→516. Ticks 505–516 select 8,192 operations each. Their whole
serialized iterations last 47.44–84.84 ms; server Engine+Session work is
26.27–39.19 ms, while protocol-observer drain adds approximately 18.71–21.19 ms
per iteration. Planner service itself is 1.08–5.21 ms in these ticks. The gap is
consecutive expensive publication/application iterations, not one 687-ms planner
call. Observer drain is fixture work, not production server CPU. Exact per-state
send/receive/apply residence is **not measured** by this trace.

Remaining measured owners, distinguished by stage:

1. Recurring 3E query/hysteresis/relevance: 500-peer reload 3,035.69 ms cumulative,
   p99/max 16.906/17.712 ms. It dominates repeated server busy time after planning
   isolation; its 64-peer service cap does not bound each peer's object work.
2. Post-3J publication: reload frame preparation/selection p99/max 13.768/13.991 ms,
   structural encode 13.923/14.372 ms, commit 5.027/5.299 ms and Remote
   materialization 2.844/13.714 ms. These scopes' nesting must be respected;
   do not simply sum all totals. Saturated downstream batches still form long
   runs of expensive ticks. Client/observer work further extends wall time.
3. Bounded planning itself: reload p99/max 9.374/9.745 ms; eviction 11.474/12.310 ms.
   The cap bounds amplification but does not promise a sub-millisecond phase.
   Convergence/critical-service tradeoffs require further measured work.
4. Independent client whole-world preflight and official reliable backlog remain
   retained blockers: approximately 89–101 ms preflight at 8,193 identities and
   approximately 3.8-second official Remote service delay. They were not rerun
   or fixed here. Loopback RPC improvements cannot close official transport.

Next investigation should separate per-object 3E work from post-selection
publication/apply burst cadence, preserving this dependency-completeness barrier.
Do not weaken 3J caps to hide convergence latency. Official transport's next slice
still needs uniquely correlated reply enqueue/select/send/receive/completion
timestamps and reliable queue ages; neither queue age nor a transport ACK pin
on journal history was established here. Overload/recovery, startup retention,
current-source security and CI remain open. The sealed security artifact remains
unchanged and is not declared green.

### Validation and proposed commit boundary

MSVC Release final checkpoint: **6/6 targeted CTest PASS, 8.41 s** (GameSession,
relevance, replication, foundation, scheduler contract and physics backend).
Benchmark build passes with the retained vendor CRT linker warning. New tests
cover inner work limits/repeated ticks, dependency-complete suspension, 3J-only
acceptance, KI-007 legacy/planned matrices, reverse Leaves, fair small budgets,
zero-work idle, snapshot lifetime, reference storage churn, soft-reference/reparent
invalidation, stale Character selection parking, fresh generation reload,
disconnect/detached cleanup and finite record-limit failure.

Final Clang 19 ASan/UBSan/LSan: **7/7 targeted CTest PASS, 57.79 s**, plus the
late-handoff fixture PASS (13 GCHR and two reliable Event messages sent after
production in the same step). `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`; Clang 19 uses
`-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all`.
No sanitizer error or leak report occurs. The seven tests add content-availability
lifecycle coverage to the six Windows targets. The raw final sanitizer log is
retained alongside the source manifest; unrelated worker containers remain up.
Documentation builds **19 pages, PASS, 2.23 s**, to
`build-3l3-worker/docs-planning-boundary-v13/`, retaining the
existing missing-custom-404 warning. This is a local build, not deployment/CI.
The prior physics matrix's 90 passing cases remain retained evidence; this slice
does not modify the physics adapter. No new official Player, full CTest, long soak,
overload, security or CI completion is claimed.

Proposed logical boundary, after validation: private complete-group continuation,
immutable selection leases and revision guards, GameSession integration and
zero-operation journal progress, bounded aggregate diagnostics, the focused
coordinator/benchmark tests, and this architecture/validation/known-issue update.
This depends on inherited KI-007/reference-index work; do not commit the entire
dirty tree merely because it shares files. Temporary worker logs and historical
variants are evidence, not production trace output. Source remains uncommitted.

## Previous slice: referrer/discovery attribution and ordered lookup (2026-09-11)

**B — FOUNDATION 3L PARTIALLY READY.** Local HEAD remains
`a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1`; no commit, push, reset or 3M.
Only Engine source changes in this slice. All inherited dirty/untracked work is
preserved. The independent transport/client, overload, startup journal, security
and current-source CI gates remain open.

### Source and measurement method

Retained artifacts under `build-3l3-worker/evidence/` distinguish:

- `ki007-index-v1` / `ki007-final-source.json`: prior indexed correctness baseline,
  46 native hashes. Its benchmark SHA256 is recorded in the previous section.
- `referrer-attribution-v1`: new fixed phase/counter scopes; MSVC 6/6 PASS,
  17.84 s. No performance claim from this intermediate executable.
- `referrer-attribution-v2`: adds per-tick selected/accepted denominators;
  MSVC 6/6 PASS, 17.18 s; unchanged 500-peer measurement. Benchmark SHA256
  `AEEACC1C99F8713680E486A38D7D9159D4CFD1C828E60B33AD4039AAE83993C0`.
- `referrer-join-v1`: one correction, call-local ordered lookup; 47/47 native
  local/worker hash matches before compilation, benchmark SHA256
  `11E15F918F7F7E63F340D854AA29F50B21F3ABC8BCC96774E05C6096337BC58C`.
  `referrer-join-repeat` repeats the same binary, not another source variant.

Each checkpoint retains a complete tracked patch inventory and native manifest;
raw logs and `*-planning.json` preserve all per-phase p50/p95/p99/max/cumulative
CPU and count summaries, scope allocation counts and worst-tick allocated bytes.
`referrer-join-v1-reviewed-owners.json` contains the gameplay results. This is
measurement provenance, **not a security review**. The sealed security artifact
is not edited or declared green.

Fixtures remain 500 or 200 peers, 50 Characters, five neighbours, 512 objects /
273,032 payload bytes, ten root-motion tracks, owner input every five ticks,
actions every 40 callbacks, reliable Event each callback, 100 sequential RPCs
per phase, 301 measured ticks per phase. Gameplay transport is loopback; Node
uses the existing real-TLS content fixture. Builds/tests and performance cases
are serialized on dockerbox, using preserved incremental caches. Quantiles use
floor((N-1)*p). Recipient distributions are **not measured**: maxima are retained.
Added instrumentation overhead is **not measured in isolation**; compare the
identically instrumented v2/join runs for the direct correction. Historical
indexed versus new instrumented totals are not a pure instrumentation experiment.

### MEASURED: why the visits exist

The source path is GameSession's bounded 3E peer update, `RecordDesiredState`,
`BuildDependencyClosure`, Pending reconciliation and full Desired/Known difference,
reverse Leave dependency construction, derived Relevant rebuilding, then
`ProduceRelevanceFrame` reference/group planning, 3J selection, encode, reliable
acceptance and commit. Selection changes during Character replacement invalidate
the affected peer plan even though most streamed scenery is unchanged.

The maximum-discovery tick, **1109**, contains 64 peer discoveries, 69,280 Desired
examinations (all already Known), 69,472 Known examinations, zero Pending
examinations and 192 inserted candidates. Closure uses 66,376 seeds and traverses
72,580 edges, including 69,454 duplicate visits. It is complete reconciliation,
not 138,752 distinct new transitions. Selected/accepted operations are 8,192,
including prior Pending and journal work: **722.67 examinations/new candidate**,
or **16.94 examinations/selected or accepted operation**. These denominators
describe amplification, not causal throughput fractions.

| Discovery tick 1109, before join | CPU ms | Allocations | Allocated bytes |
| --- | ---: | ---: | ---: |
| Dependency seed preparation | 0.522 | 438 | 2,660,896 |
| Dependency edge walk | 13.516 | 69,694 | 5,171,888 |
| Pending/Desired/Known reconciliation | 8.868 | 512 | 579,264 |
| Reverse Leave dependency index | 0.189 | 128 | 4,608 |
| Derived Relevant rebuilding | 5.611 | 69,280 | 1,662,720 |

The edge-walk scope includes property inspection and identity/catalog lookups;
its edge counter counts discovered parent/hard-reference edges, not every scalar
property visited. It is not a pure per-edge microbenchmark.

The exceptional planning tick is **1115**, with 496 planning calls. Each call
walks the sorted unique Desired set for current parent/reference costs, the
accepted-object map for KI-007 removal requirements, then Desired again for
restoration after selection. This is **peer × referrer × separate passes**, not
a complete referrer walk once per candidate group. Total visits are 536,510
Desired + 535,950 accepted + 536,510 restoration = **1,608,970**, against 4,016
selected and accepted operations: **400.64 visits/operation**, or 133.59 for
each Desired pass alone. Unique identities are guaranteed within each set/map
pass; distinct `(peer, referrer)` and global distinct-referrer counts across all
calls are **not measured**. No high-cardinality diagnostic set was introduced.

No-op evidence: zero parent changes; 484,862 Desired objects have no indexed
reference fields; 510,409 accepted objects have no accepted references. All
49,600 current reference-cost visits target something other than a pending Enter
(zero pending Enter matches); restoration has zero matching Enter references.
The accepted pass is not disposable: 25,541 accepted edges yield 496 removal
requirements and 492 emitted clears, with departing referrers accounting for
the difference. Required accepted-edge removal semantics remain KI-007's owner.

| Planning tick 1115 | Before CPU ms | After CPU ms | Before allocations / bytes |
| --- | ---: | ---: | ---: |
| Desired scan, inclusive | 138.570 | 105.575 | 49,600 / 2,380,800 |
| Of which indexed reference cost construction | 5.103 | 4.817 | 49,600 / 2,380,800 |
| Desired traversal/lookups/parent checks, exclusive | 133.467 | 100.758 | 0 / 0 |
| Accepted-edge scan | 6.628 | 6.357 | 992 / 43,648 |
| Restoration | 31.194 | 16.227 | 0 / 0 |
| Clear emission | 0.062 | 0.060 | 0 / 0 |
| Group discovery/selection | 2.108 | 1.997 | 14,084 / 451,296 |
| Of which group costing/selected-set work | 0.419 | 0.430 | 3,524 / 140,960 |
| Structural encode | 4.135 | 4.208 | 23,968 / 8,003,192 |
| Remote materialization synchronization | 17.976 | 17.792 | 4,096 / 131,072 |
| Catalog refresh | 0.169 | 0.166 | 1,899 / 116,472 |

There are 3,520 groups built, 6,056 group selection work units and zero encode
retries in this tick. Child scopes must not be summed with their parents.
Combined fixup CPU per selected/accepted operation falls from 43.95 to 31.94 us;
this is a ratio of the whole tick's work, not the individual operation's latency.
The listed allocation counts/bytes are identical after the join. Allocation
volume is temporary traffic, not retained memory. The join adds no
allocation. The remaining exclusive scan is **not separately attributed** to
Known hashing, iterator traversal and parent inspection; each remains inside
that measured scope. Reference field lookup itself is not separately timed
from reference-cost map construction. The narrow correction establishes a
causal repeated-search reduction, not that all remaining time is map lookup.

### Retained correction and bounds

`src/network/PlanningLookup.hpp::detail::FindPlanningObject` and its three
`ReplicationCoordinator.cpp::ProduceRelevanceFrame` call sites reuse sorted
map iterators within the non-mutating pass. A lookup walks at most **8** nodes,
then `lower_bound` jumps over sparse unrelated ranges; final full-ObjectId equality
validates generation. Dense lookups avoid restarting root searches. At the worst
planning tick the join records 1,680,538 advances, 2,826 fallback tree searches,
and exactly 8 maximum consecutive advances. Visit/property/group counts do not
decrease. Eight is a locality threshold, **not a per-tick planning budget**.

Continuation entries/bytes: **0 / 0**. Two simultaneously live iterators plus a
local advance count are stack-only; no iterator crosses acceptance, catalog
mutation, callbacks, disconnect or a tick. No dirty subscriptions or derived
semantic graph exist. The native test reports **8 bytes per iterator**. Whole
planning remains finite under configured world/peer
limits but **not bounded by a defensible narrow per-tick work contract**. Existing
Pending limits remain 65,536 per peer / 1,048,576 global; measured peak is 194,239.
Accepted-reference and catalog-index memory are unchanged: reload reports
35,212,704 accepted ancestry/reference bytes and 30,872 index bytes. No claim of
whole-pipeline overload safety follows from these individual finite structures.

Diagnostics add six fixed `WorkDuration` slots and nine uint64 counters, or
552 bytes per capture by native layout accounting (not an RSS measurement).
The test-only two-sided tick record adds 1,120 bytes including its two operation
denominators. The existing trace retains at most four completed phases plus one
active buffer, each capped at 1,201 records: added logical trace storage at those
capacities is 6,725,600 bytes, excluding allocator overhead. No production
timestamp/identity history or file logger is
added. Counters saturate; the helper checks the existing opt-in capture pointer.

Safety/liveness of general continuation is a **stop boundary** for this slice:
KI-007 requires complete accepted-edge removal groups, reverse Leaves need all
dependents, and `CollectGroup` treats absent Pending work as absent dependencies.
Saving an iterator and publishing an incomplete prefix is unsafe. A larger
revision-validated completeness barrier, bounded scratch and rotating fair
service design is required before adding a cross-tick frontier. No new frontier
fairness or progress claim is made; existing 3E/3J rotation is unchanged.

### MEASURED: unchanged 500-peer reload before/after

| Metric | Indexed baseline | Instrumented before | Join | Same-binary repeat |
| --- | ---: | ---: | ---: | ---: |
| Tick p95 ms | 84.859 | 84.020 | 84.342 | 85.030 |
| Tick p99 ms | 97.680 | 96.079 | 95.949 | 95.770 |
| Tick max ms | 221.427 | 227.578 | 178.456 | 189.084 |
| Combined fixup max ms | 170.473 | 176.494 | 128.261 | 134.762 |
| Combined fixup cumulative ms | 313.284 | 325.846 | 257.138 | 265.040 |
| Character/root recipient max ms | 1,200.670 | 1,210.800 | 1,147.770 | 1,168.200 |
| Converge max ms / ticks | 3,312.04 / 33 | 3,293.07 / 33 | 3,233.36 / 33 | 3,257.35 / 33 |
| General discovery maximum | 138,752 | 138,752 | 138,752 | 138,752 |
| Dependency-edge maximum | 72,766 | 72,766 | 72,766 | 72,766 |
| Worst planning Desired visits | 536,510 | 536,510 | 536,510 | 536,510 |
| Worst planning property visits | 99,692 | 99,692 | 99,692 | 99,692 |
| Pending peak | 194,239 | 194,239 | 194,239 | 194,239 |

Identically instrumented before/join fixup p50/p95/p99/max is
**0 / 4.764 / 4.839 / 176.494** versus **0 / 4.100 / 4.308 / 128.261 ms**.
The repeated maximum of 134.762 ms corroborates benefit without claiming a
confidence interval. Tick p99 and recipient tails are not materially closed.
The gap still spans ticks 1105–1117, not one >1-second tick. Accepted Character
states remain 24,678 in all reload cases; wall-second throughput is 3,149.74
indexed versus 3,182.08 join. Simulated-time throughput is not used as evidence.

| Other 500-peer phase | Indexed convergence ms / ticks | Join convergence ms / ticks | Indexed / join recipient max ms | Join tick p99 / max ms |
| --- | ---: | ---: | ---: | ---: |
| Load | 2,745.10 / 33 | 2,646.49 / 33 | 711.648 / 682.477 | 82.916 / 109.792 |
| Eviction | 1,300.30 / 32 | 1,295.15 / 32 | 581.400 / 596.435 | 69.609 / 73.872 |

Every peer converges. Selection equals commit (261,545 load/evict, 263,541
reload), actual maximum stays **8,192**, with 31 saturation ticks per phase.
Acquisition is exactly **1**; initial admission is exactly **1**, explicit reload
brings total admissions to **2**. Journal failures **0**; post-service margins
remain **10,249 / 15,340 / 10,249** load/evict/reload. The inherited startup
high-water is **16,358 / 16,384**, leaving **26** entries. It is not caused by
this lookup change and is not declared safe or increased. Aggregate peer journal
backlog high-water 3,067,500 counts peer-record requirements, not global entries.

### MEASURED: unchanged healthy 200-peer load

| Metric | Before control | Before Local | Before Node | After control | After Local | After Node |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Tick p50 ms | 2.502 | 10.040 | 10.057 | 2.448 | 10.141 | 10.185 |
| Tick p95 ms | 3.467 | 13.341 | 13.926 | 3.393 | 13.026 | 14.502 |
| Tick p99 ms | 4.055 | 40.079 | 40.327 | 3.729 | 41.451 | 39.535 |
| Tick max ms | 8.889 | 92.480 | 97.973 | 9.399 | 92.752 | 94.328 |
| Accepted Character states / wall second | 8,281.01 | 7,330.99 | 7,331.24 | 8,281.80 | 7,338.80 | 7,330.89 |
| Throughput loss vs own control | 0% | 11.472% | 11.469% | 0% | 11.386% | 11.482% |
| Character/root recipient max ms | 202.329 | 404.849 | 397.816 | 201.649 | 395.763 | 402.018 |
| RPC p99 / max ms | 35.683 / 35.857 | 145.264 / 185.741 | 143.101 / 192.214 | 35.633 / 35.986 | 141.078 / 185.971 | 149.295 / 186.800 |
| Reliable Event ACK max gap ms | 19.18 | 92.01 | 90.99 | 19.773 | 89.883 | 96.826 |
| Action p99 / max ms | 34.463 / 35.293 | 42.646 / 42.760 | 42.803 / 43.960 | 35.302 / 35.632 | 42.982 / 43.312 | 43.797 / 44.741 |

All six load cases accept exactly 41,573 Character states, not equal wall-clock
throughput. Scheduler rejections and RPC timeouts/errors/crashes remain **0**.
RPC after p50/p95 is 33.313/34.969, 41.332/44.098 and 41.058/44.795 ms.
Action quantiles have only seven completed samples per phase. Reliable Event
ACK gaps are maxima, not inferred Event service percentiles. Recipient p95/p99,
official transport queue ages and official Player completion latency are
**not measured** here. Local/Node both converge in 14 ticks (878.154/882.492 ms).
Control passes health; Local/Node exit with the expected explicit gameplay-health
failure after convergence. The Node TLS test is therefore **FAIL**, not a green
Node integration claim, despite successful acquisition and zero RPC errors.

### Validation and remaining owners

Targeted final MSVC Release: **6/6 PASS**, 17.51 s, including KI-007's ten-scenario
matrix, dependency/bootstrap, exact-cap, scheduler, GameSession, foundation and
physics tests. The new ordered-lookup test compares dense/sparse sorted queries
to independent `map::find`, checks the eight-step maximum, fallback searches,
missing generations, destroy/recreate and empty-map/new-pass behavior. Coordinator
lifecycle tests retain disconnect/relevance/removal coverage. No new persistent
identity lifetime exists.

Final-source Clang 19 ASan/UBSan/LSan: **7/7 PASS**, 76.23 s, with
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. The additional late-handoff
case passes with 13 GCHR / 2 reliable Event sends in the production step. The
raw `referrer-join-v1-linux-sanitizers.log` is retained locally. These are fresh
results, not the prior KI-007 sanitizer checkpoint. The affected physics unit
tests pass on both platforms; the unchanged prior **90/90 physics matrix** is
preserved, not rerun or represented as a new measurement. Documentation build:
**19 pages PASS**, with the existing missing-404-entry warning; deployment is
not run. Diff whitespace check passes.

The remote-build-worker and remote-docker-worker workflows keep sustained native
work on verified dockerbox (192.168.0.108), under
`C:\Sandbox\Codex\Workspaces\gargantuan-runtime-host-f1-1\engine`, with existing
MSVC and Clang caches. Native compilation uses four jobs and CTest two; sanitizer
Docker is capped at four CPUs / 12 GiB. CMake regeneration on the mounted source
took 353.8 s, then the actual incremental build and tests completed normally.
The task-owned sanitizer container exits/removes itself; unrelated containers
remain running. No source is copied back from the worker. The final native
manifest `referrer-join-final-source.json` matches all 47 local/worker files and
the measured `referrer-join-v1` source. Broader full CTest, official-network/client,
overload, current-source security and CI/deployment gates are **not executed**
by this slice. The final source and documentation inventory is retained separately
as `referrer-join-ledger-source.json` / `referrer-join-ledger-engine.patch`.

Remaining owners in this workload, rather than a generic streaming label:

1. Complete-peer referrer planning: 128.261–134.762-ms fixup maximum, with
   >500K visits per pass. Further exact lookup/no-op attribution and an explicit
   dependency-complete continuation design are required; **blocks 3L**.
2. Recurring 3E selection/closure/reconciliation: final 500-reload pre-3J exclusive
   p99/max 42.232/43.611 ms; 138,752 examinations and dependency walks remain.
   Whole-work accounting, not an outer candidate cap, is required; **blocks 3L**.
3. Structural encoding and Remote materialization synchronization: reload maxima
   12.578 and 17.792 ms respectively. These are separate measured server owners,
   not proof of official transport delay; retain for the next profile.
4. Independent client preflight (historical 89–101 ms at 8,193 replicas), official
   GNS Remote delay (~3.8 s historically), overload, 26-entry startup journal
   margin, final security and CI remain **open blockers**, not rerun/closed here.

Character graph synchronization max is 1.850 ms in final 500 reload; catalog
retirement is zero there and catalog refresh max 2.229 ms. They do not explain
the exceptional 128-ms fixup spike. Metrics rank scopes within this fixture;
unknown official-client/transport work cannot be ranked from loopback results.

Proposed commit boundary: the private ordered lookup helper and its coordinator
call sites, bounded attribution counters/scopes, ordered-lookup regression test,
benchmark diagnostic fields, and corresponding architecture/ledger/issue updates.
It depends on the preserved KI-007/reference-index baseline; do not mix unrelated
inherited Foundation changes, official transport or client work into that commit.
Nothing is committed or pushed by this slice.

## Previous slice: KI-007 correction and measured reference index (2026-09-11)

**B — FOUNDATION 3L PARTIALLY READY.** Local uncommitted Engine
HEAD remains `a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1`. Only the primary
repository is modified. The inherited 57 tracked dirty paths are preserved;
there is no commit, push, reset or 3M work. Secondary provider sources are unchanged.

### Exact source checkpoints

Local `build-3l3-worker/evidence/ki007-correctness-v1..v5-*` worker results and
v2..v5 native manifests distinguish the candidates. v5 has 46 local/worker native
hash matches. The v4 benchmark binary uses identical production source to v5;
only the order-independent test assertion changed between them. A source manifest
is not by itself proof that every cached executable is rebuilt.

- v1: accepted reference bookkeeping and exact removal requirements; original
  regression and targeted MSVC suite **6/6 PASS**.
- v2: nine-scenario WeldConstraint matrix, including four references on two
  referrers; targeted MSVC **6/6 PASS**, 19.27 s.
- v3: byte-limit rejection, fixed diagnostic counters and commit-maintained
  reference memory accounting; targeted MSVC **6/6 PASS**, 18.08 s.
- v4: independent departing soft-referrer progress; MSVC **5/6 PASS**. Its tenth
  test incorrectly required the target to remain Known after the first quantum.
  Depending on full ObjectId order, one referrer can leave and make the remaining
  clear/removal group fit in that same frame. Client preflight passed; the sole
  failure was that over-specific assertion. Sanitizers were not launched after
  the failed native gate.
- v5: assertion checks bounded dependency-complete progress and convergence
  without imposing an identity order; MSVC **6/6 PASS**, 17.69 s. Separate
  Clang 19 ASan/UBSan/LSan **7/7 PASS**, 75.55 s, `detect_leaks=1`, plus
  `--late-handoff` PASS (13 GCHR / 2 Event sends in the production step).
  Physics matrix **90/90 PASS**. The unchanged 500-peer fixture converges;
  one acquisition/initial admission, exact 8,192 cap and zero journal failures.
  Its non-starvation gate still fails. **KI-007 correctness gate closed here,
  before the index candidate was restored.**
- `ki007-index-v1`: 46 native source hashes match local and worker; targeted
  MSVC **6/6 PASS**, 17.42 s, including the new index invalidation test. Both
  benchmark and native test targets rebuilt. Final sanitizer result is recorded
  in the validation section below, not inferred from correctness-only v5.

Correctness-only benchmark SHA256:
`C7E11503A6B6E44B15AF096957836AE51E1B5E8EBDBC8FC1F34A2D86FA193A26`.
Indexed benchmark SHA256:
`6EE40F2A78A8DA377FE5341B076B7CC62840A26FFFDEEDAEB27DAA266F87F656`.
Source manifests, tracked patch inventories, raw logs and derived analyses are
retained under `build-3l3-worker/evidence/ki007-*`. Secondary Node source is not
modified or recaptured; the existing real-TLS content fixture runs final Engine.

### Correctness evidence

The original failing structural-first case now produces **two operations, one
nil fixup**, and strict client preflight succeeds. Journal-first produces one nil
update and its subsequent removal also succeeds. No client validation changed.

The ten-scenario matrix covers unknown B, explicit nil, already-Known B, same
structural progression Enter B, pending referrer Leave, destroyed referrers,
target/referrer destruction after preparation, disconnect/reconnect, genuinely
oversized groups, and soft-referrer-first recovery. It also tests fresh identity
reacquisition, multiple properties/referrers, unchanged Known before acceptance,
byte-limit retries/rejection and deliberately malformed dangling-reference frames.
Four surviving property updates plus removal have exact selected cost five;
allowance four defers them and quantum four fails without a prefix. Departure of
soft referrers may reduce a temporary oversized set without changing the quantum.
Existing dependency/bootstrap, scheduler, journal and GameSession tests remain in
the targeted suite.

Native layout reports **16 bytes per reference pair and 24 bytes per vector**.
The vector resides in an existing accepted-object record, not a new dense matrix.
Only non-nil accepted native references allocate entries. Full ObjectIds plus
frozen canonical property pointers carry no Instance/peer/Lua lifetime. The
accepted ancestry logical-byte metric includes vector capacity and is maintained
at acceptance/removal, not recomputed by a new per-tick all-object scan. The
call-local removal map is bounded by accepted edges into pending Leaves; it is
not retained after preparation. General whole-peer planning CPU remains open.

### MEASURED: unchanged 500-peer reload

All runs use 500 peers, 50 active Characters, five neighbours, the same 512-object
/ 273,032-byte package, ten root-motion tracks, owner input every five ticks,
actions every 40 callbacks, reliable Event per callback and 100 sequential RPCs
per phase. Each phase has 301 measured ticks. Gameplay transport is loopback;
Node provides real-TLS content acquisition, not official GNS gameplay transport.
Builds, sanitizers and benchmark cases did not overlap on the worker.

| Reload metric | Original full scan | Correctness-only v5 | Indexed v1 |
| --- | ---: | ---: | ---: |
| Tick p99 / max ms | 96.433 / 488.404 | 97.644 / 295.639 | 97.680 / 221.427 |
| First-pass properties, worst planning tick | 2,840,760 | 2,829,306 | 49,600 |
| Clear-pass properties, same tick | 2,836,194 | 492 | 492 |
| Restore-pass properties, same tick | 2,840,760 | 2,829,306 | 49,600 |
| Total three-pass properties, same tick | 8,517,714 | 5,659,104 | 99,692 |
| First-pass CPU ms, same tick | 187.899 | 187.492 | 139.848 |
| Clear-pass CPU ms, same tick | 190.840 | 0.069 | 0.060 |
| Restore-pass CPU ms, same tick | 54.894 | 55.730 | 30.565 |
| Combined fixup CPU ms, same tick | 433.633 | 243.291 | 170.473 |
| Emitted fixups, same tick | 494 | 492 | 492 |
| Encode retries, same tick | 0 | 0 | 0 |
| Dependency-edge maximum / tick | 72,766 | 72,766 | 72,766 |
| Character/root recipient max gap ms | 1,524.95 | 1,274.28 | 1,200.67 |
| Reload convergence ms / ticks | 3,576.08 / 33 | 3,391.68 / 33 | 3,312.04 / 33 |
| Accepted Character states / wall second | 3,031.22 | 3,112.07 | 3,149.74 |

These are workload-preserving comparisons, not statistically repeated confidence
intervals. Correctness changes accepted clearing semantics, so 494 versus 492
emitted updates is not an index-only counterfactual. v5 versus indexed v1 has
the same 492 updates and 24,678 accepted Character states. The index reduces
property traversal without altering that accepted workload. It does not reduce
dependency-edge examinations or general discovery's **138,752** maximum.

Indexed worst planning tick **1115** spends 170.473 ms in the three disjoint
fixup phases, inside 178.484 ms StructuralSelection (do not sum parent and child).
It also spends 18.200 ms in Remote materialization and 0.969 ms in Character
graph synchronization. There are 496 planning calls, 536,510 Desired referrer
visits, 535,950 accepted-object visits, 25,541 accepted-reference visits and 496
removal requirements. First-pass allocations are 50,592; restore/clear allocations
are zero within those scopes. Scalar and hard-nil property visits are zero in
the indexed passes. Nil-clearing correctness comes from accepted edges, not
from treating nil fields as reference targets.

The full reload's combined fixup CPU p50/p95/p99/max is
**0 / 4.494 / 4.723 / 170.473 ms**, cumulative **313.284 ms**. Its property
examinations p50/p95/p99/max are **0 / 3,200 / 3,200 / 99,692**.
The 1,200.67-ms gap spans ticks **1105–1117**: a sequence of expensive ticks plus
the exceptional planning tick, not a single 1.2-second server tick. Scheduler
rejections remain zero. No per-state official-network latency attribution is
inferred from this serialized fixture timeline.

| 500-peer phase | Original convergence ms / ticks | Indexed convergence ms / ticks | Original / indexed recipient max ms | Indexed tick p99 / max ms |
| --- | ---: | ---: | ---: | ---: |
| Load | 2,702.20 / 33 | 2,745.10 / 33 | 767.615 / 711.648 | 84.215 / 111.549 |
| Evict | 1,464.84 / 32 | 1,300.30 / 32 | 641.457 / 581.400 | 70.724 / 76.986 |
| Reload | 3,576.08 / 33 | 3,312.04 / 33 | 1,524.950 / 1,200.670 | 97.680 / 221.427 |

All peers converge. Acquisition count remains **1**, initial admission **1**;
explicit reload brings total admissions to **2**, not duplicate initial admission.
Actual selected maximum remains **8,192**, with 31 saturation ticks per streaming
phase. Selected equals committed: 261,545 load/evict; 263,541 reload. Pending
high-water is 190,464 load and 194,239 evict/reload; there is no new frontier.
Journal backlog high-water is 3,067,500 aggregate peer-record requirements, not
that many retained global journal entries. Failures are zero; post-service
retention margins are **10,249 / 15,340 / 10,249** load/evict/reload. Startup's
cumulative lag remains **16,358 of 16,384**, hence **26** entries of minimum
whole-session margin. No retention increase or startup-closure claim is made.

### MEASURED: unchanged healthy 200-peer load

Before is retained `late-send-v1` / `late-send-final-source.json`; after is
`ki007-index-v1`. Each uses the same 200 peers / 50 Characters / five neighbours
and schedules above. Do not compare old 401-tick cumulative work to these
301-tick runs. Quantiles use floor((N-1)*p); recipient gaps remain **max-only**.

| Metric | Before control | Before Local | Before Node | After control | After Local | After Node |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Tick p50 ms | 2.762 | 10.524 | 10.633 | 2.502 | 10.040 | 10.057 |
| Tick p95 ms | 3.662 | 14.444 | 14.442 | 3.467 | 13.341 | 13.926 |
| Tick p99 ms | 4.172 | 40.422 | 41.891 | 4.055 | 40.079 | 40.327 |
| Tick max ms | 9.531 | 99.666 | 99.546 | 8.889 | 92.480 | 97.973 |
| Accepted Character states/s | 8,277.64 | 7,315.23 | 7,316.35 | 8,281.01 | 7,330.99 | 7,331.24 |
| Throughput loss vs control | 0% | 11.627% | 11.61% | 0% | 11.472% | 11.469% |
| Character/root max gap ms | 202.076 | 398.410 | 403.002 | 202.329 | 404.849 | 397.816 |
| Reliable Event ACK max gap ms | 19.086 | 89.566 | 91.080 | 19.18 | 92.01 | 90.99 |
| RPC p50 ms | 33.039 | 41.586 | 32.653 | 33.527 | 41.602 | 41.097 |
| RPC p95 ms | 35.530 | 44.318 | 34.866 | 35.046 | 44.367 | 44.669 |
| RPC p99 ms | 35.763 | 147.320 | 124.946 | 35.683 | 145.264 | 143.101 |
| RPC max ms | 36.468 | 152.471 | 180.511 | 35.857 | 185.741 | 192.214 |
| RPC timeouts/errors/crashes | 0 | 0 | 0 | 0 | 0 | 0 |
| Action p99 / max ms | 34.730 / 35.048 | 42.886 / 43.075 | 42.337 / 42.682 | 34.463 / 35.293 | 42.646 / 42.760 | 42.803 / 43.960 |

After load accepted states are exactly **41,573 in all three cases**. The
approximately 11.47% throughput loss remains wall-clock service delay, not
scheduler rejection. Local recipient/RPC tails are not improved; do not infer
a broad health correction from the reload spike reduction. Control passes;
Local/Node streaming health remains **FAIL**. The Node Go integration exits 1
because its child converges but fails the unchanged gameplay gate, not because
content acquisition fails. This is not a green Node CI claim.

After combined fixup CPU p50/p95/p99/max (ms) is **0/0/0/0** control,
**0/0/2.760/2.907** Local and **0/0/2.684/2.820** Node. Cumulative Local/Node
is 33.674/33.473 ms. Property examinations p50/p95/p99/max are
**0/0/3,200/3,200** in both streaming cases (40,000 total), zero control.
No load fixups are emitted. General discovery maximum is **68,270**;
dependency edges maximum **53,861** in each streaming case. Old healthy traces
do not split all three fixup passes, so a comparable before combined-fixup CPU
or three-pass property total is **not measured**, not zero.

### Representation, invalidation and tradeoff

`CatalogEntry` builds reference-property membership from the immutable publication
and frozen canonical native metadata. Scalar payload replacement rebuilds borrowed
pointers with the new map but does not invalidate structural topology. Nil is
excluded from the target index and still handled by accepted-edge removal logic.
The new index test covers scalar replacement, reference replacement/nil, strict
preflight, retirement and index storage returning to one empty-world vector.

At 500-peer reload, index logical bytes are **30,872**: 24 bytes per catalog
entry plus capacity × 8-byte node pointers. The hard property bound is 1,024.
Accepted ancestry/reference logical bytes increase from **21,633,320** to
**35,212,704** for the same 540,833 records: **13,579,384 bytes** extra necessary
correctness state. Each accepted reference pair is 16 bytes, vector header 24;
capacity is explicitly capped at 1,024. Records exist only for Known identities
and release on accepted removal/disconnect. During successful bounded 3J
reconciliation, Known is contained in Desired union pending Leaves: the
conservative per-peer record bound is 131,072, not the 65,536 Desired cap alone.
The catalog index has no independent identity population or history; its
entry count follows the existing catalog lifetime/admission/retirement rules.
Complete aggregate overload accounting remains open. There are no raw live object
pointers, no per-peer catalog index and no history proportional to reload count.
These are logical storage figures, not allocator/RSS leak measurements.

INFERRED from before/after: accepted-edge grouping eliminates the irrelevant
clearing scan; canonical reference indexing eliminates scalar/nil field scans.
MEASURED: both reduce their targeted phase time and the reload maximum while
preserving convergence ticks. Neither establishes a general per-tick planning
bound. Busy-time shares are not causal throughput-loss percentages.

### Validation and remaining gates

Final indexed validation, independently executed after performance:

| Gate | Exact result |
| --- | --- |
| MSVC Release targeted suite | **6/6 PASS**, 17.42 s |
| Clang 19 ASan + UBSan + LSan | **7/7 PASS**, 75.95 s; `detect_leaks=1`, halt-on-error |
| Separate sanitizer late-handoff regression | **PASS**, 13 GCHR / 2 reliable Event sends after production in the same step |
| Current-linked physics matrix | **90/90 PASS**, exit 0 |
| 500-peer lifecycle/structural convergence | **PASS**; load/evict/reload gameplay health **FAIL** |
| Healthy control | **PASS**, process exit 0 |
| Healthy Local / Node streaming | Converged; gameplay health **FAIL**, child/integration exit 1 |
| Documentation | **19 pages built**, exit 0; existing missing-404-entry warning remains |
| Local/worker source verification | **46/46 native SHA256 matches**, checked again after all production edits |
| Whitespace validation | `git -c core.whitespace=cr-at-eol diff --check` **PASS** |

MSVC uses the retained VS2022 / 14.44.35207 Release worker build. Linux uses the
retained Clang 19 ASan/UBSan RelWithDebInfo cache and leak detection in
`codex-gargantuan-3l-toolchain:clang19-cmake331`, limited to four CPUs / 12 GiB;
CTest uses two jobs. Physics executable SHA256 is
`D5A95F4F98E5BA18C43625D3602900CE7862CCA1323B56ABD1D0A50A6EF593B2`.
Raw final test/sanitizer/physics logs are retained with the `ki007-index-v1`
prefix. Only the task-owned container ran; unrelated worker services were not
modified. The correctness-only gate above is independently green. These are
targeted suites, not full supported Windows/Linux CTest or CI closure.

Remaining owners, ordered by the measured reload spike: whole-peer fixup planning
(170.473 ms combined), Remote graph materialization (18.200 ms same tick), then
dependency/discovery bursts on other ticks (16.489/13.613 ms maxima). General
planning remains the next server task; this slice installs no general frontier.
Client whole-world snapshot/preflight cost is unchanged, and official reliable
transport's historical ~3.8-second Remote delay is **not rerun or fixed** by
loopback results. Overload, startup journal margin, current-source security,
full platform suites and CI remain open. The sealed old security artifact is
untouched. Recipient p95/p99, official transport queue ages and aggregate memory
soak/leak closure are **unmeasured in this slice**.

Proposed logical commit boundary, after review and without committing now:
accepted-reference bookkeeping/removal group correctness, targeted lifecycle
tests and bounded diagnostics; then the measured immutable reference-property
index, footprint metrics and index tests. Include matching architecture/ledger
updates. Do not sweep inherited Foundation changes into these commits merely
because the tree is dirty. No commit, push or 3M work occurred.

## Previous slice: precise fixup attribution / correctness stop (2026-09-11)

**B — FOUNDATION 3L PARTIALLY READY.** No optimization is retained. The new
required lifecycle case fails with the candidate index and with the original
full-property loops. The candidate was removed rather than declaring its
semantics validated or bypassing client preflight. This is a stopped slice,
not completion of the requested planning-work bound.

### Source and retained artifacts

Engine HEAD remains `a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1`, local and
uncommitted, with the inherited 57 tracked dirty paths preserved. No secondary
repository changes, commit, push, reset, or 3M work. Native synchronization uses
only scoped files on `dockerbox` under `C:\Sandbox\Codex`; local source remains
canonical. Four build jobs and two CTest jobs reuse the existing MSVC caches.
Performance does not overlap compilation or tests.

Artifacts under local `build-3l3-worker/evidence/`:

- `fixup-attribution-v1-source.json` and `*-engine.patch`: diagnostic-only
  baseline, 46 native source hashes. Benchmark SHA256
  `6EF631C1CEB3F3D9BB519D081E08638283D50E0D33984E52B323AF36E20906BE`.
- `fixup-attribution-v1-local-500-50.out.log`: unchanged four-phase fixture;
  `*-local-500-planning.json` retains phase quantiles, allocation counts,
  journal gauges and the complete maximum-planning tick. The earlier
  `*-discovery.json` is retained too, not overwritten.
- `fixup-index-v1/v2/v3-source.json` and patches: withdrawn index trial. v1
  failed compilation because the new memory metric was not forwarded through
  GameSessionMetrics; v2 built after that forwarding correction. v3 adds precise
  error reporting. These are not accepted performance checkpoints.
- `fixup-fullscan-reference-v1-source.json` and build/test logs: the same test
  with the original full-property loops restored; the same semantic rejection
  occurs. Its extra subset-count assertion is index-specific and expected to
  fail on a full scan; that assertion is not the correctness evidence.
- `fixup-stop-v2-source.json` / patch and build/test logs: index and its metric
  fully removed, minimal `TestSoftReferenceReplacementBeforeLeave` retained.
  Coordinator and timing-header hashes match the diagnostic-only v1 baseline;
  GameSession source/header, coordinator header and benchmark source match the
  preceding `late-send-final` checkpoint byte-for-byte.

The disposable combined benchmark executable was last built for the withdrawn
index trial; do not reuse it as a final-source performance binary without
rebuilding. The final-source build here targets the relevance regression test.
Source-hash equality does not make every cached worker executable current.

### Unchanged primary workload and reproduction

500 peers, 50 active Characters, five-neighbor placement, 512 objects / 273,032
package bytes, ten looping root tracks, staggered five-tick owner input,
40-callback action schedule, reliable Event each callback and 100 sequential
RPCs per phase. All four phases remain 301 ticks; no timing or health gate was
changed. Provider is Local. This is gameplay loopback, not official GNS.

| Reload metric | Retained previous slice | New diagnostic-only reproduction | After correction |
| --- | ---: | ---: | --- |
| Server tick p50, ms | 13.216 | 13.341 | not measured |
| Server tick p95, ms | 85.582 | 85.448 | not measured |
| Server tick p99, ms | 95.368 | 96.433 | not measured |
| Server tick max, ms | 478.171 | 488.404 | not measured |
| Discovery examinations max | 138752 | 138752 | not measured |
| First fixup-pass property examinations max | 2840760 | 2840760 | not measured |
| All three fixup-pass examinations max | not measured | 8517714 | not measured |
| Closure dependency edges max | not recaptured here | 72766 | not measured |
| Three fixup scopes, max summed ms | not separately measured | 433.633 | not measured |
| Character/root recipient max gap, ms | 1508.28 | 1524.95 | not measured |
| Reload convergence max, ms / ticks | 3551.77 / 33 | 3576.08 / 33 | not measured |
| Pending high-water | 194205 | 194205 | not measured |
| Initial acquisitions / admissions | 1 / 1 | 1 / 1 | not measured |
| Admissions including reload | 2 | 2 | not measured |
| Global selected-operation maximum | 8192 | 8192 | not measured |
| Journal failures | 0 | 0 | not measured |
| Post-service reload retention margin | 10249 | 10249 | not measured |
| Session-cumulative startup margin | 26 | 26 | not measured |
| New frontier/continuation state | none | none | none retained |

All peers converge in this reproduction. Load: 2,702.20 ms / 33 ticks,
tick p99/max 85.577/110.858 ms, Character/root gap 767.615 ms. Eviction:
1,464.84 ms / 32 ticks, tick p99/max 73.144/81.328 ms, recipient gap
641.457 ms. Load/eviction post-service retention margins remain 10,249/15,340.
The 26-entry margin is still startup-cumulative; oldest-owner attribution is
**not measured**. No journal-capacity increase or transport-ACK inference.

### Exact worst tick and amplification

Tick 1116 has 498 planning calls; distinct peer identities are **not measured**
by this aggregate counter. Fixup cost planning visits 538,670 Desired referrers.
Of 2,840,760 properties, 2,566,860 are scalars (90.36%), 49,800 reference values
and 224,100 typed hard nils. The clearing pass visits 534,344 referrers /
2,836,194 properties; restoration visits 538,670 / 2,840,760.
All passes together examine 8,517,714 properties for 494 emitted fixups, or
17,242.34 examinations per emitted fixup. This is an amplification ratio, not
a claim that every non-emitting visit is unnecessary.

3,526 dependency groups are built on this tick, with 6,064 group-node visits,
zero group-budget deferrals and zero encode retries. The enclosing structural
preparation scope is 442.308 ms, decomposed into the disjoint fixup scopes
below, 2.339 ms group selection, 2.474 ms other nested work (encoding/catalog
refresh, not separately split here) and 3.862 ms exclusive remainder. Overall structural encoding, including the later submission path,
is separately 4.233 ms. Remote materialization reconciliation is 19.412 ms;
Character graph sync is 0.954 ms. Closure/discovery is only 0.699/0.292 ms
on this tick. Its 4,326 discovery examinations differ from the peak-discovery
tick, so discovery counters alone do not attribute this tail.

| Reload phase CPU, ms | p50 | p95 | p99 | max | cumulative |
| --- | ---: | ---: | ---: | ---: | ---: |
| Fixup cost planning | 0 | 4.032 | 4.280 | 187.899 | 315.162 |
| Reference clearing | 0 | 0 | 0 | 190.840 | 193.065 |
| Reference restoration | 0 | 1.351 | 1.402 | 54.894 | 97.604 |
| Group selection | 0 | 5.356 | 5.610 | 5.822 | 169.264 |
| Structural encode | 0 | 11.665 | 12.094 | 12.267 | 368.885 |
| Dependency closure | 0 | 12.346 | 15.266 | 16.107 | 236.598 |
| Pending discovery | 0 | 7.233 | 12.464 | 13.022 | 177.138 |
| Relevance, inclusive | 10.214 | 16.501 | 23.901 | 25.201 | 3247.774 |

Nested scopes are not summed with their enclosing scopes. Allocation capture
records 75,198 allocation calls in cost planning and zero in the two later
fixup passes on tick 1116. These are allocator-call counts, not retained entries
or exact per-map bytes. No causal throughput percentage follows from busy-time
shares. The 12-tick recipient window 1105 -> 1117 spans 1,524.95 ms and contains
the spike; scheduler rejections remain zero. Full per-state generation/send/
receive/apply attribution and recipient p95/p99 remain **not measured**.

Unique property/referrer identities, unique groups versus reconstructed groups,
exact fixup candidate rejection reasons and fixup-specific scheduler acceptance
counts are **not measured**. Aggregates retain no per-identity history. The
phase's total selected/committed counts both equal 263,541, with 499 emitted
fixups, no encode retries and 999 dependency rebuilds (500 overlapping topology/
selection misses plus 499 selection-only misses). These facts do not turn all
2.84M visits into distinct semantic work. Ordinary scalar invalidation remains
excluded by the existing dependency cursor; the source still scans all scalar
properties during each of these peer-specific fixup passes.

### Correctness failure and stop rationale

The original test first restored a soft target through ordinary Enter fixup,
then destroyed/replaced it while the new target remained unknown. Both indexed
and original full-scan builds reject the old-target removal. The reduced test
establishes the accepted edge in a baseline and compares service order directly:

| Existing service order | Observed result |
| --- | --- |
| Structural removal first | One operation, zero RootPart nil fixups; client rejects with `Snapshot reference is outside the receiving scope` |
| Journal first | One RootPart nil operation applies; subsequent removal also applies and commits successfully |

This is KI-007, owned by server reference-fixup completeness. The current
publication already points to B, while the client still observes A. Testing
current B against Leaving cannot prove that no accepted edge points to A.
The client rejection is correct; this is not a stale generation resolving to
the new identity, nor a transport delay. Changing client validation is outside
scope and would hide the actual defect.

The next correction needs an explicit bounded accepted-edge or conservative
clearing proof, including its operation/group costs. This is not implemented
by quietly draining all journals first or adding a second acceptance authority.
The index candidate has no measured performance benefit yet and is removed.
Its trial patches remain available, but are not a proposed ready-to-commit fix.

### Bounds, healthy regression and validation disposition

No planning frontier, persistent candidate queue, or reference index remains.
Only diagnostics add storage: two fixed 80-byte phase slots and eleven fixed
eight-byte saturating counters, 248 bytes per WorkSample (6,064 bytes total on
the recorded layout). The test-only history remains at most five 1,201-tick
buffers with server/client samples, roughly 73 MB including tick fields.
Production does not retain this history or write per-peer/object logs. The
new per-visit counter overhead is **not independently measured**; compare the
488-ms instrumented reproduction with the less-instrumented historical 478-ms
case without treating their difference as a performance regression.

Complete planning boundedness is **UNPROVED**. Existing per-object property
cardinality (1,024), peer Desired cardinality (65,536) and 3J selected-operation
budgets do not bound total inspection to a small per-tick work quantum. No
partial dependency group, new fairness policy, queue, public priority, wire
change or semantic-authority change is introduced. Accepted late-send behavior
is restored byte-for-byte; previous invariants/evidence are not erased.

New Control/Local/Node 200-peer after-performance: **not measured**, stopped at
the failing lifecycle gate before retaining any correction. The preceding
same-input reference remains Control/Local/Node p99 4.172/40.422/41.891 ms,
max 9.531/99.666/99.546 ms; Character throughput 8,277.64/7,315.23/7,316.35
states per wall-second; recipient Character/root maxima 202.076/398.410/403.002
ms; RPC p99/max 35.763/36.468, 147.320/152.471, 124.946/180.511 ms;
Event ACK maxima 19.086/89.566/91.080 ms. These are previous results, not newly
validated after values. Recipient percentiles remain **not measured**.

MSVC candidate v2: benchmark and six target binaries build; CTest **5/6 PASS**,
relevance test FAIL due the new lifecycle case. Foundation, replication,
GameSession, scheduler and physics targets pass. The full-scan control also
fails that lifecycle case. The final diagnostic-only relevance binary is built
and its failing regression is retained; this source is intentionally **not
claimed test-green**. There is no new physics behavior or reason to discard
the previously green 90-case matrix.

Final `fixup-stop-v2` relevance executable SHA256:
`0AB4EB6CDED405B8839656EF4EA6A3C236DABA9BE9C51BEA62A7CBAB743AD9F0`.
It builds successfully, exits one for exactly the new structural-first
assertion, and confirms `journalFirstRemovalApplied=1` for the positive control.
All 46 native source hashes match local/worker after candidate removal.
Documentation builds **19 pages successfully**, exit zero, 1.86 seconds reported
Astro build time; the existing missing-404-entry warning remains nonfatal.
Whitespace diff check passes. These are not CI or deployment results.

Current-slice ASan/UBSan/LSan, final full CTest, new healthy differential,
official transport, client preflight redesign, overload, startup journal
ownership, current-source security and CI/deployment: **not executed/closed**
after the correctness stop. Previous sanitizer passes remain historical evidence,
not proof for this new test. The sealed security artifact is untouched.

Proposed checkpoint boundary only: bounded fixup diagnostics, the failing
soft-reference ordering regression, analysis helper and these current-doc/issue
updates. Do not include the removed index or unrelated inherited Foundation
changes. No commit is made; implementation should resume with KI-007 before
the next measured index/whole-peer-planning optimization. Client preflight
(~89–101 ms at 8,193 replicas) and official reliable Remote service (~3.8 s
historically) remain separate blockers. **Do not begin 3M.**

## Previous slice: reviewed owners / late-send allowance (2026-09-11)

**B — FOUNDATION 3L PARTIALLY READY.** One local handoff defect is corrected;
general pre-3J bounds and end-to-end non-starvation are not closed. In particular,
the new 500-peer reload records a 1.508-second recipient gap and a 478.171 ms
server tick. Those failures are retained, not averaged away.

### Source and methodology

Published Engine HEAD remains `a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1`.
Starting tracked dirty inventory: 55 paths, plus preserved new Foundation files.
Only primary Engine source/docs/tests/helpers changed. No commit, push or 3M.
The worker is disposable; no worker source was copied back over local source.
Node source was not changed or synchronized; its retained fixture/provenance
was used only for the TLS content-provider integration.

Local `build-3l3-worker/evidence/` preserves:

- `reviewed-owners-v5-source.json` / Engine patch and raw before logs: completed
  local-review instrumentation; native benchmark SHA256
  `271F00AC7310025A0A580E05F5143B47762D5B3C876B575672839032BF392C2D`.
- `late-send-v1-source.json` / Engine patch and raw after logs: only production
  behavior delta from that baseline is `GameSession`'s private send allowance.
  Native benchmark SHA256
  `9FE2DFC69ED1062DFE8ABC398BDB661F20D12CA3189A36B782BF565BFD735D50`.
- `late-send-v2-source.json`: all 46 native paths matched local/worker hashes
  before final validation; production code is identical to v1. Only the client
  diagnostic benchmark adds one removal trial at each population.
- Both sets of `*-discovery.json`, `*-attribution*.json` and `*-summary.json`
  preserve quantiles/counts, raw-log hashes and the worst load-gap timeline.
  The `reviewed-owners-v5-attribution-categorized.json` revision assigns the new
  Relevant-rebuild scope to pre-3J rather than the explicit other-scoped bucket;
  the earlier report is retained, not overwritten. Nested durations are inclusive;
  only exclusive scopes are summed for busy-time attribution. Busy-time shares
  are not counterfactual percentages of throughput loss.

The input workload/fixture code is unchanged: 200 peers, 50 active Characters,
five-neighbor layout, 512-object / 273,032-byte package, staggered five-tick owner
input, ten looping root-motion tracks, owner action every 40 client simulation
callbacks, reliable Event each callback and 100 sequential RPCs per phase. The
500-peer case changes only peer count. Node uses real TLS for content acquisition;
gameplay still uses the deterministic loopback transport, not official GNS.

**Important duration confound:** the existing phase terminates after convergence,
at least 240 ticks and completion of all 100 sequential RPCs. Corrected handoff
reduces that run from **401 to 301 ticks**. This is a measured outcome of the same
fixture, not a changed duration setting. Phase boundaries, action counts and
spatial/relevance evolution consequently shift. Cumulative CPU/counts and tail
percentiles are not equal-duration counterfactuals. No claim of general server
CPU improvement is made. The separate recording-transport test is the decisive
causal evidence for retaining the late-send correction.

The added private header triggered CMake glob regeneration and a worker vcpkg
dependency ABI-cache miss. Existing versions were rebuilt; caches were not
deleted. The indirect vendor build initially used its own default parallelism;
worker helpers now set `VCPKG_MAX_CONCURRENCY=4` and
`CMAKE_BUILD_PARALLEL_LEVEL=4` as well as native `--parallel 4`. There were no
concurrent performance runs/builds. This rebuild is another reason not to claim
small aggregate CPU differences as causal wins. The dedicated Engine build and
Linux container keep their existing caches. No unrelated container was modified.
The pre-existing `tsediscord-bot` name was present before sanitizer execution but
absent from the final all-container listing; its disappearance was not investigated
and no repair/restart was attempted. The other listed workloads remained running.

### Nine-review disposition and recurring CPU attribution

Before any production behavior change, source verification classified findings
1/8 **PARTIALLY PRESENT**, 2/3 **ALREADY CHANGED LOCALLY**, and 4/5/6/7/9
**PRESENT**. See the architecture checkpoint for the exact function paths and
residual variants; finding 7 is the only production behavior corrected here.

| Before load, 401 ticks | Control | Local | Node | Local 500 peers |
| --- | ---: | ---: | ---: | ---: |
| Dependency-plan hits | 13,400 | 13,320 | 13,264 | 25,164 |
| Selection misses | 0 | 200 | 200 | 500 |
| Topology misses (overlap selection misses) | 0 | 200 | 200 | 500 |
| Payload-only catalog batches | 1 | 1 | 1 | 1 |
| Topology catalog batches | 0 | 2 | 2 | 2 |
| Relevant identities rebuilt | 0 | 157,715 | 157,715 | 542,330 |
| Lazy peer-view identities copied | 0 | 0 | 0 | 0 |
| First fixup-pass referrers | 0 | 157,715 | 157,715 | 542,330 |
| First fixup-pass properties | 0 | 117,515 | 117,515 | 580,610 |
| Byte-limit encode retries | 0 | 0 | 0 | 0 |
| Desired/Known/Pending examinations, max | 0 | 68,258 | 68,258 | 106,176 |
| Examinations / new candidate, aggregate | n/a | 2.0804 | 2.0804 | 3.2370 |
| Examinations / selected or accepted work | 0 | 2.0320 | 2.0320 | 3.1683 |

Selected/accepted denominators include journal operations, not only newly
discovered candidates. No duplicate insertion interpretation is inferred from
the examination ratio. The first fixup-pass counters do not count every property
in the second post-selection pass; its CPU is still captured by the existing
scopes. Exact allocation/byte costs per fixup and retry CPU with nonzero retries
are **not measured** in these cases.

| Local server phase, ms | Before total / p99 / max (401 ticks) | After total / p99 / max (301 ticks) |
| --- | ---: | ---: |
| Relevance | 1959.50 / 10.21 / 15.46 | 1454.11 / 10.50 / 15.61 |
| Spatial query (nested) | 822.03 / 4.17 / 4.37 | 598.73 / 4.13 / 4.47 |
| Dependency closure | 31.15 / 0 / 10.14 | 31.38 / 1.51 / 10.04 |
| Pending discovery | 25.96 / 0 / 8.96 | 26.16 / 0.93 / 8.83 |
| Full Relevant rebuild | 6.42 / 0 / 2.21 | 6.12 / 0.24 / 2.24 |
| Fixup planning | 26.15 / 2.11 / 2.27 | 26.43 / 2.18 / 2.24 |
| Structural encoding | 114.48 / 9.07 / 9.67 | 115.34 / 9.39 / 9.84 |
| Remote materialization reconciliation | 26.82 / 1.91 / 2.95 | 27.24 / 2.01 / 2.93 |
| Character graph synchronization | 287.12 / 1.07 / 1.30 | 199.50 / 1.00 / 1.15 |

The recurring relevance mean is 4.8865 -> 4.8309 ms/tick: effectively unchanged,
not a claimed relevance optimization. The graph performs 640 typed Character
visits/tick in both Local runs (256,640 -> 192,640 total), with no graph changes
in load. Earlier broad descendant/Known scanning remains corrected, but typed
per-peer reconciliation still polls/allocates temporary sets. No new cache is
introduced without invalidation proof. Scalar payload publication does not
repeatedly rebuild dependency closure here; Parent, class, hard/nonnullable
reference topology, create/destroy/resnapshot and peer selection remain the
existing structural invalidators. Soft references still receive fixup processing.

### Causal late-handoff result and bounded contract

The before recording transport observes 13 late GCHR occasions and two late
reliable Events: zero of those newly produced messages reach Send after their
production in that step. At tick 81 an old GCHR message *does* send early; counting
that as the newly accepted state's Send would have hidden the bug.

After: all 13 sampled late GCHR occasions send in their production step, at
0.0024--0.0385 ms scheduler queue age, and both reliable Events send at maximum
0.005 ms. Samples cover a real structural mutation before Character/Remote
production. These are actual successful recording-transport `Send` calls, not
just scheduler acceptance. The trace is bounded to this test; zero before age
on an unsent message means **no Send sample**, not zero latency.

The canonical load's bounded producer annotations independently show scheduler
acceptance-to-successful-Send maximum ages (ms):

| Producer | Before C / L / N | After C / L / N |
| --- | ---: | ---: |
| GCHR | 18.533 / 90.684 / 78.409 | 0.478 / 0.530 / 0.564 |
| Reliable Event | 18.359 / 70.246 / 77.948 | 0.022 / 0.018 / 0.025 |
| RPC response | 15.764 / 38.708 / 77.943 | 0.008 / 0.011 / 0.006 |

All three producer classes have zero measured scheduler budget/capacity deferrals
and per-peer producer queue depth maximum one in these load runs. Phase-boundary
queued messages can make the before serviced count exceed that phase's enqueued
count by one; this is not duplicate Send. These queue-age maxima do not measure
backend packet transmission, ACK, recipient dispatch or Luau completion.

`SessionSendAllowance` shares successful bytes/messages across early, structural
and final service. Default peer limit remains 2 MiB / 1,024 messages per Step;
minimum residual quantum remains one largest legal message (default 512 KiB).
No fresh full budget is granted by a second/third flush. A non-drained result
stops further attempts until Reset next step; blocked structural work cannot be
bypassed by newly produced gameplay. Small residual capacity can go unused.
The private state is 16 bytes on the tested 64-bit ABI (replacing a boolean;
total containing-peer layout delta is not separately measured), no heap/queue,
no accumulated credits and no retained raw identities. Existing global queue
ceilings, reliable ordering, GCHR sequencing and 3J budgets remain unchanged.

Tight-budget tests cover shared byte/message exhaustion, structural-first Send,
no duplicate Send, transport WouldBlock, no same-step retry/overtake, next-step
recovery and cancelled connection generation. The local guarantee is conditional
on earlier service draining and residual capacity. Official backend delivery,
ACK and client completion are outside this assertion.

### Healthy differential: unchanged fixture, duration shown explicitly

Times below are milliseconds. Recipient gaps remain **max-only**; p95/p99 are
**not measured**. RPC/Event/action quantiles are their own sampled latency
distributions, not recipient-gap percentiles.

| Load metric | Before Control | Before Local | Before Node | After Control | After Local | After Node |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Phase ticks | 401 | 401 | 401 | 301 | 301 | 301 |
| Tick p50 | 2.73 | 10.71 | 10.48 | 2.76 | 10.52 | 10.63 |
| Tick p95 | 3.76 | 13.55 | 13.35 | 3.66 | 14.44 | 14.44 |
| Tick p99 | 4.14 | 39.31 | 40.52 | 4.17 | 40.42 | 41.89 |
| Tick max | 8.80 | 95.17 | 95.32 | 9.53 | 99.67 | 99.55 |
| Accepted Character states/s (wall) | 8211.09 | 7489.75 | 7471.59 | 8277.64 | 7315.23 | 7316.35 |
| Throughput loss vs matching control | baseline | 8.78% | 9.01% | baseline | 11.63% | 11.61% |
| Accepted Character states | 54918 | 54918 | 54918 | 41579 | 41578 | 41579 |
| Character/root recipient max gap | 201.48 | 650.29 | 633.19 | 202.08 | 398.41 | 403.00 |
| Reliable Event ACK max gap | 19.55 | 63.26 | 94.60 | 19.09 | 89.57 | 91.08 |
| RPC p50 | 48.79 | 41.99 | 58.44 | 33.04 | 41.59 | 32.65 |
| RPC p95 | 51.57 | 60.22 | 60.00 | 35.53 | 44.32 | 34.87 |
| RPC p99 | 52.24 | 175.66 | 213.40 | 35.76 | 147.32 | 124.95 |
| RPC max | 52.85 | 242.70 | 279.59 | 36.47 | 152.47 | 180.51 |
| RPC timeout/error/crash | 0/0/0 | 0/0/0 | 0/0/0 | 0/0/0 | 0/0/0 | 0/0/0 |
| Action p99 / max | 52.22 / 53.59 | 59.57 / 59.94 | 60.14 / 145.97 | 34.73 / 35.05 | 42.89 / 43.08 | 42.34 / 42.68 |
| General examinations max | 0 | 68258 | 68258 | 0 | 68256 | 68256 |

The shorter phase retains roughly the same admission burst: Local excess wall
duration versus control is 644.14 -> 660.71 ms, now amortized over fewer ticks.
The higher throughput-loss percentage is reported, not hidden. Small timing
differences do not establish causality. All after streaming health gates remain
failed; only controls pass. No recipient distribution was added in this slice.

Before Local's worst gap is tick 548 -> 560, receipt timestamps 9002.64 ->
9652.93 ms: 12 simulation ticks, 650.29 ms wall-clock. Adjacent serialized fixture
iterations cost approximately 39--123 ms. CharacterPublication executes during
those ticks, with zero scheduler rejections. This is consistent with tick-based
low-rate cadence dilated by expensive consecutive iterations, plus the separately
proven late-handoff defect; it is not a 650 ms scheduler admission rejection.
Per-recipient due/defer/receive/apply timestamps are not all retained, so their
exact additive split is **not measured**. After Local's worst is a different
six-tick window (451 -> 457); do not subtract the two gaps as a pure queue-delay
counterfactual. Full before/after load windows are in the summary artifacts.

### 500-peer results and regression accounting

| Local 500 metric | Before | After |
| --- | ---: | ---: |
| Load convergence max, ms / ticks | 2663.22 / 33 | 2723.48 / 33 |
| Eviction convergence max, ms / ticks | 1469.99 / 32 | 1470.83 / 32 |
| Reload convergence max, ms / ticks | 2729.80 / 33 | 3551.77 / 33 |
| Load Character/root max gap, ms | 800.47 | 771.62 |
| Eviction Character/root max gap, ms | 447.84 | 643.19 |
| Reload Character max gap, ms | 681.75 | 1508.28 |
| Load examinations max | 106176 | 106176 |
| Reload examinations max | 105792 | 138752 |
| Load Pending high-water | 190464 | 190464 |
| Post-service journal margin, load / eviction | 10249 / 15340 | 10249 / 15340 |
| Journal failures | 0 | 0 |
| Acquisition / initial admission | 1 / 1 | 1 / 1 |
| Admission after reload | 2 | 2 |
| Global selection cap | 8192 | 8192 |

Every peer converges. Exact cap saturation remains exercised on 31 load ticks;
Known changes only through accepted work. No frontier is added. The historical
session-cumulative startup lag remains 16,358 of 16,384 records (26-entry margin).
Load/eviction gauges above are phase-local, not a closure of startup retention.
The oldest startup peer/requirement and a defensible whole-session margin remain
**unmeasured/unproved**; no journal capacity was increased or ACK ownership inferred.

The after reload adds 499 **selection-only** dependency-plan misses in addition
to 500 topology/selection misses, and 499 Character graph membership changes.
At tick 1109 discovery examines 69,280 Desired + 69,472 Known = 138,752 identities.
At tick 1116 the 478.171 ms server tick contains 240.773 ms exclusive
StructuralSelection and 185.967 ms exclusive StructuralFixupPlanning, with
19.279 ms Remote materialization reconciliation. The 12-tick recipient window
1105 -> 1117 spans 1508.28 ms. This exposes the unbounded whole-peer planning /
fixup owner under a different phase/cadence alignment; exact attribution of the
extra selection changes to that alignment is **inferred, not isolated**. The
regression is real in this run. It is not called transport starvation, and the
local handoff fix is not called an overall non-starvation improvement.
`StructuralSelection` is the enclosing `ProduceRelevanceFrame` scope, not a
measurement of the bounded selector alone: its exclusive remainder includes the
second post-selection Desired/property reference pass, ancestry ordering and
frame preparation. The first full Desired/property pass has its own FixupPlanning
scope. Splitting that second pass from the remaining frame preparation is still
needed before attributing all 240.773 ms to a single inner loop.
In that tick the first fixup pass alone visits **538,670 referrers and 2,840,760
properties**; Remote reconciliation visits 540,164 tracked identities. Only
4,326 Desired/Known discovery examinations occur there. Thus the largest
discovery-counter tick and largest CPU tick are different: reducing the
138,752 discovery counter alone would not close the measured fixup burst.

### Client constant-increment scaling and stop gate

Ten one-Name-property updates per world; an additional final-source trial removes
one leaf. Counts are identities, not guessed bytes or allocator-hook events.

| Existing identities | Before update p50 / max, ms | Final diagnostic update p50 / max, ms | Final preflight p50, ms | One removal, ms / mapping visits |
| --- | ---: | ---: | ---: | ---: |
| 65 | 0.204 / 0.274 | 0.19 / 0.26 | 0.15 | 0.19 / 65 |
| 513 | 1.98 / 2.15 | 2.11 / 2.52 | 1.66 | 2.05 / 513 |
| 2049 | 11.30 / 13.63 | 12.39 / 13.34 | 9.70 | 11.92 / 2049 |
| 8193 | 89.70 / 94.65 | 97.53 / 100.97 | 80.41 | 106.25 / 8193 |

Every changed-property trial copies/indexes/preflights and rebuilds receiver lookup
across N identities for one operation. A removal copies/indexes/scans N, with
N-1 preflight identities. At N=8193 final median copy/semantic/live times are
1.58/1.02/1.29 ms versus 80.41 ms preflight. There was no client behavior change;
variation between independent runs is not a regression caused by the send helper,
which this direct-applier probe never executes. Copied snapshot bytes, exact
temporary allocation counts and isolated destruction/cleanup CPU are **not
measured**. No graphical Player frame claim is made from this headless probe.

The existing rejection/rollback/reference/native setter/deferred-observer
contracts prevent safely deleting whole-world preflight as a small optimization.
Implementation stops at the requested larger-transaction-design gate, with the
proposed changed/dependent-state validation seam in the architecture checkpoint.
Client production changes here are bounded counters only, not an apply queue or
weakened validation. Per-removal complexity documentation is corrected.

### Diagnostics, validation and remaining gates

New diagnostics add two fixed phase slots and 17 saturating counters. A WorkSample
contains 55 80-byte phase records, thirteen 80-byte producer records and 47
eight-byte counters: 5,816 bytes. Test capture is at most five buffers of 1,201
ticks with server/client samples (about 70.1 MB including tick fields). Production
does not retain these test histories or print per-tick logs; disabled counters
test the thread-local capture pointer. Incremental capture overhead versus fully
disabled diagnostics is **not separately measured**; both compared builds carry
the same instrumentation. Existing memory/physics ownership is unchanged.

MSVC Release: **6/6 targeted CTest PASS**, plus dedicated late-handoff probe PASS
and all 44 final client scaling/removal trials PASS. Tests include foundation,
replication relevance/dependency/bootstrap, replication/applier failure paths,
GameSession lifecycle, scheduler contract and physics backend. Control/Local/
Node and 500-peer fixture structural assertions PASS; streaming health assertions
FAIL as reported above. Node's wrapper exits nonzero because gameplay health
fails, not because TLS acquisition failed. It is not reported as green Node CI.
Final **Clang 19 ASan/UBSan/LSan: 7/7 targeted CTest PASS** in 83.46 seconds,
plus the dedicated late-handoff composition probe PASS, process exit zero.
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`; no sanitizer report.
The additional content-availability target covers lifecycle ownership. Linux
handoff maximum queue ages are 0.519 ms GCHR and 0.022 ms reliable Event;
sanitized values are not mixed with native performance samples. The disposable
four-CPU/twelve-GiB container exited and removed itself. Source remains identical
to the 46-path `late-send-v2` manifest.

Documentation: **19 pages built, exit zero**, bundled Node 24.19.0 / installed
Astro 7.2.10, 3.82 seconds reported build time. The existing nonfatal missing-404
entry warning remains. Relative Markdown links in all five changed current-doc /
issue files pass local existence checking; whitespace diff check passes. This is
not a Pages deployment or CI result. Historical checkpoints are preserved; the
new dated section does not claim every old result was rerun on this source.

Unexecuted/unclosed final gates: official GNS/Player service (historical Remote
gap ~3.8 s), full client/overload envelope, startup journal-margin ownership,
current-source security review and CI/deployment. The prior sealed security
artifact's obsolete pending-review state is untouched and not green. Earlier
physics 90-case evidence is preserved; no physics behavior changed, so the
expensive matrix/soaks are not repeated solely for completeness.

Proposed commit boundary, **not committed**: private shared step allowance and
GameSession service sites; scheduler/tight-budget and real-lifecycle handoff tests;
bounded reviewed-owner diagnostics and reproducible client-scaling fixture; these
architecture/validation corrections. Split diagnostic evidence from the behavior
change if useful for review. Do not sweep prior uncommitted physics/Character/
retirement/lifetime work or local build artifacts into this slice indiscriminately.
Next server investigation: dependency-complete selection/fixup continuation under
the newly retained 500-peer reload trace. Separate client transaction design and
official reliable-transport service remain explicit handoffs. Do not begin 3M.

## Previous slice: retired-catalog work isolation (2026-09-10)

**Foundation 3L Partially Ready. General bounded pre-3J planning is still
unclosed.** This checkpoint implements one measured sub-owner correction, as
per the instruction to follow retired cleanup if it dominates, rather than
claiming that a retirement bound also bounds complete Desired/Known/fixup work.
No general discovery frontier, new acceptance authority, wire change, content
limit change, 3M work, commit or push is included.

### Source and evidence

Engine HEAD remains `a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1`. The starting
tree contained 53 tracked modified paths plus retained new Foundation files.
All previous physics, Character, lifetime, journal and unrelated work is preserved.
Only the primary repository was modified. The unchanged Node fixture/provider
was used for the required real-TLS differential; no secondary source was synced
or rewritten. Its retained source provenance is still the prior checkpoint.

Evidence under local `build-3l3-worker/evidence/`:

- `planning-attribution-v1-source.json` and complete Engine patch: retirement
  diagnostic baseline; benchmark SHA256
  `D120FF2F75D5C896F942C7AD32AB72764287C26D853AF9521F58E39B8C7B1080`.
- `bounded-retirement-v1-source.json`: measured implementation; benchmark SHA256
  `A07FB0D96B038830AF37AB7EF270B72FDCFAE5136D95E0728FC2DAC8FBDD82EE`.
- `bounded-retirement-v2-source.json`: same production/benchmark source, with
  additional late-acceptance regression assertions only. All 44 Engine native
  file hashes matched the worker before final targeted validation. Manifests
  and patches are provenance, **not** a completed security inventory/review.
- Raw `*-local-500-50.out.log`, `*-none-200-50.out.log`,
  `*-local-200-50.out.log`, `*-node-200-50.log`, derived `*-discovery.json` and
  `bounded-retirement-v1-attribution.json` retain before/after counts and scopes.

Worker: verified `dockerbox` / `192.168.0.108` / `HostPC`, with source at
`C:\Sandbox\Codex\Workspaces\gargantuan-runtime-host-f1-1\engine` and existing
incremental native/Linux build directories. Four compile jobs, two CTest jobs;
benchmarks ran sequentially without concurrent builds. Local source remains
canonical. No worker source was copied back over it.

### Attribution: what was and was not bounded

The complete source path remains: 3E evaluated selection -> shared catalog ->
Desired and required dependency closures -> Pending cancellation -> Desired
Enter discovery -> sorted Known Leave discovery -> complete reverse-Leave
index -> full known-referrer fixup planning -> existing 3J dependency groups ->
encode -> reliable scheduler acceptance -> Known/accepted ancestry/journal
watermarks -> Remote/GCHR materialization consumers.

The old catalog refresh additionally performed retired identities x peers for
each refresh/batch. Separate timing establishes that loop as the largest
**eviction** sub-owner, not the largest healthy-load sub-owner.

| 500-peer eviction owner | Diagnostic before cumulative ms | Before p99/max ms | After cumulative ms | After p99/max ms |
| --- | ---: | ---: | ---: | ---: |
| Retirement | 2,045.601 | 85.512 / 184.170 | 8.569 | 0.301 / 0.604 |
| Catalog refresh, inclusive | 2,085.397 | 85.582 / 184.340 | 35.359 | 0.188 / 0.524 |
| Desired reconciliation, inclusive | 452.965 | 35.098 / 37.954 | 449.151 | 35.460 / 37.434 |
| Dependency closure | 108.047 | 7.402 / 8.111 | 108.536 | 7.417 / 8.112 |
| Candidate discovery | 146.007 | 13.936 / 15.020 | 144.539 | 13.679 / 14.113 |
| Reverse Leave index | 57.896 | 7.133 / 7.759 | 63.351 | 7.873 / 8.272 |
| Fixup planning | 222.040 | 11.567 / 12.228 | 238.272 | 13.194 / 13.356 |
| Pre-3J exclusive composite | 3,190.213 | 89.772 / 186.117 | 1,178.610 | 40.322 / 42.617 |

Retirement moved outside catalog refresh into bounded Main maintenance; the
exclusive composite includes it in both versions. Inclusive rows overlap and
must not be added. Old retirement visits: 1,945,801 objects and 143,447,770
peer-Known probes, maxima 295,936 and 12,883,034. New visits: 131,584 objects,
zero peer probes, maximum **4,096** objects; 512 templates released.

The new per-probe diagnostic baseline is slower than the retained prior run
(140.002-ms catalog max, 1,054.79-ms recipient gap). Instrumentation overhead
has not been independently isolated. Both comparisons below are retained;
the benefit is not dependent on selecting only the slower baseline.

Healthy load still has 68,258 maximum Desired/Known examinations at 200 peers,
106,176 at 500. The 200-peer burst remains 50,513 Desired + 17,745 Known,
47,859 dependency seeds, 53,861 edges, 32,768 unique candidates and zero
duplicate insertion attempts. Total examinations/new candidates remain 2.0804;
examinations/selected or accepted operations remain 2.0320. No counter reduction
is claimed for these scans. Per-property fixup charging and peer-discovery
service-age distributions remain **not measured**.

### Healthy differential: first after run versus preserved checkpoint

Same 200 peers / 50 Characters / five per neighborhood, staggered 12-Hz input,
ten root-motion tracks, unchanged action/Event/100-call RPC schedule, same seed,
512 objects and 273,032-byte content unit. Each load has 401 ticks and 54,918
accepted Character states. Recipient gaps are wall-clock **maxima only**.

| Load metric | Before control | Before Local | Before Node | After control | After Local | After Node |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Tick p50 ms | 2.803 | 10.527 | 10.473 | 2.844 | 10.448 | 10.523 |
| Tick p95 ms | 3.721 | 13.445 | 13.583 | 3.851 | 13.665 | 13.612 |
| Tick p99 ms | 3.989 | 38.738 | 38.553 | 4.571 | 39.901 | 42.373 |
| Tick max ms | 9.486 | 96.708 | 94.158 | 9.014 | 93.771 | 93.207 |
| Character states/s | 8,211.62 | 7,472.17 | 7,481.92 | 8,209.78 | 7,486.50 | 7,458.53 |
| Throughput loss vs control | baseline | 9.00% | 8.89% | baseline | 8.81% | 9.15% |
| Character/root recipient max gap ms | 201.244 | 630.103 | 622.587 | 201.376 | 621.101 | 680.095 |
| Reliable Event ACK max gap ms | 19.597 | 89.408 | 88.932 | 19.980 | 86.843 | 72.390 |
| RPC p50 ms | 48.915 | 57.809 | 58.102 | 48.743 | 57.884 | 42.052 |
| RPC p95 ms | 51.578 | 60.238 | 60.200 | 51.823 | 60.781 | 60.839 |
| RPC p99/max ms | 51.729 / 51.806 | 201.890 / 279.717 | 201.540 / 277.842 | 52.611 / 53.253 | 199.148 / 275.701 | 188.622 / 240.695 |
| RPC timeouts/errors/crashes | 0/0/0 | 0/0/0 | 0/0/0 | 0/0/0 | 0/0/0 | 0/0/0 |
| Action p99/max ms | 52.057 / 52.909 | 59.152 / 148.593 | 60.829 / 144.754 | 52.178 / 53.537 | 59.675 / 144.996 | 59.593 / 59.788 |
| Pre-3J cumulative ms | 321.311 | 2,052.952 | 2,040.383 | 323.673 | 2,047.282 | 2,041.118 |
| Graph cumulative ms | 181.298 | 262.082 | 269.507 | 188.255 | 271.719 | 259.858 |

No uniform improvement in load tails is claimed: the first Node recipient
maximum and tick p99 are worse. Load has no retirement work after drain, and
the remaining ~2-second pre-3J owner is unchanged. New Local relevance takes
1,948.391 ms inclusive (819.176 ms query, 15.766 ms selection-building), while
Desired reconciliation takes 82.299 ms. Load's pre-3J work accounts for 67.10%
of measured positive added busy time; that is not 67.10% of causal throughput
loss. Character scheduler rejections remain zero. Root spatial crossing and
identity checks pass. Action rejections/submission failures remain zero.

New pre-3J p50/p95/p99/max ms: control 1.312/1.611/1.751/2.129;
Local 8.657/9.710/10.945/38.255; Node 8.636/9.629/10.858/37.772.
Graph p50/p95/p99/max ms: control 0.444/0.673/0.748/0.856;
Local 0.624/0.998/1.116/1.269; Node 0.607/0.961/1.063/1.155.
Each has 640 graph Character visits/tick, 256,640 total. Examinations p50/p95/p99
remain zero because bursts occupy fewer than 1% of load ticks; maxima remain
0/68,258/68,258. New general discovery frontier entries are zero because no
such frontier was introduced, not because general work is bounded.

Character/root recipient p95/p99, Event service-gap p95/p99, RPC handler/queue/
client-completion subphases, exact per-peer discovery fairness and Player frame
timing are **not measured**. No percentiles are derived from maxima. Control
health gates pass; Local/Node still fail, so their differential executables and
the Node integration wrapper correctly return failure despite convergence.

An isolated same-binary repeat (`bounded-retirement-repeat-*`) preserves the
first result rather than replacing it: Control/Node tick p99 is 4.897/39.432 ms,
states/s 8,210.38/7,482.76 (Node loss 8.862%), recipient max gap
202.294/621.460 ms. Node convergence returns to 883.620 ms / 14 ticks versus
923.652 ms / 15 ticks in the first after run; Local first-run convergence is
879.386 ms / 14 ticks. Node repeat RPC p99/max is 198.335/277.690 ms, zero
timeouts/errors; action p99/max 60.934/144.001 ms. Its Event ACK max gap is
86.411 ms. Both Node runs still fail the unchanged health gates.

Control quiet-path mean tick is 2.625 ms before, 2.664 and 2.752 ms after;
p99 is 3.989 before, 4.571 and 4.897 after. Character throughput and maximum
tick remain close to baseline, but these samples do not establish zero overhead
or a no-regression confidence interval. The absolute control tail increase is
preserved. The retirement fast path performs zero examinations/allocations when
empty by source/test proof; the broader measured timing difference is not
attributed solely to that fast path. No general non-streaming capacity proof is
inferred from these two control runs.

### 500-peer result and journal

| Metric | Previous checkpoint | Diagnostic before | After |
| --- | ---: | ---: | ---: |
| Load convergence ms/ticks | 2,661.30 / 33 | 2,657.05 / 33 | 2,690.48 / 33 |
| Load tick p99/max ms | 82.798 / 111.422 | 82.829 / 111.824 | 83.025 / 111.697 |
| Eviction convergence ms/ticks | 2,951.69 / 32 | 3,493.52 / 32 | 1,456.53 / 32 |
| Eviction tick p99/max ms | 102.289 / 175.364 | 122.483 / 217.218 | 67.401 / 78.962 |
| Eviction Character/root max gap ms | 1,054.79 | 1,269.08 | 449.639 |
| Load/eviction examinations max | 106,176 / 106,150 | 106,176 / 106,150 | 106,176 / 106,150 |
| Load/eviction Pending high-water | 190,464 / 194,245 | 190,464 / 194,245 | 190,464 / 194,245 |
| Retirement examination max/tick | not isolated | 295,936 | 4,096 |
| Retired identity high-water | not isolated | not isolated | 900 cumulative startup; 512-unit eviction drains fully |
| Load current journal lag/margin | 6,135 / 10,249 | 6,135 / 10,249 | 6,135 / 10,249 |
| Eviction current journal lag/margin | 1,044 / 15,340 | 1,044 / 15,340 | 1,044 / 15,340 |
| Historical startup lag/margin | 16,358 / 26 | 16,358 / 26 | 16,358 / 26 |

After load peer-convergence p50/p95/p99/max is
1,569.57/2,585.58/2,656.90/2,690.48 ms; every peer converges. Eviction RPC
p99/max is 224.721/233.851 ms, zero timeouts/errors; Event ACK gap is 79.372 ms;
action p99/max is 50.887/97.388 ms. Reload converges in 33 ticks, 2,776.64 ms.
Initial acquisition/admission is exactly 1/1, eviction remains 1/1, reload is
1/2 with fresh identities. Load selected/accepted work is 261,545, with 31 exact
8,192-cap ticks; eviction selected/accepted work is 263,541, also 31 cap ticks.
No selector/per-peer budget changed. Journal aggregate backlog high-water remains
3,067,500 peer-records, maximum episode 124 ticks; all measured phases drain to
zero and journal failures are zero. None of these consumer cursor metrics is a
transport ACK metric. Startup's oldest responsible peer/entry remains unmeasured;
the 26-entry margin is still an unresolved whole-session closure concern.

### Correctness, resource contract and validation

The [architecture checkpoint](ContentAvailabilityFoundation3L_3.md#work-continuation-fairness-and-memory)
defines the 4,096-work cap, 24-byte cursor, 1,048,576 retired-identity ceiling,
generation-safe ownership and exact failure policy. The new 4,128-object test
proves saturation, repeated/older tick non-refill, two-tick coverage despite
Known endpoints, scalar revision reuse, two-peer retention, disconnect,
acceptance-only Known, eviction/recreation and cleanup. Additional final-source
assertions cover both cancellation and late acceptance of an Enter whose object
was destroyed while prepared. Existing dependency/bootstrap/reparent/hard-ref/
reverse-Leave/reference-model tests remain in the targeted suite. There is no
new partial dependency group or peer-discovery scheduler to certify.

MSVC Release: **6/6 PASS, 18.58 seconds**, final v2 test source. Included:
GameSession, replication relevance/dependency/bootstrap, replication, foundation,
physics backend and scheduler contracts. Clang 19 **ASan/UBSan/LSan: 7/7 PASS,
77.21 seconds**, final v2 source, including ContentAvailability in addition to
those six test families. The disposable four-CPU/12-GiB container used
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`; exit zero, no sanitizer report.
Documentation: **19 pages PASS, 3.99 seconds**, using the bundled Node runtime,
with only the pre-existing missing `docs -> 404` warning. Changed developer
Markdown is outside Astro's collection and separately passes relative-link and
whitespace checks. No deployment occurred. Both pre-existing Node fixture file
hashes also match their prior local/worker manifest; Node source is unchanged.
The unchanged 90-case physics matrix and prior
Node race evidence are retained, not relabeled as rerun in this slice.

Invariant status: **PASS** for tested retirement bound/rotation/cleanup,
generation safety, dependency/bootstrap correctness, acceptance-only Known,
200/500 convergence, exactly-one initial acquisition/admission, exact 8,192 cap,
unchanged semantic authority, no public priority API and no wire change.
**UNPROVED** for globally bounded general discovery/fixup work and its peer
fairness/capacity, whole-session journal margin and full pipeline overload.
**FAIL** for the unchanged healthy streaming service gates. No current-source
security/CI, official Player/reliable-transport, deployment or full-suite closure
is claimed. The sealed obsolete security artifact remains untouched and not green.

### Remaining owners and proposed boundary

1. Recurring 3E peer query/hysteresis/member work dominates healthy load; complete
   Desired/Known closure, reverse-Leave and fixup planning still produce bursts.
   Implementing a global continuation must preserve complete prerequisites and
   survive unrelated revisions without restart starvation. This task's general
   planning-bound objective remains unfinished, not transferred to transport.
2. 3J-adjacent fixup/group/encode work and fixture-only protocol observers remain
   real costs. Local load selection/encode/commit is 311.819 ms; fixture observers
   are separately 336.080 ms and are not production server CPU.
3. Official reliable transport/client service remains independently open at its
   historical ~3.8-second Remote gap. No loopback result closes that guarantee.
4. Startup journal margin, overload, full memory envelope, current security and
   CI gates remain open. Do not begin 3M.

Proposed code commit boundary, **not committed**: private catalog retention
leases, bounded retirement service, associated GameSession metrics/service call,
fixed diagnostic scope/counters, benchmark reporting, focused retirement tests,
and these documentation changes (including 3E/3I lifetime cross-references).
Do not sweep entire dirty files or unrelated prior Foundation/humanoid work into
that boundary. General bounded discovery requires further implementation and
validation; this retirement correction is not named as its completion.

The new diagnostic storage is fixed: one 80-byte phase sample and three uint64
counters add 104 bytes per opt-in WorkSample (5,520 total). The retained fixture
ceiling of five blocks x 1,201 ticks x two sides is 66,295,200 payload bytes.
Detailed history/logging remains fixture-only; production retains aggregate
counts and no per-peer/object label history. Capture-on/off timing overhead,
shared-pointer control-block bytes and long-run RSS effects are **not measured**.

## Previous slice: replication attribution and typed Character candidates (2026-09-10)

**Partial implementation; the requested general pre-3J work bound is NOT closed.**
The retained correction removes all-world Character polling from graph sync.
An arbitrary partial Pending cursor was not installed: the current 3J group
collector assumes dependency-complete Pending and reverse-Leave information.
Stopping at that correctness gate preserves acceptance-only Known and ordering;
it does not count as successful bounded discovery. No commit, push or 3M.

### Provenance and method

Engine HEAD remains `a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1`; Node remains
`f4440423c0701ff396f589fd51b6ce41edc63539`. The starting 44 Engine / two Node
native hashes matched the preserved `static-world-v3-source.json`. Nine native
paths changed in this slice: GameSession/Relevance public headers, GameSession,
ReplicationCoordinator, ReplicationRelevance, RuntimeWorkDiagnostics, and the
GameSession benchmark, GameSession tests and Relevance tests. Complete tracked
patches and the relevant new-source inventory accompany each checkpoint.
Existing lifetime, journal, physics and unrelated humanoid documentation work
was preserved, not reset or reconstructed. Node source did not change this slice.

| Checkpoint | Meaning | Benchmark SHA-256 |
| --- | --- | --- |
| `discovery-attribution-v1` | Fixed counters only; before behavior | `E887120FF932E6574F83145F74BA8608C80E5A4F9D16AC71FFC805C545C10754` |
| `character-candidates-v1` | Typed inspection subset | `2F092A2C8F89C22174833B6F2A0617E4C7A8014462CB8BCBEDBBCDB5349C0398` |
| `character-candidates-v2` | Same behavior; counts removals at the actual graph-difference loop | `E52E575CF2F34B828487C9F5FAC6A2C86A44F8B73C574ED4BF894A8BE3942E8F` |

Final local and worker native hashes match all 44 Engine / two Node entries.
The v1 graph-change counter missed removals absent from the new subset; its
eviction zero is obsolete diagnostic evidence, not a claim of no changes.
v2 correctly counts 199 removals at 200 peers and 499 at 500 peers.

Raw logs, source manifests and immutable analysis are under
`build-3l3-worker/evidence/`, mirrored from
`C:\Sandbox\Codex\Logs\gargantuan-3l3`. `*-discovery*.json` retains all 27
counter distributions and owning CPU distributions for each phase.
`*-analysis.json` retains exclusive load attribution and gameplay quantiles.
Before composite statistics use `discovery-attribution-v1-*-discovery-v2.json`;
earlier analysis artifacts remain untouched. Values below use final v2, not the
best interim run. Performance runs were sequential without concurrent builds.

The unchanged healthy fixture is 200 peers / 50 Characters, five per
neighborhood, staggered 12-Hz inputs, ten root-motion tracks, identical action,
event and 100-RPC schedules, and the same 512-object / 273,032-byte unit.
All three load phases have 401 ticks and exactly 54,918 accepted states.
Native GCHR acceptance/wall-second is distinct from recipient delivery.
Node is real TLS content acquisition; game transport is still loopback, not
official GameNetworkingSockets. The prior reference remains 6.80 / 42.23 /
41.36-ms p99 and 203 / 695 / 660-ms recipient maximum gaps.

Timing scopes measure Main-thread elapsed duration, not hardware CPU cycles.
Nested inclusive scopes must not be summed. `Pre3JExclusive` sums disjoint
exclusive relevance, spatial, catalog, Desired, closure and discovery scopes;
fixup planning remains separately visible inside the structural-selection scope.
Quantiles use floor((N-1)*p). Busy-time shares are measured ownership, **not**
counterfactual percentages of lost throughput. Allocation counts measure the
benchmark's intercepted native allocations, not all provider/OS allocations.

### A — actual amplification

The source path and multiplicative loops are in the
[architecture checkpoint](ContentAvailabilityFoundation3L_3.md#actual-upstream-path).
The maximum Local pre-selection discovery tick performs 64 complete peer plan
refreshes: **50,513 Desired + 17,745 Known + zero Pending = 68,258 examinations**.
It inserts 32,768 unique candidates with zero duplicate insertion attempts.
17,745 already-known identities appear in both set scans. It also processes
47,859 dependency seeds and 53,861 edges, with 50,947 repeated dependency visits.
This number is legitimate set reconciliation performed synchronously, not a
68,258-entry duplicate queue. The same tick has 33,025 relevant-root visits,
33,559 membership visits and 12,609 Remote argument-registry examinations.

| Load measure | Control before / after | Local before / after | Node before / after |
| --- | --- | --- | --- |
| Discovery examinations p50 / p95 / p99 | 0 / 0 / 0, unchanged | 0 / 0 / 0, unchanged | 0 / 0 / 0, unchanged |
| Discovery examinations max / cumulative | 0 / 0, unchanged | 68,258 / 213,030, unchanged | 68,258 / 213,030, unchanged |
| New candidates | 0 / 0 | 102,400 / 102,400 | 102,400 / 102,400 |
| Examinations / new candidate | not applicable | 2.0804 / 2.0804 | 2.0804 / 2.0804 |
| Examinations / selected or accepted operation | 0 / 0 | 2.0320 / 2.0320 | 2.0320 / 2.0320 |
| Pre-3J ms p50 | 1.287 / 1.277 | 8.454 / 8.647 | 8.435 / 8.689 |
| Pre-3J ms p95 | 1.606 / 1.636 | 9.549 / 9.697 | 9.511 / 9.583 |
| Pre-3J ms p99 | 1.742 / 1.758 | 10.697 / 10.664 | 10.332 / 10.926 |
| Pre-3J ms max | 2.239 / 2.070 | 40.258 / 39.907 | 38.295 / 38.286 |
| Pre-3J cumulative ms | 319.10 / 321.31 | 2,035.30 / 2,052.95 | 2,022.22 / 2,040.38 |
| Graph Character visits p50 / p95 / p99 / max | 10,000 each / 640 each | 10,000 each / 640 each | 10,000 each / 640 each |
| Graph Character visits cumulative | 4,010,000 / 256,640 | 4,010,000 / 256,640 | 4,010,000 / 256,640 |
| Actual Character graph membership changes | 0 / 0 | 0 / 0 | 0 / 0 |
| Graph ms p50 | 1.994 / 0.433 | 3.530 / 0.610 | 3.519 / 0.643 |
| Graph ms p95 | 3.005 / 0.665 | 5.020 / 0.967 | 4.838 / 0.996 |
| Graph ms p99 | 3.372 / 0.711 | 5.366 / 1.038 | 5.199 / 1.083 |
| Graph ms max | 3.723 / 0.791 | 6.364 / 1.251 | 5.521 / 1.183 |
| Graph cumulative ms | 848.22 / 181.30 | 1,455.09 / 262.08 | 1,437.82 / 269.51 |

Discovery bursts occupy fewer than 1% of these 401 ticks; zero p99 does not
erase the maximum. Selected/accepted denominators include coalesced journal
operations, not only new Enters. Graph nodes / changed nodes is undefined in
load (zero changes), not zero amplification. The old load loop rejected 93.6%
of its Character inspections; new load visits all qualify. Both paths still
call graph sync 401 times, service 80,200 peer opportunities, inspect 160,400
Remote registrations and allocate 577,440 graph-scope blocks / 23,097,600
cumulative bytes (1,440 allocations / 57,600 bytes maximum per tick).
The correction avoids unnecessary registry/RootPart work, **not** the remaining
temporary sets or all no-op synchronization. No fresh descendant walk occurs
inside this particular loop; 3E membership is already indexed.

Local closure work is 31.579 -> 30.503 ms cumulative; Pending discovery is
26.354 -> 25.767 ms, maxima 8.856 -> 8.416 ms. The much larger recurring
relevance owner is 1,933.945 -> 1,953.947 ms. Thus the 68,258 burst is a real
safety concern, but Pending insertion alone is not the largest aggregate CPU
owner. Final Local graph time falls 81.99%; pre-3J does not materially fall.
Final pre-3J accounts for 67.14% of positive added busy time. Neither percentage
is a claim that this fraction of throughput loss has been causally removed.

Unmeasured subdivisions remain explicit: dependency deduplication CPU separate
from closure, every descendant/fixup edge and per-peer replica check, graph
allocation quantiles, and full per-peer discovery-service-age distribution.
`GraphKnownChecks` counts composite eligibility tests, not individual hash probes.
No new unmeasured category is reported as zero.

### B / C — retained design, invariants and implementation

`ReplicationRelevance::BuildSelection` derives an ordered Character-root vector
while constructing the existing 3E selection. `GameSession::SynchronizeServerGraph`
uses it as an inspection hint and still verifies current registration, full
ObjectId generation, live RootPart, 3E runtime relevance and 3J-accepted Known.
All removals precede additions as before. No priority, acceptance, wire, physics,
content-limit or selector-budget changes occur. The precise existing dirty,
revision, movement, destruction and borrowed-span lifetime rules are documented
in the architecture checkpoint; no independent semantic truth is introduced.

The new vector reports 9,600 retained capacity bytes at 200 peers and 24,000 at
500 peers (1,200 / 3,000 ObjectId slots). It holds current sparse inspection
membership, not future transitions. Additional metadata is one vector object
per peer (24 bytes on this MSVC target) and a typed-root flag/padding in each
spatial entry. There is no measured per-peer vector high-water histogram.
Existing 512-peer / 65,536-spatial-root / 65,536 Desired-per-peer ceilings limit
cardinality; this is not proof of a small dense-case byte envelope or overload
closure. Capacity is reused and released on peer teardown; stale IDs cannot
pass live validation. New general discovery-frontier entries/bytes are zero
because that mechanism was **not implemented**, not because backlog is absent.
Existing Pending hard ceilings remain 65,536 per peer and 1,048,576 globally.

Detailed new counters remain opt-in, saturating fixed storage: 27 uint64 fields
add 216 bytes per WorkSample (5,416 total). The fixture's existing five-block,
1,201-tick, two-side capture ceiling is 65,046,160 payload bytes; it does not
create a production per-tick history or file logger. Dedicated capture-on/off
overhead was not measured; paired before/after runs both enable capture.

Fairness remains the existing rotating 3E peer evaluation (64 complete peers per
tick, reserved critical service) and 3J rotation. Observed cumulative 3E oldest
pending ages are three ticks at 200 peers and eight at 500; deferred-peer high
waters are 136/436. These maxima are **not** a new discovery cursor's service
interval distribution. Its p95/p99/max service opportunity, per-peer examination
slice, maximum skips, dense-work capacity and overload policy remain unproved.

A safe future discovery seam needs revision/identity continuations with charged
edge/fixup work, rotating per-peer slices and complete prerequisite/reverse-Leave
proof before exposing a group. `CollectGroup::Enqueue` currently ignores absent
Pending prerequisites; a partial frontier could therefore propose a group
without its unknown target. The subsequent hard-reference guard rejects such
a frame instead of deferring it, so this is not a demonstrated current bypass
of that guard. A partial reverse index could similarly omit a required Leave,
and Parent ordering needs a complete proof. The unsafe shortcut was not retained.
There is no production
work-cap value, resumable frontier or liveness proof to claim from this slice.

### D / F — healthy differential and non-starvation

| Metric | Before control | Before Local | Before Node | Final control | Final Local | Final Node |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Tick p50 ms | 4.362 | 13.229 | 13.083 | 2.803 | 10.527 | 10.473 |
| Tick p95 ms | 6.108 | 17.212 | 16.690 | 3.721 | 13.445 | 13.583 |
| Tick p99 ms | 6.587 | 41.793 | 43.776 | 3.989 | 38.738 | 38.553 |
| Tick max ms | 11.203 | 99.509 | 96.958 | 9.486 | 96.708 | 94.158 |
| Accepted Character states/s | 8,210.84 | 7,373.64 | 7,387.92 | 8,211.62 | 7,472.17 | 7,481.92 |
| Loss versus control | baseline | 10.20% | 10.02% | baseline | 9.00% | 8.89% |
| Character recipient max gap ms | 203.057 | 698.744 | 711.532 | 201.244 | 630.103 | 622.587 |
| Root-motion recipient max gap ms | 203.057 | 698.744 | 711.532 | 201.244 | 630.103 | 622.587 |
| Reliable Event ACK max gap ms | 19.774 | 64.855 | 68.436 | 19.597 | 89.408 | 88.932 |
| RPC p50 ms | 48.747 | 42.747 | 42.097 | 48.915 | 57.809 | 58.102 |
| RPC p95 ms | 52.135 | 61.934 | 61.340 | 51.578 | 60.238 | 60.200 |
| RPC p99 ms | 52.952 | 183.226 | 192.689 | 51.729 | 201.890 | 201.540 |
| RPC max ms | 54.398 | 251.848 | 243.660 | 51.806 | 279.717 | 277.842 |
| RPC timeouts / errors / crashes | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| Action p99 ms | 52.775 | 61.006 | 59.362 | 52.057 | 59.152 | 60.829 |
| Action max ms | 54.145 | 61.502 | 61.680 | 52.909 | 148.593 | 144.754 |
| Pending high-water, load | 0 | 73,728 | 73,728 | 0 | 73,728 | 73,728 |
| Observed load journal minimum post-service margin | 16,384 | 10,249 | 10,249 | 16,384 | 10,249 | 10,249 |

The earlier user reference (~42-ms p99, ~7,387 states/s, 660–695-ms gaps) remains
preserved separately above; the table pairs the counter-instrumented baseline
with final source. Actions use ten completed samples and the existing percentile
convention. Native observation maxima including boundary overhead are final
52.944/148.632/144.793 ms. There are no action submission failures or rejections.
RPC/Event/action tails did **not** uniformly improve. Their poorer measured
maxima are retained, not averaged away or attributed to an unmeasured cause.
Same schedule does not imply identical wall-time completion/alignment after a
runtime-speed change; no isolated transport counterfactual was run here.

Character/root recipient p95/p99 and Event inter-service p95/p99 remain **not
measured**: this slice keeps the max-only sampler, not a histogram. Reliable
Event round-trip p99/max after is 52.556/53.180, 201.161/238.066 and
196.742/235.202 ms; those are not publication/service-gap percentiles.
RPC handler/response queue/client-dispatch subphases and official Player frame
metrics remain unmeasured on this source. Root tracks retain current identity,
semantic transform and cell-crossing checks; no transform authority changed.
All load scheduler rejection counts remain zero. Control's four phases pass;
Local and Node streaming phases still fail the unchanged health gates.

Both Local gap windows have received endpoint ticks 548 -> 560. Before, recipient
gap is 698.744 ms and endpoint acceptance separation is bounded at
744.512–745.288 ms; after, 630.103 ms and 632.459–633.226 ms. Tick-start intervals
inside the final 548–560 window are 92.31, 57.33, 53.01, 87.99, 60.89, 60.55,
63.21, 54.61, 50.12, 50.43, 39.76 and 16.68 ms. This remains consecutive expensive
iterations, not one 630-ms activation. The retained `*.timeline.csv` and
`*.service-points.csv` include all Character, content, physics, relevance,
structural, action, root, Remote and publication scopes before/after. During
most cap-saturated ticks 3J selection is ~16–18 ms and fixture-only protocol
observers add ~19–21 ms outside server tick. Those observers are not production
Engine CPU. Endpoint bounds are not per-packet transport latency and cannot
exclude intermediate unreliable coalescing/loss. No complete transport trace
is inferred from them.

### E — 500-peer load, eviction and journal

| Metric | Earlier reference | Counter-instrumented before | Final |
| --- | ---: | ---: | ---: |
| Load maximum convergence ms / ticks | 3,033.13 / 33 | 3,081.32 / 33 | 2,661.30 / 33 |
| Load convergence p50 / p95 / p99 ms | 1,733.71 / 2,906.15 / 2,988.09 | 1,711.71 / 2,940.17 / 3,029.55 | 1,549.58 / 2,559.11 / 2,628.52 |
| Load tick p99 / max ms | 93.355 / 119.106 | 91.875 / 116.065 | 82.798 / 111.422 |
| Load states/s | 4,554.71 | 4,499.53 | 6,618.79 |
| Load Character/root max gap ms | 895.341 | 897.655 | 803.090 |
| Eviction convergence max ms / ticks | see retained raw log | 3,102.37 / 32 | 2,951.69 / 32 |
| Eviction tick p99 / max ms | see retained raw log / 183.318 | 106.061 / 177.990 | 102.289 / 175.364 |
| Eviction Character/root max gap ms | 1,147.26 | 1,085.84 | 1,054.79 |
| Load / eviction examinations max | not measured by new counters | 106,176 / 106,150 | 106,176 / 106,150 |
| Load / eviction Pending high-water | 190,464 / 194,245 | 190,464 / 194,245 | 190,464 / 194,245 |
| New discovery frontier | none | none | none |
| Typed candidate capacity bytes | none | none | 24,000 |
| Journal aggregate backlog high-water | 3,067,500 | 3,067,500 | 3,067,500 |
| Maximum backlog episode, ticks | 124 | 124 | 124 |
| Historical cumulative journal lag / margin | 16,358 / 26 | 16,358 / 26 | 16,358 / 26 |
| Observed load current lag / margin | not measured | 6,135 / 10,249 | 6,135 / 10,249 |
| Observed eviction current lag / margin | not measured | 1,044 / 15,340 | 1,044 / 15,340 |

500-peer load remains 401 measured ticks; the pre-load baseline ends at 301 ticks
in both new runs under the existing minimum-wall-duration/RPC-completion rule.
Before/final accepted load state counts are 59,371/59,372, not falsely described
as identical. Every peer converges; load accepted/selected operations remain
261,545 with 31 exact 8,192-cap ticks. Initial acquisition/admission are 1/1;
eviction retains 1/1 and reload becomes 1/2 with fresh runtime identities.
Final reload converges in 2,756.91 ms / 33 ticks. Journal failures are zero and
every recorded phase ends with zero journal backlog.

Load examinations total 828,660, or 3.2370/new candidate; p50/p95/p99/max are
0/0/106,044/106,176 before and after. Graph load visits fall 10,025,000 -> 298,745
(25,000 -> 745 each tick), and time 5,151.34 -> 538.71 ms. Graph eviction time
falls 2,478.93 -> 300.67 ms, but the ~1.055-second gap persists. Final eviction
catalog refresh p99/max is **63.680/140.002 ms**, versus 60.037/138.230 before.
The owning function repeatedly tests retired identities across peer Known sets;
the exact share of that internal loop is source-inferred, not separately timed.
Dependency, reverse-Leave and fixup work also persist: final eviction Desired
max 37.819 ms; fixup max 11.800 ms; group-selection max 7.491 ms.

The 26-entry margin is **startup's session-cumulative maximum**, already present
before these phases. New before/after-service gauges measure catalog sequence
minus each peer's committed consumer cursor; they are not an ACK backlog. Current
load journal production is 3,077,500 aggregate peer-record examinations, eviction
522,000, not that many new unique history entries. No capacity increase was
made. The exact oldest startup peer/entry and production-rate envelope were not
captured, so a defensible whole-session minimum margin remains **UNPROVED**.
This result corrects the previous eviction attribution; it does not close the
bootstrap concern or claim that transport pins history.

### G / H — invariant and validation status

| Invariant | Status on tested source |
| --- | --- |
| Eventual load/eviction/reload convergence | PASS in 200/500 fixtures |
| Exactly one acquisition / initial admission | PASS |
| Global 8,192 cap; configured per-peer/global selection | PASS; unchanged |
| Existing rotating 3E / 3J fairness | PASS targeted tests; new discovery fairness UNPROVED |
| Dependency-safe ordering / bootstrap / acceptance-only Known | PASS targeted regressions; no partial discovery installed |
| General bounded pre-3J discovery | UNPROVED; large synchronous scans remain |
| No all-world Character scan in healthy graph-sync path | PASS; related candidates still polled, no dense-case service guarantee |
| New candidate cardinality / existing Pending ceilings | PASS source/fixture checks; dense memory/overload envelope UNPROVED |
| Stale typed identities / peer cleanup / eviction-reacquisition | PASS targeted tests; no new transition frontier to certify |
| Semantic authority / no public priority API / no wire change | PASS scoped source inspection; not final security certification |
| Healthy streaming and 500 eviction service envelope | FAIL |
| Full journal-retention envelope | UNPROVED; startup margin remains narrow |

MSVC Release final v2: **6/6 targeted CTests PASS, 18.84 seconds**, including
GameSession, relevance/reference, replication/dependency/bootstrap, foundation,
physics reference and scheduler contracts. New tests cover typed reference
equality, stale connection removal, Character destroy/replacement, subtree
reparent/detach/return, storage release and 32 unrelated distant Characters
remaining outside two-peer graph inspection. Existing randomized 3E lifecycle
and 3J acceptance tests are retained. They are not misreported as tests of a new
bounded discovery implementation.

Final v2 Clang 19 **ASan/UBSan/LSan: 7/7 PASS, 77.48 seconds**, including the
same six test families plus ContentAvailability. The disposable container used
four CPUs / 12 GiB, `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`; exit code zero and no sanitizer
report. Logs preserve the exact completed build and test results. This is
targeted coverage, not full-suite certification.

The **physics matrix passes 90/90 cases**, six shapes x five Part counts x three
repetitions, exit zero and empty stderr. Its source is unchanged from the prior
settled-static correction. The 512-Part representative-rigid maximum repeated
p99 is 6.555 ms; assembly-heavy 14.786 ms. Oversize diagnostic assemblies remain
diagnostic, not newly legal content units. No package/physics limit changed.

Documentation build: **19 pages PASS in 3.57 seconds**, bundled Node 24.19;
existing missing `docs -> 404` warning only. These developer Markdown files are
outside Astro's content collection and separately pass relative-link and
whitespace checks. No deployment. Historical green validation below remains
scoped to its source; it is not substituted for these final-source runs.

Unexecuted final gates: official reliable transport/Player, overload/recovery,
complete current-source security, full current-source CI and deployment,
full native suites and fresh Node race. Node is unchanged in this slice; prior
race evidence is preserved. The sealed security artifact is untouched and its
obsolete pending-review checkpoint is **not green**. No fresh RSS drift/plateau
classification, recipient histogram or official packet-phase trace is claimed.

### I / J / K — remaining owners and proposed boundary

1. **Recurring relevance and complete pre-3J plans:** final Local 2,052.95 ms
   aggregate pre-3J versus control 321.31 ms, with 68,258/106,176 discovery bursts.
   Blocks bounded runtime service. Next: dependency-complete discovery/accepted
   reverse-index contract with charged, resumable peer/group work and reference
   tests; do not insert an unsafe Pending prefix.
2. **Retired catalog cleanup at 500-peer eviction:** 140.002-ms maximum owning
   scope and 1,054.79-ms recipient gap. Blocks eviction service. Next: isolate
   retired-object/peer Known traversal, its invalidation and bounded reclamation.
3. **3J-adjacent fixup/encode plus remaining graph temporary work:** final Local
   selection/encode/commit category 309.93 ms versus control 1.31 ms; graph still
   allocates 1,440 blocks/tick. Preserve dependency budgets while measuring each
   possible change independently. Fixture-only observer work is separately
   339.99 ms and must not be attributed to production Engine.
4. **Official reliable transport/client service:** independent historical
   ~3.8-second Remote delay, not ranked against loopback CPU by incompatible
   units. Blocks 3L. Existing GNS default reliable ordered stream carries both
   structural and RPC traffic. Retained ~1.44-MB reliable queue high-waters and
   prompt-handler evidence remain the starting point; final-source queue depth/
   age, transport selection/send, client receive/completion and lifecycle
   dependencies still need sampled tracing. Server discovery is **not ruled
   out** by this partial slice. No transport redesign belongs in this change.

Proposed eventual commit boundary: typed 3E inspection projection and its
GameSession consumer, bounded diagnostic counters/memory metric, focused tests,
and these two ledger sections. Keep only those hunks, not entire dirty files:
earlier physics/lifetime/journal fixes and unrelated humanoid work predate this
slice. Diagnostic-only coordinator hunks may accompany the profiling ledger.
No general bounded-discovery commit is ready; no source was committed or pushed.

**B — FOUNDATION 3L PARTIALLY READY.** Do not begin 3M.

## Previous pass: attribution and settled-static physics

This section supersedes older current-checkpoint wording below, not retained
measurements. Engine/Node HEAD remain `a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1`
and `f4440423c0701ff396f589fd51b6ce41edc63539`. Local source remains uncommitted;
no push, reset, unrelated-file overwrite or worker-to-local source replacement.
Node's two pre-existing integration changes are untouched. New native diagnostic
changes are enumerated in `owner-attribution-v3-source.json`; the final physics
slice is `static-world-v3-source.json` (44 Engine / two Node native hashes).
The worker's 44 Engine hashes matched before terminal validation. A source
manifest is provenance, not a completed security review.

### Exact differential and before/after

Same deterministic 200 connected peers / 50 active Characters / five per
neighborhood, 12-Hz staggered owner inputs, ten root-motion tracks, real client
Luau actions/events/100 RPCs per phase, same 512-object / 273,032-byte unit.
Control, Local OnDemand and real TLS Node OnDemand run sequentially, without
concurrent worker builds. Each load contains 401 ticks and 54,918 accepted
Character states (4,596,300 state bytes). GCHR stays unreliable sequenced.
The “states/s” column is scheduler acceptance per wall second, not recipient
throughput; recipient gaps are independently observed. Provider acquisition is
not a game transport lane. The official graphical Player is not this fixture.

| Metric | Before control | Before Local | Before Node | After control | After Local | After Node |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Tick p50 ms | 4.343 | 19.441 | 19.540 | 4.353 | 13.221 | 13.337 |
| Tick p95 ms | 6.026 | 29.402 | 29.833 | 5.898 | 17.190 | 17.150 |
| Tick p99 ms | 6.572 | 61.340 | 62.262 | 6.798 | 42.230 | 41.360 |
| Tick max ms | 10.365 | 103.939 | 99.554 | 9.614 | 96.870 | 96.580 |
| Character states/wall-second | 8,208.66 | 4,250.77 | 4,234.95 | 8,208.52 | 7,386.52 | 7,385.51 |
| Character state bytes/wall-second | 687,014 | 355,763 | 354,439 | 687,003 | 618,207 | 618,122 |
| Loss versus control | baseline | 48.22% | 48.41% | baseline | 10.01% | 10.03% |
| Recipient Character/root max gap ms | 203.073 | 1,100.640 | 1,203.500 | 202.815 | 695.362 | 659.970 |
| Reliable Event ACK max service gap ms | 20.560 | 127.566 | 106.378 | 20.378 | 64.688 | 89.972 |
| Reliable Event round-trip p99 / max ms | 53.661 / 55.868 | 305.412 / 332.325 | 304.664 / 399.488 | 53.854 / 54.522 | 183.754 / 322.969 | 204.584 / 238.321 |
| RPC p50 / p95 ms | 48.781 / 52.125 | 93.207 / 116.871 | 87.393 / 117.689 | 48.877 / 52.431 | 42.173 / 62.051 | 58.371 / 62.414 |
| RPC p99 / max ms | 52.610 / 53.120 | 306.665 / 332.449 | 292.705 / 303.861 | 52.642 / 52.799 | 183.593 / 242.424 | 206.283 / 279.471 |
| RPC timeouts / errors / crashes | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| Action request/result p99 / max ms | 52.179 / 54.692 | 105.607 / 153.749 | 118.003 / 119.323 | 52.170 / 53.487 | 60.013 / 61.034 | 63.611 / 144.970 |
| Character scheduler rejections | 0 | 0 | 0 | 0 | 0 | 0 |

Action quantiles use ten completed phase samples, with the existing percentile
convention (p99 does not become the maximum for ten samples). Native final
observation maxima, including the boundary action, are 53.524 / 61.071 /
145.009 ms after. No action submission failures/rejections occur. Root-motion
requests/commits are 2,010/2,010 control and 1,970/1,970 Local/Node; their
wall-time-dependent update counts legitimately differ while the configured
tracks/schedule are identical. Each produces 32,631 root GCHR states and ten
spatial crossings; identity and authority checks continue passing.

Missing metrics are **not measured**, not zero: recipient Character/root
p95/p99 (existing recipient sampler retains maxima, not a distribution),
Event inter-service p95/p99 (round-trip distribution is different), precise
RPC handler/response-produced/client-dispatch timestamps, action validation
subphases, and official Player frame/event-loop distributions on this slice.
Producer scheduler ages are measured but cannot substitute for complete RPC
phase timing, oldest still-queued age or actual packet-send/receive timestamps.
No unreliable Event delivery guarantee or measurement is inferred.

### Causal window and retained evidence

Before Local's gap spans recipient times 9,078.46–10,179.1 ms, endpoint state
ticks 548–560. The full per-tick/service-point files are adjacent to
`owner-attribution-v3-local-200-50.out.log`. Tick-start intervals across the
window are 114.08, 82.16, 76.72, 126.40, 102.47, 104.01, 105.23, 95.89,
93.44, 93.68, 82.26, 62.70 and 54.80 ms. Server recurring physics is about
19.5–20.7 ms/tick through most of the window, and client physics adds roughly
20 ms after apply. Dependency discovery peaks early; 3J selection remains
roughly 16–18 ms through tick 557, with about 19–21 ms protocol-observer work.
The full trace includes Character/action/root/Remote service points every tick:
service exists in tick units but wall-clock cadence dilates catastrophically.

Before endpoint acceptance separation is bounded at 1,085.964–1,086.789 ms,
versus the 1,100.640-ms recipient gap. After it is 731.620–732.441 ms versus a
695.362-ms recipient gap. Different endpoint queue delays can expand **or
contract** observed separation; these bounds are not per-packet transit times
and do not prove that no intermediate unreliable state was coalesced/lost.
This is many consecutive expensive ticks, not one 1.1-second admission.
The earlier official ~3.8-second Remote gap has an independent accepted
reliable structural backlog and remains open; no wire/lane change was made.

`AnalyzeAttribution.ps1` and `*-analysis-v3.json` preserve exclusive busy-time
accounting, gameplay quantiles and raw-log hashes. Earlier analysis JSON remains
immutable: v2 corrected PowerShell's integer `Math.Max` overload in proportional
shares; v3 adds existing gameplay lines with underscore-bearing field names.
Raw timings/counters never changed. The architecture document summarizes the
largest owners; no exclusive time is double-counted from backend subprofiles.

### Validation and gates on this slice

- MSVC Release compilation passes. Six targeted CTests pass in 18.21 s on
  `static-world-v3`, including physics reference/lifecycle and producer-order
  diagnostics. v1's test-only capture-pointer compile error is retained; it was
  corrected in v2 before tests ran and does not count as a pass.
- Clang 19 ASan/UBSan/LSan: seven targeted CTests pass in 77.62 s, with
  `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and UBSan halt-on-error enabled.
  `evidence/static-world-v3-linux-sanitizers.log` includes the completed build
  and terminal CTest result. This is exact-slice targeted coverage, not a fresh
  full-suite or official graphical Player certification. The disposable
  sanitizer container exits normally; incremental caches remain intact.
- Control's four phases pass. Local and Node converge in load/evict/reload but
  **fail unchanged health gates**. Native nonzero/Node Go-test failure is a
  service-health failure, not hidden as an integration pass.
- At 200 peers: exactly one acquisition and initial admission, fresh reload
  admission count two; pending high-water 73,728 during load; selected equals
  committed 104,840; exactly 12 global-cap ticks at 8,192; zero journal failures;
  journal load backlog drains in 49 ticks with maximum lag 6,314. Local peer
  convergence p50/p95/p99/max is 599.081/888.583/933.605/933.605 ms (max 15 ticks),
  versus 754.846/1,247.10/1,329.39/1,329.39 ms (14 ticks) before. Node after is
  578.924/878.606/919.727/919.727 ms (14 ticks). Faster wall convergence does not
  imply fewer convergence ticks.
- No new staging queue, semantic priority, alternate Desired/Known set, client
  apply queue, public authority API or wire-version change. All-static no-work
  state is a count and a flag; dynamic/changed worlds remain fully stepped.
- Final full-suite, official Player, overload/recovery, final security review,
  current-source CI and publication/deployment are not closed by targeted tests.
  No final legal-unit non-starvation envelope is claimed.
- The older sealed security artifact still contains an obsolete pending-review
  checkpoint. It has **not** been edited or declared green. Correcting the seal
  requires a valid scan/seal flow; this instrumentation/physics delta also needs
  final review. Prior manual inventory is preserved as historical evidence only.
- No new lifetime ownership queue was introduced, but the historical Local
  residual RSS drift is still unclassified. This finite differential's peak
  RSS includes large bounded debug captures and is not a new leak/plateau
  proof. Historical long soaks and Node race evidence remain preserved, not
  discarded or relabeled as final current-source full coverage.

### 500-peer and zero-peer physics checks

The same production binary preserves 500-peer load/evict/reload convergence.
Local load convergence p50/p95/p99/max is
1,733.71/2,906.15/2,988.09/3,033.13 ms, maximum 33 ticks. Acquisitions/admissions
are exactly 1/1 initially and 1/2 after fresh reload. Load selected and committed
both equal 261,545; 31 ticks saturate at 8,192. Pending high-water is 190,464
on load and 194,245 including eviction. Journal failures remain zero, but
maximum lag 16,358 against retention 16,384 leaves only **26 entries margin**.
This is not a comfortable worst-case retention envelope. Journal backlog
high-water is 3,067,500 aggregate peer-record positions and drains in at most
124 ticks; it is a backlog count, not extra copied journal history.

500-peer load control/Local tick p99 is 12.650/93.355 ms, max
30.516/119.106 ms; accepted Character rates are 8,843.80/4,554.71 states/s;
recipient max gap is 202.788/895.341 ms. Eviction still reaches **1,147.26 ms**
Character/root gap and 183.318-ms maximum server tick; RPC max is 393.433 ms.
It fails health. The 500-peer control also fails a later control phase, so its
entire run is not mislabeled a healthy baseline. This scale exercise protects
convergence/caps, not an A verdict or a replacement for the healthy 200-peer
differential. No new retention, peer-budget or selection-cap change was made.

The zero-peer matrix reruns all 90 cases (six semantic shapes, five Part-count
settings, three repetitions) with coherent body/constraint counts and cleanup.
The table reports the maximum of the three repetition p99s, not a pooled p99:

| Parts | Anchored sensor first tick max ms | Settled sensor p99 max ms | Simple rigid p99 max ms | Representative rigid p99 max ms | Assembly-heavy p99 max ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| 64 | 0.302 | 0.0008 | 0.05 | 0.23 | 1.25 |
| 128 | 1.197 | 0.0010 | 0.11 | 0.69 | 2.43 |
| 256 | 3.251 | 0.0022 | 0.22 | 2.37 | 6.27 |
| 384 | 5.217 | 0.0019 | 0.39 | 4.81 | 11.82 |
| 512 | 7.445 | 0.0025 | 0.45 | 6.61 | 14.95 |

Atomic attachment max for sensor worlds at those counts is
0.602/1.140/2.035/3.035/4.656 ms. Assembly attachment at 512 Parts reaches
10.274 ms and subsequent frame max 17.491 ms. Non-spatial fixtures register
zero rigid bodies/constraints; maximum measured attach is 0.83 ms. The
standalone first-step/activation cost is not the end-to-end streaming cost.
Counts are Parts: a containing Folder adds one content object, and a chain
adds N-1 WeldConstraints; 512 Parts means 513 sensor objects or 1,024 assembly
objects. Oversize diagnostic cases do **not** become legal package units.
No dependency component was split or package builder/limit changed. Matrix
source and raw values remain in `PhysicsActivationBenchmark.cpp` and
`evidence/static-world-v3-physics-matrix.out.log`.

The documentation build passes using bundled Node 24.19.0, 19 pages in 4.16 s,
with the existing missing `docs -> 404` warning. System Node 20 was rejected
by Astro before build; no system toolchain was installed or changed. These
developer Markdown documents are outside Astro's content collection and are
also checked separately for links and whitespace. No Pages deployment occurs.

**B — FOUNDATION 3L PARTIALLY READY.** No 3M. The next measured owners are
pre-3J relevance/transition production and per-peer Character graph work;
official reliable transport head-of-line service remains a separate blocker.

This is a measured **B checkpoint**, not a claim that work isolation is complete.
Implementation checkpoints now exist, but the full non-starvation gate still
fails. The [architecture checkpoint](ContentAvailabilityFoundation3L_3.md)
contains phase attribution, the exact Character-gap timeline, official RPC
correlation, legal-unit sweep and subsystem-owned remaining requirements.

## Scope and reproducibility

Local dirty Engine and Node trees are canonical. The authorized worker is
`dockerbox` / `192.168.0.108`, verified as `HostPC`. Source is under
`C:\Sandbox\Codex\Workspaces\gargantuan-runtime-host-f1-1\{engine,node}`.
No commit, push, reset, whole-tree replacement or worker-to-local source copy
was used. Unrelated humanoid architecture work and build outputs are preserved.

MSVC Release uses the existing combined build at
`C:\Sandbox\Codex\Builds\gargantuan-node\runtime-host-f1-1-baseline-msvc`
and Engine test build at
`C:\Sandbox\Codex\Builds\gargantuan\runtime-host-f1-1-baseline2-msvc-gns-vs`.
Builds use four jobs and existing dependency caches. Linux uses the existing
Clang 19 ASan/UBSan build and `codex-gargantuan-3l-toolchain:clang19-cmake331`,
with four CPUs, 12 GiB and two concurrent CTests. Node race uses two Go build
jobs and `GOMAXPROCS=2`; only verification, never performance measurement,
overlaps independent builds. No unrelated worker containers are modified.

Reproduction helpers and raw evidence are under `build-3l3-worker/` locally
and `C:\Sandbox\Codex\Logs\gargantuan-3l3` on the worker. Important files:

- `evidence/starting-source.json` and `instrumented-source.json`: provenance;
  the latter verifies all 38 Engine and two Node scoped file hashes.
- `RunDifferential.ps1`, `RunNodeDifferential.ps1`, `RunUnitSweep.ps1`:
  exact fixtures, process arguments and bounded runs.
- `evidence/instrumented-v2-{none,local}-200-50.out.log` and
  `instrumented-v2-node-200-50.log`: refined timing and gameplay results.
- Adjacent `.summary.json` and `.timeline.csv`: parsed phase/tick evidence;
  `AnalyzeWork.ps1` retains inclusive durations without summing nested scopes.
- `evidence/unit-{64,128,256,384}-local-200-50.out.log`: smaller legal units.
- `evidence/official-near-max/`: Player/server logs, process-memory samples,
  RPC and transport CSVs, service-gap windows and analysis JSON.
- `BuildAndVerify.ps1`, `RunOfficial.ps1`, `RunSanitizers.ps1` plus
  `RunSanitizers.sh`, and `RunNodeRace.ps1`: verification commands.

The differential reports the existing fixture's percentile convention. The
new official correlation helper uses nearest-rank percentiles and says so in
its JSON; retain the raw samples when comparing with older floor-index reports.
Inclusive per-tick phase percentiles are not percentiles of individual calls.

## Latest implementation verification (supersedes historical status below)

The latest measured source is `accepted-membership-v1` (41 Engine / 2 Node hashed
runtime files; complete security inventory separately contains 58 Engine / 2 Node
changed/new files). It compiles under MSVC Release and passes four targeted
foundation/replication/relevance/GameSession suites (18.09 s). Four previously
red reparent-before-parent-removal cases now preserve the child; accepted
ancestry and Parent-only watermarks are described in architecture checkpoint 9.
Local and real-TLS Node finish load, eviction and reload with zero journal
failures, zero Character scheduler rejections, one shared acquisition/admission
at load and two admissions after fresh-identity reload. All 100 RPC calls per
phase complete; these are functional results, **not latency passes**.

Load p99 is 6.760 ms control, 61.582 ms Local and 60.887 ms Node. Maximum
Character observed wall-clock gaps are 201.788 / 1072.83 / 1220.62 ms respectively.
Character throughput is 8209.58 / 4327.99 / 4188.38 states per wall second,
or -47.28% / -48.98% for Local/Node versus control. Node's maximum gap worsened
despite lower server p99; it is an explicit remaining failure.
Bounded relevance initially regressed the differential. Subsequent exact
selection reuse, direct journal cursor lookup, selected ancestry ordering and
bounded journal reads with lazy membership copying
now have measured combined benefit, but catastrophic service gaps remain.
Checkpoint-by-checkpoint attribution and preserved failed runs are in the
architecture document. No latency acceptance is claimed.

The complete 90-case zero-peer physics matrix passes registration/cleanup
assertions. Its direct WorldRoot measurements and which cases exceed the legal
512-object package unit are documented in architecture checkpoint 3. This is
not a replacement for final legal-max Server-to-Player testing. Neither the
512-object / 1 MiB limit nor package partitioning has changed.

Linux sanitizers for the earlier `typed-nil-fixups` checkpoint pass 5/5 targeted suites in 59.60 seconds
(Clang 19, ASan + UBSan, LSan `detect_leaks=1`, four compile jobs and two test jobs).
The exact source is the typed-nil checkpoint; new relevance cancellation,
disconnect/reload and critical bootstrap tests are included. This does not
replace the full final-source failure-injection/sanitizer matrix. All older
sanitizer, race, full CTest and official Player evidence below applies only to
its named historical source. The `lazy-journal-view-v1` six-suite combined
Clang 19 ASan/UBSan/LSan pass includes Foundation's cursor-range test plus the
new journal/relevance tests and passes in 76.43 s, with leak detection enabled.
That older run does not include subsequent structural ancestry work. The current
`accepted-membership-v1-linux-sanitizers.log` passes **6/6 in 76.02 s**, Clang 19
ASan + UBSan + LSan with leak detection enabled, including the current accepted
ancestry, Parent watermark, bounded group scratch and membership delta cases.
This is still targeted, not the full final-source failure-injection matrix.

Current security discovery covered all 60 changed/new Engine/Node files and 20
hand-authored helpers. Scan `2fb6b60a-c9f3-4496-baa8-b6b1d004635e` completed with
zero discovered candidates, but sealed coverage retains an obsolete seven-file
deferred checkpoint and says **partial**, despite accepted final complete input
and completed per-file review receipts. The architecture checkpoint records the
exact discrepancy and source-hash verification. Complete manual inventory/review
and incomplete sealed-report consistency must not be conflated. Final-source
security reconciliation, full Windows/Linux validation, overload/critical
lifecycle closure, commit, push and terminal CI/deployment remain outstanding.
No commit or push occurred; local published HEADs are unchanged.

### Required before/after table at the current B checkpoint

All values below are milliseconds unless stated otherwise. Before means the
exact `instrumented-v2` healthy differential; after means `accepted-membership-v1`,
not a final accepted implementation. `NM` means **not measured reliably** for
that requested metric. This preserves missing data rather than substituting
aggregate throughput, a differently shaped official fixture or inclusive CPU.

| Metric | Before control | Before Local | Before Node | After control | After Local | After Node |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Tick p50 | 6.342 | 20.544 | 20.728 | 4.418 | 19.233 | 19.483 |
| Tick p95 | 14.073 | 91.222 | 91.183 | 6.085 | 29.409 | 29.547 |
| Tick p99 | 15.887 | 104.447 | 96.707 | 6.760 | 61.582 | 60.887 |
| Tick max | 32.830 | 297.256 | 276.047 | 9.928 | 98.811 | 103.078 |
| Character states / wall-second | 8216.18 | 2742.93 | 2724.38 | 8209.58 | 4327.99 | 4188.38 |
| Character throughput delta | baseline | -66.62% | -66.84% | baseline | -47.28% | -48.98% |
| Character p95 server publication gap | NM | NM | NM | NM | NM | NM |
| Character p99 server publication gap | NM | NM | NM | NM | NM | NM |
| Character max server publication gap | NM | NM | NM | NM | NM | NM |
| Character max recipient-observed gap | 208.430 | 1117.750 | 1075.960 | 201.788 | 1072.830 | 1220.620 |
| RemoteEvent p99 service gap | NM | NM | NM | NM | NM | NM |
| Reliable-event ACK max service gap | 31.252 | 338.172 | 316.638 | 20.778 | 123.797 | 103.227 |
| RemoteFunction p50 round-trip | 49.514 | 174.313 | 175.043 | 48.882 | 91.498 | 90.954 |
| RemoteFunction p95 round-trip | 51.223 | 199.592 | 210.985 | 51.957 | 115.631 | 120.541 |
| RemoteFunction p99 round-trip | 51.668 | 452.913 | 420.468 | 52.539 | 299.798 | 290.611 |
| RemoteFunction max round-trip | 52.592 | 497.539 | 454.213 | 52.554 | 321.277 | 300.186 |
| Per-request server handler latency | NM | NM | NM | NM | NM | NM |
| Per-request response queue latency | NM | NM | NM | NM | NM | NM |
| Per-request client completion latency | NM | NM | NM | NM | NM | NM |
| Action request-to-observed-result max | 53.020 | 199.217 | 207.884 | 53.634 | 148.751 | 115.251 |
| Root-motion max server publication gap | NM | NM | NM | NM | NM | NM |
| Root-motion max recipient-observed gap | 208.430 | 1117.750 | 1075.960 | 201.788 | 1072.830 | 1220.620 |
| Graphical Player frame p95 | NM | NM | NM | NM | NM | NM |
| Graphical Player frame p99 | NM | NM | NM | NM | NM | NM |
| Graphical Player frame max | NM | NM | NM | NM | NM | NM |
| Player event-loop max gap | NM | NM | NM | NM | NM | NM |
| Official Player Character max gap | NM | NM | NM | NM | NM | NM |
| Official Player Remote max gap | NM | NM | NM | NM | NM | NM |
| Relevance CPU p99 / max | 6.683 / 10.909 | 74.711 / 78.872 | 70.749 / 73.059 | 1.687 / 1.903 | 10.432 / 14.908 | 11.034 / 15.536 |
| Pre-3J dependency/discovery CPU p99 / max | NM | NM | NM | 0 / 0 | NM / 20.540 | NM / 23.216 |
| Peer journal preparation CPU p99 / max | 3.197 / 13.897 | 7.004 / 160.572 | 7.055 / 153.306 | 0.108 / 1.857 | 2.443 / 2.606 | 2.483 / 2.691 |
| Physics activation-only CPU p99 / max | NM | NM | NM | NM | NM | NM |
| World physics total CPU p99 / max | 0.064 / 0.103 | 20.877 / 22.527 | 21.317 / 21.647 | 0.061 / 0.086 | 19.942 / 20.513 | 20.106 / 20.888 |
| 3J selection CPU p99 / max | 0 / 0 | 40.774 / 53.722 | 40.939 / 42.737 | 0 / 0 | 16.504 / 16.865 | 16.562 / 21.780 |
| GRPL queued bytes high-water | NM | NM | NM | NM | NM | NM |
| Client structural backlog high-water | NM | NM | NM | NM | NM | NM |

Missing-data reasons: the benchmark keeps recipient maxima and aggregate service
scopes, not per-relationship generated/accepted/sent timestamps or publication-gap
percentiles. RPC samples measure round trip, not all handler/queue/receive/dispatch
intervals. Event ACK timing is not a reliable/unreliable full event-service matrix.
The healthy differential is not the independently hosted graphical Player, and
its combined frame work cannot stand in for graphical frame timing. Physics
scopes include existing rigid-step work rather than isolating every registration/
broadphase/assembly operation. The original trace lacked the later fine-grained
pre-3J phases; current DesiredState max is shown only as an inclusive envelope,
not a falsely additive sum. Transport and received-not-applied queue high-waters
are not exposed by this loopback fixture. There is no introduced client apply
queue; lack of a queue measurement does not imply zero cost or zero backlog.

Separate original official near-max Player measurements (512 objects,
1,048,197 bytes, real GNS) remain Local/Node Remote maxima 3785.027/3795.658 ms,
Character maxima 3620.652/3640.422 ms, event-loop maxima 57.984/57.292 ms and
reliable queued-byte high-waters 1,443,536/1,444,046. They are not silently mixed
into the different healthy fixture above, and no current official rerun is claimed.

## Expanded implementation request: 157-item checkpoint ledger

This ledger answers the expanded request at the current **B checkpoint**. “Final”
requirements that are not complete are explicitly marked; this is not a final
publication report. Earlier 110-item/42-answer material below is historical.
All referenced evidence paths are relative to `build-3l3-worker/` unless stated.

| # | Required result | Current evidence or explicit gap |
| --- | --- | --- |
| 1 | Starting published Engine SHA | a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1. |
| 2 | Starting Node SHA | f4440423c0701ff396f589fd51b6ce41edc63539. |
| 3 | Starting uncommitted diff inventory | Starting tracked Engine: 39 files, +1397/-111; Node: two files, +113/-2. Full numstat and source hashes in evidence/starting-source.json; complete current 60-file tracked/new snapshot and full Engine/Node patches in security-accepted-membership-v1. Current tracked Engine is 49 files, +3222/-410; new untracked source/docs are separately inventoried. Unrelated humanoid files preserved. |
| 4 | Final Engine SHA | No final commit. Local published HEAD remains item 1; measured source is uncommitted accepted-membership-v1. |
| 5 | Final Node SHA | No final commit. Local published HEAD remains item 2; two integration files remain uncommitted. |
| 6 | Exact starvation workload | 200 peers / 50 active Characters / five trusted neighborhoods; five-tick staggered inputs; ten root-motion tracks; same actions, reliable events and 100 RPCs per phase; one full Engine client plus observers; 512 objects, 273032 bytes, package version 17; 401 load ticks. See architecture and retained helper arguments. |
| 7 | Original control tick p50/p95/p99/max | 6.3416 / 14.0726 / 15.8869 / 32.83 ms. |
| 8 | Original Local tick p50/p95/p99/max | 20.5442 / 91.2218 / 104.447 / 297.256 ms. |
| 9 | Original Node tick p50/p95/p99/max | 20.7277 / 91.1833 / 96.7068 / 276.047 ms. |
| 10 | Original Character states/s | Control 8216.18; Local 2742.93; Node 2724.38 states per wall-second. |
| 11 | Original Local Character throughput delta | -66.62% for the exact instrumented-v2 Local run; earlier equivalent result approximately -66.8%. |
| 12 | Original Character max service gap | Local recipient-observed 1117.75 ms; Node 1075.96 ms. Complete server-publication distribution not measured. |
| 13 | Original Remote max service gap | Separate original official GNS Player: Local 3785.027 ms, Node 3795.658 ms. |
| 14 | Worst causal tick timeline | Original and after CSV service points retained. Original t448 relevance 68.56 ms / selection 41.85 ms; t454 relevance 65.77 ms / journal 153.75 ms. Current Local endpoint ticks 548–560 span approximately 1.06 s acceptance separation. |
| 15 | Explanation for ~1.1-second Character gap | Many consecutive expensive server/client iterations stretch simulation-tick publication cadence; no Character scheduler rejections. Endpoint acceptance bounds corroborate the long wall interval; complete per-state transport history not measured. |
| 16 | Explanation for ~3.8-second Remote gap | GRPL bytes were accepted ahead of promptly handled RPC responses in the same GNS ordered stream. Client event-loop gaps below 58 ms do not explain 3.8 s. Exact packet-send versus queue-residence split not measured. |
| 17 | Relevance fan-out root cause | All-peer semantic/dependency invalidation and repeated full relevance selection; narrowed revisions and rotating evaluations reduce this. Work inside one peer evaluation remains collection-sized. |
| 18 | Dependency invalidation finding | Yes, ordinary journal cursor advance unnecessarily invalidated structural dependency plans. |
| 19 | Dependency invalidation fix | Separate DependencyCursor; compare Parent/class/hard-reference edges. Scalar/property/transform/attribute and soft-reference-only changes do not rebuild closure unless their semantic selection changes. |
| 20 | Relevance model after fix | Current trusted state is evaluated for a bounded rotating set of peers; same roots plus unchanged membership/owner state can reuse exact selection. 3E owns semantics. |
| 21 | Relevance work cap | 64 peer evaluations per tick default; valid 2–512. Not an object-candidate CPU bound. |
| 22 | Relevance backlog high-water | 200-peer differential: 136 deferred peers; 500-peer scale: 436. Not a staged object matrix. |
| 23 | Relevance oldest-work age | 3 ticks at 200 peers; 8 ticks at 500. Wall-clock age distribution not measured. |
| 24 | Pre-3J root cause | Full dependency closure, pending cancellation/discovery, Known enumeration, leave-dependency indexing and fixup scans occur before the selector ceiling. |
| 25 | Structural discovery cap | No independent total object-level discovery cap yet. A load can create 32768 pending transitions before 8192 selected operations. Remaining blocker. |
| 26 | Structural staging high-water | No separate discovery queue introduced. Existing 3J pending reaches 73728 at 200 peers / 190464 at 500. |
| 27 | Structural oldest-work age | No separate structural-discovery oldest-age metric; not measured. Existing pending ages do not substitute for undiscovered work. |
| 28 | Journal root cause | Historical-prefix scans, whole-view copies, overbroad dependency invalidation and large coalesced journal bursts. |
| 29 | Journal cap/change | Direct deque cursor indexing; borrowed read-only view; retry-inclusive 1024 records/peer and 32768 globally per tick (hard 8192/65536). Catalog refresh is a separate not-fully-bounded path. |
| 30 | Journal lag p95/p99/max | p95/p99 not measured. Cumulative individual max 6314 at 200 / 16358 at 500, including bootstrap, versus 16384 retained records. Aggregate 500-peer lag high-water 3067500 peer-records; maximum episode 124 ticks. |
| 31 | Journal failure count | Zero in current differential and 500-peer convergence runs. |
| 32 | Physics root cause | Dense touch-enabled sensor/rigid world cost persists after atomic registration; fixed/substep catch-up amplifies slow ticks. CanCollide=false does not disable CanTouch participation. |
| 33 | 64-Part physics result | 64 Parts: maximum attach / maximum-run steady p99 ms: disabled 0.64/<0.01; sensors 0.54/0.18; simple 0.57/0.06; representative 0.90/0.21; chain 1.09/1.22. |
| 34 | 128-Part physics result | 128 Parts: disabled 1.12/<0.01; sensors 1.10/0.87; simple 1.35/0.13; representative 1.66/0.68; chain 2.77/2.42 ms. |
| 35 | 256-Part physics result | 256 Parts: disabled 2.16/<0.01; sensors 2.06/2.25; simple 2.08/0.22; representative 3.26/2.33; chain 4.26/6.42 ms. |
| 36 | 384-Part physics result | 384 Parts: disabled 3.69/0.01; sensors 2.83/3.76; simple 3.10/0.33; representative 4.64/4.96; chain 6.78/10.78 ms. Chain exceeds legal package-object count. |
| 37 | 512-Part physics result | 512 Parts: disabled 4.30/0.01; sensors 4.91/5.94; simple 4.88/0.45; representative 6.75/6.86; chain 10.72/15.40 ms. Parts plus Folder or chain fixtures may exceed 512 total objects. |
| 38 | Final physics decision A/B/C | Not closed: neither A nor a measured-safe B reduction nor C safe partial activation has been established. Physics World Activation owns this gate; claiming a final decision now would be unsupported. |
| 39 | Original legal object limit | 512 total objects per independently streamable unit. |
| 40 | Final legal object limit | Unchanged 512; compatibility ceiling, not final proven service envelope. |
| 41 | Original legal byte limit | 1048576 bytes. |
| 42 | Final legal byte limit | Unchanged 1048576; no final safe byte-envelope approval. |
| 43 | Package partitioning changes | None. Existing builder extracts legal Workspace children; oversize children remain bootstrap. It does not implement the requested automatic dependency-safe subdivision. |
| 44 | Hard dependency group behavior | Selector rejects an oversize coherent dependency group before Known changes. Builder larger-world automatic partitioning / large assembly policy remains unimplemented; do not silently split. |
| 45 | Transport/RPC root cause | Same reliable ordered backend stream plus structural-first admission/send arbitration and queued bytes. Priority cannot preempt already accepted bytes. |
| 46 | Same-lane/arbitration/client classification | A established; additional queue/admission stages also contribute. D is possible but exact decomposition remains unmeasured. C is not the dominant explanation in the original official trace. |
| 47 | Transport changes | No transport arbitration/lane change retained; bounded diagnostic scopes only. |
| 48 | Protocol version result | No wire change: existing GNS envelope v2 and GRPL v1 preserved. |
| 49 | GRPL queue high-water | Original official Local 1443536 / Node 1444046 reliable queued bytes (aggregate, not a separately tagged final GRPL-only queue). Current official high-water not measured. |
| 50 | RPC/control queue high-water | Not measured independently; no new RPC/control lane exists. |
| 51 | GCHR contention result | GCHR service suffers stretched ticks and competes in the send path. Scheduler rejections remain zero; independent actual packet-delay decomposition not measured. |
| 52 | Client materialization root cause | Official full-frame apply costs tens of milliseconds; continued polling disproves a single 3.8-second apply freeze. Current dense fixture also pays client rigid physics. |
| 53 | Client apply architecture change | No staged client queue introduced. Existing coherent frame apply/signal semantics retained. |
| 54 | Client operation cap | Existing protocol/frame limits only; no new per-tick client apply operation cap. |
| 55 | Client byte cap | Existing protocol/transport limits only; no new incremental client apply byte cap. |
| 56 | Client backlog high-water | Not measured as received-not-applied operations/bytes. |
| 57 | Client oldest-work age | Not measured; no introduced client queue age metric. |
| 58 | Critical lifecycle client rule | Existing protocol lifecycle/order checks remain; no new prompt-service guarantee under scenery backlog proven. |
| 59 | Stale client create cancellation | No new queued-create cancellation mechanism to validate. Existing identity/epoch checks retained; final client backlog cancellation matrix unrun. |
| 60 | Zero-work control overhead | Healthy load control p99 15.8869 → 6.7595 ms; no exact whole-engine zero-allocation-per-idle-tick guarantee claimed. |
| 61 | Current control tick p50/p95/p99/max | 4.4176 / 6.0854 / 6.7595 / 9.9275 ms. |
| 62 | Current Local tick p50/p95/p99/max | 19.2334 / 29.4088 / 61.5818 / 98.8106 ms. |
| 63 | Current Node tick p50/p95/p99/max | 19.4828 / 29.5469 / 60.8869 / 103.078 ms. |
| 64 | Current control Character states/s | 8209.58 states/wall-second. |
| 65 | Current Local Character states/s | 4327.99 states/wall-second. |
| 66 | Current Node Character states/s | 4188.38 states/wall-second. |
| 67 | Local throughput degradation | 47.28% below control; still unacceptable. |
| 68 | Node throughput degradation | 48.98% below control; still unacceptable. |
| 69 | Control Character p95/p99/max gap | Server-publication p95/p99/max not measured. Recipient maximum 201.788 ms. |
| 70 | Local Character p95/p99/max gap | Server-publication p95/p99/max not measured. Recipient maximum 1072.83 ms; endpoint acceptance separation 1059.629–1060.413 ms. |
| 71 | Node Character p95/p99/max gap | Server-publication p95/p99/max not measured. Recipient maximum 1220.62 ms; endpoint acceptance separation 1219.221–1219.989 ms. |
| 72 | Control RemoteEvent | Reliable-event 402 offers / 398 in-window ACKs; maximum ACK-service gap 20.778 ms. Unreliable event matrix not run. |
| 73 | Local RemoteEvent | Reliable-event 402 offers / 398 in-window ACKs; maximum ACK-service gap 123.797 ms. Last in-flight ACKs are not labeled dropped. |
| 74 | Node RemoteEvent | Reliable-event 402 offers / 398 in-window ACKs; maximum ACK-service gap 103.227 ms. Unreliable event matrix not run. |
| 75 | Control RemoteFunction p50/p95/p99/max | 48.882 / 51.957 / 52.539 / 52.554 ms. |
| 76 | Local RemoteFunction p50/p95/p99/max | 91.498 / 115.631 / 299.798 / 321.277 ms. |
| 77 | Node RemoteFunction p50/p95/p99/max | 90.954 / 120.541 / 290.611 / 300.186 ms. |
| 78 | Handler latency | Not measured as request-linked per-handler distribution; aggregate RemotePump and original prompt-handler traces are retained. |
| 79 | Response queue latency | Not measured as request-linked queued→selected→sent latency. |
| 80 | Client completion latency | Not measured as request-linked received→dispatched completion latency. |
| 81 | RemoteFunction timeouts | Zero in 100-call-per-phase current differential and original official stress. |
| 82 | RemoteFunction crashes | Zero in those completed runs; historical lua_close lifetime crash correction retained. |
| 83 | Control action latency | Current maximum request→observed result 53.634 ms; sampled p99 about 51.982 ms. |
| 84 | Local action latency | Current maximum 148.751 ms; sampled p99 about 112.153 ms. |
| 85 | Node action latency | Current maximum 115.251 ms; sampled p99 about 105.636 ms. |
| 86 | Action rejection/timeout count | No action submission failures, rejections or unexpected endings in current differential; 11 accepted/resolved per load. Separate timeout count is not instrumented. |
| 87 | Control root-motion cadence | Load 2000 root requests / 2000 commits; 32631 observed root GCHR states. Full cadence distribution not measured. |
| 88 | Local root-motion cadence | Load 2110 requests / 2110 commits; 32631 observed root GCHR states. Wall-time service remains degraded. |
| 89 | Node root-motion cadence | Load 2050 requests / 2050 commits; 32631 observed root GCHR states. Wall-time service remains degraded. |
| 90 | Root-motion max gap | Recipient maxima 201.788 / 1072.83 / 1220.62 ms for control/Local/Node; exact server publication maximum not measured. |
| 91 | Root-motion spatial crossing | Ten crossings per load; old/new spatial membership, current identity and minimum ground height assertions pass. Streaming does not become transform authority. |
| 92 | Final Player frame p50/p95/p99/max | Not measured on current official legal-max graphical Player. Original distinct fixture Local/Node frame p99 18.224/18.229 ms, max 43.895/43.172 ms. |
| 93 | Player frames >16.7 ms | Not measured for current official Player. |
| 94 | Player frames >33.3 ms | Not measured for current official Player. |
| 95 | Player frames >50 ms | Not measured for current official Player. |
| 96 | Player frames >100 ms | Not measured for current official Player. |
| 97 | Player event-loop max gap | Current official not measured; original Local 57.984 / Node 57.292 ms. |
| 98 | Player Character max gap | Current official not measured; original Local 3620.652 / Node 3640.422 ms. |
| 99 | Player Remote max gap | Current official not measured; original Local 3785.027 / Node 3795.658 ms. |
| 100 | Zero-peer activation | 90 zero-peer physics matrix runs pass registration/cleanup. Non-spatial content creates zero rigid bodies, maximum direct attach 0.0342 ms. Not full 0/1/32/100/500 end-to-end activation decomposition. |
| 101 | One-real-client activation/materialization | Original real GNS Player evidence retained; current one Engine client is loopback differential, not a replacement official-product run. |
| 102 | 32-peer convergence | Prior 32-peer streaming convergence retained; final current dedicated 32-peer scale run not repeated. |
| 103 | 100-peer convergence | Prior 100-peer streaming convergence retained; final current dedicated 100-peer scale run not repeated. |
| 104 | 500-peer convergence | Current 500-peer Local load converges by 4081.94 ms / 33 ticks; eviction 3244.29 ms / 32 ticks; reload 4030.48 ms / 33 ticks. Health gate still fails. |
| 105 | 500-peer acquisition count | Exactly one provider acquisition for the shared load. |
| 106 | 500-peer admission count | Exactly one authoritative admission for load; fresh reload increases admissions to two, not 500. |
| 107 | 3J selected maximum | 8192 selected operations/transitions per tick; exact saturation retained. |
| 108 | 3J pending high-water | 73728 at 200-peer load; 190464 at 500-peer load. |
| 109 | Peer convergence p50/p95/p99/max | Current 200-peer Local 733.962/1218.94/1300.37/1300.37 ms; Node 848.701/1336.95/1419.79/1419.79 ms. 500-peer Local 2163.77/3875.22/3997.68/4081.94 ms. |
| 110 | Critical lifecycle under backlog | Targeted bounded bootstrap/relevance owner tests pass, including 192-peer bootstrap under backlog. Full join/replacement/revoke/destroy/disconnect latency matrix remains unclosed. |
| 111 | Eviction during staging | Targeted deferred relevance eviction prevents stale selection/materialization. Full pre-3J cursor fixture not applicable yet because that mechanism is not implemented. |
| 112 | Reload during old removal | Fresh identity reload and accepted parent-removal regressions pass. Exhaustive old-Leave/new-reload concurrent pressure remains unclosed. |
| 113 | Overload demand high-water | Final combined overload not measured; historical bounded 64-unit demand fixture retained. |
| 114 | Overload relevance high-water | Final combined overload not measured; normal-load relevance deferred maxima are items 22–23. |
| 115 | Overload structural-staging high-water | No discovery queue introduced; final combined overload not measured. |
| 116 | Overload 3J high-water | Final combined overload not measured; normal-load pending maxima are item 108. |
| 117 | Overload transport high-water | Final combined overload transport high-water not measured. |
| 118 | Overload client high-water | Final combined overload client high-water not measured. |
| 119 | Overload max tick | Final combined overload max tick not measured. |
| 120 | Overload Character max gap | Final combined overload Character gap not measured. |
| 121 | Overload Remote max gap | Final combined overload Remote gap not measured. |
| 122 | Overload drain time | Final combined overload drain time not measured; no claim of giant-tick-free recovery. |
| 123 | Disconnect under backlog | Targeted disconnect/cancellation tests pass; simultaneous saturation of all final stages is not established. |
| 124 | Server Stop under backlog time | Final all-queues-nonempty Stop latency not measured. Existing cancellation/join and unwind tests retained. |
| 125 | Player Stop under backlog time | Final Player backlog Stop latency not measured; no staged client queue introduced. |
| 126 | Lifecycle soak cycles | Historical Windows 1000-cycle and Linux 10000-cycle content evidence retained. No current final-source long lifecycle pressure rerun. |
| 127 | Same-process soak count | Historical 100 same-process legal-max lifetimes plus 100 Local/100 TLS Node unwind iterations retained; not current final whole-pipeline proof. |
| 128 | Stale peer result | Full ConnectionId lookup and disconnect/slot-generation reuse tests pass for current relevance cursors. |
| 129 | Stale ObjectId result | Full ObjectId generations, destroy/reload and pending stale-Enter rejection pass targeted tests. |
| 130 | Stale projection result | Projection/spatial crossing and removal assertions pass targeted/current differential cases; broad final randomized matrix not claimed. |
| 131 | Stale client staged-work result | No new staged client work. Required future client-queue generation/cancellation cases remain unimplemented. |
| 132 | Stale server staged-work result | Current peer cursors retain identities rather than raw peer pointers and re-evaluate current state. No partial pre-3J discovery state introduced. |
| 133 | Retained decoded-document bound | 16 MiB retained decoded-document hard/default charge remains; measured current decoded high-water 1623360 bytes. This is not total heap/RSS. |
| 134 | Aggregate pipeline memory high-water | Final simultaneous aggregate pipeline memory high-water not measured. Historical overload RSS 341024768 bytes / decoded charge 16151680 retained as separate evidence. |
| 135 | Local residual RSS classification | Not finally classified. Earlier near-max Local steady-quarter drift about 1.55 MB remains explicitly open; plateau and clean leak-sanitizer evidence do not prove every operation-correlated residual is allocator retention. |
| 136 | Windows RSS | Current 200-peer load RSS/Private: control 75382784/73977856; Local 144076800/145666048; Node 148697088/147525632 bytes. 500-peer Local RSS peak 385093632. Points, not a final plateau test. |
| 137 | Linux RSS | Historical Linux 10000-cycle Local RSS 510734336–530538496, final drain 521146368; Node 530849792–544493568, final drain 536050347 bytes. Current six-suite sanitizer run is not an RSS soak. |
| 138 | Security inventory method | Normal durable launcher, all 19 pages, complete git HEAD tracked binary patches plus relevant new files and frozen SHA256 snapshots; 60 canonical files + 20 helpers reviewed; 9083 generated rows explicitly excluded. |
| 139 | Security review result | Complete manual source-backed diff review found zero plausible new candidates. Sealed tool report says partial because obsolete seven-file checkpoint survived finalization; consistency remains unresolved and no security-clean claim is made. |
| 140 | ASan | Current six targeted suites clean under Clang 19 ASan; full final failure-injection matrix not run. |
| 141 | UBSan | Same current six targeted suites UBSan clean. |
| 142 | LSan | Same current six targeted suites LSan clean with detect_leaks=1, 76.02 s combined. |
| 143 | Node race | Node production unchanged; retained Windows race run passed in 61.56 s and earlier Linux CGO race passed. No new Node production fix. |
| 144 | Windows CTest | Current 4/4 targeted MSVC suites in 18.09 s. Historical supported full 53/53 retained for earlier source, not relabeled final. |
| 145 | Linux CTest | Current 6/6 targeted Clang sanitizer suites in 76.02 s. Historical full supported Linux result retained for earlier source; current full CTest not run. |
| 146 | Official Node → Server → Player | Original official TLS Node→Server→Player functional stress passed but service gaps failed; current final official rerun not performed. |
| 147 | Docs build | Current local Astro build passes: 19 pages in 2.96 s, exit 0, existing Node 24/dependencies, isolated output build-3l3-worker/docs-accepted-membership-v1. Nonfatal existing missing docs/404 entry warning retained. This is not deployed CI; devdocs Markdown is checked separately. |
| 148 | Pages deployment | No new Pages deployment; final terminal state not applicable until publication. |
| 149 | Native CI terminal | No new Native/Linux sanitizer CI run; no final commit/push. |
| 150 | Node CI terminal | No new Node CI run. Historical published SHA CI retained; dirty integration diff not published. |
| 151 | Published Engine commits | None during this continuation. |
| 152 | Published Node commits | None during this continuation. |
| 153 | Local/origin HEAD | Engine HEAD equals its locally stored origin/main at a68f76b. Node HEAD equals its locally stored origin/master at f444042; this Node checkout has no origin/main ref. No final fetch/publication equality is claimed and dirty changes are not published. |
| 154 | Tracked worktree status | Tracked Foundation changes remain uncommitted and preserved. This is not the requested clean final worktree. |
| 155 | Foundation 3L verdict | B — FOUNDATION 3L REMAINS PARTIALLY READY. |
| 156 | Whether 3M may begin | No. |
| 157 | Exact next recommendation | Implement dependency-safe bounded pre-3J discovery in its replication owner, then measured physics/unit and transport service corrections. Reconcile final security artifacts and run the final matrix before publication; do not create 3L.4 or start 3M as a workaround. |

## Expanded request: 78 direct answers

1. **What caused the ~104 ms Local tick p99?** Repeated relevance/selection, pre-3J reconstruction, journal bursts and rigid physics. Original inclusive relevance p99/max 74.711/78.872 ms, journal max 160.572 ms; phases overlap and cannot be summed as percentages.

2. **What caused the ~97 ms Node tick p99?** The same Engine domains: relevance p99/max 70.749/73.059 ms, journal max 153.306 ms, world physics p99 21.317 ms. No evidence Node provider execution is the dominant CPU cause.

3. **What caused the ~1.1-second Character publication gap?** Consecutive expensive iterations stretch tick-based publication cadence. It is a recipient-observed gap, not a measured complete server-publication distribution. Current endpoint acceptance bounds still span ~1.06 s Local / ~1.22 s Node, with zero scheduler rejections.

4. **What caused the ~3.8-second Remote gap?** Prompt server handlers followed by responses queued behind reliable structural bytes. Original official client keeps polling with <58 ms max event-loop gaps, excluding a single 3.8 s apply freeze as the dominant explanation.

5. **How much was relevance fan-out?** Original Local p99/max 74.711/78.872 ms; current 10.432/14.908 ms. Inclusive domain, not an exclusive contribution percentage.

6. **How much was dependency-plan invalidation?** Exact invalidation and reuse produce recorded per-checkpoint improvements; there is no independent scalar percentage of the full tick. Ordinary property traffic no longer rebuilds the structural closure.

7. **How much was pre-3J structural discovery?** Current 500-peer closure max 14.281 ms, pending discovery max 11.259 ms; 139404 dependency visits and 106176 candidate examinations in worst ticks. 32768 pending insertions can precede the 8192 selector cap.

8. **How much was journal processing?** Original Local journal-preparation max 160.572 ms; current 2.606 ms. Direct indexing and lazy membership views remove historical-prefix and whole-view-copy costs; catalog refresh remains separately qualified.

9. **How much was physics activation?** Current Local total world-physics p99/max 19.942/20.513 ms. Atomic activation and steady rigid steps are different costs; the 90-case matrix separates attach from steady stepping, not every backend microphase.

10. **How much was transport arbitration?** Original reliable backlog is causal, but actual queued→selected→packet-sent time is not fully measured. No numeric exclusive attribution claimed.

11. **How much was Player application?** Original official apply max about 37.8 ms Local / 37.4 ms Node, with event-loop max below 58 ms. This contributes spikes, not the full multi-second gap; current official rerun unperformed.

12. **Did ordinary journal mutations invalidate structural plans?** Yes.

13. **Is that fixed?** Yes for dependency-plan invalidation, with targeted parent/hard-reference/destroy/reload/Character tests. Not a claim that all journal consumers are bounded.

14. **What mutations invalidate plans now?** Create/destroy/retire/revive, ancestry, hard-reference or class dependency changes, world scope/generation and semantic/required selection changes. Ordinary scalar, transform and soft-reference changes alone do not.

15. **Is relevance fan-out bounded per tick?** Partly: evaluated peers per tick are capped, but per-peer query/closure work and initial native AddPeer evaluation are not a sufficient object-level CPU bound.

16. **What is its work unit?** One complete peer evaluation; default 64, supported 2–512.

17. **Can staged relevance become stale after eviction?** No partial object-result queue was added. A peer can retain its old coherent selection temporarily; live generation checks and stale-plan gating prevent old-generation acceptance after retirement. Targeted eviction tests pass.

18. **Can staged relevance become stale after movement?** Previously coherent selection can lag authoritative movement until its bounded evaluation turn; evaluation uses current focus/revision. There is no queued precomputed object result that is blindly applied.

19. **Does eventual Desired equal the reference model?** Yes in the deterministic and randomized flat-world reference tests. Broader fully staged dependency-graph/reference coverage is not claimed.

20. **Is pre-3J discovery bounded?** No sufficiently narrow total object-level discovery cap exists yet. This remains a primary blocker.

21. **What is its work unit?** Current diagnostics count dependency visits and candidate examinations; no enforced total discovery work unit/cap has been implemented.

22. **Can structural staging become another truth set?** No new staged truth set was introduced. Any future discovery cursor must derive from existing semantic Desired and never independently own Desired/Known.

23. **Does Known still change only through 3J acceptance?** Yes on the policy-managed GameSession/3J path. Legacy native SetRelevant has pre-existing immediate semantics and must not be described as this acceptance path.

24. **Does 3J retain its cap?** Yes: exact configured 8192 default global selection maximum holds in current scale runs.

25. **Is journal scanning bounded?** Peer reads/retries are capped at 1024/32768 default per peer/global per tick. Direct journal reads no longer scan history prefixes. Catalog and other consumers are not thereby proven fully bounded.

26. **Is journal retention sufficient?** Not finally proven. Current 500-peer cumulative max 16358 is only 26 below 16384 retained entries; p95/p99 final lag and overload retention remain unmeasured.

27. **Is physics activation atomic?** Yes; no semantically live partial-physics activation was introduced.

28. **Is the legal unit small enough to make that safe?** Not established. Smaller-unit historical experiments still failed the healthy service gate, and the final package/assembly envelope is unclosed.

29. **Final object limit?** Unchanged 512 total objects; not a final measured-safe envelope.

30. **Final byte limit?** Unchanged 1048576 bytes; not a final measured-safe envelope.

31. **Automatic partitioning of larger authored content?** Not the requested automatic subdivision. Existing legal Workspace children can become units; oversize subtrees stay bootstrap.

32. **Can hard dependency groups be split incorrectly?** No new splitter was added, so no unsafe new split is claimed. Future automatic partitioning must preserve hard dependency components and explicitly reject oversize atomic groups.

33. **Explicit large-assembly handling?** Not a final new policy. Existing oversize bootstrap/selector failure behavior must not be presented as the requested measured production rule.

34. **Are RPC responses behind GRPL in the same reliable stream?** Yes in the current GNS backend, which uses the default reliable ordered lane.

35. **If not, what delayed them?** Not applicable to the same-stream premise; additional early flush/admission ordering also exists, while exact subphase split is unmeasured.

36. **Did wire semantics change?** No.

37. **Did protocol version change?** No; GNS envelope v2 / GRPL v1 remain.

38. **Can GRPL still cause multi-second RPC delays?** Yes, the original official reproduction remains unclosed; no transport correction or final official rerun establishes otherwise.

39. **Can GRPL still starve GCHR?** Regular wall-clock GCHR service remains unproven. Current gaps exceed one second despite zero publication scheduler rejections.

40. **Is client structural application bounded?** Not by a new per-tick dependency-safe application budget. Existing frame/transport size limits remain.

41. **Client operation bound?** No new client per-tick operation cap; existing protocol limits only.

42. **Client byte bound?** No new client per-tick apply byte cap; existing protocol limits only.

43. **Can client backlog grow indefinitely?** No new unbounded client queue was introduced. Existing transport bounds do not establish full received-not-applied accounting or overload closure.

44. **Can critical lifecycle starve behind scenery?** Not ruled out; prompt lifecycle guarantees across all final backlogs remain unclosed.

45. **Can obsolete queued creates apply after reload?** No new queued-create mechanism exists. Existing epoch/identity rules remain; the proposed staged-client cancellation fixture has not been implemented or proven.

46. **Control tick p99 now?** 6.7595 ms.

47. **Local tick p99 now?** 61.5818 ms.

48. **Node tick p99 now?** 60.8869 ms.

49. **Local Character degradation now?** 47.28% below current control.

50. **Node Character degradation now?** 48.98% below current control.

51. **Worst Character gap now?** Recipient maximum 1220.62 ms Node in the healthy differential; 1294.4 ms in the separate 500-peer Local scale run. Exact per-relationship server-publication maximum not measured.

52. **Worst RemoteEvent gap now?** Reliable-event ACK service max in current healthy load: 123.797 ms Local. Full event p99/unreliable-service matrix not measured.

53. **RPC p99/max now?** Local 299.798/321.277 ms; Node 290.611/300.186 ms; control 52.539/52.554 ms. Separate original official multi-second gaps remain unresolved.

54. **Is 100-call RPC crash-free?** Yes for current 100 calls per phase and retained official stress; zero observed crashes/timeouts in those completed runs.

55. **Owner-action p99/max now?** Sampled p99 control/Local/Node about 51.982/112.153/105.636 ms; aggregate observed maxima 53.634/148.751/115.251 ms. Full phase-tagged action matrix unclosed.

56. **Root-motion max publication gap now?** Recipient root-state maximum 1220.62 ms Node; exact server-publication max not measured.

57. **Is root motion spatially correct during streaming?** Current ten-crossing fixtures pass semantic CFrame, 3K/3H membership and identity assertions, with matching request/commit counts.

58. **Can final-legal-max content freeze Player?** The legal envelope is not approved safe. Original apply/frame spikes are tens of ms, but Player Character/Remote service still stalls for seconds under reliable backlog.

59. **Player max frame now?** Current official not measured; original separate near-max frame max Local 43.895 / Node 43.172 ms.

60. **Player max event-loop gap now?** Current official not measured; original Local 57.984 / Node 57.292 ms.

61. **Do 500 peers still cause one acquisition?** Yes, exactly one.

62. **One authoritative region lifetime?** Yes, exactly one admission/lifetime for shared load; a fresh reload is a second server lifetime, not a per-peer duplicate.

63. **Does every peer converge?** Yes in current 500-peer Local load/evict/reload and retained 32/100-peer fixtures. This is convergence, not service health.

64. **Are journal failures zero?** Yes in the completed current differential/scale runs; final retention/overload acceptance remains open.

65. **Can sustained control traffic starve structure?** Not yet tested with a new arbitration policy; none was implemented. Reverse-starvation gate remains unclosed.

66. **Is overload bounded at every stage?** Not proven simultaneously across all requested stages. Individual existing limits and historical content overload are not a replacement combined bound.

67. **Does overload drain incrementally?** Final recovery not measured; no guarantee claimed.

68. **Can disconnect proceed under saturation?** Targeted disconnect tests pass; all-stage saturated disconnect timing remains unmeasured.

69. **Can Server Stop proceed?** Existing Stop/cancellation and targeted lifetime cases pass; final all-backlogs-nonempty bounded latency is not measured.

70. **Can Player Stop proceed?** Existing teardown evidence retained; final Player apply-backlog Stop latency not measured.

71. **Are staged identities generation-safe?** Current relevance cursors use full generation identities and no raw peer/object pointer. No new partial structural/client cursor was introduced. Future staged mechanisms still require their own proof.

72. **Is Local residual RSS a leak?** Not finally classified. Earlier small-content plateaus and clean sanitizers do not dismiss the later ~1.55 MB Local steady-quarter residual.

73. **Total pipeline memory high-water?** Not measured as a simultaneous final aggregate. Current logical charges and process RSS snapshots are documented separately.

74. **Is final security review complete?** Manual exact-snapshot inventory and source-backed review are complete, with zero discovered candidates. The sealed tool coverage is still partial due an obsolete retained checkpoint; final artifact consistency is not closed.

75. **Are sanitizers clean?** Current six targeted Clang 19 ASan/UBSan/LSan suites pass with detect_leaks=1. Full final-source sanitizer/failure-injection matrix is not claimed.

76. **Is ordinary non-streaming performance regressed?** Current healthy control improves to p99 6.760 ms. Full current 32/500-peer ordinary non-streaming regression matrix has not been rerun; no universal no-regression claim.

77. **Is Foundation 3L ready?** No. Pre-3J work, physics/legal envelope, reliable transport service, client/overload/final validation and security artifact consistency remain material gates.

78. **May 3M begin?** No.

## Security authority/resource questions at this checkpoint

These answers describe the implemented delta, not unimplemented queues or a
complete production non-starvation guarantee. The sealed-report consistency
qualification above remains in force.

| Question | Evidence-backed answer |
| --- | --- |
| Can a client choose relevance processing priority? | No new capability: the cap/cursors are trusted native configuration; only authoritative owner-Character changes mark critical relevance. |
| Can a client choose structural staging priority? | No new priority surface; no separate discovery staging layer was introduced. Existing critical classification remains Engine-owned. |
| Can a client inflate a staging cursor without normal authoritative content? | No new object-level cursor exists. Peer cursors contain full existing ConnectionIds and are constrained by the session peer limit; clients do not supply cursor positions. |
| Can one legal region exceed staging hard bounds? | It cannot exceed the configured peer/journal/selected-operation ceilings through new controls. However, there is no adequate total pre-3J object-work cap yet, so a legal unit still causes excessive synchronous work. |
| Can stale peer identities remain in staged work? | A rotation cursor may retain an old full value, but lookup reacquires the current map entry and cannot bind another generation. Disconnect/reuse tests pass; no raw peer pointer is retained. |
| Can stale ObjectIds remain in staged work? | Old identities may remain temporarily in existing pending/Desired bookkeeping; generation checks, retirement and stale-plan gating prevent their acceptance as a new identity. Tests cover eviction/reload. |
| Can evicted content reappear from queued client work? | No new client apply queue was introduced. Existing ordering/epochs remain, but the requested future staged-client cancellation matrix is not proven and no blanket new guarantee is claimed. |
| Can a client force critical-lifecycle classification? | No direct classification API. Critical work derives from authenticated/validated lifecycle state under existing server authority, not a client priority label. |
| Can malformed structural input create unbounded client backlog? | No new backlog queue was added and existing decoder/transport ceilings remain. Complete simultaneous client-backlog resource proof is still open; absence of a new queue is not proof of total memory bounds. |
| Can transport priority bypass authentication or ordering? | No transport priority/order change was implemented. Existing GameSession role/phase and publication guards remain. |
| Can RPC/control bypass required lifecycle dependencies? | No new bypass was added; current Remote availability/publication and accepted-materialization gates remain. A future independent lane must explicitly re-prove this ordering boundary. |
| Can structural backlog memory grow indefinitely? | Existing pending and scheduler byte/cardinality limits remain; policy-path ancestry is tied to Known lifetime. No new dense staging matrix. Final end-to-end server/wire/client accounting and legacy native Known bounds remain qualified. |
| Can diagnostics grow memory/log output without bounds? | New WorkCapture has fixed aggregate storage; benchmark trace blocks and smoke output have explicit caps. No per-peer/object high-cardinality production label or unbounded timestamp history was introduced. |

## Historical instrumentation-only verification

Windows native build: **pass**, including the diagnostic executable and rebuilt
official Player/Server/packager distributions. Existing LNK4098 CRT warnings
remain visible; this is not a warning-free result.

Windows CTest: **18/18 targeted tests pass**. The initial selected run has 17
tests in 16.15 seconds; the content test's real CTest name is
`gargantuan_content_availability`, and its separate corrected invocation passes
in 1.20 seconds. The target set covers foundation, physics backend, soft body,
animation, render publication, scheduler contract, replication, relevance,
spatial region index, Remote, Remote Luau, Character networking, GameSession,
real transport, Remote real transport, Character real transport and GameSession
real transport, plus content availability. Do not call this a full CI run.

Official Node-to-Server-to-Player: **functional pass**, Local and TLS Node,
eight near-maximum-content churn cycles each, 100 RPCs each, no timeout/error
or crash. The same runs **fail Foundation service latency**: worst Remote
service gaps are 3,785.027/3,795.658 ms. Headless operation does not validate
the graphical/GPU path.

Node race: **pass**, `go test -race -p 2 -count=1 -timeout 10m ./...`, exit 0 in
61.56 seconds using Go 1.22.12, CGO and the existing Windows GCC. Optional
external native integration cases are not implied by the default package suite.
Separately, the rebuilt content-scale executable passes 100-iteration Local
and real-TLS Node exception-cleanup suites, with no stale worlds, providers or
objects in the reported iterations. These are lifetime checks, not another
performance benchmark or the full mixed pending-work lifecycle soak.

Linux targeted sanitizers: **5/5 pass**, exit 0, CTest wall time 63.44 seconds.
Clang 19 ASan/UBSan and `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` report no
finding in content availability, GameSession, replication, relevance and rigid
physics tests. This cached Linux configuration has GNS disabled; Windows real
transport coverage is separate. CMake regeneration took 358.3 seconds before
the 16 incremental compile/link steps; this is build overhead, not runtime
latency. The first sanitizer command failed
before compilation because Windows shell quoting stripped the CTest regex
quotes. The corrected runner mounts a shell script instead; that invocation
failure is neither a code finding nor a sanitizer pass.

Security: **incomplete**. The desktop Security Diff Scan launcher returned
`The selected scan target changed while the scan was starting. Try again.`
It returned no authoritative scan ID or inventory. No substantive final scan
or clean result is claimed. The security skill requires preserving scan
identity and not replacing failed/missing scans; no replacement scan, invented
ID, empty-inventory pass or sealed report was produced. Source stabilization
and a valid launcher recovery are required before the publication gate.

Memory: the new eight-cycle official runs peak at 69,369,856 bytes Local and
72,818,688 Node RSS. Each selected steady window has only seven samples over
approximately 0.75 seconds; these short-window slopes cannot classify Local's
residual growth as a leak or a plateau. The retained 3L.2 long Windows/Linux
soaks and Local near-limit residual drift remain the applicable historical
evidence; no new arbitrary-duration or combined client/server-pipeline bound
is claimed from these short runs.

## Requested report: 110 items

Numbers match the supplied 3L.3 prompt. Unless marked historical, performance
figures below use the refined run's equivalent **load** phase, not a comparison
of a shorter bootstrap phase against load. All times are milliseconds.

| # | Result |
| --- | --- |
| 1 | Starting Engine HEAD: `a68f76bbf9bc4d75bac1de40bdc7b8dc4837b2a1`. |
| 2 | Starting Node HEAD: `f4440423c0701ff396f589fd51b6ce41edc63539`. |
| 3 | Engine HEAD unchanged; the candidate is uncommitted. Refined executable SHA-256: `852A4704C40F5BAF363838A5D66A618DC62AA270C57939D030734EB62CE28648`. |
| 4 | Node HEAD unchanged; two starting dirty files remain canonical and uncommitted. |
| 5 | Starting tracked Engine diff: 39 files, +1,397/-111. Node: two files, +113/-2. Untracked provenance is separate. |
| 6 | Checkpoint tracked Engine diff: 47 files, +1,555/-127; Node unchanged +113/-2. New diagnostic header/docs/helpers and pre-existing untracked artifacts are not in tracked diff counts. |
| 7 | One 512-object / 273,032-byte region; 200 connected peers, 50 active Characters, five per neighborhood, 12-Hz staggered inputs, ten root-motion tracks, actual actions/Remotes, one real client Engine. |
| 8 | Equivalent no-stream tick p99 15.8869; initial uninstrumented reproduction 14.1039. |
| 9 | Initial Local p99 94.9392; instrumented V1 95.3195; refined 104.447. This is repeated reproduction, not a correction. |
| 10 | Initial loss about 66.8%; refined Local 66.62%, Node 66.84%. |
| 11 | Initial Local Character gap 1,081.29; refined 1,117.75. Official Remote gap is a distinct 3,795.658 maximum across providers. |
| 12 | Architecture tables retain content, Engine, relevance, planning, selection, incremental preparation, commit, Character and Remote phase distributions. |
| 13 | Recurring synchronous 3E relevance p99 74.71 Local, plus rigid physics p99 20.77. Admission fan-out adds selection and a 160.57 incremental-preparation burst. |
| 14 | Differential real-client rigid step mean/p99/max 12.66/21.76/24.77. Official Player apply p99/max 13.583/37.816 Local, decode max 3.398. |
| 15 | Differential resident rigid physics dominates repeated client Engine cost; official structural apply dominates measured receive CPU, but neither explains a 3.8-second CPU stall. |
| 16 | Official RPC handlers run promptly; completion is delayed after handling alongside a large accepted reliable structural backlog on the shared default GNS lane. |
| 17 | Multiple mechanisms across fixtures: synchronous CPU/fan-out in the simulated-transport differential; queue ordering/reliable-delivery contention in the official path. Backend pacing versus retransmission is not separately timed. |
| 18 | No server work-isolation correction in this checkpoint; internal bounded diagnostic capture added. |
| 19 | No new client staging/work-isolation correction. Prior 3L.2 native-preflight and journal/render fixes are retained. |
| 20 | Legal-unit limit unchanged. |
| 21 | 512 objects. |
| 22 | 1,048,576 encoded bytes. |
| 23 | Existing compatibility ceiling only. No tested smaller size passes all health gates; no empirically safe replacement limit is asserted. |
| 24 | Existing ContentAvailability admission budgets and 3J pending/selection limits remain. No new full-chain per-tick fan-out/journal CPU bound. |
| 25 | No newly staged client-operation queue; existing protocol/Poll/snapshot/semantic limits remain, not a new wall-time budget. |
| 26 | No new client staged-byte allowance. Existing bounded messages/queues remain; no new aggregate client-pipeline proof. |
| 27 | Existing critical bootstrap ordering remains. Accepted reliable backlog can still delay critical service; no newly guaranteed latency priority. |
| 28 | Existing generation/identity/session/materialization checks remain. No new staging cancellation mechanism is introduced. |
| 29 | 3E remains sole relevance authority. |
| 30 | 3J remains sole structural scheduling authority; diagnostics do not bypass acceptance. |
| 31 | Selected global cap remains 8,192; observed selected/committed load transitions both 104,840 Local/Node; journal failures zero. |
| 32 | Not sufficiently bounded: all-peer due work, selection construction and journal preparation still monopolize parts of a tick. |
| 33 | Retained 3L.2 real-TLS 32-peer structural load/evict/reload convergence 14/2/3 ticks; not freshly rerun here. |
| 34 | Retained real-TLS 100-peer convergence 8/7/8 ticks; not a current health pass. |
| 35 | Retained real-TLS 500-peer structural convergence 35/32/33 ticks. No new healthy 500-connected proof. |
| 36 | Historical 500-peer shared acquisition count one, including cached reload. |
| 37 | Historical 500-peer initial admission one, second lifetime admission after reload. |
| 38 | Dense 500-Character baseline is already unhealthy; historical baseline RPC p99 reaches 1,607.707 with action pressure. Do not blame all of this on streaming. |
| 39 | Healthy selected shape is the 200-connected / 50-active workload in item 7. |
| 40 | Its no-stream and provider pre-stream phases satisfy explicit health gates; it isolates incremental streaming cost. |
| 41 | No-stream Character recipient wall throughput 8,216.18 states/s. |
| 42 | Local 2,742.93 states/s. |
| 43 | Node 2,724.38 states/s. |
| 44 | Local -66.62%. |
| 45 | Node -66.84%. |
| 46 | No-stream maximum Character load gap 208.430. |
| 47 | Local load 1,117.750; largest refined Local phase gap remains load. |
| 48 | Node load 1,075.960; largest refined Node phase gap remains load. |
| 49 | No-stream tick p99/max 15.8869/32.830. |
| 50 | Local load p99/max 104.447/297.256; reload max 321.836. |
| 51 | Node load p99/max 96.7068/276.047; reload max 283.654, eviction p99 102.209. |
| 52 | No-stream Event ACK p50/p95/p99/max 50.164/58.000/59.107/76.909; 396 measured load samples. |
| 53 | Local Event ACK 162.515/201.816/453.081/570.001; not healthy. |
| 54 | Node Event ACK 164.088/199.518/423.131/549.947; not healthy. |
| 55 | No-stream RPC p50/p95/p99/max 49.514/51.223/51.668/52.592. |
| 56 | Local RPC 174.313/199.592/452.913/497.539. |
| 57 | Node RPC 175.043/210.985/420.468/454.213. |
| 58 | Zero RPC timeouts in completed differential phases and official 100-call cases. |
| 59 | Zero RPC crashes/access violations in these completed cases. |
| 60 | No-stream action p50/p95/max 50.543/51.109/52.979, zero submission failures. |
| 61 | Local action 177.961/194.244/199.178, zero submission failures but unhealthy latency. |
| 62 | Node action 177.208/194.920/207.846, zero submission failures but unhealthy latency. |
| 63 | No-stream root-motion requests/commits 1,930/1,930; 33,033 received root GCHR states. |
| 64 | Local 2,060/2,060; 32,968 root GCHR states; service gaps unhealthy. |
| 65 | Node 2,040/2,040; 32,968 root GCHR states; service gaps unhealthy. |
| 66 | Eleven cell crossings and minimum Y 2.495 per load; targeted spatial/relevance/Character tests pass. |
| 67 | Official near-byte-max headless Player frame p99/max Local 18.224/43.895, Node 18.229/43.172. Not GPU or all-legal-input proof. |
| 68 | Official Character gap Local 3,620.652 / Node 3,640.422. |
| 69 | Official Remote gap Local 3,785.027 / Node 3,795.658. |
| 70 | Official event-loop max Local 57.984 / Node 57.292; no >250-ms stall observed in these cases. |
| 71 | Local/Node differential and official near-max evict converge. |
| 72 | Reload converges with a fresh lifetime; cached content acquisition remains one. |
| 73 | Normal official cleanup completes; new pressure-timed disconnect during every mixed pending stage is not established. |
| 74 | New combined overload maximum tick not measured. Ordinary Local reload already reaches 321.836. Historical 64-unit overload is separate evidence. |
| 75 | No new overload Character health pass; ordinary healthy-baseline streaming fails. |
| 76 | No new overload Remote health pass; official ordinary near-max service gaps remain multi-second. |
| 77 | Historical Server-only overload drains in 70 ticks; no new non-starving combined Player backlog-drain result. |
| 78 | Local residual RSS remains unresolved as a long-run classification; current eight-cycle sampling is too short. |
| 79 | Current Local/Node RSS peaks 69,369,856/72,818,688 bytes; no fresh long Windows plateau proof. Retained 3L.2 1,000-cycle profiles remain historical. |
| 80 | No fresh long Linux RSS plateau run. Retained Local/Node 10,000-cycle small-entry evidence remains historical. |
| 81 | Current 512-object differential decoded high-water 1,623,360 bytes; existing retained decoded ceiling 16 MiB unchanged. |
| 82 | No fresh combined pipeline high-water accounting. Do not sum inclusive RSS, caches and logical byte charges into a fictitious disjoint total. |
| 83 | Eight official churn cycles per provider in this checkpoint; historical broader lifecycles retained. |
| 84 | Those eight cycles use the same Server process; 100 RPCs per official provider. Separately, current Local and TLS Node 100-iteration native exception-cleanup suites pass. Not the full requested 100-cycle mixed pressure soak. |
| 85 | No new staging mechanism. Existing generation-safe focused tests pass; all requested mixed pending-stale-stage schedules remain incomplete. |
| 86 | No stale ObjectId observed in the completed focused/reload cases. |
| 87 | No stale projection observed in the completed focused/spatial/reload cases. |
| 88 | Official cleanup and current Local/TLS Node 100-iteration exception cleanup pass; no stale providers observed. No new full mixed pressure proof. |
| 89 | Same-port restart evidence retained from 3L.2; no newly timed pressure restart in this checkpoint. |
| 90 | Disconnect under pressure not newly timed. |
| 91 | Server Stop under pressure not newly timed. |
| 92 | Original UAF fixes retained; current targeted GameSession/foundation, Local/TLS 100-iteration exception cleanup, and 100-call/official cases pass without an access violation. |
| 93 | Current Clang 19 ASan: all five targeted tests clean, exit 0. Prior supported full coverage remains historical. |
| 94 | Current UBSan: the same five targeted tests clean with halt-on-error and no recovery. |
| 95 | Current LSan: the same five targeted tests clean with `detect_leaks=1`; not a fresh full-suite/GNS sanitizer pass. |
| 96 | Current Windows Node `go test -race ... ./...` passes, exit 0 in 61.56 s. Optional native integration cases are not implied by this default package suite; explicit native Local/TLS cleanup is separately green. |
| 97 | Current Windows targeted CTest 18/18 pass, not full Windows CI. |
| 98 | Current Linux targeted CTest 5/5 pass in 63.44 s under ASan/UBSan/LSan, GNS disabled. Not full Linux CI. |
| 99 | Official Local and TLS Node-to-Server-to-Player functional pass; latency fail. |
| 100 | Final security inventory/review incomplete: starter returned target-changed error and no scan identity. |
| 101 | Astro docs build passes, 19 pages in 3.57 s with bundled Node 24; existing missing-404-entry warning remains. New developer Markdown links and 110/42 ledger numbering checked separately. |
| 102 | No Pages publication triggered for this uncommitted checkpoint. |
| 103 | No new Native CI result; local worker tests are not remote CI. |
| 104 | No new Node CI result; worker race/functional tests are not remote CI. |
| 105 | No Engine commit published. |
| 106 | No Node commit published. |
| 107 | Dirty local trees preserved; scoped worker copies verified; logs/metrics only copied back. |
| 108 | B — FOUNDATION 3L REMAINS PARTIALLY READY. |
| 109 | 3M may not begin. |
| 110 | Correct the measured 3E/3J bounded-work seam and reliable critical-service interference directly, with independent rigid-backend profiling. No automatic 3L.4. Rerun affected acceptance/security/publication gates before changing B. |

## Direct answers: 42 questions

1. The 3.8-second official delay is predominantly after prompt server handling,
   alongside accepted reliable structural backlog on the common GNS lane while
   the Player keeps polling. Exact packet pacing/retransmission shares remain
   unmeasured; see the timestamp correlation, not an invented exact RPC trace.
2. Repeated all-peer 3E relevance plus resident rigid physics dominate recurring
   server p99. Structural selection and incremental journal preparation cause
   the larger admission/catch-up outliers. Content SetParent alone is not the
   ~98-ms repeated tick cause.
3. CPU/service cadence in the differential, reliable queue/transport ordering
   in the official path; zero scheduler rejections does not exclude downstream
   starvation. No claim that Character simulation itself is the dominant cost.
4. Work before, within and after structural selection matters. The 8,192 output
   cap is obeyed but does not bound relevance, pending discovery, journal work
   or bytes already accepted by transport.
5. No. The all-peer due pass remains too expensive and insufficiently spread.
6. No. Existing count ceilings do not bound the entire pending-creation path's
   time/work per tick.
7. Structural activity accompanies delayed GCHR service; exact per-GCHR queue
   versus materialization-prerequisite delay is not separately instrumented.
8. Yes, the official evidence places large RPC completion delay after handler
   execution alongside accepted structural backlog.
9. Existing admission/eviction and selected/pending count limits remain. No new
   complete per-tick content-induced work-isolation guarantee was implemented.
10. Existing protocol/Poll and semantic limits remain. No new client staged
    operation/byte budget or frame-time guarantee was implemented.
11. No: 512 objects / 1 MiB.
12. The smaller-unit sweep found no size passing all health gates, so lowering
    the limit is not presented as a proven correction.
13. Not observed in the new official headless near-max cases: event gaps stay
    below 58 ms. The all-legal-input and graphical guarantees remain unproved.
14. Yes: Character and Remote service gaps still exceed one second, reaching
    approximately 3.64 and 3.80 seconds officially.
15. Indefinite delay is not proved by finite runs, but a required bounded
    critical-lifecycle/service guarantee is absent. LocalPlayer appears promptly
    in these traces; that does not close every overload/bootstrap schedule.
16. No new unbounded staging queue was added; existing queues have count/byte
    ceilings. Aggregate latency and full pipeline retention remain open gates.
17. No such stale application was observed in exercised identity/epoch/reload
    checks. New staged-work mixed schedules are not comprehensively proven.
18. Existing generation checks remain and focused cancellation/reload tests
    pass. No new server staging implementation exists to claim validated.
19. Yes, 3E remains the relevance authority.
20. Yes, 3J remains the structural scheduling authority.
21. Yes in retained 500-peer evidence: one shared acquisition; not newly rerun.
22. Yes, dense 500 active Characters exceed the established healthy baseline.
23. 200 connected, 50 active, five Characters per neighborhood, staggered 12-Hz
    owner inputs, ten root-motion tracks and actual client gameplay/Remotes.
24. Yes, all no-stream phases and provider pre-stream phases pass health gates.
25. Refined load Character wall throughput falls 66.62% Local and 66.84% Node.
26. Tick p99 rises 88.560 ms Local and 80.820 ms Node against equivalent control.
27. There is no new fix. Refined differential maximum is 1,117.750 ms; official
    Character maximum is 3,640.422 ms across providers.
28. There is no new fix. Official Remote maximum is 3,795.658 ms.
29. No in latency: submissions succeed but streaming action-result latency
    reaches about 199/208 ms Local/Node in load.
30. No in wall-time publication. Produced/committed root motion agrees and
    spatial crossings are correct, but recipient publication has >1-second gaps.
31. No in latency. Differential Event ACK maxima are 570/550 ms in load;
    official Remote service remains delayed by seconds.
32. Yes in completed 100-call-per-phase and official Local/Node runs: no crash,
    timeout or RPC error. This is not an arbitrary-load safety guarantee.
33. Not established. Current short eight-cycle windows cannot classify residual
    Local drift; retained longer RSS evidence is not relabeled perfectly flat.
34. Yes, the retained decoded-document budget is still 16 MiB; current load
    high-water is 1,623,360 bytes, not all transient allocation.
35. Existing component limits remain; no fresh complete aggregate pipeline
    accounting/non-starvation proof is claimed.
36. Not proven, and ordinary feasible streaming already fails service health.
37. Convergence occurs, but the measured selection/journal bursts still
    monopolize parts of a tick; non-starving backlog drain is not proven.
38. No stale work observed in completed focused and ordinary churn cases.
    The entire mixed pending-fan-out/RPC-active lifecycle matrix remains open.
39. No. The final scan failed to obtain an authoritative identity/inventory.
40. Yes for the current five targeted Linux tests under ASan/UBSan/LSan.
    Historical full coverage is retained separately; no fresh full-suite or
    Linux GNS sanitizer result is implied.
41. No. Functional convergence and attribution do not close non-starvation.
42. No. Foundation 3M remains gated.

## Historical instrumentation-only terminal verification

This section is updated only from completed command results, never inferred
from an empty error log or a still-running build.

- Node race: exit 0, 61.56 s; `evidence/node-race.log`.
- Linux ASan/UBSan/LSan: exit 0, 5/5 tests, 63.44 s;
  `evidence/targeted-linux-sanitizers.log`. The task-owned `--rm` container
  exited and was removed normally; build/dependency caches were retained.
- Local exception cleanup: exit 0, `CONTENT_UNWIND_SUITE_OK iterations=100`;
  TLS Node exception cleanup: exit 0, Go test 4.23 s with the same success marker.
- Documentation: Astro exit 0, 19 pages in 3.57 s. It retains the existing
  missing `docs -> 404` entry warning. The new developer documents are Markdown
  outside the Astro content collection: their relative links and exactly
  110 report rows / 42 direct answers were verified separately.
- Canonical local source hashes rechecked: 38 Engine and two Node files match
  the tested manifest; final worker hashes also match all 40 files with zero
  mismatches. Engine and Node `git diff --check` pass. No task build/test process
  remains, and all six pre-existing unrelated worker containers remain running.

B — FOUNDATION 3L REMAINS PARTIALLY READY
