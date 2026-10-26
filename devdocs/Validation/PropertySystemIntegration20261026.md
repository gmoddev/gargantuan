---
status: integration-candidate
owner: runtime-authoring
---

# Property integration into the qualified 3L baseline

This is an integration/provenance record, not a new architecture authority.
Existing prepared-property, concrete-default, reset, transaction, and EditorHost
contracts remain authoritative.

## Source map before implementation

| Source | Identity | Disposition |
| --- | --- | --- |
| Qualified main | `81fca069c1a0bf2aad1aa10eaf480260f5137293` | Base; retain immutable pinned journal records and `POOLED_CORRECTNESS_V1` core closure. |
| Prepared foundation | `8cb1d75192ecc99d7004a84be23673226a8e1484` | Reuse bounded coordinator, preparation failure atomicity, allocation-free COMMIT, and semantic Undo/Redo. |
| EditorHost batching | `04e1353edfe24ffc9d23968f8791afaf245f8ee8` | Reuse negotiated batch V1, whole-request authority/identity checks and prepared success response. |
| Reset/default design | `a10779015186b5d31416bdb44d72920fb6ec7bbc` | Accepted ancestor; implementation below supersedes its recorded blockers. |
| Concrete defaults | `5035d00d19e325058a3fcdb57d143057142e8371` | Reuse classgen literals, frozen sparse metadata and canonical concrete/declaring resolver. |
| Authoritative reset | `7ff53be396221f225fdbfb375d0295353a6291ab` | Primary complete implementation; contains all preceding source commits. |
| Studio architecture docs | `1e067f2f32e904130c5065022974a71ad2f31ab6` | All three commits are ancestors of the primary source, which also corrects their implementation status. No separate replay. |

The primary source adds 53 changed paths relative to its common ancestor with
main. Its property/default/batch implementation is absent from main; historical
standalone Windows/Linux qualification remains evidence at its original source,
not certification of this integration. The source branches and open PRs remain
untouched. Original author/commit identities are retained through those refs and
the independently verified repository-preservation bundle from 2026-10-25.
The 21 original commits are authored by `Not_Lowest
<66854785+gmoddev@users.noreply.github.com>`; the coherent integration import does
not rewrite those commits or attribute their historical CI executions to itself.

## Ownership adaptation

Main retains `shared_ptr<const ChangeRecord>` records. The old prepared candidate
uses `deque<ChangeRecord>`, making its copy assignment and COMMIT swap incompatible
with main. Adapt that candidate to shared immutable handles and allocate each new
record during PREPARE. Existing retained handles remain shared; prepared records
are never installed or externally visible on rejection. COMMIT keeps the journal
lock through state/history/revision installation and performs only nonthrowing
container swaps and sequence updates. Pinned readers survive later publication,
eviction, clear, and scope retirement without mutable aliases.

No second registry, mutation authority, transaction manager, journal format,
replication protocol, or scheduler is introduced. Existing value-returning reads
remain independent copies. Defaults stay in the frozen canonical class schema;
reset applies through ordinary checked mutation or the existing prepared batch.

## Compatibility and scope

- Preserve explicit schema-v7 opt-in and parameterless/explicit v6 compatibility.
- Retain whole-selection batch bounds, generation/scope/revision checks, no-op
  normalization, one-action publication, and captured-value Undo/Redo.
- Studio v7 validation/cache and single/selection reset UI remain deferred
  consumer work; they are not necessary to preserve existing v6 editing.
- Preserve KI-006 core closure and all original Foundation 3L acceptance records.
  Physical/provider and performance certification remain deferred.
- Prevent automatic Pages deployment while preserving intentionally requested
  publication from main. CI qualification never authorizes deployment.

## Focused integration qualification

The supplemental Windows worker tested source
`805b2bb09090fa1512aa4c624ecaa94af879fb4b`, comprising the import/adaptation and
manual Pages guard. Subsequent candidate edits only update documentation links
and this integration record. MSVC 19.44 Release, GNS disabled, six build jobs:

- Five affected CTest targets passed: prepared property, concrete defaults,
  foundation, replication, and replication relevance (14.16 seconds total).
- Existing preparation/allocation sweeps passed with 34/44/49/51/36 rejected
  allocation positions across their five modes. Thirty COMMIT boundaries passed
  with allocation denied, including forward/Undo/Redo pinned-record coverage.
- Pinned readers retain the same old record handles across prepared publication;
  copied value reads cannot mutate them. They remain valid after replay,
  eviction, clear, and authoritative world retirement. Rejections preserve exact
  retained handle identity, and the reader fence covers `ReadPinned` directly.
- Concrete defaults covered 179 properties / 16 overrides, 20 native reset
  cases, single-editor reset, four selection modes, 256 writes, and 257-write
  rejection. Existing schema, stale-generation/project, authorization, conflict,
  partial-preparation failure, replication, and repeated-reset tests passed.
- All seven workflow/JUnit regressions passed, including the real CTest
  subprocess case and manual-main-only Pages condition. All workflow YAML parses.
- The documentation smoke build produced 19 pages with Node 24.19.0 / Astro
  7.2.10 after refreshing an inconsistent isolated dependency copy. This is a
  local build smoke, not a new dependency-lock or deployment certification.

Remote source/build cache paths remain
`C:\Sandbox\Codex\Workspaces\gargantuan-prepared-property` and
`C:\Sandbox\Codex\Builds\gargantuan-prepared-property\msvc-release`. The missing
source was restored without deleting the existing build/dependency cache;
all 35 recursive dependency pins match. The two historically tracked Animator
generated files differ after classgen only in formatting, without semantic
changes. No generated source is imported from another worktree's dirty state.
Raw focused XML/logs are retained in
`C:\Users\aiden\.codex\artifacts\property-integration-20261026` and worker
`C:\Sandbox\Codex\Logs\property-integration-20261026`.

The final readiness gate is the existing Windows Release, Linux ASan/UBSan, and
GNS ASan/UBSan/LSan PR CI jobs, including all five Release correctness workload
completion records. PR CI records its actual GitHub merge checkout. Before
readiness, compare that checkout's tree to the candidate tree and confirm the
base identity. The final source identities and terminal CI results belong in
the PR/handoff without a self-referential receipt-only commit. PR evidence does
not substitute for manual package, physical, provider, or performance certification.

## Exact-head retirement plan

These are candidates after this integration is accepted into main. No deletion
is authorized by an unmerged candidate or green CI alone.

| Remote branch | Expected remote head | Remaining protection before retirement |
| --- | --- | --- |
| `prepared-property-foundation` | `8cb1d75192ecc99d7004a84be23673226a8e1484` | Active prepared-property worktree. |
| `feature/editorhost-property-batch` | `04e1353edfe24ffc9d23968f8791afaf245f8ee8` | Active batch worktree; base of open PR #4. |
| `architecture/property-reset-default` | `a10779015186b5d31416bdb44d72920fb6ec7bbc` | Head of open PR #4. No corresponding local branch. |
| `feature/concrete-property-defaults` | `5035d00d19e325058a3fcdb57d143057142e8371` | Active concrete-default worktree. |
| `feature/authoritative-property-reset` | `7ff53be396221f225fdbfb375d0295353a6291ab` | Active authoritative-reset worktree and primary provenance source. |
| `docs/studio-authoring-ux-architecture` | `1e067f2f32e904130c5065022974a71ad2f31ab6` | Head of draft PR #3; active local worktree holds earlier `25c4da3d72c18d473bffcd21c095189d255fb470`. |

The other four local branches match their listed remote heads. Tracked state in
the five active source worktrees was clean at inspection; untracked ownership
must still be checked before any later worktree action. The original user main
checkout has unrelated changes and is outside this integration.

After acceptance: reconcile superseded PRs #3/#4, recheck every exact remote and
local head, obtain worktree-owner disposition, verify uncommitted/untracked work,
and retain the original source/evidence refs or preservation bundle. Check the
merged implementation against the source map rather than using ancestry alone:
this integration deliberately imports a coherent final tree. Delete only refs
whose expected identities and dependencies still match; preserve changed or
uncertain refs. No bulk pruning. The Studio repository's similarly named branch
is a separate source and is not a deletion candidate in this Engine plan.

Studio review confirmed that selection/layout/buffers stay non-authoritative,
Engine owns history/defaults, and multi-object/reset UX requires this bounded
Engine surface. Existing schema-v6 editing remains compatible. Validated v7
cache consumption and reset UI are deferred Studio implementation, not missing
Engine correctness. This task stops at the qualified PR boundary without merging.
