---
status: bounded-negative-diagnostic
authority: non-normative
date: 2026-11-09
---

# Selected late 128² self-intersection search

`FindNonlocal3DWitness.py` searched selected late frames of the matched
timing 128² +10 µm sphere trajectories for a **strict nonlocal**
triangle intersection. The exact Engine was
`f501fd51cf4f5fe72c523d02323186cc669440b6` and native was
`46c3dfeeee5629562ca134a627a157f8c76faf99`. It verified each
compressed trace and fixture source hash and each selected frame's full
float32 position and velocity payload hashes before searching. The code
uses 0.12 m three-dimensional AABB buckets and tests disjoint-vertex
face pairs only. A positive witness requires a triangle edge to cross
the other triangle's plane with both endpoint plane distances >1 µm,
segment fraction in `(0.001, 0.999)`, and every target-face barycentric
coordinate >0.001. These margins make a reported witness independently
checkable as an interior segment-through-face crossing. Analytic positive
and separated negative controls pass.

| Trajectory | Selected frames (projected flip faces) | Nonshared AABB-overlap candidate pairs | Strict witness |
| --- | --- | ---: | --- |
| XPBD ES +10 µm | 34 (478), 38 (614), 39 (624) | 10,388 / 10,326 / 10,561 | None |
| Jolt IG with face cull 2 +10 µm | 37 (701), 38 (756), 39 (738) | 11,402 / 11,896 / 11,675 | None |

All 32,258 faces were inserted in the spatial buckets for each selected
frame. The six searches tested 66,248 candidate pairs and 620,296 bucket
postings without reaching the 500,000-pair, 600,000-posting, or
512-cells-per-face limits on any frame. The highest projected flip count
for each trajectory occurs in a tested frame. Thus the observed projected
orientation flips do **not** establish a nonlocal 3D crossing. This is a
bounded negative result, **not** a proof that all 40 frames or all types
of cloth self-contact are clear. Coplanar or tangential overlap, very
near-edge contact, shared-vertex and neighboring-face folds, and unselected
frames remain outside this strict witness search.

The previous small-cloth independent checker found no disjoint-face
intersection in nine XPBD E 9²/17² cases spanning 540 frames and 83,651
candidate pairs; that result likewise did not certify shared-neighbor
folds. The six selected-frame machine outputs are
`NonlocalWitnessESFrame34/38/39.json` and
`NonlocalWitnessJoltIGFrame37/38/39.json`, with aggregate
`Nonlocal3DSelectedFrameReview.json`, all in
`C:/Users/aiden/.codex/artifacts/jolt-cloth-contact-correction-20261109/`.
