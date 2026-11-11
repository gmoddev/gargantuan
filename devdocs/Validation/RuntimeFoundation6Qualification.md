# Runtime Foundation 6 qualification

Status: core implementation measured and integrated; broader qualification and
separate optimization experiments remain in progress. This isolated continuation
starts from qualified Foundation 5 head
`da902ea481e627d7c4d789033076b3be7dc38334`, stacked on `foundation/renderer-5`.
Main remains `5b6782e35c4e24fd80bc8d5526104c615c72d84d` at reconciliation.
PR 21 and PR 22 remain separate, open draft foundations. No Node, Studio or LUI
source belongs to this campaign.

## Baseline and comparison contract

The baseline checkpoint `2f4ef8d90126c24a236d52476c769dd199ae99a8` changes
benchmark fixtures and CMake only and preserves the Foundation 5 runtime.
Candidate runtime changes are measured against identical fixtures. Existing Foundation 5 raw evidence is
retained in `C:/Users/aiden/.codex/artifacts/renderer-foundation5-20261111`.
New evidence belongs to sibling `runtime-foundation6-20261111`.

`gargantuan_runtime_foundation6_benchmark` extends the existing semantic fixture.
`--count=N --density-matrix` constructs one world, then measures 0%, 0.1%, 1%,
10%, 50% and 100% changes without reconstructing that world between densities.
`--split-construction --diagnostic-marks` separates world setup, group setup,
Part allocation, detached transform initialization, adoption, initial publication,
measurement, semantic destruction and final object/resource release.
Every measured frame checks the exact journal sequence delta, projected update
count, no create/remove/resync, and retained object count. Ordinary native body
admission is the default. The explicitly tagged renderer-only mode remains a
controlled candidate-list fixture, not a physics admission test.

Examples, each with a fresh raw sample path:

```text
gargantuan_runtime_foundation6_benchmark 12 --count=10000 --density-matrix --group-width=256 --split-construction --diagnostic-marks --samples=<fresh.csv>
gargantuan_runtime_foundation6_benchmark 12 --count=10000 --density-matrix --property=material --group-width=256 --split-construction --diagnostic-marks --samples=<fresh.csv>
```

`--property=mixed` measures both transform and material changes. `--writes=N`
performs distinct repeated writes while publication coalesces final object state;
the journal must still contain every required ordinary mutation. Raw frame rows
and invocation receipts preserve count, writes, property mode, density, native
physics state, group layout and profiling state. Profile clocks are attribution,
not a performance candidate. A separate allocation binary counts calling-thread
C++ new requests; C allocations, driver allocations and other threads are outside
that metric. Full process memory is a separate observation.

## Competing experiments

| Workstream | Candidate / gate | Present state |
| --- | --- | --- |
| Ordinary mutation/lifecycle | Retained journal append/rollback, unobserved signal allocation; preserve nested/destructive observers and exact journal/history | Integrated core; focused Windows regression and comparison pass |
| Explicit batching | Existing prepared-property contract; atomic observer visibility differs from ordinary setters and remains explicit | Semantic and allocation-rollback gates pass; no general throughput advantage demonstrated |
| Dirty distribution | Single-object journal batch unions renderer-final-state domains; reliable records remain uncoalesced | Integrated core; capture/ack parity gates pass |
| Typed extraction | Reuse validated identity/ancestry; exact-model inverse reuse; full final fields retained | Integrated core; Windows dense publication improves |
| Storage alternatives | Direct Instance extraction versus synchronized generation-safe SoA versus immutable per-object snapshots | Measured component alternatives; synchronization costs exceed one-consumer benefit |
| Physics synchronization | Constant-time admission membership and scoped physics-authored callback suppression | Integrated core; focused lifecycle/reentrant callback gates pass |
| Compact physics view | Derived dynamic readback and ordered pending controls with conservative full-scan fallback | Separate experiment; sparse/dense/churn comparison pending |
| Replication / F1 | Existing 3I typed catalog and 3J accepted per-peer history remain owners | Source review; no network behavior change justified yet |
| Typed CFrame setter | Validated property/identity handoff and direct wire encoding | Separate experiment; small setter improvement, no broad stage or allocation win |
| Signal dispatch / dirty callbacks | One-subscriber lifetime-preserving dispatch; per-registration dirty coalescing | Separate candidates undergoing independent comparisons and fault gates |

The alternatives probe includes actual authoritative setters, dirty capture,
cache synchronization and consumer work. It verifies exact dirty identities,
incremental retained-state parity, stale generation retirement and full rebuild.
It does not qualify physics, networking, asset-backed MeshParts or production
capacity from primitive extraction alone.

## Ownership and admission

Instance/DataModel remain authority on Main. Ordinary callbacks remain
synchronous. Render-final-state coalescing is distinct from reliable journal
delivery and explicit prepared authoring batches. No alternative becomes an
authoritative writer or exposes native storage to Luau.

The shared worker was occupied by `LuiSdkRecovery20261111` during initial source
work. No compilation or benchmark was admitted under that lease. Component
builds may reuse this campaign's validated Foundation 5 source/build caches only
after a fresh cooperative lease and explicit ownership handoff; frozen Foundation
5 packages remain immutable. Full networking/F1 and cross-platform qualification
use the existing canonical suites and acceptance contracts, without new targets
or exemptions. No source candidate is qualified by this document alone.

## Measured core checkpoint

Windows comparison [43056868901](https://github.com/gmoddev/gargantuan/actions/runs/43056868901)
compares baseline `2f4ef8d90126c24a236d52476c769dd199ae99a8` with
`fbd0ae9026a43f6a5e263c98e1aaf3953dd2318e` on the same hosted Windows machine.
The integrated core at `6c9145c74fe77fad31d6c3c4c5128999b749def7` has identical
`include/`, `src/` and `tools/classgen/` contents to that candidate; its additional
fixture correction does not alter the runtime. Full canonical qualification of
the integrated checkpoint remains a separate gate.

Host: AMD64 Family 25 Model 1, four logical processors, Windows 10.0.26100,
MSVC 19.51, Release, PCH off, NoGNS. Both exact sources build and pass the
foundation, prepared-property, publication, physics and fixture gates before
frozen binaries are measured. All 66 measured processes exit zero. Each reported
ordinary phase pools 24 raw frames per arm across two processes in A/B/B/A order.
Profile clocks and allocation counting use separate executions. Process receipts,
EXE/DLL hashes, compiler output, raw CSV, stdout/stderr and source manifests are in
`C:/Users/aiden/.codex/artifacts/runtime-foundation6-20261111/HostedWindowsCore-43056868901`.

| Workload | Baseline mean ms | Core mean ms | Interpretation |
| --- | ---: | ---: | --- |
| 50K native Parts, all changed: setters | 98.797 | 103.150 | 4.4% regression remains under investigation |
| Same: immutable publication | 84.203 | 53.352 | 36.6% lower |
| Same: projection apply | 9.806 | 9.529 | Small change |
| Same: sum of these component stages | 192.806 | 166.031 | 13.9% lower; not whole-engine or GPU frame time |
| 50K native Parts, 500 changed: stage sum | 1.775 | 1.472 | 17.1% lower |
| 1K native Parts, all changed: stage sum | 2.221 | 1.690 | Lower in this fixture |
| 10K native Parts, all changed: stage sum | 36.742 | 29.907 | Lower in this fixture |
| 25K native Parts, all changed: stage sum | 96.257 | 80.752 | Lower in this fixture |
| 50K initial adoption, excluding detached transform initialization | 56,146 | 1,326 | Combined journal/admission change; not isolated attribution |

Part allocation is about 80 ms in both arms. Initial full publication is about
69 versus 71 ms; unchanged-state inverse reuse does not optimize every full
build. Peak process working set is 720,426 versus 687,620 KiB. Dense 10K
calling-thread C++ new counts and requested bytes do not decrease. Neither the
working-set observation nor the combined lifecycle result identifies a single
optimization as its sole cause. Zero-change times near 0.003 ms are noise-scale.

The separate same-source attribution replay
[43062292991](https://github.com/gmoddev/gargantuan/actions/runs/43062292991)
reduces 50K final-state extraction from 63.841 to 36.917 ms on its host;
publication is 87.257 to 58.511 ms and setter/dirty time is 107.184 to 110.267 ms.
Inclusive diagnostic timers are not additive predictions for an unprofiled run.

## Alternative architecture evidence

At 10K Parts, one sequential consumer costs 2.849 ms with direct Instance
extraction, 2.889 ms with synchronized selective SoA, and 5.093 ms with
immutable per-object snapshots. Random access costs 3.275, 3.319 and 6.225 ms.
The SoA logical payload is 995,316 bytes and snapshot values are 1.6 MB, excluding
container/allocator overhead. These are bounded component results including
authority synchronization, not a universal rejection of derived storage or
immutable renderer publications. An authoritative ECS replacement is unsupported.

Existing explicit prepared mutation preserves its separate atomic observer
contract. A warmed 1K comparison costs about 1.5 ms and 860 KiB versus ordinary
mutation around 0.85 ms and 224 KiB. It produces four actions/revisions versus
1K ordinary changes while preserving required records. That semantic difference
prevents treating it as a transparent faster ordinary-setter replacement.

The isolated typed CFrame comparison
[43048909412](https://github.com/gmoddev/gargantuan/actions/runs/43048909412)
uses instrumentation baseline `2d7806492734090f58211ccf049e3df6350d7b82` and
typed candidate `795c2d28c2252c54a148e846d2ed54b1e5c80794`.
It removes the measured second lookup and reduces encoding time, but dense 50K
setter time changes only 101.234 to 98.881 ms and component stage sum changes
176.148 to 177.157 ms. The four setter observations drift from baseline 105.795,
candidate 100.335, candidate 97.427 to baseline 96.673 ms. Sparse 50K stage sums
regress 1.9–4.8%. The allocation replay
[43059877450](https://github.com/gmoddev/gargantuan/actions/runs/43059877450)
reports identical dense 50K counts in both arms: about 100,016 C++ new calls and
36,235,398 requested bytes per frame. This candidate remains experimental.

## Realistic and full-engine evidence

Gameplay comparison [43057067667](https://github.com/gmoddev/gargantuan/actions/runs/43057067667)
uses identical 512-object content and 18 sample-asset hashes, eight active
characters and 32 simulated peers (one full client, 31 observer transports).
All eight processes and 40 phases report healthy service and phase gates;
root-motion commits, eviction/reload and eventual backlog drain remain correct.
It is a real GameSession simulation, not 32 physical network clients.
Trace-off frame-work P95 during load is 3.424/2.562/2.556/3.155 ms in A/B/B/A
order; reload is 3.163/2.271/2.248/2.930 ms. These support a directional gain,
not precise mutation attribution. Load maxima remain 46–119 ms, reload maxima
50–114 ms, and load convergence P95 264–321 ms. Healthy service does not mean
every frame meets 16.667 ms. Tick cadence changes journal volume; the input
fixture is identical but its dynamic mutation stream is not byte-identical.

Corrected realistic comparison
[43065693718](https://github.com/gmoddev/gargantuan/actions/runs/43065693718)
passes at fixture-only baseline `8831e09f39ecd103585342eab0193570737614f2` versus
core candidate `9be409347e971a88d338ddb41446b781ea8d9358`.
It includes canonical MeshPart reimport/resource parity, hierarchy changes,
actual ReplicaApplier updates, animation, soft-body and replication fixtures.
Independent review verifies the ZIP digest, extraction bounds and all 64
stdout/stderr hashes. All 32 processes exit zero. For 512 MeshParts with 64
changed, steady publication mean is 0.1266 to 0.0871 ms; receiver publication
after actual ReplicaApplier mutation is 0.0982 to 0.0614 ms. Receiver apply
remains 16.928 to 16.016 ms (P95 19.648 to 16.426 ms), a much larger cost than
publication. Reimport of 512 dependents produces exactly 512 updates, one mesh
create/remove and no full resync. Its publication A/B/B/A observations are
0.8344/0.5706/0.5844/0.7815 ms. Hierarchy and receiver generation-replacement
gates pass. The 128/0 quiet case is near measurement noise.

The shared-asset 1K MeshPart adoption observations are
629.463/30.684/30.785/620.692 ms; asset full resync shows no gain. The 100-rig
animation total is about 1.281 to 1.245 ms, 500-root-motion total stays around
3.6 ms, and 1,024-vertex cloth has no meaningful delta or stale/drop events.
The 8,193-object replication preflight remains about 109–113 ms. These measured
limits do not justify a new replication authority or global dirty bus.

## Rejected signal-container optimization

The one-subscriber fast path at `1385d1ec3abdb2c5d0ad0f517959d7b7c06a1027`
is compared with common profiling baseline
`a0f33623688b63cc3810c215662cd63f4b0577c7` in
[43065688927](https://github.com/gmoddev/gargantuan/actions/runs/43065688927).
Both 10-test regression sets and all 36 processes pass. It preserves owning
connection lifetime, recursion, deferral and cleanup ordering, but does not
remove the required shared ownership copy or callback. Unprofiled steady dense
10K stage sums worsen 27.128 to 27.577 ms; 50K worsens 160.605 to 161.819 ms.
Profiled 50K native signal time changes 38.674 to 38.310 ms, while renderer-only
time worsens 37.195 to 37.701 ms. Counts match and the snapshot timer remains
essentially unchanged. This does not support promotion; the source and verified
artifact remain isolated. Per-registration physics dirty coalescing is a
different candidate and requires its own regression/fault and performance gates.

## Qualification boundaries and unsuccessful evidence

Canonical baseline [43054671698](https://github.com/gmoddev/gargantuan/actions/runs/43054671698)
passes the complete Windows NativeCI job, including existing GNS/F1 Release
workloads. It qualifies baseline runtime, not the new integrated runtime.
Linux Release/ASan/UBSan and GNS sanitizer jobs fail during dependency install:
Ubuntu mirror DNS prevents fetching required Clang 19 and shader tooling before
engine compilation. The runner's preinstalled Clang 16–18 cannot replace that
canonical contract. `HostedLinuxInfrastructureFailure.md` preserves the logs
and exact image identity; no tests are marked passed from those attempts.

Earlier fixture failures are retained: a missing stream operator, an incorrect
DataModel service call, a prematurely released test DataModel, invalid zero-change
probe arguments, and a colliding compact-view lifecycle fixture were corrected
before reruns. The compact-view timed comparison still fails independently of
its passing unit/fault gates and remains experimental. Neither a failed run nor
its frozen artifact is discarded. Physical GPU, full integrated Windows and
Linux/sanitizer acceptance remain outstanding. Production physics algorithms,
transport wire formats, F1/D03 gates and renderer defaults are unchanged.
