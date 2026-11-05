# Cross-foundation geometry and streaming qualification

This record separates actual source-specific evidence from the accepted design.
The Windows worker code is `de0cf4850eda262075d0741219b795508a30b10c`, tree
`f55ff8c2e93678dfbf0a84a8e7540b80400beaa8`. All 18 strict worker executables
pass. [Draft PR16](https://github.com/gmoddev/gargantuan/pull/16) records current
hosted checkout identities, terminal results, final measurements and KI-3M-001
closure status. Documentation-only delivery changes do not change the compiled
inputs of this worker code; their separate identity must remain explicit.

## Implementation and authority

PackageBuilder derives conservative package-space aggregates from the exact
captured canonical Mesh, explicit Collision companion and inherited Box geometry.
It validates finite vertex coverage, rigid transform/scale and outward rounding,
aggregates every supported member, and retains an entire unsupported subtree in
bootstrap. Skinned, initially unanchored, malformed, unresolved or unindexable
geometry cannot silently disappear from a partially bounded unit.

Manifest v1 CoarseBounds remains the representation; namespace, logical unit
identity, payload digest, dependencies and immutable package semantics remain
unchanged. Build and full Validate/Load independently rederive geometry coverage
from pinned bytes. An internally rehashed historical format-2 underbound is
rejected by both Validate and Load, before official runtime admission.

3M owns desired demand; 3L owns byte verification, dependency retention, admission
and eviction. 3K/3H remain derived runtime projections, 3E owns relevance and 3J
owns accepted materialization. Standalone MeshPart roots now use geometry AABB
distance for broadphase and all exact enter/leave predicates. Character-rooted
descendants and ordinary Parts retain their existing policy. Revision-aware
shared geometry caches prevent vertex scans during peer evaluation and ordinary
pose updates. Unknown geometry has an explicit bounded fallback only for peers
with a trusted focus. No new wire registry or client authority is introduced.

Current canonical geometry is distinct from an old native shape retained after
a failed projection update. The independent lifecycle test reproduces canonical
-400 -> +200000 native rejection -> +400 recovery, checking retained old queries,
current canonical relevance, successful replacement and retirement separately.
Native readiness does not become a content or Known authority.

## Actual Windows acceptance

The strict fixed recipe passes all four groups at the exact worker source:

| Group | Result | Run receipt |
| --- | --- | --- |
| Packaging, mesh relevance, snapshot proof, full streaming | 4/4 | `964a8683-1d0a-43ad-9cb2-b73b7545ed90` |
| Independent bounds, consumer, lifecycle, authoring, performance | 5/5 | `f50a789d-262f-4502-9229-00c8e70f70d7` |
| Startup, streaming scenarios, metadata, diagnostics | 4/4 | `7dd149a3-d01c-4c18-aa99-8e4778629207` |
| Asset foundation, solid geometry, physics, existing relevance, GameSession | 5/5 | `e1f5d4eb-c164-42b6-8923-87db60ed9fff` |

These fixtures use actual OBJ and Boolean imports, primitive and shared meshes,
multiple units and dependencies, save/reopen, Build/Validate/Load, and the local
package provider after removing authoring source. They check trusted focus
movement/teleportation, physics contact/query, renderer publication, accepted
off-pivot client materialization, release/reacquisition with fresh identities,
revision/dependency denial, replacement, disconnect and zero native resources
after retirement. The transport is simulated; GPU and physical-network behavior
are not measured. The independent lifecycle fixture uses handwritten CAS units;
writer qualification comes from the separate bounds/consumer/full-flow tests.

EditorHost tests pass 14 checks for existing catalog access, exact token and
capability denial, stable project state and declared local Mesh/Collision/bind-pose
inspection. Manifest package-space diagnostics remain bounded by existing caps.
Declared asset bounds are labeled declarations and are not producer proof.
No Studio/LUI source or UI is changed.

The incremental worker build passed in 96.109 seconds with two jobs and 124
classified generated outputs. The approved 90-file executable/runtime/sample/
fixture seal is `edf90b3077acab9a5f64fca7235a04035a3055fa5ca08c3a70b4df09770d4365`.
Source export SHA256 is `1510b3862d244b09fb8bd35d931bb416d1bdca5315671f3ece79913fade3fc15`;
its 1,373-file /31,650,762-byte census is
`70efc459ae049c21fbdf6f21779b79f4244bff8b525c937bfd9fc57c4aa9ced4`.
Build pins are `3d5f30bce78cc344ab5fc660c9e6e2a0869f7c9f130a9678bc1e28983bfe7e73`.
Supplementary modes have separate operator/input seals and must preserve all
original 90 binary inputs; their receipts are not substituted for these runs.

## Performance boundaries and preserved baseline

Diagnostic baseline `54b3530b6705f424a241c0413e65a433402519b0` preserves the
96a88 composition and uses the identical candidate benchmark file, Git blob
`23c3e7c21ac1da23ff5aa4fb472cf36c58cc8d59`. Each of three separate processes
reproduces the historical underbound. This is evidence of the old gap, not
qualification of baseline bounds. Workload: 4,225 vertices, 8,192 triangles,
128 MeshParts sharing one Mesh reference, 174,737 OBJ bytes and 450,227 canonical
Mesh/Collision artifact bytes. Fresh imports retain their actual per-run IDs and
digests; identical geometry does not imply identical Mesh artifact bytes.

| Baseline wall scope | Median microseconds | Range |
| --- | ---: | ---: |
| Compound OBJ import | 16,722 | 16,429–17,640 |
| Capture | 12,603 | 12,214–12,712 |
| Build total | 215,074 | 215,046–221,311 |
| Build Snapshot | 344 | 338–347 |
| Build Validate | 5,302 | 5,239–5,343 |
| Build StageRuntime | 119,262 | 118,933–121,138 |
| Build StageContent | 8,030 | 8,001–8,247 |
| Build HashManifest | 81,805 | 81,438–85,520 |
| Build Finalize | 615 | 605–639 |
| Separate Validate | 82,717 | 80,526–82,873 |
| Separate Load including validation | 81,601 | 80,852–82,583 |

Median OS root-process CPU was 468.75 ms and median peak working set was
23,404,544 bytes (22.32 MiB). The implementation root independently verified the
92,870-byte baseline ZIP SHA256
`378bd4babd5396d3e4eae83b687a701d7b6505348224a724c1ea3971db494821`,
all 37 inventory lengths/digests and three actual source/pins/exit/reaping records.
The corrected candidate passes geometry coverage in all three separate processes;
each reports `captureBoundsCoverGeometry=1`, `manifestBoundsCoverGeometry=1` and
`underboundDetected=0` with the same geometry/count/byte workload.

| Scope | Baseline median | Candidate median | Observed change |
| --- | ---: | ---: | ---: |
| Compound import | 16.722 ms | 18.042 ms | +1.320 ms |
| Capture | 12.603 ms | 18.197 ms | +5.594 ms / 44.4% |
| Build Snapshot | 0.344 ms | 5.616 ms | +5.272 ms |
| Build total | 215.074 ms | 235.046 ms | +19.972 ms / 9.3% |
| Separate Validate | 82.717 ms | 92.539 ms | +9.822 ms / 11.9% |
| Separate Load | 81.601 ms | 92.197 ms | +10.596 ms / 13.0% |
| Whole-process OS CPU | 468.750 ms | 515.625 ms | +46.875 ms |
| Peak process working set | 23,404,544 B | 23,465,984 B | +61,440 B / 0.26% |

Candidate ranges are Capture 17.994–18.308 ms, Build 232.711–237.217 ms,
Validate 92.081–94.589 ms and Load 91.212–92.818 ms. The subtree-inspection subset
is 2.570–2.662 ms, median 2.587 ms. New proof work is visible in Snapshot/Capture;
these small before/after samples do not isolate every integration cost or prove
that import/runtime-copy variation is caused by the bounds producer. Artifact
bytes and unit payload bytes remain 450,227 and 81,157; total package bytes increase
by 39,509, including changed runtime executables, not just bounds serialization.

The separate native quick benchmark uses 128 triangles with 1 and 100 consumers.
First mesh creation is 57.5 and 34.4 microseconds; additional shared creation at 100
consumers has median 1.7 microseconds. First replacement is 54.0 and 28.6 microseconds;
remaining shared replacements have median 2.7 microseconds. Per-shape retirement
has median 1.1 and 0.1 microseconds. Shared native mesh bytes are 4,872, replacement
peak is 9,744, and active native resources/consumers/shapes return to 0. These
finite synthetic native scopes are distinct from CAS admission and full-flow
clocks, and exclude unknown native per-shape storage.

The policy quick benchmark covers 1K/10K metadata entries and 1/10/100 focuses.
Its 10K/100-focus retained steady phase reports zero allocations over 12 samples;
the all-moving harness phase averages 167.358 microseconds (max 177.3), with 5,832
allocation calls and 178,096 peak requested bytes in that phase. This phase
includes fixture focus updates and policy work. A separate in-memory service
scope observes 8 acquisitions/admissions; it is not disk-provider latency. Existing
large-world baselines are preserved rather than replaced by these quick cases.

The final 262,690-byte candidate evidence ZIP is SHA256
`653aafbbd12033b306ac720830be0cd0393831ef2f26d7a768d8c7706c1e33d0`.
The implementation root independently verified all 125 inventory lengths/digests,
the 18 strict exit 0/reaped receipts, three performance process records and both
supplementary benchmarks. Adding the two benchmark executables preserved every
original 90 inputs byte-for-byte; the final 92-file seal is
`59c6d090eba8bf263ff47bddfeb94cabeaeecb6ea9be060ed5cb176236706d67`,
with supplemental pins
`771ddfa32f78e4bc4b7879be37cb9a8c32b49cffe5fe5c85cef4f49872e276b9`.
The original 90-file operator/pins/input manifest remains preserved separately.

`SpatialInspectionDuration` measures only top-level subtree inspector calls in
Capture, including lazy first decode. It excludes snapshot/resolver setup, unit
encoding and later proof passes. Build phases partition that same Build total;
explicit Validate and Load are additional calls. Admission/reacquisition includes
synchronous native creation during parenting, while eviction includes native
retirement during destruction. The subsequent `post*PhysicsStepUs` clocks measure
safe-point updates, not isolated creation. Existing bounded collision and policy
benchmarks supply separate scopes; they are not added to full-flow observations.

OS CPU/peak working set are whole-process observations, not per-stage allocation
or Job-tree totals. Native mesh bytes exclude unknown shape storage/builder
scratch; collision retained-artifact bytes and logical 3E cache estimates have
different scopes from RSS. The legacy 3E UpdateCpuNanoseconds counter uses a
steady clock. Three processes provide ranges and medians, not latency percentiles,
long-duration memory guarantees or physical-network certification.

## Independent review and original failures

Independent producer, consumer/security and lifecycle source reviews found no
remaining blocker after correction. They checked captured identity/closure,
Collision extents, rigid transforms/rounding, exact returned bytes, bounded
fallback, no client authority, and resource lifecycle. Performance review corrected
wall-time attribution and required an absent-before-focus gate plus a real 3E
evaluation before retention assertions.

Original failed attempts remain failed evidence: inherited PATH overflow before
compilation; temporary copy/staging paths; an MSVC authoring lambda argument;
a sample with no independently eligible static unit; a Box-only mesh verifier;
and a client fixture lacking trusted LocalPlayer. Minimal corrections preserve
the production 240-character path guard, all assertions, native ownership and
client authority. The readonly verifier correction has independent static review
and passes the unchanged focused test. No failure is relabeled success.

Hosted source716 records these first causes independently: Windows fails on the
same authoring lambda; Linux ASan/UBSan runs 84 tests with the three known fixture/
verifier failures and zero skipped; GNS ASan/UBSan/LSan passes 11/11. Corrected
source results are recorded separately in PR16 with actual merge-checkout tree
equality. Cancelled earlier attempts do not count as passing qualification.

## Worker ownership, delivery and continuation

Only the independent Studio chat operates dockerbox. It reconciled the expired
migration hold by bounded actual workload inspection and acquired host-local
generation 2 at `2026-11-04T23:52:42.735Z`, renewing while owned work runs. Tokens
remain private. Native Jobs run hidden and are checked/reaped before release.
Checked release completed at `2026-11-05T01:09:43.679792Z`: generation 2 is Free,
the task-owned process census is empty, and all 55 native Jobs across successful
and failed attempts are checked/reaped. Caches remain preserved, global settings
are unchanged, and no persistent task process remains. The independently checked
cleanup/release JSON SHA256 is
`879289295a2feccf04f95e3e3c1a47f01374e688ee15b50abb98c08846b73fac`.
No global daemon repair or unrelated process termination was performed.

Source is under `C:\Sandbox\Codex\Workspaces\cross-foundation-candidate`, build
under `C:\Sandbox\Codex\Builds\cross-foundation-candidate\msvc-release`, and
evidence under `C:\Sandbox\Codex\Artifacts\CrossFoundation`. Baseline paths use
`cross-foundation-baseline`. The private persistent vendor cache census covers
19,986 files /547,078,614 bytes, SHA256
`d1af2ce5e9d162fbcbdfe505b40fe5bd4ecede76919f2741dd127601974618a8`.
Incremental source/dependency state is preserved. This implementation root runs
no parallel host workload or sustained local compilation.

Exact upstream identities and preserved histories are in
[the dependency ledger](CrossFoundationDependencyLedger.md). PR14/PR15 remain
draft dependencies; no owner branch, Studio/LUI, main, release or deployment is
changed. POOLED_CORRECTNESS_V1 and KI-006 remain protected; historical physical
Local32/Node32 certification is not restarted. Conservative static package bounds,
Character-rooted relevance, bind-pose fallback, retained native failure state,
simulated transport and unmeasured GPU/allocator scopes retain the limits stated
above. Follow PR16 and the source-specific evidence/checkpoint for continuation;
do not restart historical baselines or bypass the worker lease.
