---
status: integration-in-progress
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

## Qualification and retirement

Run existing prepared failure/allocation sweeps, defaults/reset, EditorHost,
schema, journal/replication and lifecycle regressions, plus focused pinned-record
and deployment-guard regressions. Use the required Windows/Linux/GNS CI jobs and
five Release correctness workloads. Final source identities and terminal CI are
reported in the PR/handoff without a self-referential receipt-only commit.

Property ancestors are retirement candidates only after acceptance into main,
open-PR reconciliation and worktree ownership checks. This task deletes no source
branch or worktree and stops at the qualified PR boundary, without merging.
