# Foundation 3M qualification

This is a pinned historical qualification receipt for PR #13. The subsequent
[streaming readiness integration](../CurrentArchitecture/TrustedResidencyStreamingReadiness3M.md)
changes official startup to exact shared-snapshot ownership and adds optional
diagnostics. The binding and construction measurements below describe the pinned
source, not a new measurement or qualification of that integration.

## Source and scope

Measured implementation source: `1cacc5ac200d209ef4a2c126e98127876b4273dd`, based on
completed 3L merge `81fca069c`. [Draft PR 13](https://github.com/gmoddev/gargantuan/pull/13)
owns the isolated 3M change. The original working checkout and concurrent
Asset 2B / Geometry 1A branches were not modified or merged.

This qualifies trusted default-space demand policy separately from 3L core
acceptance. D03 `POOLED_CORRECTNESS_V1`, KI-006 core closure, exact native/ACK
ownership, finite accounting and historical receipts remain unchanged. Physical
and provider certification remain deferred. No historical Local32 or real-TLS
Node32 physical certification campaign was restarted.

## Worker correctness

The `dockerbox` Windows worker built MSVC 19.51.36260 x64 Release with Ninja,
two compile jobs, GNS/Node disabled and precompiled headers disabled. A stable
campaign source/build namespace retained incremental dependency outputs. The
private FetchContent cache is
`C:\Sandbox\Codex\Cache\foundation-3m-trusted-residency`; existing vendor trees
were copied into the isolated source namespace and shared workloads were preserved.
The official Server host and all selected test executables linked successfully.
All 14 changed source/configuration/test overlay files were SHA-256 compared
against the local source before the preceding run. The final incremental overlay
recompiled the policy and both changed tests, then relinked all selected consumers.

The final focused CTest run passed **9/9 in 31.58 seconds**:

| Coverage | Result |
| --- | --- |
| Existing ContentAvailability lifecycle/security | PASS |
| Paged coarse query, cursor identity, aggregate membership ceiling | PASS |
| Policy ordering, deadlines, boundaries, lookahead, hysteresis, denial | PASS |
| Dense movement, settled best candidates, same-tick work limits, mixed-cost focus fairness | PASS |
| Local provider and production GameSession integration | PASS |
| Policy benchmark smoke | PASS |
| Existing GameSession, relevance, Character and Remote suites | PASS |

The real local-package fixture verifies one owned request across 500 equivalent
focuses, dependency-complete admission of ordinary Part units, projection into
the existing 3K/3H spatial path, policy-only release, another native
owner's retention, hysteresis grace, eviction and fresh ObjectIds, corrupt
provider rejection and explicit retry, stale pending demand withholding, exact
manifest/dependency replacement denial, and asynchronous preparation/direct-stop
lifetime safety. A backlog larger than the dispatch search window verifies
actual accepted request counts, deadline order and interleaved release progress.

The accepted GameSession fixture exercises the actual server focus feed and
controller velocity corridor with a normally materialized Character/RootPart.
With 500 continuously updated native focuses it completes **24/24 reliable
Remote round trips**, accepts **3 Character publications**, and retains its one
ready peer. Maximum observed frame cost is **556.2 microseconds** in the final
focused run. These are bounded progress observations in a short shared-process
fixture; they do not establish physical service, 500 connected clients, or an
unconditional wall-clock guarantee.

## Performance qualification

The benchmark includes all 1/10/100/500-focus by 1K/10K/65,536-unit combinations,
construction and hot allocation attribution, a simple linear geometry reference,
moving dense query and teleport cancellation, hysteresis cleanup, and live 3L
integration at three manifest scales. Construction, the first binding step,
recurring policy cost and existing 3L service cost are reported separately.
The final quiet-worker run exited zero in **6.484 seconds**, using **4.859 CPU
seconds**, with zero native compilers at start and an observed process peak
working set of **257,277,952 bytes**. Windows jobs were coordinated with the
concurrent asset/geometry campaign and the separate Studio replay window.

All 12 focus/content combinations completed 128 warmup ticks and 64 samples per
hot phase. Each sample enforces cell, membership, revalidation, eligibility and
delta ceilings. All cleanup cases retire every candidate, demand and pending
delta. The dense moving fixture completes four moving sweeps; teleportation
invalidates its prior cursor. The CSV has 27 columns and 80 data rows.

| Content units / focuses | One-focus churn P99 (us) | All focuses moving P50 / P95 / P99 (us) | Warm retained allocation bytes |
| --- | ---: | ---: | ---: |
| 1,000 / 500 | 35.6 | 433.9 / 487.0 / 501.0 | 1,830,143 |
| 10,000 / 500 | 42.6 | 489.2 / 513.0 / 541.4 | 4,746,455 |
| 65,536 / 500 | 49.0 | 602.7 / 659.4 / 781.6 | 22,066,871 |

`policy_steady` times Step alone. `policy_churn` times one native focus update
and Step, with fixture position calculation outside the timer.
`policy_all_moving` includes the fixture position driver, all native focus
updates and Step. Its maximum at 65,536/500 is 900.1 us. These are aggregate
tick samples under bounded scheduling, not a promise that all 500 pending
focus evaluations complete within one tick. The dense 1,000-unit single-focus
cursor fixture has P99 905.9 us and max 1,163.0 us.

`policy_state` is the allocation census through construction, focus insertion
and warmup; its elapsed time includes harness validation and is not setup-only
latency. Its retained bytes include policy-owned manifest/index/focus/demand
allocations, excluding the caller's input manifest, allocator overhead and
measurement headers. Later phase byte counts describe allocations created in
that phase, not the policy's entire lifetime footprint. The 65,536/500 warm
census is approximately 21.04 MiB. Whole-process working set also includes the
harness, provider fixtures and allocator behavior.

The live-service fixture uses static overlapping focuses, eight native
requirements and existing 3L. It requires exactly eight accepted requests,
acquisitions and admissions at each scale. This is a separate workload from
the null-service all-moving matrix:

| Content units / focuses | First policy binding (us) | Policy hot P50 / P95 / P99 (us) | Existing 3L hot P50 / P95 / P99 (us) |
| --- | ---: | ---: | ---: |
| 1,000 / 10 | 180.6 | 1.0 / 1.1 / 1.3 | 17.0 / 86.5 / 91.3 |
| 10,000 / 100 | 776.6 | 1.8 / 93.2 / 99.2 | 160.4 / 207.1 / 225.9 |
| 65,536 / 500 | 7,435.1 | 106.3 / 123.9 / 157.1 | 1,056.9 / 1,201.6 / 1,219.8 |

The live hot distribution has 31 samples and reflects bounded warmup/progress,
not a long-duration steady-load capacity certificate. At the largest scale,
policy construction is 112.035 ms off Main; manifest encoding is 1.054 seconds
and provider readiness is 1.553 seconds. The first binding comparison remains
a visible one-time Main cost. No existing 3L service scan is attributed to
the spatial evaluator. A simple linear geometry reference is included only as
a geometry cost comparison; it does not implement policy ranking or lifecycle.

## Review and known limits

Independent policy, spatial, correctness and performance reviews corrected
continuous-movement cursor starvation, stale pending publication, hidden
candidate/contributor scans, priority-window inversion, release starvation,
same-tick budget reuse, aggregate index amplification, and preparation lifetime
ownership. Final review additionally corrected focus rotation when a remaining
revalidation budget cannot fit a costly focus. Equal-cost and mixed-cost
regressions ensure later dense focuses progress under continuous movement.
Independent source review confirmed that a deferred focus fits a fresh tick's
validated budget. Regression tests cover those observable contracts. No policy path
directly changes 3E relevance or 3J Known/materialization state.

Moving dense sweeps are an explicit bounded approximation, revalidated against
current facts and followed by a settled sweep. The first exact manifest/service
binding comparison remains O(manifest entries) once on Main; recurring spatial
evaluation does not scan the manifest per focus. Existing 3L service scans are
attributed separately. Native requirements have one slot per key, so independent
gameplay producers aggregate upstream. These are documented implementation
limits, not a second content lifecycle or a creator-facing streaming API.

Qualification assumes conservative authored unit bounds. The pre-existing
Size-box approximation for non-normalized MeshParts is tracked as
[KI-3M-001](../../KNOWN_ISSUES.md#ki-3m-001-packaged-mesh-coarse-bounds-need-conservative-geometry-metadata).
The smallest asset/package metadata contract is documented with 3M; collision,
physics and geometry compilation remain with their owning campaign. Arbitrary
mesh streaming is not certified by these fixtures.

## Reproducibility and artifacts

Worker source: `C:\Sandbox\Codex\Workspaces\foundation-3m-trusted-residency`.
Build: `C:\Sandbox\Codex\Builds\foundation-3m-trusted-residency\msvc-release`.
Raw logs/JUnit: matching campaign directories under `Logs` and `Artifacts`.
Local raw evidence: `C:\Users\aiden\.codex\worktrees\foundation-3m-validation`.

Raw scale evidence is committed as [CSV](Foundation3MScale.csv),
[live service measurements](Foundation3MScaleService.txt), and
[process/source receipt](Foundation3MScaleProcess.json). The measured executable
SHA-256 is `F76001491CD517DED17FFD3723475EC4DD891600CE3C8BC5CBC08F612EE01655`.

The initial serializer fixture compile mismatch, PowerShell native-stderr
wrapper stop, publication-versus-acquisition assertion and missing RootPart in
the service probe are retained in preflight logs. They were corrected without
changing 3L or Character/Remote acceptance. The final nine-suite JUnit receipt
contains zero failures.

Hosted evidence for the preceding implementation `5eba1f76c` uses actual merge
checkout `a6e1d0f2210d3b0beec79aa39363a06d6a0b67f5`:
[Linux headless ASan/UBSan](https://github.com/gmoddev/gargantuan/actions/runs/41778537177/job/126404544627)
passed all 64 registered tests, and
[GNS ASan/UBSan/LSan](https://github.com/gmoddev/gargantuan/actions/runs/41778537238/job/126404500772)
passed all 11 tests plus its profiled production admission/pooled-service gates.
Both downloaded receipts retain `POOLED_CORRECTNESS_V1`. These earlier results
do not substitute for the final fairness patch's hosted checks. Final Windows,
Linux and GNS terminal job links and actual checkout provenance are maintained
on PR 13; in-progress checks are not passes.
