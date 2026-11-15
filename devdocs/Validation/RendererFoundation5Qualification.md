# Renderer Foundation 5 qualification

Status: qualified experimental foundation; no default or mainline promotion.
Foundation 5 is isolated
on `foundation/renderer-5`, stacked on Foundation 4 at
`94f920485564798742bc22a061cbc3a415034fa3`. No mainline acceptance is implied.

## Final acceptance scope, 2026-11-11

Draft continuation: [PR 22](https://github.com/gmoddev/gargantuan/pull/22), base
`foundation/renderer-4`. Foundation 4 PR 21 and its raw evidence remain intact.
The final runtime checkpoint is `7f3d84e12dee9d56faa659a47dbf8f18ba4df25d`.
The instance-record optimization is `5250798037753bb9d8998576701b0ec29e78f2f4`;
the final matched instancing controls use `ad58172cbd8516c9f95ba65c223efc123d6330b7`.
Subsequent source `17fbb658f34f3799ffc8e096497c8c95e9c61e7a` changes benchmark
fixtures and their CI coverage only. The native rendering code/shaders are
unchanged after `7f3d84e12`.
No Studio, LUI, Node or production physics files were changed.

| Area | Disposition and boundary |
| --- | --- |
| CPU submission | Extra candidate walk/allocation and duplicate culling removed; complete instance-payload equality accelerated. Measured native results below. |
| Native instancing | Qualified experimental, off by default. Vulkan contiguous single-range rigid/untextured opaque batches preserve traversal order. Incompatible objects use ordinary draws. |
| Spatial IBL | Qualified experimental authored probes: 32 published, 12 resident, two sampled per pixel, one local prefilter in flight. No automatic capture or GI. |
| Directional shadows | Qualified optional four-cascade path, 512/1024/2048 resolution, per-layer reuse, temporal and far-receiver controls. Legacy map remains default. |
| Local shadows | Qualified eight-layer 1024² spotlight atlas with deterministic denial/eviction and unshadowed fallback. Point cube shadows unsupported. |
| HDR assets | Qualified bounded Radiance RGBE square/panorama import and version-7 linear float artifacts; EXR unsupported. |
| Texture residency | Qualified bounded source/mip-view retention, demand/eviction and staging. Complete base-texture streaming and block compression deferred. |
| Visibility | Qualified explicit whole-scene static envelope. BVH rejected for measured mixed/dynamic costs; authored LOD, GPU occlusion and terrain selection deferred. |
| Recovery | Rejection/full-resync, canceled uploads, recreation, resize, retirement and bounded churn tested. Physical driver removal was not forced. |
| Studio | Complete Engine-side schema/EditorHost handoff; concurrent Studio application unchanged. |

Existing Foundation 4 behavior remains the production reference. These
experimental additions do not establish universal GPU compatibility, a complete
streaming world, multi-view sharing, diffuse visibility or automatic probe baking.

## Executed gates

Hosted run [43011261391](https://github.com/gmoddev/gargantuan/actions/runs/43011261391)
at exact `ad58172cbd8516c9f95ba65c223efc123d6330b7` passed Windows Release,
Linux Vulkan and Linux sanitizer lanes. Sanitizer, software Vulkan and physical
Windows Vulkan evidence have distinct acceptance scopes.

Final runtime run [43018500754](https://github.com/gmoddev/gargantuan/actions/runs/43018500754)
at `7f3d84e12dee9d56faa659a47dbf8f18ba4df25d` also passed all three lanes,
including the admitted solid-caster shader and corrected mixed-material fixture.
Benchmark follow-up [43021058515](https://github.com/gmoddev/gargantuan/actions/runs/43021058515)
at `17fbb658f34f3799ffc8e096497c8c95e9c61e7a` passed Windows Release,
Linux Vulkan and ASan/UBSan, including the explicit candidate fixture and raw
sample writer. Its one-frame six-scenario semantic run is a functional/count
gate, not a physical CPU performance percentile.

The persistent MSVC Release worker package passed all 11 focused CPU gates.
Native coverage is cumulative across the recorded checkpoints: unchanged
HDR/probe/residency paths retain their earlier native gates, and changed opaque
and shadow paths receive focused requalification. At `ad581`, focused
contiguous-instancing, scene-envelope and layered-shadow gates passed
independently on both physical adapters. Final outdoor shadow controls at
`ad581` cover far receiver distance 200 and all three published resolutions,
with nonvacuous lit/shadow controls and exact warm/fresh images.

Native broad controls execute 21 scenarios with synchronization on and off
(42 rows per adapter). All required stable-resource, skinning and restart gates
pass or are explicitly inapplicable. Scenarios include cloth/deformable topology,
GUI plus 3D, texture/environment scenes, 1/10/100/500 rigs and 16–256-bone palettes.
The final solid-depth runtime then passes the complete 11-gate AMD set at
`7f3d84e12`, plus the 42-scenario broad set on AMD at that source and GTX at
`81b36b13442a3058e56138fcef1d1d971947b972` (same runtime/shader code). GTX's
ten other native gates pass at `81b36b1`; its newly strengthened shadow fixture
passes at `7f3d84e12` after the geometry correction. The earlier failed fixture
remains failed in its original receipt.
Exact-pixel gates additionally cover coplanar/reversed/near-tied order, overlap,
nonunit normals, positive first-instance offsets, mask/range/mesh fallbacks,
HDR/SDR source equivalence, actual global/local IBL transition, shadow foliage,
noon/pole motion, split transitions, moving/posed casters and atlas saturation.

Two independent 4096-frame probe churn cycles per adapter passed. Each receipt
contains 4286 CSV frames including initial controls, 3410 source updates, at most
12 resident probes and 3,292,992 logical cube-array bytes. AMD private memory
plateaus at roughly 279–303 MiB with about 225 MiB dedicated GPU memory; GTX is
roughly 330 MiB private and 202 MiB dedicated. WDDM counters are process samples,
not allocator ownership totals; one startup sampling timeout is retained. Each
cycle lasts approximately 24–25 seconds: this is bounded churn, not a multi-hour
soak. Old failed captures, timeouts and unsuccessful hypotheses remain present.

| Physical scope | AMD checkpoint | GTX checkpoint |
| --- | --- | --- |
| Initial HDR/probe/residency and ordering gates | `0dd942b948f16bd760743b7aa6e523bb5e3ea24c` (10 passed; shadow fixture failed and is preserved) | Integrated worker packages; raw gate receipts retained |
| Broad 42 scenarios | `6363bb47978e6692d33d862f8250d61853bea2c3` | `5250798037753bb9d8998576701b0ec29e78f2f4` |
| Two 4096-frame probe churn cycles | `15d20fb6eae3780d8ccf879ef060f106d97c6300` | `5250798037753bb9d8998576701b0ec29e78f2f4` |
| Final focused depth/envelope/shadows and 84-run A/B/B/A | `ad58172cbd8516c9f95ba65c223efc123d6330b7` | Same source and package |
| Admitted solid-depth runtime | Full 11 gates + broad 42 at `7f3d84e12` | Ten gates + broad 42 at `81b36b1`, corrected full shadow gate at `7f3d84e12` |

Additional `ad581` local-light controls publish 512 lights (448 visible under
the existing bounded selection), including static and moving cases. On GTX,
512 static/moving CPU submission is about 0.469/0.680 ms and frame P95 about
0.922/1.145 ms. On AMD, static submission is 0.176–0.244 ms and moving
0.341–0.357 ms across the counterordered runs. Over-budget tile and dropped
index counters explicitly show the existing bounded fallback; no light/shadow
capacity was silently increased. These are illumination controls, separate from
the saturated shadow-atlas tests.

## Final counterbalanced performance

Both adapters completed 84/84 same-binary A/B/B/A runs at `ad581`: 1280×720,
HDR, shadows off, bounded-two frame admission, 30 warmups and 120 measured frames.
Each cell below averages the two per-run summary values for that arm. P95 is the
mean of per-run P95 values, not a percentile recomputed from pooled samples.
CPU submission and whole-frame time are separate; neither is GPU execution time.

| Workload / adapter | Submit ordinary → contiguous, ms | Frame P95 ordinary → contiguous, ms |
| --- | ---: | ---: |
| Static 50K / RX 7900 XT | 14.956 → 12.559 | 21.029 → 18.430 |
| Static 50K / GTX 1650 SUPER | 21.464 → 11.864 | 28.988 → 17.071 |
| Dynamic 50K / RX 7900 XT | 15.575 → 17.173 | 33.606 → 42.739 |
| Dynamic 50K / GTX 1650 SUPER | 22.223 → 18.873 | 48.437 → 51.971 |
| Half culled 50K / RX 7900 XT | 10.167 → 11.912 | 14.836 → 17.475 |
| Half culled 50K / GTX 1650 SUPER | 12.530 → 9.122 | 15.399 → 13.434 |

Static 50K records 98 world draws instead of 50,000, and 50,000 actual instanced
objects per frame. Total draws including sky/resolve are 100 versus 50,002.
Warm static instance uploads are zero. Fully culled, camera-away and mixed
fallback workloads often regress with instancing alone. Compact traversal
regresses the measured 50K controls on both adapters and is not promoted.

The separately controlled scene envelope reduces wholly culled/camera-away
submission from approximately 3–5 ms to 0.02–0.06 ms. Its measured 120 frames
report 120 rejections, zero rebuilds, exactly 6,000,000 culled objects and 240
sky/resolve draws. Unknown, intersecting and changing scenes keep ordinary
classification. Instancing, compact traversal and envelope remain explicit
options rather than silently changing the default workload.

## Attribution, rejected experiments and corrected claims

The initial proof-mode instance batch reduced draws but cost about 19.80 ms in
coverage preparation, increasing 50K static CPU submission to 24.39–24.58 ms.
That throughput approach is rejected. The final map walk removes an extra
roughly 3.9-ms preparation traversal and about 1.6 MB/frame temporary allocation
observed in its earlier instrumented version. Native final paired results,
rather than microprobe savings, determine admission.

The complete 224-byte record has standard-layout/trivial-copy and explicit
offset proofs. Actual-cache microprobes compare scalar equality with complete
payload `memcmp` and retain all final material fields. A denser ObjectId lookup
saved only 0.16–0.27 ms at 50K and was rejected. A standalone four-way BVH lost
to linear traversal in mixed/moving cases despite winning fully outside queries;
the smaller conservative envelope was the justified integration.

Fine instance substages sample 1/128 candidates only under profiling. Profile
on/off controls exist for both GPUs. C++ allocation counting has a separate
binary, counts calling-thread new requests and excludes SDL C, driver and other
thread allocations. The pinned SDL public API supplies no GPU timestamps;
recording, submit, fence and readback clocks are labeled host measurements.

The retained HDR decoder audit reproduces 376 accepted truncated prefixes out
of 688 with the old header and zero with the hardened decoder. Header metadata,
finite/radiance bounds, source-group identity and reimport tests pass in current
native and hosted lanes.

The earlier spotlight frustum hypothesis used coordinates outside the ±1e6
local-light publication bound. Its stored VP equals canonical GLM(P*V), and a
100K in-range search found no old false rejection. The defensive direct-adapter
roundoff guard remains; no production culling defect is claimed. The old
hypothesis and corrected crosscheck remain together in the evidence.

The original one/six/eight-layer cost fixture inherits an alpha-masked material.
Its 1000-caster GTX wall times are approximately 22.65/80.86/142.93 ms. It is a
synthetic maximal-overlap masked workload. Six spotlight-equivalent depth faces
are a cost proxy, not implemented point shadows or typical scene cost. All
cases allocate the same 32 MiB atlas and touch 4/24/32 MiB. Whole-frame fence
latency includes receiver and frame work. Point shadow admission remains gated.

### Solid-caster depth correction

The isolated `0e717eed5b5af97c4ba8757125c347e9094538a1` experiment splits
nonmasked depth rendering from the alpha-test shader. Both share vertex/palette,
D32, strict LESS/write and sidedness state. Sixteen same-binary runs cover
opaque and masked 1000-caster off/on/on/off on both adapters, four warmups and
24 measured frames per layer workload in each run. Each arm/layer has 48
measured samples. The table uses the native fixture's upper median; means,
P95 and ordinary statistical median artifacts are also retained.

| Adapter / layers | Opaque fence off → on, ms | Masked fence off → on, ms |
| --- | ---: | ---: |
| RX 7900 XT / 1 | 2.870 → 2.060 | 2.811 → 2.896 |
| RX 7900 XT / 6 | 11.989 → 7.011 | 12.107 → 12.120 |
| RX 7900 XT / 8 | 16.088 → 8.973 | 15.872 → 15.913 |
| GTX 1650 SUPER / 1 | 21.279 → 17.485 | 22.061 → 21.331 |
| GTX 1650 SUPER / 6 | 77.536 → 33.681 | 77.867 → 77.810 |
| GTX 1650 SUPER / 8 | 138.840 → 106.207 | 138.991 → 139.189 |

The opaque six-layer wall upper median falls from 13.663 to 8.191 ms on AMD and
80.310 to 36.309 ms on GTX. Opaque on records exactly 1000/6000/8000 solid draws;
masked and opaque off record zero. These are synthetic maximal-overlap whole
frame observations. Not every tail improves: AMD one-layer wall P95 rises from
4.579 to 5.241 ms, and low-cost masked tails vary. Splitting the shader is a
justified solid-caster optimization, not a universal per-frame latency promise.
Reduced fragment bindings/resources and changed discard behavior occur together;
the data does not uniquely identify early-Z as the hardware mechanism.

The integrated adapter enables this solid path, creates its optional pipelines
only when an eligible caster actually records, and falls back after allocation
failure. It does not add a DataModel setting or change masked/transparent shadow
semantics. Additional parity controls include the legacy directional path and
single-sided/mirrored and solid→mask→solid switches. An initial mixed fixture
failed because a flanking opaque caster lay across the camera's center ray;
alpha changes affected its visible silhouette but not the sampled receiver.
`7f3d84e12dee9d56faa659a47dbf8f18ba4df25d` moves those solids off that ray while
retaining the original center and bounded-region acceptance thresholds. Failed
images and receipt remain in `GTX-worker-qualification/GTX-81b36-all-gates`.

## Engine semantic attribution

The original physics-backed aggregate semantic benchmark timed out at its
240-second bound; a grouped-parent variant also timed out after emitting valid
individual rows. Those process verdicts remain failures. The fixture constructs
and tears down separate live DataModels as well as timing mutations, publication
and projection; process duration cannot be attributed to the renderer loop.

The explicit `--renderer-only --group-width=256` candidate fixture at `17fbb658`
retains real Parts, authoritative setters, journal/dirty capture, publisher and
projection. It excludes native body construction and supplies the existing
World.Parts candidate list that body admission normally populates. Exact initial
object and per-frame update counts are mandatory. This is a controlled candidate
fixture, not normal physics admission or a second production scene authority.
Physics dirty-signal bookkeeping still contributes to setter timing. Raw rows
encode fixture shape and profile mode; aggregate rows require their invocation
manifest. Per-frame journal/classification/accumulator values are explicitly NA
because those counters expose only aggregate attribution.

The 12-frame profile-off aggregate run reached its bound after five completed
scenarios and flushed 60 raw frames. Its first three static 1K/10K/50K cases
have approximately 0.001–0.002 ms incremental publication and projection means.
The following are valid bounded measurements from its complete timed rows,
not a claim that the aggregate process passed:

| Live Part workload | Setter/dirty mean / P95, ms | Publication mean / P95, ms | Projection mean / P95, ms |
| --- | ---: | ---: | ---: |
| 50K, 500 changed/frame | 1.207 / 1.316 | 0.828 / 0.930 | 0.087 / 0.111 |
| 50K, every Part changed/frame | 110.079 / 118.101 | 89.288 / 96.039 | 11.822 / 13.141 |

Each row has twelve samples, not a production percentile guarantee. The
profile-on combined run was stopped at worker lease expiry and retains its
three static rows/36 raw samples without a completed-process receipt. The
earlier physics-backed two-frame profiled run supplies separately labeled
aggregate stage attribution. These are not matched on/off speedup controls.

A private, unmerged scenario selector and lifecycle-marker probe at
`4a1ffd5f96be32d1d36bd06f68ff47687a62c051` then completes both focused dynamic
runs. Each process has twelve raw frames, exactly 600,000 updates and no
create/remove/resync operations. Profile-off mean setter/publication/projection
is 107.884/95.096/11.370 ms; profile-on is 116.596/95.438/11.733 ms. Profiling is
attribution overhead, not a candidate optimization.

Markers isolate 69.8–69.9 seconds constructing/adopting Parts, about 76 ms in
initial publication, 2.59–2.70 seconds measuring twelve frames, and 0.55 seconds
in teardown. Repeating the aggregate fixture's four 50K worlds plus 1K/10K worlds
exceeds the 240-second process bound. The timeout was a workload-sizing issue;
neither teardown nor native body construction alone explains it. A moved-out
native backend does not remove live Part construction/adoption costs.

The profiled publication includes 73.757 ms final-state extraction, 4.944 ms
dirty capture, 0.271 ms dirty expansion, 5.834 ms construction and 2.929 ms
state-cache reconciliation. These are nested/partial attribution, not a promise
that their sum exhausts the phase. Setter aggregate attribution reports
16.944 ms journal, 1.685 ms classification, 13.070 ms accumulator and 84.897 ms
remaining setter work. The remainder is not attributed exclusively to one
implementation mechanism.

The large fully dynamic live-Instance cost is upstream of native GPU draw
submission. Instancing's draw reduction does not remove setters or final-state
publication. The renderer can support large mostly static environments while
50K independently changed Parts per frame remain outside a qualified real-time
end-to-end envelope. Keep typed incremental extraction/coalescing and source
mutation costs ahead of a broad temporal/post stack in the investment order.

## Evidence and reproduction

All task evidence is rooted at
`C:/Users/aiden/.codex/artifacts/renderer-foundation5-20261111` (call this `E`).
Full raw CSV, stdout/stderr, commands, captures and failure receipts are retained.
Prior Foundation 4 evidence is in sibling `renderer-foundation4-20261110`.

| Evidence | Relative to E |
| --- | --- |
| Exact 53-member final native package | `Worker-ad581/renderer-evidence/renderer-package.json` |
| Final AMD / GTX A/B/B/A | `contiguous-qualification/AMD-ad581-final-abba`, `contiguous-qualification/GTX-ad581-final-abba` |
| Paired analysis and input hashes | `FinalAbbaAnalysis/paired-means.csv` |
| AMD final focused depth/envelope/shadow gates | `AMD-ad581-final-focused` |
| Admitted solid-depth runtime native gates/broad | `AMD-7f3d8-all-gates`, `AMD-7f3d8-broad`, `GTX-worker-qualification/GTX-81b36-all-gates`, `GTX-worker-qualification/GTX-7f3d8-shadows` |
| Hundreds of local lights | `AMD-ad581-many-lights`, `GTX-worker-qualification/GTX-ad581-local-lights-v2` |
| Engine semantic raw/failed combined run | `GTX-worker-qualification/Worker-17fbb-semantic-final`, `GTX-worker-qualification/Worker-81b36-grouped-semantic` |
| Completed isolated semantic attribution | `semantic-case-isolation/Worker-4a1-dynamic-semantic`, `semantic-case-isolation/semantic-case-4a1ffd5f.bundle` |
| Complete GTX focused/broad/churn/profile receipts | `GTX-worker-qualification` |
| AMD broad/churn/profile receipts | `AMD-6363-broad`, `AMD-15d-probe-churn`, `AMD-525079-profile-controls` |
| Record equality / rejected dense lookup | `instance-apply-microprobe`, `instance-lookup-microprobe` |
| Solid-depth raw paired reduction and hashes | `SolidDepthCostPooled-v2`, `AMD-0e717-opaque-cost-*`, `AMD-0e717-masked-cost-*`, `GTX-worker-qualification/GTX-0e717-solid-depth-cost` |
| Rejected BVH / proof batching | `visibility-probe`, `AMD-acf-instance-compact-ABBA` |
| Independent source reviews | `FinalIndependentReview-ad581.md`, `FinalIndependentReview-81b36.md`, `M15-independent-review-0dd942b94.md`, `ShadowM5M6IndependentReview-0dd942b.md` |
| Corrected spotlight/opaque culling checks | `ValidSpotSearch-100k.txt`, `OpaqueCullingCrosscheck-525079.md` |

Final source is `ad58172cbd8516c9f95ba65c223efc123d6330b7`;
benchmark SHA-256 is `05DA6BD4248B2D4C70736EAE66D6E16473CCA655093608CAD7733E7399B4E03B`;
manifest SHA-256 is `EF7FA15991841615D2C3DCBF9B22D0CA00D3626A9B07727617AB50325D2B42D8`.
Each adapter's `source.json` stores every shader hash and Vulkan loader stderr
identifies the physical adapter/driver DLL actually used. No software adapter
is counted as physical evidence.

The final native runtime/benchmark follow-up package is `Worker-17fbb`:
54 members, source `17fbb658f34f3799ffc8e096497c8c95e9c61e7a`, manifest SHA-256
`1885E269844F0AC76451FE11E1F2B8D13B6DD2071E5157DED96F91DF356E8FD4`.
Its renderer benchmark SHA-256 is
`BBA91C1EF12636B330D415C1620F6B6875EC930A2F739A2CB0653EAA30E84BF4`,
semantic benchmark SHA-256 is
`7ACAD732B58FBB6D07CD2E7A6FF075392292C4797D044518ECDB1E286E644B9B`,
and solid-depth fragment SPIR-V SHA-256 is
`D3A2D3F4239115D2744FC926736DF58C11AB517E1522946D23E28408A821CCD9`.
The earlier `ad581` package remains the matched A/B/B/A package, not this later
binary. Hosted status is also retained locally in `FinalHostedStatus-17fbb.json`.

From E, with a fresh output child name, reproduce final matched controls with:

```powershell
python contiguous-qualification/RunContiguousAbba.py --package Worker-ad581 --expect-source ad58172cbd8516c9f95ba65c223efc123d6330b7 --output Reproduction-ad581 --group core --group compact --group envelope
```

The runner validates all package members and writes source/runner/command
receipts before executing. Its output directory must be a fresh child of
`E/contiguous-qualification`. Reproduce focused shadows or churn with
`RunBoundedLifecycle.py --package <absolute-Worker-ad581> --source <full-source>
--output <fresh-absolute-output> --mode shadows|churn`. For broad controls use
`--mode broad` against the receipt's matching package and full source (`Worker-6363bb`
for the recorded AMD broad run). Run one
GPU timing workload at a time. Hidden subprocesses disable native fault dialogs.

Native builds use the cooperative dockerbox lease, three compile jobs and
persistent `C:/Sandbox/Codex/Workspaces/renderer-foundation5-20261111` and
`C:/Sandbox/Codex/Builds/renderer-foundation5-20261111/msvc-release` directories.
The cache disables precompiled headers because MSVC path mapping broke SDL's
PCH path; this is a worker build setting, not a source workaround. Every package
records the small Animator formatting diff produced by classgen and its hash.
The worker lease is released only after all task-owned workloads are idle.

Both worker leases are now released after an idle-process census; public
receipts are `worker-release-106.json` and `worker-release-107.json`. Lease 106
became overdue during an idle continuation pause; no new work was admitted
under it. Its incomplete profiled run was stopped, reconciled and released.
The final isolated probe used fresh generation 107. The worker checkout is
restored to `17fbb658f`; incremental caches remain, and no task-owned process
continues. Private probe source/binary and unsuccessful evidence remain archived.

## Historical checkpoints and failures

The following entries retain their original intermediate scope. Statements of
pending qualification describe those earlier checkpoints and are superseded by
the final evidence above; no failed run is relabeled successful.

### Preserved intermediate evidence

Task evidence is under
`C:/Users/aiden/.codex/artifacts/renderer-foundation5-20261111`.
Every native package carries its exact source and SHA-256 member manifest.
Raw samples, failed runs, driver diagnostics, and captures remain separate.

The physical RX 7900 XT package from CI run `42982605485`, source
`acf08817993296a1d01e15280b64581b6c265b29`, passed HDR viewport, ordinary depth
ordering, proof-mode native instancing pixel parity, HDR IBL, and reflection
probe GPU gates. Its shadow fixture failed at the animation-pose receipt
check. Subsequent source fixes distinguish actual skinned depth receipts from
static caster coverage; they require a new native run.

That package's proof-mode instancing performance is rejected: at 50K static
objects, world draws fell from 50,000 to 98 per frame, but CPU submission rose
from 16.56–17.16 ms to 24.39–24.58 ms. Detailed attribution measured 19.80 ms in
instance preparation. Profiling overhead and host drift prohibit treating
unmatched runs as speedups. Contiguous Vulkan instancing is a separate,
default-off experiment pending native parity and matched performance evidence.

Linux CPU tests at the same source exposed a real compact-traversal ordering
bug: an unordered-map reserve can reorder entries even for transform-only
publications. The correction preserves the previous map-reserve behavior and
invalidates derived compact traversal after each reserve attempt. Other
failures came from inconsistent shadow-cache and asset-import fixtures; their
corrections retain the original acceptance facts.

## Next integrated acceptance run

The checkpoint through `132b903ddf0500982ff1a91ab029bf389b26fdef` includes
contiguous-instancing shader parity and overlap fixtures, compact-order
correction, source-image LRU, scene-envelope culling, shadow pose receipts,
and canceled-frame recovery. Windows qualification now explicitly builds and
runs the focused publication, asset, and shadow-cache tests in addition to the
aggregate suite. Linux Vulkan and ASan/UBSan remain distinct evidence lanes.

Required remaining evidence includes both physical GPUs, exact-pixel native
gates for the revised paths, matched ordinary/contiguous performance controls,
shadow temporal and failure-recovery runs, bounded lifecycle workloads, and
independent source/evidence review. Scene-envelope culling, compact traversal,
and instancing remain opt-in until their individual acceptance gates pass.

CI run `42994327636` at `525be49a0ee7c05096e2ed7bec346db9af228ddb`
stopped during linking in all three lanes: the spotlight allocation test now
exercises projection-dependent caster checks but linked only GLM. The target
now links the existing engine core. Its partial artifact is retained. The
packaged publication executable passed; the asset executable lacked its
absolute CI-source font/PNG fixtures and failed before import qualification.
Those fixtures now ship with the test, with explicit missing-input reporting.
The partial artifact does not constitute a complete GPU package.

Subsequent integrated corrections cover canceled probe-buffer backing,
bounded coherent-probe extraction after invalid sources, Radiance header
assignments, and spotlight cone/pole handling. New native fixtures include
actual global/local IBL transitions, equivalent mixed HDR/SDR face decoding,
noon shadows, a bounded one/six/eight-face shadow cost comparison, and optional
probe residency/reimport/resize churn. A separate opt-in allocation benchmark
counts calling-thread C++ new requests; it does not instrument production or
the ordinary performance binary and does not count SDL/driver C allocations.
All remain subject to the next integrated build and physical gates.
