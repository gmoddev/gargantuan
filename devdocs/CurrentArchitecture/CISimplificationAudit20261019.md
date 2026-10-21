---
status: implemented-pending-original-ci
owner: build-and-test
date: 2026-10-19
related_code:
  - .github/workflows/native-ci.yml
  - .github/workflows/gns-sanitizers.yml
  - CMakeLists.txt
  - tests/test_ci_junit_output.py
  - tests/test_scheduler_trace.py
---

# CI simplification and qualification debt audit

## Scope and decision

Baseline PR6 is OPEN/DRAFT on `codex/3l-capture-lifecycle-qualification`,
head `899d77caeb38c8e34cc236fbabb887eceb9270fa`, base
`foundation/3l-content-availability`. This change preserves ancestry and all
production code/contracts. The audit worktree is attached to that exact branch.
The older local checkout's branch is preserved as
`codex/3l-capture-lifecycle-qualification-local-68ee4f35e`, at its original
commit with its D01 edit and untracked file untouched. No unrelated checkout
is pulled, reset, cherry-picked or pushed for this audit.

Keep all three native configurations and all five real workload modes. Remove
duplicate branch-push execution, the Linux dependency, routine ETW and two
identical mock invocations. Retain existing authoritative readers rather than
introduce a new proof inventory or verifier layer. No impact/path filter is
introduced without a sufficiently complete dependency graph.

## Measured baseline

All six original jobs at899d77 passed, attempt1. Native push38849211275,
Native PR38849216574, GNS push38849211295 and GNS PR38849216630 retain their
original results. Push checked out899d77; PR checked out synthetic merge
`18df443369b27383f4d2a748b064f51b37bdd728`. GitHub commit/tree metadata and
original checkout logs establish identical tree
`399e46f30bd73a5efc18a395f614b3650686e5bb` at this baseline. That equality
is measured here, not assumed for future base/head combinations.

| Job | Push duration | PR duration | Uploaded artifact bytes, push / PR |
| --- | --- | --- | --- |
| Windows Release | 51m37s | 60m50s | 43646507 / 43975709 |
| Linux headless ASan/UBSan | 26m58s | 36m11s | See pinned metadata below |
| Linux GNS ASan/UBSan/LSan | 27m57s | 21m35s | See pinned metadata below |

The complete six-job cohort used13508 job-seconds (225.133 runner-minutes),
with87875136B uploaded. The branch-push half used6392s (106.533 minutes)
and43779598B. Removing that half is a measured baseline comparison; future
runner speed, billing multipliers and cache effects are not measured savings.
Native Windows→Linux elapsed spans were78m38s push and97m04s PR. With the
same observed job durations, parallel scheduling would have a longest job of
51m37s /60m50s respectively; that is arithmetic, not a post-change measurement.

Windows build took25m19s /30m37s, configure6m03s /8m14s, CTest4m32s /4m55s,
Farm32 tooling4m08s /4m48s, and traced reliable workloads6m53s /7m14s.
The Windows original logs are about46.7MB:23762 RemoteResourceSpan rows
account for34.363MB and23762 RemoteSpan rows for10.987MB in the inspected
push log. This change does not remove those fixture diagnostics and must not
claim their console cost was eliminated.

Prior cf45 independently shows the same-tree duplication:211.333 runner-minutes,
89028842B uploaded. Its two native ZIPs expand to295.620 /296.578MB; their
five ETLs account for84.083 /84.345MB expanded,19.853 /19.825MB compressed,
plus decoded CSVs. Current routine jobs no longer produce those trace sets;
actual new artifact size and duration must come from the fresh original cycle.

Raw metadata/logs, per-step and artifact measurements:
`C:\Users\aiden\.codex\artifacts\ci-simplification-audit-20261019-v1\measurements.json`,
SHA256 `dde2645b12f84d8bde68b7ac4e8bf4235f472a0dad67aa5fac3d5d10f6d3fff5`.
The audit uses original evidence, not new physical runs or timing experiments.

## Coverage and cost ownership

| Decision | Job / group / verifier | Real coverage and stage | Cost / failure consequence |
| --- | --- | --- | --- |
| KEEP | Windows Release complete CTest | Runtime, security, generation safety, native attribution, ACK/retirement, packaging, deterministic D03; every PR/main/full dispatch | Cold native build dominates; test failures remain fatal and raw JUnit uploads on failure. |
| KEEP / CONSOLIDATE | Linux headless sanitizers | Independent compiler/platform and core memory safety; every PR/main/full dispatch | No Windows inputs. Runs independently and retains evidence even if Windows fails. |
| KEEP | GNS sanitizers, retry safety and profiled admission | Native reference/retry/lifetime safety and GNS integration; every PR/main/full dispatch | Different from Release timing and GNS-disabled headless tests. No blanket removal of tests labeled optional. |
| KEEP | Five standalone Release workload modes | POOLED Full, POOLED structural32, FULL_RESERVATION Full, ordinary32, structural32 | Same original native argv/order/nonzero guards and600s per-case bounds via five fixed10-minute steps; unchanged90-minute job bound. Not duplicated by CTest. |
| MOVE TO MANUAL | Scheduler native helper, ETW/decode/coverage | Causal attribution, not a product latency predicate | All three explicit diagnostic inputs are nonqualifying; no package/Linux qualification. Noncapture Python regression remains routine; absent-helper tests report skips. |
| CONSOLIDATE | Branch push plus PR | Future PR merge checkout covers integration; main pushes cover the merged commit | Three automatic jobs per PR update instead of six. A branch with no PR has no automatic full run; use manual dispatch. Stable check names remain. |
| REMOVE | Second worker/client capture-mock calls | CTest already runs exact same scripts/arguments | Retain `gargantuan_farm32_worker_capture_hook` and `gargantuan_farm32_client_capture_source`; remove only duplicate workflow calls. |
| MOVE TO MANUAL | `gargantuan_gns_service_reproducer` | Historical investigation executable, no CTest/canonical consumer | EXCLUDE_FROM_ALL only; source and explicit target remain available. |
| KEEP | GNS second incremental build | Detects generated-header/instrumentation dependency cycles | A superficially repeated build protects a distinct dependency-graph fact. |
| KEEP | Source/contract/JUnit/archive checks | Original run/attempt, actual checkout, bounded artifact integrity | One `VerifyCI` call already consumes three jobs/two workflows. No eight-job requirement or new inventory version needed. |
| KEEP MANUAL-RELEASE | Official package and native projection validation | Closed deployment/runtime inventories, ABI/assets and exact-source package | Already manual-only; no new build/copy job. Package must come from the verified manual Windows job. |
| KEEP | Farm32, offline replay and provider readers | Authority, source/deployment/lifecycle/capture, independent fairness/recovery/provider conjunction | Similar names cover different trust boundaries. One accepted replay result per immutable input set; re-run only for changed inputs/reader or a demonstrated missing fact. |
| KEEP MANUAL-PHYSICAL | Local32, real-TLS Node32 and final acceptance | Player-facing D03, resources, recovery, provider parity | Separate authorized campaigns; none launched or claimed by this audit. |
| KEEP | Docs deployment | Public site on main only | Draft PR docs remain visible in GitHub; no unmerged site deployment. |

No test/tool source is deleted. Removed execution is limited to duplicate push
cohorts, two duplicate mock calls, default investigation-target compilation and
mandatory routine trace collection/decoding. Historical strict F1 CLI/tests
remain available. Standard CTest explicitly uses HYBRID_F1_V1; packet-scale
running deficits remain diagnostic only there. Hard completion, native history,
FIFO, execution/turnover, fairness, exact conservation, security and recovery
predicates are unchanged.

## Trigger, status and provenance policy

- PR opened/synchronized/reopened: all three checks, including draft PRs,
  without branch-push duplicates. No changed-path shortcuts.
- Main push: all three checks for the actual merged source.
- Default manual Native + manual GNS on one SHA: full exact-source release
  cohort; Native owns the one official package. Existing index versions1/2
  and original failure receipts retain their semantics.
- Any manual scheduler diagnostic input: distinct nonqualifying Windows job;
  no qualified package or Linux evidence. It cannot replace a required job.
- PR concurrency cancels obsolete PR attempts only. Cancellation/skip is
  NOT QUALIFIED; current jobs still need actual terminal success.

Non-PR concurrency keys use the unique run ID, not branch ref. Even with
`cancel-in-progress: false`, GitHub replaces an older pending run sharing a
group; unique keys prevent a diagnostic or later main/manual run from evicting
queued qualification. This queue behavior was caught in independent review
before the one validation cycle; no extra scheduler or queue is introduced.

GitHub protection/rules APIs at the baseline report no enforced required-check
contexts for main, the PR head or actual base; repository rulesets are empty.
No protection setting is changed. The three established check names remain
unchanged so externally configured branch policies do not need renaming.
Linux has no dependency/artifact download from Windows. All existing checkout,
contract markers, JUnit names, artifact names and always-run uploads remain.
Original SHA and merge SHA remain distinct; PR feedback is not branch-source
release/package evidence. The final reader intentionally continues rejecting
PR and diagnostic substitution.

## Documentation visibility and current 3L scope

D01, D02, D03, the ownership decision, KI-006 and the physical ledger are
committed at the verified PR head and visible on GitHub. The final-acceptance
overview omitted the later exact-turnover attribution; this change adds a link
to that committed evidence, without inventing a result or rewriting history.
`docs.yml` deploys main/manual and is unchanged; no public site deployment is
requested. Native f30, qualifier cf45, prior899d77 and the incoming audit commit
keep separate identities.

Local32 remains FAIL, Node32 and final conjunction unrun, KI-006 OPEN,
Foundation3L incomplete. No D03 adoption decision, production behavior,
deadline, transport, physical campaign,3M, release or merge is included.

## Bounded validation and maintenance

Workflow syntax is checked with checksum-verified actionlint1.7.12 (all three
workflows). Targeted tests exercise event/status invariants, exact five-command
ordering and signed failures, retained helper selection, original CI/source/
package and HYBRID-versus-legacy counterexamples. The local JUnit subprocess
test reports SKIPPED when CTest is unavailable; the hosted run supplies the
actual CTest execution. Native-helper tests are explicitly NOT QUALIFIED by
routine absence, not reported as passed.

Run one fresh automatic PR cycle after the coherent batch. Its three original
jobs and actual checkout/JUnit/contract artifacts, not a documentation PASS
label, own the result. Results are recorded in the PR checks and final audit
delivery without adding another documentation commit solely to qualify that
receipt. One attributable correction may be requalified; unrelated failures
stop the audit with original evidence. No automatic retries or manual duplicate
package cycle are needed for this non-release change.

Full native/sanitizer checks remain required for source, ABI/generated schema,
dependencies, build/workflow, tests or qualification-reader changes. Routine
script/unit results provide focused diagnosis, not replacement native evidence.
Release requires the source-bound manual cohort/package; transport or workload
changes additionally require their separately authorized native/physical gates.
An immutable input replay needs repetition only for a concrete changed input,
changed reader, missing acceptance fact or explicit independent trust boundary.
Do not add another verifier merely to attest an existing verifier's PASS label.
