# Cross-foundation integration implementation checkpoint

The isolated implementation branch is `integration/geometry-streaming-authoring`
at `C:\Users\aiden\.codex\worktrees\cross-foundation-integration`.
Composition starts at `96a88d9e68b1e9b8bca54fc8d26e93b22ac69d29`, preserving
exact PR15 `96226fc4d46581154e02a71486a36c26766ec361` and PR14
`30750f88bb76cd5d363eb84f72e117ce1446f940`. No owner branch/worktree is changed.

Implementation and qualification share one integration lineage and one future
draft PR. The existing Studio chat independently contributes test/inspection
commits from `integration/cross-foundation-independent-qualification-20261104`;
it alone owns this campaign's worker recipe/lease/execution. This root and its
agents have run no remote operation or sustained local compilation.

## Source implementation in review

- Canonical artifact-backed package bounds derive Mesh, declared Collision
  companion and inherited Box envelopes in package space. Complete unsupported
  subtrees remain bootstrap. Build/Validate rederive the proof and reject
  underbounds, including self-consistent historical format-2 packages. Load
  carries the validated manifest and hashes exact returned payload bytes.
- Existing bounded Engine/EditorHost streaming pages expose manifest
  namespace/digest/space/bounds. Existing EditorHost asset catalog adds declared
  local bounds, declared Collision identity and representation, with explicit
  bind-pose labeling rather than producer trust.
- Geometry-aware standalone MeshPart relevance is explicitly authorized and
  being independently reviewed. Four exact enter/leave checks use geometry
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

All new combined native tests are source-only and unrun. `git diff --check`
passes. Upstream PR14 and PR15 qualification remains valid at their pinned
sources and is not promoted to the new integration. A complete immutable
source candidate, private input census, Windows/Linux/sanitizer tests,
independent evidence review and performance/resource observations remain due.
KI-3M-001 is not marked closed before these gates pass.

The current host-local WorkerLeases migration hold is not admission; checked
chat release or nominal expiry does not bypass it. Qualification must obtain
the actual host-local lease, retain its token privately, renew it while work
runs, use at most two compile jobs and retire only task-owned processes before
explicit checked release. No historical physical Local32/Node32 run is planned.
