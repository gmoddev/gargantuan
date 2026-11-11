# Renderer Foundation 5 qualification

Status: implementation and qualification in progress. Foundation 5 is isolated
on `foundation/renderer-5`, stacked on Foundation 4 at
`94f920485564798742bc22a061cbc3a415034fa3`. No mainline acceptance is implied.

## Preserved evidence

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
