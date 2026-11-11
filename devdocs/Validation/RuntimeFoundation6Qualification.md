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
| Compact physics view | Derived dynamic readback and ordered pending controls with conservative full-scan fallback | First representation rejected; selective readback remains isolated after unstable worker timings, with fresh hosted comparisons pending |
| Replication / F1 | Existing 3I typed catalog and 3J accepted per-peer history remain owners | GNS sanitizer transport/admission gates pass at 30fc; consolidated Release/F1 acceptance remains separate |
| Typed CFrame setter | Validated property/identity handoff and direct wire encoding | Separate experiment; small setter improvement, no broad stage or allocation win |
| Signal dispatch / dirty callbacks | One-subscriber lifetime-preserving dispatch; registration flags, epochs and hot/cold tokens | Rejected for promotion: callback savings do not improve the complete drained path |

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

The optional live renderer path in `gargantuan_runtime_foundation6_benchmark`
uses the same authoritative Part setters, journal count, typed publication and
projection delta gates as the ordinary rigid fixture. For a bounded offscreen
SDL run on a machine with an SDL GPU device:

```powershell
.\gargantuan_runtime_foundation6_benchmark.exe 3 --count=1000 --changes=1000 --draw-backend=sdl --draw-instancing=contiguous --samples=runtime6-live.csv
```

`--draw-backend=headless` runs the same path without SDL GPU submission.
The default backend remains `none`. The draw path first submits and drains the
initial full publication. Each measured frame mutates live Parts, publishes,
draws, and waits for idle before the separate test projection apply. Raw CSV
records `DrawAndWaitMs`, `MutationToCompletionMs`, backend, instancing selection,
headless object/update counts or SDL submitted-frame, resync, draw, upload and
instance counters. The completion timing is a host wall clock through an SDL
idle wait, not a GPU timestamp. The SDL gate requires one submitted frame per
publication, actual object draws and no full resync after the initial frame.
An explicit contiguous transform run also requires actual contiguous batches.
The direct Luau closure fixture separately measures reflected property writes
and synchronous script callbacks under ServerRuntime capabilities. Its timed
lua_pcall excludes Script scheduling/resumption and full Engine frame work;
ordered journal/state, retained connections, denied/invalid/read-only mutation
and nested callback gates execute outside or around that bounded measurement.

The historical dirty-drain comparison includes the core, registration cache and
common fixtures at 6f5c63cade57fbcf5f426bd866dae163db8e58c5. The registration
cache was subsequently rejected and removed. The consolidated candidate retains
the measured core and disabled diagnostics, adds lifecycle corrections, and does
not include the experimental selective dynamic readback view. Its broader
qualification must not be inferred from the historical dirty-drain source.

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

## Dirty callback and compact physics experiments

Per-registration dirty-binding comparison
[43068137180](https://github.com/gmoddev/gargantuan/actions/runs/43068137180)
compares the same profiling baseline
a0f33623688b63cc3810c215662cd63f4b0577c7 with candidate
0bbdd1870b340a3645a65867259d76930302fa80. All 36 processes and mandatory
regression/allocation-recovery gates pass. The 50K dense off-clock setter mean
falls 105.013 to 94.850 ms; the four process means are
105.149/94.712/94.987/104.878 ms. Publication plus projection changes 58.904 to
60.678 ms. Dense 10K setters change 19.724 to 17.304 ms. At 50K native bodies,
diagnostic callback time for a new mark falls 18.385 to 9.819 ms and coalesced
marks fall 13.102 to 1.809 ms. Every expected journal and callback count matches.
Peak working set increases about 1–3 MiB; allocations are not measured.

That fixture does not drain physics between frames. Its gain does not establish
a complete simulation-frame result. The separate first-mark comparison
[43072892853](https://github.com/gmoddev/gargantuan/actions/runs/43072892853)
adds StepPhysics(0) outside setter/profile timing, then verifies dirty drain,
backend pose and exact journal delta on every frame. It uses identical fixture
sources 7e66edb874b446b7410f582d99e15fc39e7e5c86 and
6f5c63cade57fbcf5f426bd866dae163db8e58c5. That attempt fails before timing
because lazy AssetService creation appends legitimate journal records inside
the first safe point. Identical prewarm correction moves that setup outside
timing and preserves exact per-frame journal checks.

The corrected [43075408692](https://github.com/gmoddev/gargantuan/actions/runs/43075408692)
passes all 24 processes. At 50K with clocks off, setter plus physics drain
changes 181.292 to 184.511 ms (+1.8%); drain alone changes 76.727 to 82.889 ms.
Removing the reset loop with a registration epoch does not settle the tradeoff:
[43079909984](https://github.com/gmoddev/gargantuan/actions/runs/43079909984)
passes 60 processes but the full-drain path is +6.3% at 10K and +0.8% at 50K.

The next hot/cold variant keeps the weak Part directly in the tracked map and
separates callback tokens, removing an extra dereference during body updates.
[43085470888](https://github.com/gmoddev/gargantuan/actions/runs/43085470888)
compares f85d9f37752dbfa2de9ffe33640894d2ab4c2316 with
cab3aefa90c361f8093a17c203cbc4dfb3541fff. All 60 processes and focused gates
pass, but 50K setter plus drain worsens 177.103 to 183.560 ms (+3.6%), including
drain 73.062 to 80.687 ms (+10.4%). Its profiled repeated, undrained callbacks
improve 10.837 to 2.323 ms; each drained frame instead has 50K new marks.
The complete-path regression rejects promotion of all three registration
variants. Their isolated source and correctness evidence remain preserved;
integration removes the token optimization while retaining conservative
dirty-insertion failure recovery.

The first compact view passes all 72 processes in
[43068853213](https://github.com/gmoddev/gargantuan/actions/runs/43068853213),
sources 19b1ace4014d16435d3cb7166b3c42c16fce0ab4 and
bae94835d45daf404202957694ebad95bd639e52. The 18 cases cover 900 static/100
dynamic and 1K dynamic worlds, zero/sparse/dense controls and 0/1/16 lifecycle
operations per frame, with 30 samples after four warmup frames in A/B/B/A order.
Step time improves in 14 cases but full frame improves in only four; median
candidate/baseline ratios are 0.947 for step, 1.246 for full frame and 1.154 for
frame P95. New per-control ancestry/index work and shifted PartPositions on
removal overwhelm the readback saving. This representation is rejected for
integration, with its source and successful correctness evidence retained.

The next readback-only experiment preserves baseline controls, ordered Part
removal and motion publication, and indexes only accepted dynamic body-state
reads. Independent review caught a real same-step fallback defect: an invalid
derived record set a full-scan flag but returned until the next step.
ca384c6c1e507359fc28cb92eff3414f659b6dae scans immediately and adds a corrupted
derived-body-ID regression checking same-step published velocity and recovery.
[43073019344](https://github.com/gmoddev/gargantuan/actions/runs/43073019344)
qualifies that corrected source against the shared baseline. It remains isolated.

## Qualification boundaries and unsuccessful evidence

### Script lifetime corrections

Windows [43072499672](https://github.com/gmoddev/gargantuan/actions/runs/43072499672)
access-violates after all measured frame rows and semantic validation succeed.
Linux ASan [43077722014](https://github.com/gmoddev/gargantuan/actions/runs/43077722014)
reproduces coroutine use-after-free in all 24 GC/disconnect variants. Source
inspection identifies `Script::Step` rooting stack index 1, a library table,
instead of the new coroutine. The correction roots the actual top-of-stack
thread before loading/resuming and restores the incoming stack height.

The same sanitizer matrix
[43082581701](https://github.com/gmoddev/gargantuan/actions/runs/43082581701)
passes 24/24. Windows diagnostic
[43082867443](https://github.com/gmoddev/gargantuan/actions/runs/43082867443)
passes 96/96; normal Luau/headless
[43082871987](https://github.com/gmoddev/gargantuan/actions/runs/43082871987)
passes 44/44 with exact journal/callback counts. Paired 10K Luau CFrame timings
show no demonstrated mutation speedup. Unrooted timings remain historical.

Source d25a8b3b2e8662d23a20e96317647f6840e91527 also defers cleanup during
active resume and snapshots activation across nested callbacks.
[43085516350](https://github.com/gmoddev/gargantuan/actions/runs/43085516350)
passes Foundation and all 24 stress processes under ASan/UBSan. It does not
contain later Engine-owned VM lifetime/logical-shutdown corrections; those
reachable game-destruction paths require final combined-source qualification.

### Worker lifecycle, sandbox and fixture qualification

Exact worker source `36988c6ac5701868afae9e92080db50a77522f3f` passes all
11 focused Release CTests on MSVC 14.51.36231, PCH off, NoGNS. This includes
Foundation's actual Engine-frame, task, native setter, input and physics-motion
destruction paths; GUI and remote callbacks; prepared mutation/history,
publication, physics, dirty-allocation recovery, pinned content fixture and
package spatial metadata, plus both Luau mutation smokes. The original JUnit,
binary hashes and log are preserved under
`WorkerFocused-369-20261111T214208Z`; log SHA-256 is
`4fe74c5f36d4d2556b760af2a74f91be69cf9e991058c42f143c2e63004fd8d3`.
This is focused Windows qualification, not complete canonical/GNS/GPU acceptance.

Earlier worker sources c63 and 232 each pass 10/11 gates and fail Foundation;
both original receipts remain. New Engine fixtures initially left ProcessService
stopped, so Engine::Step returned without running their Scripts. Startup is now
explicit, the body setup checks the actual accepted anchored body rather than
assuming no built-in character bodies, and logical shutdown is observed during
child retirement rather than after Engine teardown has finished. Assertions and
real callback paths remain mandatory.

The retention test also exposed a production binding defect. LoadIntoState
assigned the shared `script` global before luaL_sandboxthread installed its
private environment. Pinned Luau `3fc82b1071ab387531175869afc4fb528464afa4`
creates a new environment with fallback to the prior globals; child task threads
inherit their caller's environment. Assigning `script` after sandbox creation
prevents cross-Script identity changes and removes the main VM's last-Script
root. Two-Script yielded/deferred identity, dynamic getfenv lookup, main sentinel,
stack height, 1000 restarts, transient churn and idle weak-owner retirement all
pass. No global was cleared merely to satisfy the retention gate.

Fresh exact-source ASan/UBSan and broader acceptance remain separate gates.

### Canonical package compatibility receipts

Manual core Native [43068787823](https://github.com/gmoddev/gargantuan/actions/runs/43068787823)
passes Windows 146/146 CTests and five Release workloads, then fails the final
package fixture assertion; Linux ASan/UBSan passes 97/97. Version 5 adds NodeIds
and eight default Part controls, expanding the historical 512-object payload
from 273032 to 399154 bytes. Removing only these additions recovers the version
4 structure. The old baseline PR job skipped manual-only packaging.

The repaired qualified producer pins and hash-checks the historical version 4
payload/manifest and verifies authored values plus current loader defaults.
Normal package capture defaults to version 5; explicit content version selection
rejects unsupported, mixed and mismatched versions before output commit. Load
and spatial metadata inspection use the manifest version. Original D03 byte/count
constants remain unchanged. Targeted fixture/spatial tests and actual manual
packaging remain required; matching stored bytes does not claim identical
post-materialization replication bytes.

Canonical baseline [43054671698](https://github.com/gmoddev/gargantuan/actions/runs/43054671698)
passes the complete Windows NativeCI job, including existing GNS/F1 Release
workloads. It qualifies baseline runtime, not the new integrated runtime.
Earlier Linux Release/ASan/UBSan and GNS sanitizer attempts failed during dependency install:
Ubuntu mirror DNS prevented fetching required Clang 19 and shader tooling before
engine compilation. The runner's preinstalled Clang 16–18 cannot replace that
canonical contract. `HostedLinuxInfrastructureFailure.md` preserves the logs
and exact image identity; no tests are marked passed from those attempts.
Later exact-source Native/GNS jobs installed dependencies successfully and are
building. Linux Release comparison 43069269328 then exposed a test-selector
error: its core suite requested an unbuilt realistic executable after seven
other gates passed. The selector is corrected without changing engine sources;
[43072898111](https://github.com/gmoddev/gargantuan/actions/runs/43072898111)
reruns the original source pair. There is no current blanket Linux DNS blocker.

Earlier fixture failures are retained: a missing stream operator, an incorrect
DataModel service call, a prematurely released test DataModel, invalid zero-change
probe arguments, and a colliding compact-view lifecycle fixture were corrected
before reruns. A second compact fixture confused moving Creates with Updates;
it now checks the final projected transform. The initial live-render fixture
failed Windows compilation due min/max macros; NOMINMAX corrects both sources.
The first dirty-drain attempt requested an incorrectly named CTest and ran no
measurements. All failed artifacts remain preserved. Physical GPU, full integrated Windows and
Linux/sanitizer acceptance remain outstanding. Production physics algorithms,
transport wire formats, F1/D03 gates and renderer defaults are unchanged.

## Physical ordinary rendering: source-bound complete-path limits

The physical RX 7900 XT comparison freezes baseline
`dfeb595268020ac62e56b1a604218e122f6dc714` (Foundation 5 runtime plus the
coroutine-root correction) and candidate
`30fcadbe31958f4495c66604c1935553c32f7664`. Both arms use one identical SDL DLL
and 18 shader outputs. Canonical shader blobs and the 26 actual checkout input
hashes match; the receipts distinguish Git CRLF conversion from source changes.
The donor glslc executable, version, recipe and output hashes are retained.

All 16 fresh A/B/B/A processes pass the authoritative journal, publication,
object/draw, submitted-frame, resync and selected Vulkan-device gates. Each has
34 frames; analysis excludes the first four, leaving 60 samples per arm/case.
Independent verification checks 20 frozen files per arm and all 48 CSV/stdout/
stderr hashes. The host is Ryzen 9 7950X3D, 32 logical processors, AMD Radeon
RX 7900 XT, AMD Vulkan 26.10.1; starting physical memory load is about 80% with
6.5 GB available. Evidence and the independent per-process/tail review are in
`PhysicalRX7900XT-Ordinary-dfeb-30fc-20261111` under the campaign artifact root.

| Actual Parts / changed per frame | Publication mean ms, baseline to candidate | Mutation to completion mean ms | Completion P95 ms |
| --- | ---: | ---: | ---: |
| 1K / 1K | 0.421 to 0.296 | 1.704 to 1.631 | 1.982 to 2.149 |
| 10K / 10K | 10.391 to 6.697 | 32.094 to 25.221 | 40.491 to 27.040 |
| 50K / 500 | 0.473 to 0.328 | 24.374 to 22.666 | 31.251 to 28.831 |
| 50K / 50K | 48.436 to 40.892 | 153.801 to 156.665 | 176.081 to 180.336 |

Every candidate process improves publication. At 50K dense updates, however,
setter time increases from 76.401 to 83.353 ms and draw/wait from 28.963 to
32.418 ms, offsetting that saving. The 1K result is mixed; sparse 50K also has
process-order drift. These are offscreen mutation-through-SDL-idle wall times,
not GPU timestamps, a complete Engine frame, or multiplayer capacity. Ordinary
drawing remains the reference; no universal frame improvement is claimed.

## Consolidated lifetime and input teardown qualification

The selective candidate 30fc passes 12 focused worker CTests. Its full manual
Native run [43109060352](https://github.com/gmoddev/gargantuan/actions/runs/43109060352)
then passes 159/160 Windows tests and 106/107 Linux sanitizer tests. Both fail
only `gargantuan_player_runtime`: early logical Lua shutdown disconnects the
default Script Destroying callback before it can unbind legacy input handles.
The consolidated correction closes native ActionMap facade admission and
retires its owned handles during Engine teardown. Direct authored InputService
definitions remain outside that table. Native Destroying/ActionEnded reentry
and repeated teardown have explicit regressions. Full qualification of this
correction is required; the failed historical run is retained.

Completed Scripts also exposed a native/Luau retention cycle through their
own registry-rooted coroutine and sandbox `script` userdata. The correction
retires terminal roots while retaining independently rooted tasks, callbacks
and Remote requests. It covers native later-resume boundaries and bounded
reconciliation of Luau coroutine.resume/close. An initial attempt falsely
treated failed nested self-resume as completion; a later guard mistook returned
values for a still-suspended coroutine. Worker 08cf passes 11/12 focused tests
but fails the two success-with-return-value retirement checks. The consolidated
implementation captures pre-resume coroutine state, distinguishes actual errors
from resume preflight failures, and retains all reproductions. These changes
are correctness work, with no asserted Lua mutation speedup.

The earlier baseline 36988c6 sanitizer
[43105610779](https://github.com/gmoddev/gargantuan/actions/runs/43105610779)
passes three lifecycle CTests and all 24 GC/disconnect stress processes. Its
archive SHA-256 is
`663d086651c31ae5c606e1f531ae12d7b75f918fd5ca6dbde62adc962c5d4ecd`.
It qualifies that exact source, not the later retirement or input corrections.

GNS sanitizer [43109099942](https://github.com/gmoddev/gargantuan/actions/runs/43109099942)
qualifies 30fc with 11/11 selected gates and all 12 byte-admission cases. Each
eventually delivers all 1,470,700 submitted bytes with matching message counts;
pooled conservation is 6635 = 5925 + 710, outstanding zero. Its archive SHA-256
is `1a9046aca802edf5ddd0e03230852866e602fd3a7a5cea6bd31b41b3d3995d28`.
The low-rate large-chunk case still has 187,930 bytes pending at offer end and
P95 875.143 ms. These are sanitizer safety results: `safe_to_enable=0` and
`physical_f1_qualified=0` remain unchanged.

## Selective readback: retained unsuccessful timing evidence

The full 41-case worker comparison verifies all 44 frozen-file hashes, all
328 raw-stream hashes and 164 successful processes. Twenty semantic fields
match in every setup, warmup and sampled frame. Its timings are unsuitable for
promotion: one 50K idle quartet slows in adoption before activation, physics
and validation, while an adjacent case improves. A second 20-process capture
pins both arms to the same logical processor and records CPU time, page faults,
working set and host counters. Its two identical idle quartets reverse direction.
The index-inactive fully dynamic guard also slows, and validator time sometimes
masks a loss in measured production stages.

Host counters show near-total CPU occupancy and the pinned child executes for
only about half its wall duration. A separate bounded process sample finds
multiple older sshd processes consuming CPU. A newly created, provenance-tracked
diagnostic SSH session exits normally; ownership of the older sessions is not
established, and they are left untouched. Worker disk also falls below the named
build reserve. These observations invalidate timing attribution; they do not
prove the index itself caused the loss or that affinity fixes it. Failed runs,
exact hashes and telemetry remain in `WorkerReadback-369-30fc-20261111`.
The final candidate excludes this view while fresh same-source-pair hosted
Windows and Linux comparisons run. Production solver behavior is unchanged.
