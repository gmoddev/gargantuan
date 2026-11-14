# Renderer Foundation 5 scene envelope experiment

The standalone M9 experiment is preserved in commit `77ebb15b5702002beda9b19b610f6a408a39c6fc` and
`C:/Users/aiden/.codex/artifacts/renderer-foundation5-20261111/visibility-probe/`.
Its exact LF source SHA256 is `3C5CA2B046A8300FF1C29D082CA9E5A62731D1CD56FFCD753D43A49F92979FE6`.
The three rotated 30-case runs include static, fully culled, rotated camera, mixed visibility, 1% dynamic,
and 100% dynamic at 1K, 10K, 25K, 50K, and 100K objects. An adversarial case checks near planes,
million-unit coordinates, rotations, extreme scale, and ordered visible identities after refits.

At 50K fully culled, median linear traversal was 0.727 ms, the four-way BVH query 0.046 ms after
43.7 ms construction and 3.82 MB of bounds/index storage, and the whole-scene envelope below 0.001 ms
after 1.65 ms construction with 56 bytes of bounds. At 50K mixed visibility, BVH was 1.217 ms versus
linear 0.964 ms. The BVH lost in most mixed and moving cases, so it is rejected for this renderer workload.

The production prototype adds `SDLRendererOptions::EnableSceneEnvelope`, default off. It builds lazily
after two object-stable publications, caches only finite affine bounds for visible primitive or static mesh
objects, and declines the fast path for missing mesh bounds, skinning, pose state, and extreme transforms.
Any object, mesh, or pose operation invalidates it. Camera-only publications reuse it. When the entire
envelope is outside the camera frustum, the ordinary object traversal is skipped and every eligible visible
object is credited to `CameraCulledObjects`. All other frames retain the original map traversal and draw order.
`EnvelopeRejections`, `EnvelopeBuildNanoseconds`, and `EnvelopeRebuilds` expose admission and lifetime costs.

Matched `--scenario=culled --culled-percent=100` and camera-away A/B/B/A runs
at `ad58172cbd8516c9f95ba65c223efc123d6330b7` passed on both physical adapters.
After the bounded build, mean submission falls from approximately 3–5 ms to
0.02–0.06 ms, with 120/120 envelope rejections, zero measured rebuilds, exact
6,000,000 culled objects and only sky/resolve draws across 120 measured frames.
Exact-pixel, invalidation and mixed-visibility controls retain ordinary behavior.
The option remains off by default and explicitly admitted only for its whole
scene case. Authored mesh LOD contracts can be considered separately; no
automatic simplification or production spatial index is part of this experiment.
