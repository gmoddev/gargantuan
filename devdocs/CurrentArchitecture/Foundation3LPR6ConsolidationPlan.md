---
status: proposed-not-executed
owner: runtime-networking
reviewed: 2026-10-20
---

# PR6 history consolidation preparation

## Actual comparison boundary

PR6 targets `foundation/3l-content-availability`, not `main`. Its verified base
and merge base are `eb2ce26ab1455670f4e7b53426cc3521403110c5`. At the start of
this closeout, head `4bf317c04979cce200510f8780ce0aa41cbc7301` contains428 commits
above that base. GitHub's abbreviated100-item commit display is not the count.
Do not pull/rebase from another branch or retarget the PR as part of cleanup.

## Recommendation: GitHub squash merge is sufficient

Prefer one eventual squash commit, **Finalize Foundation3L bounded networking
and non-real-time correctness contract**, into the PR's actual base. It preserves
the reviewed final diff without a preparatory force push. The PR discussion,
accepted ADRs and retained evidence carry the engineering decisions; hundreds
of intermediate timing/CI correction commits need not enter the target history.
This plan authorizes no merge, squash, rebase, force push, tag or release.

If maintainers require several independently reviewable commits instead, use
these four logical groups in a separately authorized interactive rebase:

1. Bounded structural planning/admission, peer rotation, gameplay and recovery
   integration, including demonstrated source-production corrections.
2. Pinned GNS native-positive accounting, packet/reference and ACK ownership,
   native-worker/socket lifetime, capacity/retention separation and DATA arbitration.
3. Final D03 product policy, trusted host/profile composition, diagnostic timing
   and mandatory correctness/compatibility regressions.
4. Existing CI, package, physical capture/lifecycle/security tooling and canonical
   records, preserving the original failed-run receipts and deferred release scope.

These are final-diff/hunk groups, not a blind cherry-pick of commit subjects.
Repeated compiler fixes, first-fault trigger fixes, parser/schema corrections,
retry/observer experiments and superseded timing campaigns can fold into their
owning final implementation. Do not delete their surviving tests, production
fixes, instrumentation or historical evidence merely because a commit was temporary.
Meaningful decisions remain in D01/D02/D03 and the ownership decision history.

Concrete examples to fold while retaining their final behavior:

| Commits | Owning final group |
| --- | --- |
| `87eccfd7a` duplicate PR build removal |4: retain the final three-job/five-workload topology |
| `f30e80bac`, `9a70efc43`, `8c5a22327` writer closure, original-log corroboration, bounded artifact reads |4: one final evidence implementation with original failure custody |
| `ed09e7126`, `9ecc96619` hosted appraisal handling |4: retain the final identity-checked implementation, not successive correction commits |
| `3bf749867`, `67d3772a4`, `5839497fd` native-stage/copy/zero-first-send diagnostics |2: retain bounded final instrumentation and historical receipts |
| `76f03b7c5` reversed worker timeline rejection |2: retain this correctness assertion; consolidation does not delete it |
| `43504d3d5`, `b420a7fd1`, `70088e119` final product policy, static assertion migration, full-log offline replay bound |3/4: fold the three-string correction and bounded replay change into the final policy/evidence implementation |

The closeout's execution source isb420a7fd1 (430 commits above the actual base);
reviewed analyzer70088e119 is431 above it. Later closure-only documentation does
not change the native payload or replace either execution identity.

## Provenance before any later rewrite

- Retain the original head/base and complete ordered commit listing, plus a Git
  bundle of the original PR range. Verify it can resolve original execution and
  analyzer commits before deleting an old branch/ref. No tags are needed.
- Keep raw source/contract receipts, original GitHub job metadata and artifact
  ZIPs/JUnit, workload logs, local native fault tests and historical physical
  captures. CI artifact expiration is independent of Git history retention.
- Preserve original `SourceCommit`, native/package/analyzer identities, failure
  run IDs and hashes. Do not replace them with a squash SHA or call a historical
  timing FAIL a PASS under the new contract.
- Record final tree identity and intended base immediately before consolidation.
  A rewrite changes commit identities even if every file is identical. Existing
  exact-source receipts certify their recorded source; tree equality establishes
  payload correspondence, not a test execution on the new SHA.
- If the base or runtime payload changes, run affected correctness checks and the
  required exact-source cohort on the resulting candidate. Interactive rebase
  therefore costs an additional reviewed-source CI cycle; it is not preferred.
  A later squash merge's normal required merge/main checks retain their own SHA.

## Current disposition and next action

PR6 remains draft and history is intact. The [core receipt](../../tests/Foundation3LCoreAcceptance20261021.json)
is PASS: KI-006 CLOSED and core Foundation3L COMPLETE. Physical Local32/real-TLS
Node32 deployment certification remains explicitly deferred. Original API/ZIP/log
evidence and the verified pre-consolidation Git bundle are retained under
`C:/Users/aiden/.codex/artifacts/foundation3l-nonrealtime-closeout`.
The source43504 cohort's failed tooling check and skipped workloads remain FAIL;
sourceb420's successful cohort is a separate execution. The existing descendant
analyzer path records70088 without claiming native binaries ran at that SHA.

**Next action before merge:** human review of the final diff, exact core receipt,
deferred release scope and this squash plan, followed by explicit authorization
to mark ready and perform the eventual GitHub squash into the actual PR base.
Satisfy the repository's required prospective head/merge checks at that time;
a changed runtime/base requires fresh affected qualification. No consolidation,
force push, merge, tag, release or Foundation3M work is performed here.
