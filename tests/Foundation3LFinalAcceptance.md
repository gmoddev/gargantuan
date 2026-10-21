---
status: current
owner: qualification
last_verified: 2026-10-21
related_code:
  - tests/foundation3l_acceptance.py
  - tests/test_foundation3l_acceptance.py
  - tests/PhysicalGameSessionFarmAcceptance.ps1
---

# Final Foundation 3L evidence conjunction

## Current core contract — 2026-10-20

The accepted [D03 non-real-time revision](../docs/adr/D03-hybrid-pooled-service-and-permit-ownership.md#final-adoption--2026-10-20-non-real-time-correctness-contract)
introduces `POOLED_CORRECTNESS_V1`. Core completion and physical/provider release
certification are separate scopes. All older conjunctions and receipts below
retain their historical source/contract requirements; none becomes a new PASS.

Core correctness requires exact accepted/native-positive/ACK-retired/terminal
conservation, generation/token/reference ownership, FIFO, security, finite
resources, backpressure, capacity separation, actual DATA/retry selection and
absence of software-created starvation under continuing eligible execution.
Recovery requires exact causal/current client state and complete accepted work.
Wall-clock structural/ordinary/Main/turnover/fairness/gameplay/convergence targets
remain measured diagnostics. Target misses cannot independently terminate peers
or fail the new core contract. Invalid evidence and missing completion stay fatal.

Use the existing reader with `--contract POOLED_CORRECTNESS_V1 --source-commit
<tested-SHA> --ci-index <original-index> --ci-index-sha256 <pin> --output <new-file>`.
It verifies the same three original hosted jobs, source/contract receipts, JUnit,
packaged process-isolated session and all five Release end-of-correctness records.
It does not accept supplied provider PASS labels or call historical physical
replayers for this scope. Missing/aborted mandatory work is not PASS.

**Current disposition (2026-10-21): KI-006 CLOSED; core Foundation3L COMPLETE
under POOLED_CORRECTNESS_V1 / CORE_ENGINE_CORRECTNESS.** The existing reader
returned PASS; no mandatory core correctness blocker remains. See the
[retained machine-readable receipt](Foundation3LCoreAcceptance20261021.json).

| Evidence | Qualified identity / result |
| --- | --- |
| Executed native/package source | `b420a7fd1567632b376760e65fc71386c9bddcf4` |
| Reviewed offline analyzer | `70088e119e5f4f68cb030035d025d5068da63240`; descendant, only replay input-size/test changes after execution source |
| [Native CI39537685852](https://github.com/gmoddev/gargantuan/actions/runs/39537685852) | Windows97/97; Linux ASan/UBSan53/53; all five Release workloads completed with zero correctness failures; package/projection checks pass |
| [GNS CI39537688764](https://github.com/gmoddev/gargantuan/actions/runs/39537688764) | ASan/UBSan/LSan11/11 plus profiled session, production admission matrix and pooled profile PASS |
| CI inventory SHA256 | `be050c699c0e9ecee90cfbbf71f3903d47cf4965d2e5ee780f7e7ff0da38cb34` |
| Final receipt SHA256 | `28a97a812e59177631d91069cf319757d2329b82b53d474b4a068ddb9080edf1` |

Original API metadata, ZIPs/JUnit and full job logs are preserved under
`C:/Users/aiden/.codex/artifacts/foundation3l-nonrealtime-closeout/hosted`.
The five-workload Windows log is46,759,381bytes. The reviewed analyzer reuses
the existing128-MiB diagnostic archive read cap for full job logs; JUnit stays
16MiB. A focused17-MiB/all-five positive and missing-fifth negative regression
pass. Hashes, identities, source and completion predicates are unchanged.
This is the existing execution/analyzer provenance path, not a test execution
on the later documentation commit. Timing measurements remain in the pinned
original log; a passing campaign establishes no unconditional OS service bound.

The initial closeout cohort at43504d3d5 remains FAIL: native97/97 and both
sanitizer suites passed, but a stale scheduler source assertion still searched
the pre-policy `Run()`/`Observe(...)` signatures. All five workloads were skipped.
The three-string correction passed its focused test before the fresh cohort
above; no runtime change or blind performance retry occurred. Original failed
metadata/logs are in `cohort-43504d3d5` beside the current hosted directory.
The41-case production preflight, affected native coverage, separate-process
packaged composition, complete pooled single-peer and pooled structural32
preflights pass. Failed bootstrap and probe sequencing preflights remain retained.

PR6 stays draft. No physical campaign, history rewrite, merge, release or3M
follows from this receipt. The [consolidation plan](../devdocs/CurrentArchitecture/Foundation3LPR6ConsolidationPlan.md)
defines the next human review and eventual squash boundary.

Deferred release/integration work remains explicit: Local32 physical behavior,
real-TLS Node32 deployment/provider parity, physical resources and performance,
and future engine-level performance requalification. Existing native and packaged
provider security/protocol/lifecycle regressions remain mandatory core coverage.
Physical requirements that establish correctness must be satisfied before that
deployment scope is certified; exclusion from core does not certify them.

### Correctness evidence boundary

The revised-profile preflight exposed a real bootstrap integration defect:
separated capacity owned an untimed bootstrap lease without publishing its
token/byte offer to the feedback ledger. Exact ACK retirement then correctly
rejected the still-owned native lease. Bootstrap now publishes the existing
untimed sentinel offer and returns capacity on positive native evidence;
ACK ownership remains independent until exact retirement. The production
regression covers both transitions, duplicate prevention and zero timing
ownership. The real pooled refresh and packaged separate-process session pass
after this correction. The initial failed preflight remains retained.

The strengthened single-peer recovery check also exposed a fixture sequencing
error: callback completion discarded its probe before native ACK reconciliation.
The completed probe now remains available until the next existing scheduled
probe, so exact postpass quiescence can be observed. No ACK, byte or resource
predicate is removed and no timeout is enlarged.

Historical reports left combined pressure/soak campaigns unclosed. Those
campaigns are not retroactively qualified. The core properties have current
focused production regressions, exercised by the required native/sanitizer suites:

| Property | Existing evidence path |
| --- | --- |
| Old removal plan versus destroy/reload before READY | `ReplicationRelevanceTests.cpp`: bounded five-peer planner replaces an unfinished removal input, recreates the object, and rejects old-generation resurrection. |
| Pending/deferred RPC and peer teardown | `RemoteTests.cpp`: unmaterialization/cancellation/remove-peer terminate ownership exactly once; old connection generations cannot inherit state. |
| Acquiring content cancellation and repeated residency | `ContentAvailabilityTests.cpp`:100 acquiring cancellations and100 eviction/reload cycles, exact counters, no authoritative cancelled content, no requests/documents after Stop. |
| Session/bootstrap/admission failure atomicity | `GameSessionTests.cpp`: failure matrix, rejected structural admission destroys that peer in the same cycle, terminal state census,100 connection churn and100 production lifecycle compositions. |
| Native pending I/O versus ACK, failure, close and generation reuse | Existing dedicated native ownership/worker fixtures: ACK-before-commit, negative/partial-positive results, cancellation, exact terminal purge and paused-result lease tests. |
| Native protected/retry fairness and funding | Real encrypted collector fixtures, bounded ready FIFO/class quantum, parent/child wire conservation; native timing is separately diagnostic. |

This supplies the defined core properties without claiming an exhaustive
100-cycle combined physical pending-fan-out/RPC-pressure soak. Such integrated
deployment pressure remains in the deferred release scope. A demonstrated
ownership or lifecycle counterexample would still block core correctness.

The tested pre-revision source4bf317c04 passed Windows97/97, Linux53/53 and GNS11/11
but failed first Release timing on Main152.287ms/turnover164.753ms; its remaining
four workloads were skipped. This remains FAIL under its old contract.

## Current scope and CI maintenance (2026-10-19)

### Capacity implementation checkpoint — 2026-10-20

[The opt-in capacity/ACK correction](../docs/adr/D03-hybrid-pooled-service-and-permit-ownership.md#capacity-separation-implementation-checkpoint--2026-10-20-partial--blocked)
passes focused resource/admission/lifecycle regressions but is **not latency
qualified or enabled by the standard profile**. The actual shared-pacer
counterexample gives 66646us ordinary first-send against 50000us under retained
retry competition; it is a deterministic model, not a physical measurement.
No new contract or reader interpretation is adopted. Existing `HYBRID_F1_V1`
and legacy predicates, historical failures and source pins remain binding.
CI regression success cannot enable the candidate, close KI-006, or substitute
for a native service correction and fresh matching physical/provider evidence.
Local32 remains FAIL, Node32/provider parity/final acceptance unqualified.
No physical campaign is authorized by this implementation checkpoint.

The [2026-10-19 D03 revision review](../docs/adr/D03-hybrid-pooled-service-and-permit-ownership.md#authorized-revision-review--2026-10-19-blocked-on-healthy-peer-progress)
recorded independent healthy-peer admission blocked by ACK-held capacity and
funding. The implementation checkpoint above resolves those two admission
counterexamples only in the opt-in candidate. Protected native DATA service is
still unqualified, so production health and acceptance readers remain
`HYBRID_F1_V1`. Neither relaxing turnover nor reinterpreting old receipts follows
from this partial implementation. KI-006 stays OPEN; Foundation3L NOT_COMPLETE.

The committed [final bounded attribution](../devdocs/FutureArchitecture/Foundation3LNativeEgressOwnershipDecision.md#32-final-bounded-local32-attribution-and-architecture-decision-2026-10-16)
supersedes the missing-predicate status below: f30/cf45's fresh four-client
prerequisite passes, while Local32 `7efff6f1-8d59-4fa8-8cef-c1f27cfa9b80`
fails exact turnover164.511ms against150ms. Node32 and final acceptance remain
unrun. All historical failures retain their identities; KI-006 remains OPEN.

The [CI simplification policy](../devdocs/CurrentArchitecture/CISimplificationAudit20261019.md)
does not change this conjunction. `VerifyCI` already verifies three distinct
jobs from two source-bound workflows; the historical eight-job operational
envelope is not a reader requirement. Default manual Native+GNS dispatches
provide the three-job exact-source cohort, with the package from that same
verified Windows run. No new inventory format or meta-verifier is introduced.
PR merge checkouts and optional diagnostic results are not branch-head package
evidence. All byte pins, source/contract checks, actual JUnit, package projection
and independent provider requirements remain unchanged.

## Latest continuation — actual Local32 FAIL (2026-10-15)

Native source3182, official package961f, native32/recovery and corrected
four-client PASS retain their exact scopes. Tooling head
`bdef7003828553fee2fb2c41f368fb0c60886f8b` passes its six original hosted jobs;
receipt `8bf1c20585b73c0f2d6a16913b6a2462c769f0f3b9941a2b02af65c50dc49dc7`.
Earlier b629 CI FAIL remains unchanged.

Controller RAM and verified worker disk remediation permit actual Local32.
Run73f40b17 fails the capture-start disk reserve before gameplay; preserved
fresh run `ab0df092-fe93-4e6e-918a-2ae448479b3e` then fails during gameplay
on compound D03 execution-health guards. First retained rejection is peer13:1,
stderr line627; exact first Main/turnover/invalid predicate, elapsed interval
and execution owner are unmeasured. No native F1/publication/work-tick/fairness
summary or native result is retained before the abort. Coarse resource samples
and separately buffered log tails cannot supply the missing certificate.
Diagnostic capture export/owned cleanup do not promote a failed provider.

There is no successful Local host terminal or complete Local conjunction;
real-TLS Node32 was not staged/launched. Final typed acceptance is therefore
not invoked with fabricated or missing provider inputs. The five archived
build trees preserve full cache state; restore BOTH native/quote trees to
their identical C: paths before native build/test work. See the
[physical failure assessment and ranked next steps](../devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md#actual-local32-attempts-and-execution-evidence-blocker-2026-10-15).
**Foundation3L NOT_COMPLETE; KI-006 OPEN; PR6 draft. No retained D03 bound changes.**

## Latest continuation — worker disk resolved, controller RAM external (2026-10-15)

The existing native3182/official package and corrected four-client PASS remain
source-specific prerequisites. Verified archival of24 inactive task-owned
diagnostic builds preserves23206846575B on worker D: and frees actual C: to
38653771776B, above the unchanged36507222016B Farm32 reserve. Raw evidence,
current native source/build and dependency caches remain intact; exact
retirement receipt is
`0657658695a071405dadad76d62fd30b171995dead408aee118263a04b928436`.
The controller's05:28:52UTC free RAM5595303936B remains2994630656B below
8589934592B. No idle task-owned process, compatible alternate placement or
independently justified lower floor resolves it. User-managed free RAM is
required. Fresh inert templates preserve the official source/package and
separate analyzer identities; no Full32 identity, secret or campaign exists.
Collection and retained provider storage need actual-size checks before use.

The later documentation-head b629 Native push CI38637417269 remains FAIL on
the sampler's5000ms initialization bound during injected `Throw` setup;
retained phases reach counter-load completion at5058.023ms. No production D03
failure or correctable defect is established. Prior source3182 qualification
is not relabeled, and all-current-head/final-source CI PASS is not claimed.
See the [resource/CI checkpoint and resumable plan](../devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md#autonomous-full32-resource-remediation-2026-10-15).
**Local32, real-TLS Node32, resource/provider parity and final conjunction remain
unqualified. Foundation3L NOT_COMPLETE; KI-006 OPEN; PR6 draft; no3M or merge.**

## Latest closeout checkpoint — prerequisite PASS, full32 blocked (2026-10-14)

The current source3182 native subset/official CI package and corrected whole
four-client run `a9f5e41e-78f3-414f-a742-3115243a5891` pass. Offline analyzer
dd394 replays the unchanged original run as `MEASURED_PASS`, receipt
`da7b1b93a985bc1c06432efe1b5aca5e845174e34687fbb02c2f1a53d76259da`;
controlling-host completion `2026-10-14T05:08:28.676000+00:00`. Original reader
rejection remains preserved, and qualifier24bfd/analyzerdd394 both pass their
original six-job source/tool regressions. Native, qualifier, analyzer and this
later documentation receipt retain separate identities under the A/B policy.
No native executable is relabeled by a documentation commit.

Fresh full32 readiness fails: client free memory4235452416B <8589934592B;
worker capture disk29550948352B <36507222016B. No resources are silently waived,
no user apps/caches/unverified evidence removed, and no Local32/Node32 started.
Their raw campaigns, provider parity and final conjunction remain absent.
**Foundation3L NOT_COMPLETE; KI-006 OPEN; PR6 draft; no3M or automatic merge.**

## Current measured scope — corrected observer source3182 (2026-10-14)

Source `3182a164d5bf87f5051e11ecbd1c749b73c282dc` passes all eight original
hosted CI jobs and canonical CI/package verification, receipt
`bf7ba58d06e38c8052f0ee628156ea05231398c872f25de341a41a6d9ff39e00`.
Fresh native32 `3102a9ac-ec36-4190-b690-c827957e3441` passes strict whole
available conjunction `MEASURED_SUBSET_PASS`, failures[], SHA256
`933d74cb4a547251ef707a49fd4d34aa1d77133d86b23669cb901d294ebb6700`.
All32 native D03, exact accounting,7424 eligible episodes and all three fixed
service/causal recovery cases pass. Main63.152ms, turnover95.252ms and
ordinaryFIFO9.148ms preserve the original limits. This is same-host native
evidence; it does not supply the physical inputs below. Fresh corrected
four-client, Local32, real-TLS Node32 and final acceptance remain unqualified.
Native execution source3182 and later qualifier/analyzer child remain distinct
under the existing A/B procedure. KI-006 OPEN; PR6 draft; no3M or merge.

## Current physical checkpoint (2026-10-14)

Control-only run `3f1df06a-2e89-4db8-8809-91cb6dad7df9` passes its actual
endpoint preflights and lifecycle. Subsequent four-client physical run
`237be7c6-2c47-42d2-96cc-0f06664116b9` remains FAIL; strict replay rejects
its failed native result. The first invalid record is legal null-feedback
accounting before the first maximum grants, incorrectly classified by the
probe as missing native service. The focused HYBRID fixture correction retains
every observation, separate genuine native evidence and an explicit verified
live-completion cut before owned cleanup. It requires fresh qualification;
old rows without phase/state/ordinal cannot be retrospectively recertified.
See the [physical receipt](../devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md#d03-a7-four-client-continuation-2026-10-14).
Legacy F1 semantics and all existing D03 deadlines remain unchanged. Current
source four-client, Local32, real-TLS Node32 and final acceptance are unqualified.
KI-006 OPEN; PR6 draft; no3M or merge.

## Current measured scope — A7 native subset (2026-10-13)

Native source `5a2e735c96dd2a7983681a090a4610b90772d368`, tree
`03f22a072ccb36bb0b7b1a2700b7b5de7a8c4218`, passes all eight original
exact-source CI jobs and canonical CI/package replay (receipt
`ae41955737123cef16ff3e08048edd27533565cb8948e8e978733a852dd43f9f`).
Its sole same-host native32 run `6aa1c66f-35e2-45c9-8f92-1c7761705641` has
strict `MEASURED_SUBSET_PASS`, failures[], conjunction
`adc5f3d89e74e30bc9d98d4145068390e8f335cf48822f6f50ef38967e602a4c`.
All retained D03 native, ordinary FIFO, execution, fair admission, exact
accounting, fixed service-recovery and causal-convergence subsets pass.
Historical A5 and other failures retain their original source/verdict.

This native subset does not supply the physical inputs below. Same-source
four-client, canonical Local32, real-TLS Node32, physical provider parity and
final acceptance remain UNQUALIFIED. Controller RAM around4.97GiB is below
the full32 client8GiB floor; four-client qualification may proceed separately.
The incoming qualifier-only child keeps native source A7 distinct from its
analyzer/adapter/pin identity and requires its own qualification. Updated pins
are prepared inputs, not a physical PASS. KI-006 remains OPEN; PR6 remains draft.

## Explicit qualification contract

Standard `HYBRID_F1_V1` qualification uses the adopted D03 product contract,
including its2026-10-13 raw-running-curve scope amendment. The retained raw
four-grant/four-client structural checks remain required with explicit matching
contract identity. Legacy running deficits are diagnostic in that profile;
native completion, positive first-send truth, exact packet/reserve/delivery
accounting, reliability, order and lifetime remain fatal. This subset does not
prove full D03: both provider campaigns independently enforce50ms ordinary FIFO,
100ms Main,150ms turnover,1181.25ms fair32 admission, gameplay, resource,
backpressure and canonical recovery/convergence requirements.

The fixed20s service-recovery receipt is independent of the longer C3/C5
structural convergence deadline. The current fixture enrolls probes only from
one matching-case ordinary cessation notice on existing ScalePhaseControl,
after the native host actually ceases production. The client retains its first
receive clock, completed offered handshake and accepted RPC/Event ownership;
duplicate/stale notices cannot reset it and GRPL provides no service fallback.
The server still measures cessation-to-complete-receipt≤20s. Ten RPC/Event
probes, nine inter-offer waits and all latency/error/attempt gates remain.
The replicated marker and exact final Names/source identities remain required
for independent structural convergence, not for enrolling ordinary probes.
Missing notification, late receipt, partial cohorts or missing causal/name
evidence cannot produce PASS. A5's earlier marker-gated FAIL is preserved;
it proves no earlier service result. Explicit V3 source-empty fairness endings
are separately measured from grants; generic `no_work` remains unproven and
legacy evidence is not reinterpreted. The A7 native subset qualifies these
corrections; each physical campaign must still independently qualify the
unchanged guarantees before provider or final gates can close.

Legacy F1 evidence retains its original full curve semantics. An explicitly
selected profile must match the current-source CI, package, four-client and
provider evidence; missing, unknown or mixed identity cannot qualify HYBRID.
Historical failures and the exact retained historical exception below remain
unchanged. No earlier source becomes a current HYBRID PASS through this amendment.

CI diagnostics retain all raw trace sets. The reader bounds the original
compressed ZIP to 128 MiB and its directory to 10,000 members, validates every
path and duplicate, and reads only one JUnit member (16 MiB cap) and one source
marker (128-byte cap). The aggregate expanded size of unread members is not a
reader allocation bound: those members are neither extracted nor decompressed.
All required source, digest, job, test-identity and outcome checks remain.
This offline parsing correction uses the A/B procedure below; it does not
relabel the already-qualified native candidate or require dropping raw traces.

CI inventory version1 and legacy F1 keep their strict API-step requirement.
Explicit HYBRID inventory version2 may corroborate a unique required step whose
raw API state is `pending` with a null conclusion, only when the exact-source
run and job are both terminal successes. Missing, duplicated, failed, skipped,
running or unknown required-step states cannot use this path. The original
metadata is never rewritten; the result records its pending step verbatim.

The affected job entry adds `OriginalJobLog` with `File` (`Path`, `Sha256`),
integer `JobId`, `RunId`, `Attempt`, and the exact original GitHub job-log API
`Url`. This original log is pinned and bounded to16MiB. It must identify one
timestamped CTest invocation, a complete sequential passed case list and one
zero-failure summary. Its full case names and count must exactly match the
artifact's JUnit, with no skipped cases. The original artifact finalization and
download URL must match the independently pinned artifact and workflow run;
execution and upload must fall within the job's timestamps. Existing artifact
digest, source marker, contract, attempt and required-test checks remain fatal.
An incomplete log, a later successful rerun, another job's log, a summary alone
or a caller-supplied PASS label cannot substitute. Version2 preserves this log
corroboration separately from the unchanged raw API step state. Qualify the
offline reader as analyzer B; its evidence remains the original native source A.

`foundation3l_acceptance.py` is an offline final conjunction for the fixed
canonical workload in `PhysicalFundingGateReview3L.md`. It does not start a
process on either physical endpoint, change acceptance bounds, close KI-006,
start 3M, or merge a pull request. Its only subprocesses are local Git inspection
and the repository's existing PowerShell offline farm replayer.

Run from a clean checkout of the reviewed offline analyzer commit. When that
commit is the physical execution source, `--analyzer-commit` may be omitted.
When a later analyzer B corrects offline evidence parsing, pass B explicitly;
`--source-commit` remains the native execution candidate A.

Freeze the native/analyzer execution candidate A before its build. Dispatch CI
and the qualified-scale artifact on A and preserve that clean A checkout for
final replay. Same-source four-client evidence is the default; the sole exact
historical retention exception below preserves its separate native source.
A later child B may record the newly measured qualifier artifact pins without
changing A's native execution source. Qualify B's adapter/pin changes separately;
the native manifests keep their actual source identities. This avoids a
build/hash self-reference cycle. A final PR documentation or
pin receipt commit does not retrospectively relabel an A executable as B. This
checker proves CI and physical execution provenance for the explicit A source;
it does not waive required separate validation of the B qualifier.
The checker requires A to be an ancestor of clean HEAD B and records B plus
hashes of the fixed final/four-client entrypoints, their F1 dependencies, and
all farm dependencies enumerated by `PhysicalFarmAnalysisInventory.ps1`. It
still replays raw four-client evidence in this
invocation. CI, the qualified package and both provider manifests remain pinned
to A; the four-client archive stays pinned to its actual execution source.
A B receipt is analyzer provenance,
not a claim that B's native runtime was physically qualified.

## Exact retained four-client prerequisite

For provider candidate `c916b7d4f868177ae00b4b05dacfa7b8dbb1f563` only,
the retained F1 Phase 1 prerequisite has native source
`9ae8f68c2c8ed50589b5c54ed56657fa6fd02aad`, physical run
`454bf4f8-c8f2-4c18-9866-1b61218da514`, lifecycle
`7a48260b-6911-4394-bbb5-65a65e17bac0`, and raw input-map SHA-256
`d4ebc1849258d0bf30abeb37ca9e0de3737d533e7ac7df99fd449351d861e8ca`.
The input map is hashed before parsing. Only this exact pair and map select the
unchanged raw four-client replayer's historical source. A passing result must
retain the exact native source/run/lifecycle, candidate pins and raw input
hashes; it is labeled `RETAINED_F1_PHASE1` / `RetainedFourClientMixed`.
There is no arbitrary source, ancestor-based retention, summary bypass or
native-equivalence claim. Other maps use the original same-source replay rule.

Retention does not qualify the changed native candidate: both current-source
Local32 and Node32 grant-scoped F1 gates remain mandatory, along with every
other provider, CI, package and controller-sequence gate. The historical
completion must still precede both provider stages; Local completion must
precede Node staging. No historical receipt or failed provider is relabeled.

```powershell
python tests/foundation3l_acceptance.py `
  --source-commit <40-lowercase-hex> `
  --contract HYBRID_F1_V1 `
  --analyzer-commit <clean-reviewed-analyzer-HEAD-if-different-from-source> `
  --farm-inputs <raw-farm-arguments.json> `
  --powershell-path <absolute-pwsh.exe-path> --powershell-sha256 <runtime-sha256> `
  --four-client-inputs <raw-four-client-arguments.json> `
  --ci-index <ci-index.json> --ci-index-sha256 <independently-retained-sha256> `
  --provenance-index <provenance.json> --provenance-index-sha256 <independently-retained-sha256> `
  --output <new-output-path.json>
```

The output path must be new and outside the immutable farm roots. Exit 0 means
every required typed gate passed; exit 2 means an incomplete or failed
conjunction. Malformed, stale, mismatched or explicitly rejected raw evidence
throws and cannot produce PASS. Missing optional evidence inputs produce
INCOMPLETE. The command does not accept a four-client PASS flag or an existing
farm summary as evidence. The four-client argument map is passed only to the
fixed tracked `tests/foundation3l_four_client.py:Replay(Inputs, ExpectedCommit)`
implementation. Until that implementation and its complete inputs are present,
the four-client gate remains unmeasured.

The four-client input map includes `ServerCsv`, `AdmissionsCsv`, and a
`ClientCsv` map keyed by the four exact nonce strings. `RawCsvSha256` contains
`Server`, `Admissions`, and those same four nonce keys. The admissions CSV is
`physical-gns-server-admissions.csv`, recorded at successful grant acceptance;
it preserves token, generation, exact bytes and the original activation value.
Only the production pre-Ready sentinel identifies bootstrap work. Every other
accepted grant requires its native finite-service certificate. The endpoint
collector copies these bounded files into its evidence root before sealing the
manifest, and final replay joins each supplied CSV hash to that sealed copy.
PowerShell is selected by its explicit absolute executable path and hash,
following the existing analysis-inventory/runtime pin pattern. There is no
`pwsh` PATH fallback and no caller-selected replay script. The fixed farm
script's `#requires -Version 7.0` remains enforced. Windows subprocess creation
uses `CREATE_NO_WINDOW`.

## Provider replay

The farm argument JSON contains the named parameters of
`PhysicalGameSessionFarmAcceptance.ps1`, excluding `OutputPath`. The six raw
Local/Node report/root parameters are required. Real Node TLS, owned Node
process resources, both provider capture index pairs, outer capture receipts
and coordinator results are required to obtain a final PASS. The fixed
PowerShell replayer is executed in this invocation and independently checks
all indexed raw files. Its deliberately INCOMPLETE outer observation is not a
failure and cannot itself authorize final PASS.

The final typed conjunction requires each provider's:

- five-phase, all-32-client workload and exact terminal conservation;
- generation-safe, grant-scoped F1 native completion and sticky failure evidence;
- ACK-gated grant lifecycle, recorded wait bound and fixed-workload fairness;
- for HYBRID, independently complete continuous eligible-peer episodes across
  source-proven atomic candidate replacement, retaining the earliest clock and
  the1181250-us bound. Missing/unknown/interrupted coverage cannot pass; distinct
  unaccepted candidate bytes are not claimed as delivered. Original exact-demand
  coverage and legacy F1 semantics remain visible and unchanged;
- accepted Character state chain, due-service cadence and all-window ordinary
  successful-send demand (including forced Character traffic);
- native probe clock calibration, work-tick limits and designated producer
  RPC/Event cadence;
- all-role logical cleanup and Remote ownership release;
- nonce-bound 32-tuple capture, complete manifests and lifecycle reconciliation;
- fixed service recovery, immutable exact W, C3/C5 finite-prefix convergence,
  and sampled bounded journal retention. Native cumulative retention extrema
  include unprinted ticks: they must bound every printed observation, remain
  monotonic, preserve nonnegative coverage and the16384-record cap; equality
  with sparse printed extrema is not required;
- source-bound live final-Name observations: prospective bounded recovery
  cohorts require `source_replica_v1` witnesses for all32 structural/mixed
  targets on every client. Authoritative source identity must match the server;
  local identity remains generation-safe, unique and diagnostic within the
  current replica epoch. Missing/ambiguous/stale binding and wrong Name remain
  fatal. Server causal/native ACK evidence cannot substitute for the client map;
- role-local complete process sweeps, actual host/NIC observations and bounded
  evidence inventories.

Node additionally requires the actual negotiated authenticated TLS RPC and its
independently owned process resource receipt. Cross-provider exact workload and
deployment pins must match. General infinite saturated-source fairness, an
invented CPU/RSS/NIC percentage SLA, 31 additional Remote producers, and
cross-host one-way timing are not new gates. First-send F1, ordinary demand,
delivery, publication, recovery and packet reserve remain distinct evidence.

## Exact-head CI inventory

Retain original GitHub REST run metadata, the complete jobs response and each
artifact metadata response, plus the original downloaded ZIP bytes. The index
has this shape (all paths are relative to its directory):

```json
{
  "Format": "GargantuanFoundation3LCI",
  "Version": 1,
  "Jobs": [
    {
      "Name": "Windows x64 / MSVC 19.50+ / Release",
      "Run": {"Path": "native-run.json", "Sha256": "<sha256>"},
      "Jobs": {"Path": "native-jobs.json", "Sha256": "<sha256>"},
      "ArtifactMetadata": {"Path": "native-artifact.json", "Sha256": "<sha256>"},
      "Archive": {"Path": "native-ci-diagnostics.zip", "Sha256": "<sha256>"}
    }
  ]
}
```

There must be exactly three entries: Windows Release, Ubuntu headless ASan/UBSan
and Ubuntu GNS ASan/UBSan/LSan, with their exact current workflow job names.
The two native jobs must belong to the same run attempt. Required CTest steps
must have completed successfully, as must their jobs and whole workflow runs.
Only `push` and `workflow_dispatch` runs are accepted. PR merge runs cannot
substitute for an exact candidate checkout.

Each diagnostics ZIP must match GitHub's artifact digest and size, belong to
the expected run/head, and have been created during the selected job attempt.
The bounded JUnit reader requires the relevant native/F1/funded-ACK tests,
rejects failures and missing/skipped required tests, and permits existing
optional platform skips. It also requires `qualified-source-commit.txt` from
the actual CI checkout. Successful diagnostic uploads use `if: always()`;
their file lists and artifact retention policy are otherwise unchanged.

These are offline checks over externally collected GitHub metadata. The
independently retained inventory pin is the collection trust boundary, not a
signature invented by this script. No network access or credential is used.

## Package provenance and gate order

The separate pinned provenance index is:

```json
{
  "Format": "GargantuanFoundation3LProvenance",
  "Version": 1,
  "QualifiedPackage": {
    "ArtifactMetadata": {"Path": "qualified-artifact.json", "Sha256": "<sha256>"},
    "Archive": {"Path": "qualified-scale.zip", "Sha256": "<sha256>"}
  },
  "LocalClientStage": {"Path": "local-client-stage.json", "Sha256": "<sha256>"},
  "NodeClientStage": {"Path": "node-client-stage.json", "Sha256": "<sha256>"},
  "LocalHostTerminal": {"Path": "local-host.terminal.json", "Sha256": "<sha256>"}
}
```

The qualified package must come from the verified Windows dispatch job as
`qualified-scale-<candidate>`. Verification streams its original ZIP without
extraction: at most 1 GiB compressed, 2 GiB expanded, 22,000 entries and 256 MiB
per entry. It rejects redirected/duplicate/extra/missing entries and validates
every file against both role deployment inventories. Actual binary, package,
content-manifest and deployment hashes must equal the independently replayed
Local/Node run manifests. Matching a self-reported source SHA alone is
insufficient.
The builder's two retained source-fixture files and canonical workload
descriptor, plus the bounded exact `qualification-contract.txt` marker for
HYBRID, are the only allowed files outside the two deployment inventories;
they remain bound by the original GitHub ZIP digest and bounded separately.

Retain the root-owned CLIENT campaign stage configurations. Their RunId,
SourceCommit and ManifestSha256 must bind the same indexed provider manifests.
The same-source or exactly retained four-client replay must expose an evidence-backed `CompletedUtc`
with `CompletionClockDomain = CONTROLLING_HOST_UTC`. Both provider stage
`CreatedUtc` values must follow that completion. The controlling-host
`farm_outer_endpoint.py` Local `host.terminal.json` must additionally prove
`Outcome=COMPLETED`, `ChildExitCode=0`, and `ChildTreeReaped=true` for that exact
Local RunId. The separately replayed Local coordinator must succeed with the
stage's CoordinatorRunId. Enforce Local stage <= Local host `EndedUtc` <= Node
stage: a later successful Node receipt cannot repair overlapping or reversed
campaign order. This is broad session ordering
from one controller's UTC records, not cross-host native timestamp subtraction.
If either required completion receipt is absent, order remains NOT_MEASURED.

## Validation and remaining real inputs

The fixture suite mutates every provider conjunct and independently exercises
CI failures, missing tests, stale attempts, digest changes, PR-merge source
mismatches, package inventory corruption and premature provider staging. It
creates only small temporary local files and never claims physical results.

Real final qualification still needs the same-source or exactly retained four-client raw replay,
successful post-change exact-head CI ZIPs, the actual qualified-scale ZIP,
complete Local and Node raw campaigns with new F1/ordinary-demand records, real
TLS/resource/capture/lifecycle evidence and retained controller stage records.
Historical missing fields stay unmeasured; this tool never rewrites receipts.
