# Documentation accuracy and integration reconciliation

Status: **pre-integration reconstruction; draft required**. Audited 2026-11-14.

Integrated Engine source: `2a4c1d2b03e40178b97334bb6f1f2eeedd51fe9f` (fetched
remote main). The original dirty `foundation/3l-content-availability` checkout
was preserved. All campaign edits use `docs/reconstruction-20261114` in a
separate worktree. Runtime production sources, qualification receipts and other
campaign branches/worktrees are outside this patch.

Renderer Foundations 3–5 and Runtime 6 are not ancestors of this baseline.
Read-only prospective inspection used
`4b32a822a26492907005ce18c3b1045550bfce86`; moving integration refs are not source
pins. The documentation PR must remain draft until the Engine integration is
complete and the exact resulting main SHA has been reconciled.

The reconstructed hierarchy starts at the site home, Getting Started, Engine
Architecture, API, Renderer/Physics, Networking, Engine/Studio/Node, Contributing
and History. Original detailed contracts remain at their established paths.
The machine-readable inventory classifies every document and explicitly marks
unverified legacy prose rather than certifying it by directory name. The API
catalog extracts every class/service declaration, includes source hashes and
indexes native headers; extraction is not proof that a native method is usable
in every capability context. The guides explain tested behavior and limits.

Studio verification uses the locally available remote-tracking LUI tree
`6ae8f3dc427bc5221e978ae627eab6224e5ccea6`, distinct from the older local checkout.
Node verification uses `f4440423c0701ff396f589fd51b6ce41edc63539` and excludes
dirty integration files from qualified claims. The [cross-repository audit](NetworkStudioAudit.md)
records provenance and ownership. LUI owns current Studio UI/workflows, Native
Host owns native resources and IPC, and Engine EditorHost owns authoritative
project mutations/source/history. No Studio or Node campaign was modified.

Core 3L correctness is qualified under D03 `POOLED_CORRECTNESS_V1`; this does
not relabel historical physical F1 failures, unrun Node32, or deployment
performance/provider certification as passing. Every original measurement keeps
its source and acceptance identity. Physics remains production Box3D/XPBD;
experimental Jolt is not promoted.

Independent source reviews and check results are recorded in
[Reviews.md](Reviews.md). Subsystem corrections and limits are in
[API audit](ApiAudit.md), [renderer/physics audit](RendererPhysicsAudit.md) and
[network/Studio audit](NetworkStudioAudit.md).

## Required reconciliation before merge

1. Obtain exact remote main after Engine integration and record its SHA/tree.
2. Compare changed public declarations, source paths, renderer/runtime contracts,
   build options, tests and the Studio/Node integration contracts.
3. Reconcile every pending claim and new mainline documentation conflict without
   rewriting historical receipts or changing acceptance thresholds.
4. Refresh declaration catalog, inventory and traceability; compile examples,
   run integrity checks and build/verify the site.
5. Resolve independent review findings, then require applicable PR checks/review.
6. Only then merge; inspect post-merge documentation CI and deployed links if
   publication was explicitly requested. CI build is separate from Pages deploy.

Until that external integration gate is satisfied, the deliverable is the
reviewable documentation branch and draft PR, not a completed mainline merge.

## Pre-integration validation

- Complete inventory: 250 documents, including notices and text receipts;
  47 current, 16 incomplete, 58 historical, 5 routing duplicates and 124
  unverified. Unknown historical review dates remain unknown.
- Declaration catalog: 67 classes/services, 440 members and 214 native header
  entries. Source hashes use canonical UTF-8/LF text so Windows and Linux agree.
- Six Python checker regressions and five Node link-transformer regressions pass.
- A real Luau compiler accepts all five standalone files and three current
  document examples (eight total), without executing gameplay.
- Locked Bun 1.3.13 / Node 24 / Astro build produces 34 pages; the generated-site
  checker verifies routes, anchors and local repository targets. Final exact
  link count is recorded in the validation command output/PR.
- Current and historical repository-link checks report zero unresolved targets.
  Twenty historical sibling links now use original source pins, preserving
  dirty-state caveats; three receipt links had only their anchor spelling fixed.
- No production runtime source, native tests or other campaign state is changed.

Documentation PR CI must also pass at the published head. Syntax and source
review do not establish full Engine typechecking, runtime gameplay execution,
new graphics support or physical/provider/network performance qualification.
