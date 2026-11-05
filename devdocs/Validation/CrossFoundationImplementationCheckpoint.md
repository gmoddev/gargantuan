# Cross-foundation integration implementation checkpoint

The isolated implementation branch is `integration/geometry-streaming-authoring`
at `C:\Users\aiden\.codex\worktrees\cross-foundation-integration`.
Composition starts at `96a88d9e68b1e9b8bca54fc8d26e93b22ac69d29`, preserving
exact PR15 `96226fc4d46581154e02a71486a36c26766ec361` and PR14
`30750f88bb76cd5d363eb84f72e117ce1446f940`. No owner branch/worktree is changed.

Implementation and qualification share one integration lineage and
[draft PR16](https://github.com/gmoddev/gargantuan/pull/16). The existing Studio chat independently contributes test/inspection
commits from `integration/cross-foundation-independent-qualification-20261104`;
it alone owns this campaign's worker recipe/lease/execution. This root and its
agents have run no remote operation or sustained local compilation.

## Implemented source

- Canonical artifact-backed package bounds derive Mesh, declared Collision
  companion and inherited Box envelopes in package space. Complete unsupported
  subtrees remain bootstrap. Build/Validate rederive the proof and reject
  underbounds, including self-consistent historical format-2 packages. Load
  carries the validated manifest and hashes exact returned payload bytes.
- Existing bounded Engine/EditorHost streaming pages expose manifest
  namespace/digest/space/bounds. Existing EditorHost asset catalog adds declared
  local bounds, declared Collision identity and representation, with explicit
  bind-pose labeling rather than producer trust.
- Geometry-aware standalone MeshPart relevance is independently reviewed.
  Four exact enter/leave checks use geometry
  AABB distance; Character/ordinary Part policy stays unchanged. No-focus
  fallback and shared-asset scan amplification findings are source-corrected,
  with independent static re-review and targeted regression assertions. A
  subtree-root/no-service-creation finding is also source-corrected.
- Local package/physics/render/GameSession tests cover actual imported and
  generated geometry, save/reopen, dependency demand, contact/query,
  release/reacquisition, fresh identities and off-pivot accepted materialization.
  Independent producer/consumer/lifecycle/authoring/performance tests are
  composed as separate commits and registered with the ordinary test system.

## Qualification status

Windows worker source `de0cf4850eda262075d0741219b795508a30b10c` passed all 18
strict executables: four package/geometry, five independent, four streaming and
five affected regressions. Its incremental build passed in 96.109 seconds with
124 classified generated files. All observed owned Jobs were checked/reaped.
The 90-file binary/runtime/sample/fixture seal is
`edf90b3077acab9a5f64fca7235a04035a3055fa5ca08c3a70b4df09770d4365`.

Discovery failures are retained: process PATH/TEMP and fixture staging paths;
an authoring lambda compile error; a legacy sample with no static eligible unit;
the mesh verifier comparing Box fallback rather than cached geometry; and a
NetworkClient fixture missing trusted LocalPlayer setup. Minimal corrections
preserve validation limits, assertions, client authority and native ownership.
Independent verifier re-review confirmed no mutation, service creation or vertex
rescans. Actual package/provider/off-pivot client and native failure/recovery
fixtures pass. The diagnostic baseline independently reproduces the historical
underbound; its raw archive and all 37 inventory hashes were independently checked.

Qualification is recorded per exact source in
[CrossFoundationQualification.md](CrossFoundationQualification.md) and PR16.
Upstream PR14/PR15 evidence is not promoted to this source. The delivery may add
documentation after the worker code commit; compiled inputs must remain
byte-identical and hosted checks identify their own actual checkout tree.

The sole independent operator acquired host-local lease generation 2 after actual
migration reconciliation, renews while work runs, uses two compiler jobs, and
releases only after checked owned-job cleanup. Lease/cleanup receipts stay with
the qualification evidence; this root performs no parallel host operation.
No historical physical Local32/Node32 run is included.
