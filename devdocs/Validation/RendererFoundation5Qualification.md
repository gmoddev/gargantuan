# Renderer Foundation 5 qualification

Status: qualified experimental foundation; no default or mainline promotion.
Foundation 5 is isolated
on `foundation/renderer-5`, stacked on Foundation 4 at
`94f920485564798742bc22a061cbc3a415034fa3`. No mainline acceptance is implied.

## Final acceptance scope, 2026-11-11

Draft continuation: [PR 22](https://github.com/gmoddev/gargantuan/pull/22), base
`foundation/renderer-4`. Foundation 4 PR 21 and its raw evidence remain intact.
The runtime checkpoint is `5250798037753bb9d8998576701b0ec29e78f2f4`;
`ad58172cbd8516c9f95ba65c223efc123d6330b7` adds final test controls and documentation.
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
| Hundreds of local lights | `AMD-ad581-many-lights`, `GTX-worker-qualification/GTX-ad581-local-lights-v2` |
| Complete GTX focused/broad/churn/profile receipts | `GTX-worker-qualification` |
| AMD broad/churn/profile receipts | `AMD-6363-broad`, `AMD-15d-probe-churn`, `AMD-525079-profile-controls` |
| Record equality / rejected dense lookup | `instance-apply-microprobe`, `instance-lookup-microprobe` |
| Rejected BVH / proof batching | `visibility-probe`, `AMD-acf-instance-compact-ABBA` |
| Independent source reviews | `FinalIndependentReview-ad581.md`, `M15-independent-review-0dd942b94.md`, `ShadowM5M6IndependentReview-0dd942b.md` |
| Corrected spotlight/opaque culling checks | `ValidSpotSearch-100k.txt`, `OpaqueCullingCrosscheck-525079.md` |

Final source is `ad58172cbd8516c9f95ba65c223efc123d6330b7`;
benchmark SHA-256 is `05DA6BD4248B2D4C70736EAE66D6E16473CCA655093608CAD7733E7399B4E03B`;
manifest SHA-256 is `EF7FA15991841615D2C3DCBF9B22D0CA00D3626A9B07727617AB50325D2B42D8`.
Each adapter's `source.json` stores every shader hash and Vulkan loader stderr
identifies the physical adapter/driver DLL actually used. No software adapter
is counted as physical evidence.

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
