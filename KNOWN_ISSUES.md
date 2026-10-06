# Known issues

This file tracks verified current defects and engineering gaps that are useful
to contributors but do not belong in the feature roadmap. An entry remains
open until its resolution criteria are implemented and verified.

## KI-002: Box3D diagnostics are not integrated with engine logging

- Status: Open
- Priority: Low
- Area: Physics observability
- Upstream reference: [teamfireworks/gargantuan commit `7a38dd9`](https://github.com/teamfireworks/gargantuan/commit/7a38dd9e188d25f784264ff0000e69dd4c7e63b1)
- Relevant code:
  - `include/gargantuan/Log.hpp`
  - `src/Log.cpp`
  - `src/Main.cpp`

Box3D warnings currently bypass Gargantuan's structured logging categories.
This makes physics failures and performance warnings harder to diagnose.

Resolution requires forwarding `b3SetLogFcn` through a clearly named
Physics/Box3D logging category, preserving pretty and JSON output behavior, and
verifying that representative Box3D warnings are emitted once at the intended
severity. The upstream abbreviated `B3D` category should be adapted to the
repository's subsystem-oriented logging convention rather than copied verbatim.

## KI-005: Instance has no pre-removal DescendantRemoving lifecycle signal

- Status: Open
- Priority: Low
- Area: Luau Instance lifecycle API
- Relevant code:
  - `assets/classes/Instance.luau`
  - `include/gargantuan/classes/generated/Instance.hpp`
  - `src/classes/Instance.cpp`

The current hierarchy API exposes `DescendantRemoved` after removal, but no
`DescendantRemoving` signal for observers that need to inspect the prior
hierarchy state. This is an API-completeness gap, not a correctness defect: the
engine does not currently promise the missing signal.

Resolution requires deciding whether both pre- and post-removal descendant
signals belong in the supported lifecycle surface. If adopted, define exact
ordering, parent/hierarchy visibility during the callback, reparent versus
Destroy behavior, subtree ordering, and reentrancy rules before adding the
schema declaration, native signal, and tests.

## KI-007: Soft-reference replacement can invalidate a structural removal frame

- Status: Resolved and validated (2026-09-11); published on `foundation/3l-content-availability` in `36d4eb668` (not merged).
- Priority: High.
- Area: Server replication reference-fixup completeness / journal ordering.
- Relevant code: `src/network/ReplicationCoordinator.cpp`,
  `tests/ReplicationRelevanceTests.cpp::TestSoftReferenceReplacementBeforeLeave`.
- Evidence: [3L.3 validation ledger](devdocs/CurrentArchitecture/ContentAvailabilityFoundation3L_3Validation.md).

A client has accepted Character.RootPart -> A. The server destroys A and changes
RootPart to fresh B, which is not Known or Desired for this peer. Servicing the
pending structural removal before the property journal emits no nil update:
the current-value predicate tests B against Leaving, overlooking the client's
old edge to A. Client snapshot preflight rejects the frame with
`Snapshot reference is outside the receiving scope`. The failure reproduces
with the original full-property loops, not only the withdrawn reference-index
candidate. The original failing checkpoint is retained as historical evidence.
No client validation was weakened.

The correction records accepted native reference values alongside existing
accepted ancestry. Structural removal derives required clear/replacement work
from those old edges, including Known referrers no longer Desired. A complete
clear/removal group is charged to the existing 3J allowance; incomplete groups
defer and genuinely oversized groups follow the existing failure policy. Soft
referrers that are themselves leaving may progress independently. Only accepted
frames update metadata/Known, and full identities preserve lifecycle safety.

Correctness-only v5 passes MSVC 6/6 and Clang 19 ASan/UBSan/LSan 7/7 with leak
detection, plus the original regression, ten-scenario matrix, 90 physics cases
and unchanged 500-peer convergence/acquisition/admission/8,192-cap checks.
Unknown/known replacement, nil, same-progression Enter, multiple reference fields,
destroy during preparation, fresh identity, disconnect and byte-budget rejection
are covered. Deliberately dangling frames remain rejected by client preflight.
This closes the ordering defect, not general planning work or Foundation health.

<a id="ki-006-content-coupled-gameplay-latency-exceeds-the-3l2-readiness-envelope"></a>
<a id="ki-006-remaining-real-client-and-physical-recipient-service-qualification"></a>

## KI-006: Remaining physical pooled-service qualification

2026-10-06 execution candidate `ed09e7126` passes all six original CI jobs
and official dispatch. An offline archive-reader limit rejects aggregate
expanded bytes of unread diagnostics after retention grows to five trace sets.
The focused analyzer-only correction preserves original ZIP bytes and all
actual-read/provenance bounds; the existing A/B procedure keeps native/live
execution and packages pinned to ed09. [Qualified-source checkpoint](devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md#qualified-execution-source-and-offline-archive-bound-correction-2026-10-06)
does not establish Local32/Node32. Exact package verification and fresh
physical measurement remain required. **KI-006 OPEN; 3L B — PARTIALLY READY**.

2026-10-06 `92e3c9b9` PR Windows refuses setup because Appraiser count is
one despite no exact registered task. This is a real resource-precondition
failure before production measurement. Bounded hosted cleanup now permits
only the checked System32 Appraiser image, with captured handle and fresh
PID/birth/name/path identity; final actual absence remains mandatory.
Unknown identity, API errors and reappearance fail without arbitrary targets
or retries. [Original refusal](devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md#active-hosted-telemetry-without-a-registered-task-2026-10-06)
is preserved. User hosts and all five workload/latency gates are unchanged.
Fresh original CI/package/preflight/Local32 remain required. **KI-006 OPEN;
3L B — PARTIALLY READY; Node32 not run**.

2026-10-06 `0dd022697` stops at hosted task lookup on push, PR and official
dispatch before any native build/test/workload. The image lacks the exact
registered Appraiser task. The minimal correction requires successful task
enumeration and actual process absence; missing target permits no mutation
and is truthfully NOT_REGISTERED. Active process, duplicate target and API
error remain failures, and a present target still requires Disabled readback.
A fresh process census precedes the five unchanged timing workloads.
[Historical setup failure](devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md#hosted-task-absence-is-a-resource-fact-not-a-setup-defect-2026-10-06)
is preserved; no physical result is inferred. New original CI and packaging,
then fresh Local32, remain required. **KI-006 OPEN; 3L B — PARTIALLY READY**.

2026-10-06 candidate `be949192c` remains CI-disqualified: PR Full/mixed
RPC p95 232.4581 ms exceeds 150 ms; its loss-free ETL measures scheduled
compatibility telemetry consuming CPU during main-thread ready waits.
The blocking API/owner remains **NOT MEASURED**. Official dispatch also fails
first pooled mixed Event/action latency with a 1.645-second client poll;
that command lacks a failing-window scheduler trace. Only ephemeral hosted
VM telemetry preparation and the existing first-command trace/Normal launch
are corrected; production, workload and latency gates remain unchanged.
[Both original failures](devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md#hosted-telemetry-contention-and-pooled-observation-gap-2026-10-06)
remain failed. Fresh original CI, package and Local32 are still required.
Client memory now exceeds 8 GiB; no physical run is inferred. **KI-006 OPEN;
3L B — PARTIALLY READY; Node32 not run; no 3M or merge**.

2026-10-05 source `09aa075ad` passes focused native, GNS and PR Windows
checks, but original push Windows job `112045140196` fails pooled aggregate
recovery RPC p95: 170.268 ms >150 ms for peer 0, with 48/48 completed and
zero errors. All scheduler wrappers were downstream and never reached.
Exact wait/scheduler cause and original API priority are **NOT MEASURED**;
successful same-source checks do not replace the failed original. Only that
second logical command is now routed through the existing bounded trace and
verified Normal launcher, preserving flags, clocks, gates and one execution.
[Raw failed evidence](devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md#pooled-aggregate-hosted-timing-failure-before-scheduler-capture-2026-10-05)
remains historical failure. Focused helper and new original hosted checks,
then fresh resource preflight and Local32, remain required. **KI-006 OPEN;
Foundation 3L B — PARTIALLY READY; Node32 not run; no 3M or merge**.

2026-10-05 qualified Name candidate `b9f8ded29` reaches 32 actual clients
in fresh run `1072bd82-e3b6-4956-b919-778caa5dcaab` and still fails recovery.
All 32 cursors are below fence 23,246 at the unchanged 35,112,557-us bound;
reference sealing at 65,080,874 us only delays reporting. Completed Main
recovery ticks average 55.28 ms. The reference worker's exclusive interference
and stale-feedback incidence are **NOT MEASURED**. A narrow execution correction
separates live convergence measurement from frozen replay and refreshes current
native feedback before eligibility, preserving the original clock, exact work,
50-ms freshness, admission, ACK/retirement and F1 semantics. That candidate
passes four focused native CTest entries, including a real-GNS old-path
failure and corrected admission/denial/conservation. New hosted CI is required
before another actual Local32 run. [Failed evidence and cleanup](devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md#corrected-name-candidate-actual-local32-still-fails-recovery-2026-10-05)
remain preserved. **KI-006 OPEN; Foundation 3L B — PARTIALLY READY; Node32 not
run; no 3M.**

2026-10-05 actual Local32 run `441c7f4c-870f-4e8f-92c9-66674adc86ff`
reaches production on native `9ae8f68c2` after all six original tooling
`999d3b0f` hosted jobs pass. It fails recovery convergence at the unchanged
31,088,518-us workload-derived bound: all 32 cursors are still below cessation
fence 23,240 at that deadline, and 29 remain incomplete at terminal shutdown.
All 1,466 post-cessation accepted frames match the frozen per-peer prefix
exactly. The 644 completed recovery ticks have median 59.519-ms work dominated
by preparation. Name size-preflight rejects already represented history and
unselected trailing records, causing avoidable discarded candidate work;
its exclusive physical contribution is **NOT MEASURED**. Qualify a narrow
exact-output correction and new native CI before retrying. Do not change
admission, freshness, recovery deadlines or F1 constants. Failed diagnostic
captures and shutdown-induced client disconnects do not establish provider
acceptance. See the [actual receipt](devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md#actual-local32-recovery-convergence-failure-2026-10-05).
**KI-006 OPEN; Foundation 3L B — PARTIALLY READY; Node32 not run; no 3M.**

2026-10-05 tooling `d66444f3` passes the corrected sampler and all 95 native
CTest entries plus 371 Python checks. Its original push `37275013641` then
fails five unchanged 150-ms RPC p95 gates in FULL_RESERVATION aggregate
recovery; observed p95 values are 162.058, 165.997, 163.524, 165.514 and
166.892 ms. All accepted requests complete without errors. The failed fourth
command `--reliable-workload-32` has no overlapping scheduler trace or measured
child priority; CPU/wall/sleep observations alone do not establish its cause.
The passing PR and GNS jobs cannot replace the failed push. Extend the existing
bounded fixed-case diagnostic to that exact command once, preserving its
arguments, order, latency gates and original exit; new CI is required before
Local32. No production/F1/admission change or physical retry is justified by
this evidence. **KI-006 OPEN; Foundation 3L B — PARTIALLY READY.**

2026-10-05 original tooling `a4baec83` native push `37267234328` fails
the five-second resource-sampler startup gate (Windows 94/95; dependent Linux
skipped). Parent counter preparation is outside the timed startup; exclusive
child phases are **NOT MEASURED**. The child recompiles fixed C# already
prepared in its parent, and the workflow omitted the failed fixture's retained
raw directory. Qualify bounded counter-code preparation/loading and retain
phase diagnostics without extending startup/query/stop limits. New original
hosted CI remains mandatory before physical launch; a green PR job cannot
replace the failed push. Retained F1 PASS is unchanged. **KI-006 OPEN;
Foundation 3L B — PARTIALLY READY; no 3M.**

2026-10-05 Local attempt `78b900ef-4784-4cc2-8704-ce5c37edf332`
failed before its host listener or role launch. A separate harmless worker
ownership witness reproduced `ModuleNotFoundError: dependency`: the fixed helper
directory contains only the adapter and ACL module, while the verified per-run
stage owns the pinned dependency and Coordinator exports. The correction loads
and rechecks those stage-owned sources explicitly and preserves bounded startup
diagnostics plus the original launch error when cleanup is also unproven.
The original failed attempt remains failed; source and new original CI
qualification precede any new physical attempt. **KI-006 OPEN; Foundation 3L
B — PARTIALLY READY; no 3M.**

2026-10-04 Local32 bootstrap infrastructure failure: all six original hosted
jobs pass for tooling `929da7d0`; the previous `1aeb7221` failed run is
separately closed. Fresh run `68bfec9e-1335-4f45-b7ad-9ea502bd2592` proves
the projected Server UDP correction with 28 actual Ready clients, then fails
`clients_or_manifest_not_ready` at tick 1,201 before workload clock records.
The client launch loop synchronously samples process and adapter/CIM resources
between starts; retained same-host native timestamps include 6.357-second and
5.614-second launch gaps. The exact blocking OS call is **NOT MEASURED**.
Decouple bounded sampling from launch while preserving startup resource
coverage, process identity, the bootstrap bound and every acceptance limit.
The worker's original FAILED/false-reap terminals and missing capture index are
separate failure-path defects: cleanup truth needs persistent containment
independent of exit success, and stopped failure captures need explicit
diagnostic export that strict acceptance still rejects. Preserve the failed
receipts; separately prove current idle and retire this run's exact secrets.
See the [physical receipt](devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md#local32-bootstrap-launchsampling-failure-2026-10-04).
**KI-006 OPEN; Foundation 3L B — PARTIALLY READY; no 3M.**

2026-10-04 Local32 game-UDP prerequisite: tooling `7a3e7ea8` passes its six
original hosted checks and the preceding `f0c18bed` failed run is independently
closed. Fresh run `1aeb7221-8c47-4ccd-98c1-389d486e1c4c` starts the unchanged
native Server from its exact compact projection, then slot 0 bootstrap times
out before Ready. Retained captures match 21 incoming requests and zero
responses. Current compiled firewall policy restricts apparently broad UWP
exceptions to package identities; the exact original WFP drop decision is
**NOT MEASURED**. The launcher had established coordinator TCP permission
without verifying game UDP permission for the new Server executable path.

The bounded correction verifies the sealed projection and a run-owned inbound
UDP 39450 rule restricted to both fiber addresses and the Private interface,
and reconciles only that unchanged rule on every launch exit. Stale names,
changed ownership/scope, explicit matching Blocks or differing effective
filters fail closed. Production transport/admission/F1 and installed services
are unchanged. The 240 farm and 47 optimized outer tests pass without skips,
with ROOT separately replaying the 12 focused game-rule cases. Current CI,
exact fresh failed-run cleanup,
Local32/real-TLS Node32 and final acceptance remain required. Original failed
terminals and incomplete captures are preserved. **KI-006 OPEN; Foundation 3L
B — PARTIALLY READY; no 3M.**

2026-10-04 second Local32 startup attribution: tooling `578edaa4` has all six
original push/PR Windows, Linux and GNS jobs green. The prior failed run's
six control secrets were separately retired with independently replayed
cleanup evidence. Fresh Local run `f0c18bed-722e-4820-86f6-20be1e6f60c1`
passes staged imports and capture configuration, then exits with native package
integrity failure before readiness. Its closed runtime contains the same 58
files and hashes as the passing startup diagnostic, but eight absolute asset
paths are 270 characters rather than 229. The pinned executable lacks the
Windows long-path opt-in. A startup-only native regression reproduces exit 3
with identical bytes at 270 characters and exit 0 for the unchanged Server and
Player from shorter roots. The exact syscall in the physical failure remains
**NOT MEASURED**; the native catch reports generic integrity failure.

The qualifier now rejects runtime member/ancestor paths beyond the native
package builder's existing 240-character limit before publication and during
prepared validation. Eighteen new boundary/role/helper regressions and the
existing 23 projection cases pass locally. No native production, admission,
F1 constant, transport policy, or installed-service change is made. Current
correction CI, this fresh failed run's exact secret retirement, corrected
Local32/Node32 provider qualification, and final acceptance remain required.
Neither historical failure is relabeled PASS. **KI-006 OPEN; Foundation 3L
B — PARTIALLY READY; no 3M.**

2026-10-04 hosted scheduling attribution: source `3e030710` Native PR
`37173701281`, Windows job `111351799079`, passes 95 CTest cases but fails
standalone command 3, FULL `upper` action 17 at 1892.3673 ms against 250 ms.
Commands 4/5 never start and dependent PR Linux is skipped. The retained,
loss-free scheduler trace measures 1889.0000 ms off CPU and 3.3673 ms scheduled
inside that action. Its largest 750.7253-ms ready/preemption gap coincides with
Compatibility Appraiser and its PowerShell child consuming approximately 93%
of four-core running time. Independent Windows Performance Toolkit decoding
joins every original CSwitch row, identifies the SYSTEM appraiser command and
records base-6 workload/launcher threads versus base-8/9 competitors. Original
API priority classes remain **NOT MEASURED**.

The fixed trace launcher now requests ordinary Normal class and verifies actual
class/main relative priority before resuming its owned child. A bounded API
regression reproduces unspecified Below Normal/Idle inheritance and verifies
explicit Normal under all three parent classes. All 33 helper tests pass in
normal and optimized modes without skips. No background task, service, workload
or latency limit changes. This corrects launch-policy dependence, not the
historical failed result; fresh original hosted CI remains required before any
new candidate adoption. Failed-run cleanup and both provider matrices remain
outstanding. **KI-006 OPEN; Foundation 3L B — PARTIALLY READY; no 3M.**

2026-10-04 Local32 startup attribution: tooling `2537f4517` has successful
original push/PR Windows, Linux and GNS checks. Fresh Local run
`cf89463c-cd4a-4d18-849a-cb66b51ba97e` passes staged imports and strict capture
configuration, but the native server exits 3 before readiness. The official
NativeA ZIP and deployed Server both contain one undeclared native-package
member, `deployment-sha256.json`; every declared content size/hash matches.
CI appended this qualifier inventory after native package validation. The
qualifier had verified deployment bytes without establishing native runtime
closure. A source-owned clean runtime projection must preserve the original
envelope/pins and keep inventory evidence outside the executable package.
Original failed terminals remain failed; supplemental cleanup must prove idle
and retire the six exact secrets separately. Local service, Node32 and final
acceptance remain **NOT MEASURED**. Retained F1 PASS is unchanged.
**KI-006 OPEN; Foundation 3L B — PARTIALLY READY; no 3M.**

The clean-projection implementation is now locally qualified: 23 projection
cases, 228 farm Python tests, 32 acceptance tests, and actual bounded NativeA
Server/Player startup from unchanged declared content pass. No bind/connect or
provider measurement occurs. Required current-source CI, exact failed-run
secret retirement and fresh Local32/Node32/final acceptance remain outstanding.

2026-10-04 decoder continuation: the development Full native child passes all
eight cases, but its original diagnostic fails at the obsolete five-million-row
cutoff with only 309,432,716 CSV bytes. Removing that independent cutoff retains
the existing 512-MiB byte and 180-second time bounds. The corrected SDK helper
passes 31 tests in both normal and optimized Python modes without skips. A
separate read-only replay decodes the unchanged ETL into 7,731,540 rows and
478,326,842 bytes in 16,609 ms without reported loss or truncation. The original
diagnostic remains failed; no workload or capture is rerun. New original hosted
CI and independent candidate qualification still precede physical adoption.
**KI-006 OPEN; 3L B — PARTIALLY READY**.

2026-10-03 c33 original CI continuation: Native PR `37158397178` fails logical
command 3, FULL_RESERVATION `--reliable-workload`, upper case. RPC p95 is
218.8903 ms against 150 ms; action 17 is 556.5228 ms against 250 ms. Native
95/95, helper 20 and hosted tooling 295 pass; dependent Linux is skipped.
Fourth/fifth commands never start, so no scheduler trace measures this failed
invocation. Request-linked runtime overlaps are known; their OS/blocking cause
remains **NOT MEASURED**. The next correction instruments that original command
using the existing bounded Full helper exactly once, preserving all gates and
arguments. Dormant V5 stays unadopted with null pins. No physical attempt or
production-policy change occurred. **KI-006 OPEN; 3L B — PARTIALLY READY**.

2026-10-03 aggregate diagnostic continuation: source `8854feecf` adds complete
bounded request/resource evidence and preserves unsigned native failures through
signed process exits and all five CI guards. A fresh development trace passes all
three phases with 7,424 paired records, no loss and verified cleanup; 20 noncapture
tests pass without skips. The original CI p99 failure remains unexplained, and
all earlier failed development/physical receipts remain failed. New original
hosted CI and a separately reviewed launcher must qualify before physical work.
No production or acceptance invariant changes. **KI-006 OPEN; 3L B — PARTIALLY READY**.

2026-10-03 continuation after capture-root qualification: source `3c8f65d39`
passes 315 local tooling tests and its original PR Windows 95-case CTest/tooling
steps, but Native PR `37151679339` fails three FULL_RESERVATION aggregate RPC
p99 gates in the fifth standalone's normal phase. All requests complete and
no pooled F1 first-send failure is measured. Aggregate request endpoints were
absent, so the exact cause remains **NOT MEASURED**. Request-linked bounded
timing/scheduler evidence must qualify before another physical launch; no
threshold or workload is weakened. Dormant V4 is not adopted or executed.
The failed Local `b52d6e7b` separately has all six secrets retired and idle
closure, with original failed receipts preserved. Retained F1 PASS remains
valid; **KI-006 OPEN; Foundation 3L B — PARTIALLY READY**.

2026-10-03 20:30 UTC continuation: the client resource gate passed with 19.177
GiB available at 19:52 UTC. Fresh Local run
`b52d6e7b-6ebb-44e2-b12c-a2b4fb88bbc0` reached its control barrier, then both
capture controllers failed before capture readiness because staging had not
created their sealed per-run capture parent directories. Complete bounded
stderr proves `WinError 2` at strict configuration validation. Neither native
farm nor production service was measured. Historical failed role terminals
retain their unproven child-tree cleanup state; subsequent cleanup requires a
separate exact-run closure. The correction provisions only the verified sealed
capture parent under its actual endpoint identity and exercises the strict
consumer before role launch. Full qualification and a fresh candidate are
required before another attempt. Local32, real-TLS Node32 and final acceptance
remain unqualified; **KI-006 OPEN; Foundation 3L B — PARTIALLY READY**.

The following 15:10 UTC checkpoint is historical:

2026-10-03 15:10 UTC checkpoint: current tooling source `1ccefb336` passes both
original Native workflows (95 Windows / 53 Linux cases each, tooling and five
standalones) and both original GNS sanitizer workflows (10 cases each). Original
artifacts and executed checkout identities are independently reconciled. The
controlled worker's complete 95-case/five-workload attempt `da348bc1` also passes
after a prospectively qualified short-TEMP environment correction. Historical
hosted timing/F1 failures and earlier controlled failures remain failed; new
passing observations do not attribute their unmeasured causes. See the
[current qualification receipt](devdocs/CurrentArchitecture/ContentAvailabilityFoundation3L_3Validation.md#current-tooling-and-controlled-capacity-qualification-2026-10-03-1510-utc).
The physical native candidate and acceptance gates are unchanged. Client free
memory is 7.781 GiB against the required 8 GiB at 15:09:45 UTC. Fresh Local32,
real-TLS Node32, parity and final acceptance/audit remain unmeasured. **KI-006
remains OPEN; Foundation 3L remains B — PARTIALLY READY.**

The original `138510b02` push/PR Native runs are both terminal failures,
with both corresponding GNS sanitizer runs successful. Push run `37115422832`
passed all 95 CTest cases, then failed FULL_RESERVATION mixed Event 238 at
435.3776 ms. Its exact interval does not intersect any retained maximum-phase
span; the 3399.7984 ms client-poll maximum begins 177.4484 ms **after** that
Event completed. It cannot explain the failed Event. PR run `37115424856`
passed 94 of 95 tests and failed the statistics-boundary fixture's unchanged
F1 predicate after successful byte/ACK/retirement conservation. The original
log lacks that failing grant's exact first-send timeline. The fixture now
prints its original bounded completed-grant snapshot and segment records on
F1 failure as well as conservation failure, without changing either predicate.
Neither historical failure is classified as an infrastructure failure, and
neither is replaced by a passing diagnostic or a different host's result.

The retained manual scheduler diagnostic from hosted run `37115431988` passed
its unchanged workload but its original decoder classified 299,065 CSwitch
version 5/28-byte events as unsupported, leaving diagnostic coverage
`INCOMPLETE` and the hosted job failed. Offline native replay identified one
exact observed version 5 layout and reproduced the original 160,537 accepted
CSV rows byte-for-byte. The decoder now recognizes only its four proven common
scheduler fields; this implementation correction does not change the original
run's result or qualify the cause of prior RPC/Event timing failures. A fresh
hosted execution and causal analysis remain necessary.

2026-10-03 causal-diagnostic continuation: head `457d9be28` push Windows
passes 95 CTest cases and the standalone workloads, but PR Native run
`37110761630` fails FULL_RESERVATION recovery RPC p99 (491.791 ms) and
Event RTT (491.774 ms). Recovery step 50 spans 423.31 ms across sequential
runtime/session calls with zero measured, quantized thread/process CPU
increments. This does not distinguish a subordinate wait from descheduling;
the exact slow Remote identity and kernel thread states were not measured.
Fixture-only bounded chronology now retains those identities, original
submission/callback timestamps, native thread IDs, QPC anchors and paired
maximum phase intervals for a causal scheduler diagnostic. The failed run
remains failed. No latency threshold, workload, native candidate or provider
gate changes; fresh Local32 remains unlaunched.
One controlled worker execution of the chronology fixture passes all original
FULL workload gates and verifies 745 Remote records, 71 actions, 16 clock
anchors and 72 paired phase spans. Recovery RPC p99/max is 36.7056 ms and Event
maximum is 36.6985 ms. This validates the observer on that host, not the cause
of the hosted failure or a replacement for required CI.

2026-10-03 08:43 UTC continuation: fresh Local32 remains unlaunched after the
staging correction. Later hosted qualification exposes distinct failures:
Native `37106409866` fails recovery RPC/Event timing with nonexecuting host
time; `37107293083` fails the ACK statistics-boundary combined assertion and
strict four-grant F1; `37107296441` fails recovery RPC/Event/action timing.
The four-grant peer-0 native timeline independently reproduces a running
deficit of 86,063,889,408 byte-us while finite completion passes at 35,210 us.
The exact ACK predicate and native sender cause remain NOT MEASURED in those
historical logs. Failure-only diagnostics retain all predicates and all four
peer timelines. One controlled-worker execution of each updated fixture passes
at `018cc2883`; that does not erase or explain the hosted failures. Required
hosted qualification and the full provider gates remain outstanding.

2026-10-03 UTC fresh funded-ACK F1 checkpoint: execution source `9ae8f68c2`
passes physical run `454bf4f8-c8f2-4c18-9866-1b61218da514`, lifecycle
`7a48260b-6911-4394-bbb5-65a65e17bac0`. Independent raw replay verifies
128 maximum grants, four peer service curves, 32 common drain intervals,
complete zero-loss bidirectional captures, gameplay, exact retirement,
successful lifecycle and restored endpoints. An offline-only verifier fix
correctly binds preassignment request UUIDs to the later assigned lifecycle;
physical evidence and production behavior did not change. See the
[fresh physical receipt](devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md#funded-ack-candidate-f1-physical-phase-1-passed-2026-10-03-utc).
Farm32 near-capacity recorder evidence now passes with separately measured
cleanup in `6e9b80d8-671c-4870-9566-63f67b803a4b`: a 15,050,735,616-byte ETL,
all 9,669,000 marked packets in both captures and zero recorder loss. The raw
outer cleanup failure and 668 synthetic client receive gaps remain unchanged;
the hash-bound supplemental closure records later exact ownership/socket
checks. See [capture receipt](tools/physical-qualifier/docs/FARM32_ETL_NATIVE_STOP.md).
The 07:12 UTC client preflight now passes with 11,806,433,280 B available.
Local run `d05185d7-b2dc-4c3c-8a78-1ad4c782faed` then stopped before capture or
workload because the staged controller's `farm_capture_directions` dependency
was omitted; isolated worker Python also needs explicit sibling import setup.
Both-host import-only reproduction confirms this infrastructure defect. Failed
receipts are preserved; later cleanup verifies processes/ports/capture clear
and run secrets retired. Both 32-client provider matrices and final acceptance
remain unmeasured; corrected staging must qualify before a fresh attempt.
Historical synthetic socket-loss attribution is not a new pooled-service gate.
**KI-006 OPEN; Foundation 3L B — PARTIALLY READY.**

2026-10-03 UTC diagnostic checkpoint: candidate `9ae8f68c2` passes the
exact-source hosted Windows Release, Linux ASan/UBSan and GNS sanitizer jobs.
Original JUnit artifacts independently verify 95 Windows, 53 Linux and 10 GNS
tests; source-owned CI and official package verification both pass. The installed native-stop diagnostic
`a17d3e70-d5c7-428e-afa2-2277ab550e3a` preserved every marked packet in both
captures, but the client application missed 629 valid captured datagrams.
The precise Windows receive-path loss remains **NOT MEASURED**. A subsequent
manually elevated drop-diagnostic setup failed before capture because Windows
ellipsized its filter name. Fixed-input, ownership-checked Administrator cleanup
has now removed its exact retained filter. The corrected short-name helper
completed fresh run `ccd6f159-acd1-4b43-bb69-b58947e4970f`: both applications
received every DATA packet, both full captures contain all 3,460,300 marked
packets with zero reported capture loss, and endpoint/filter cleanup passed.
Its result is **DIAGNOSTIC_ONLY_NOT_NEAR_CAP**. The drop-only observer contains
no drops and all-zero flow counters, so positive observer coverage is
**NOT MEASURED**. The historical 629-packet loss remains unexplained; its failed
receipt and both failed/incomplete setup receipts remain unchanged.
See the [complete native-stop checkpoint](tools/physical-qualifier/docs/FARM32_ETL_NATIVE_STOP.md).
The client briefly reported 8.54 GiB available, then fell below 1 GiB; the latest
post-diagnostic reading is 4.36 GiB. The unchanged 8-GiB provider preflight is
not currently cleared. No fresh F1,
Local/Node provider or final acceptance PASS is claimed. **KI-006 OPEN;
Foundation 3L B — PARTIALLY READY; no 3M or merge.**

Capture-infrastructure implementation checkpoint: the Farm32
[exact-session native ETW Stop candidate](tools/physical-qualifier/docs/FARM32_ETL_NATIVE_STOP.md)
has bounded ownership, partial-Start/expired-lease cleanup, retained native-loss
evidence, and UTF-8-without-BOM marker tests. A separate 5-GiB diagnostic
preserved the previously affected ETL band and both captures independently
contain every marked packet, but that run still failed with 290 client
application receive gaps. Neither it nor source/mock validation qualifies the
full near-capacity profile. Fresh capture qualification and subsequent
provider gates remain outstanding; **KI-006 OPEN; Foundation 3L B — PARTIALLY
READY**. Historical F1 physical PASS is unchanged.

2026-10-02 causal recovery implementation checkpoint: the
[C3/C5 cessation-fence amendment](devdocs/CurrentArchitecture/PooledReliableServiceRecoveryContract3L.md#causal-cessation-fence-amendment--2026-10-02)
now has generation-safe source evidence, a finite accepted-prefix verifier and
an independent offline reconciler. The immutable frozen reference includes
captured pending/in-flight relevance planning; continuing motion remains live.
Thirty-two tracker cases, 37 offline causal cases and the actual production
source R1–R10 regressions pass. These are deterministic results, not a fresh
Local or real-TLS Node 32-client provider qualification. The source tests also
exposed and corrected missing preparation evidence and zero fingerprints in
the planned-frame path; accepted bytes are now joined to their exact prepared
candidate before payload ownership moves.

A separate controlled old/new regression proves speculative, unaccepted Leave
could retire server Character materialization while the client received no
structural bytes, diverge materialization epochs and suspend owner actions.
Retaining committed materialization until accepted structural removal fixes
that reproduction, including repeated reversals and accepted Leave/reentry.
The exact local-refusal branch in historical run
`43af4af0-06de-49b6-917d-f0557816b9ab` remains **NOT MEASURED**; this new
reproduction does not retroactively supply missing evidence. See the
[current validation ledger](devdocs/CurrentArchitecture/ContentAvailabilityFoundation3L_3Validation.md#causal-recovery-and-materialization-corrections-deterministically-validated-2026-10-02).

Final execution-changing candidate CI and fresh provider qualification remain
outstanding. The client `DESKTOP-B8V8NAN` has approximately 1.27 GiB available
against the unchanged 8-GiB preflight minimum; the worker `HOSTPC` has
approximately 19.5 GiB, which does not waive the client gate. Provider execution
still requires all existing preflights. The established F1 physical Phase
1 PASS below remains valid. **KI-006 OPEN; Foundation 3L B — PARTIALLY READY;
no 3M.**

The entries below are historical checkpoints at their recorded revisions.

2026-10-01 corrected F1 Phase 1 checkpoint: [fresh physical run](devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md#corrected-f1-physical-phase-1-passed-2026-10-01)
`e120a1f0-47ae-45e0-869f-9d0450795bc4` passed with four real clients,
128 exact 512-KiB grants, all finite/running F1 peer curves, 29 common
four-grant first-send episodes, and the derived pool bound. Accepted and
retired both reached 67,134,512 B; captures were bidirectional for all four
tuples with zero reported loss, and the physical coordinator, outer lifecycle,
and cleanup all passed. This resolves the F1 Phase 1 transport-service blocker
recorded below without changing any F1 constant or the 2 MiB/s admission
model. The distinct 32-actual-client Local and real-TLS Node matrix, then the
final Foundation 3L acceptance sweep, remain **NOT MEASURED**. The current
benchmark still needs a qualified 32-actual-client farm rather than protocol
observers. **KI-006 OPEN; Foundation 3L B — PARTIALLY READY; no 3M.**

2026-09-30 final F1 Phase 1 checkpoint: the [second and last authorized fresh
attempt](devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md#f1-phase-1-reached-native-drain-and-failed-its-running-bound-2026-09-30)
passed the corrected control and server-readiness barriers, connected four
actual GNS clients, and captured all four tuples in both directions. Two
legal 1,258-byte structural grants delayed their final 123 unique first-send
bytes and exceeded the unchanged 18,025,216,000 byte-µs within-grant bound
under valid/current native feedback. This is an **F1 PRODUCTION SERVICE
FAILURE**, even though the separate finite completion envelope passed.
No common four-peer interval or maximum-grant qualification occurred.
The bounded retry budget is exhausted; accepted bytes converged and both
endpoints were restored. **KI-006 OPEN; Foundation 3L B — PARTIALLY READY.**
The next task is native transport/packetization timing attribution and an
explicit architecture decision before further physical authorization.

2026-09-30 post-control-correction F1 checkpoint: a fresh attempt reached the
real worker child, launched capture and the pinned F1 server probe, then
stopped because the qualifier required a `event=listening` text marker absent
from the F1 binary. A worker-local no-client loopback diagnostic verified that
the probe owned the correct UDP port throughout the six-second timeout while
the marker remained absent. No client probe or F1 grant began, so physical
F1 service is **NOT MEASURED**. [Receipt](devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md#f1-phase-1-stopped-at-an-impossible-server-live-marker-2026-09-30).
The corrective qualifier uses live PID-owned socket state; its deterministic,
package, CI and fresh physical preflights are required before the one remaining
bounded attempt. **KI-006 OPEN; Foundation 3L B — PARTIALLY READY.**

2026-09-30 F1 physical checkpoint: the [single F1 Phase 1 attempt](devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md#f1-physical-phase-1-stopped-at-the-worker-control-barrier-2026-09-30)
stopped before GNS or capture when the actual worker endpoint timed out
connecting to the client control barrier. Interactive normal-LAN preflight
had passed, but did not qualify that exact execution path. The staged files
and worker candidate payload were rolled back; no F1 physical service metric
was measured. **KI-006 remains OPEN; Foundation 3L remains B — PARTIALLY
READY.** The next gate is control-connect diagnosis and deterministic
qualification before a separately authorized fresh F1 Phase 1 attempt.

2026-09-29 F1 architecture correction: [D01's F1 amendment](docs/adr/D01-pooled-service-curve.md#f1-amendment--finite-active-grant-drain-capacity-2026-09-29)
defines 16 MiB/s as finite accepted-grant first-send drain capacity, with a
separate within-grant running-rate proof. It supersedes generation-persistent,
semantic-busy and sustained post-credit-offer service interpretations without
changing 2 MiB/s peer admission or ACK-gated grant ownership. Deterministic
tests obtain four exact 512 KiB grants through production admission, verify
their finite and common running curves with controlled first-send events, and
retire all four receipts. Native attribution, deliberately slow service,
asymmetric grants and credit/ACK gaps pass separate deterministic checks.
Physical Phase 1 remains pending; KI-006 stays OPEN and Foundation 3L stays
B — PARTIALLY READY. Older entries
below record their original checkpoint and are not F1 evidence.

The [D01 service-curve decision](docs/adr/D01-pooled-service-curve.md)
supersedes the short ACK-positive window rule discussed in the 2026-09-28
receipt below. That receipt remains historical; no D01 physical Phase 1 has
run and this issue remains open.

2026-09-29 D01 implementation checkpoint: native and qualifier regression
suites pass, and a D01 candidate probe and exact-source manifest are staged
separately from the installed readiness probe. A worker-local four-process
loopback preflight failed the new per-peer curve during its first full grant:
the native maximum deficit reached 772,421,812,288 byte-µs against the exact
101,911,296,000 byte-µs bound. This is a charged service gap, not an added
handoff exemption or a physical Phase 1 result. The next physical Phase 1
attempt remains unexecuted and KI-006 remains open.

Current 2026-09-28 preflight: the worker's updated 30-second capture service
passed installed-runtime and 26.656-second ETL export qualification. The
32-wave four-peer workload candidate remains unqualified. Bounded loopback
variants accepted and retired over 67 MiB aggregate with four-grant high
water, but the corrected latest run formed zero all-peer eligible windows and
retained a real feedable below-floor interval. ACK-positive samples were
interrupted by ACK-empty samples; 17 synthetic evaluator cases pass, including
an idempotency regression for an earlier double-counted diagnostic summary. The
apparent terminal GNS limit result was misclassified: native result `3` is
`k_EResultNoConnection`, after the qualifier producer exhausted its
512-sample cap and shut down. This is now an architecture decision about the
existing queue/grant and per-window ACK-positive measurement contract. No
fresh physical Phase 1 attempt was launched and the physical probe pin was
unchanged. The 16-MiB/s floor, POOLED_SERVICE profile, and all later
acceptance gates remain unchanged. [Current receipt](devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md#local-four-peer-contract-remains-blocked-after-bounded-demand-correction-2026-09-28).
KI-006 remains OPEN; Foundation 3L remains B — PARTIALLY READY.

Single diagnostic Phase 1 attempt, 2026-09-28: **OPEN / UNDERFED QUALIFICATION
WINDOW; CAPTURE EXPORT FAILED AS RUN.** Four real clients reached Ready and
four grants activated, but 4,449 sampled journal-demand rows yielded only
two four-grant attributed-backlog rows and no eligible service-floor interval
or qualified batch. The producer completed 83 RPC and 82 Event samples
without a terminal scheduler rejection; the worker failed the sustained
overlap proof and the clients then lost the server. The old 6.714-ms under-floor
row remains indeterminate. A complete client pcap was finalized, while the
worker service's fixed 15-second hook deadline interrupted an export later
measured at 19.262 seconds. Post-run raw-ETL recovery proved all four
bidirectional tuples and cleared the owned service state, but did not change
the failed capture gate. A bounded 30-second helper source correction and
45-second endpoint acknowledgement wait have local regression coverage;
the installed service is unchanged. The temporary worker probe was rolled
back, and the one permitted physical attempt was not retried. Canonical
per-peer/aggregate service, later 3L gates and 32 actual clients remain
**not measured**. [Detailed receipt](devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md#one-diagnostic-phase-1-attempt-no-sustained-four-grant-window-2026-09-28).
KI-006 stays OPEN; Foundation 3L remains B — PARTIALLY READY; no 3M or merge.

Post-run attribution, 2026-09-28: the corrected Phase 1 under-floor row is
**INDETERMINATE** as a canonical service test. The 16-MiB/s arithmetic is
correct, but continuous feedable four-peer backlog and the ordering of the
producer's terminal transport rejection were not recorded. The producer's
bounded gameplay cadence does not establish a flood defect. Local abort
capture finalization and service-stop acknowledgement are corrected with
regression coverage; neither the new instrumentation nor these adapter
changes had been deployed to the physical endpoints at that checkpoint. The
single later diagnostic run is recorded above.
See the [attribution receipt](devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md#post-run-attribution-of-the-corrected-phase-1-stop-2026-09-28).
KI-006 stays OPEN and Foundation 3L remains B — PARTIALLY READY.


Corrected strengthened four-client Phase 1, 2026-09-28: **OPEN / FAILED
SERVICE-FEEDBACK GATE; CAUSE NOT YET ISOLATED**. The qualifier now launched
exactly one producer and three non-producers. The runner-owned tunnel proof
passed, four actual GNS clients connected and wave 1 began. Pinned native
feedback observed four active grants with journal demand, then one peer
first-sent 38,995 B in 6,714 microseconds against the unchanged 112,643-B
floor. The worker recorded `floor_failure=1` and zero qualified four-grant
batches. The producer client also saw `Transport rejected scheduler
submission`; the evidence does not establish its causal order with the floor
failure. Client abort capture was incomplete and worker capture stop timed
out at the endpoint, so the full capture gate also failed. Both endpoint and
outer lifecycle verdicts were FAIL. The single corrected attempt was not
retried; temporary state was cleaned and evidence retained in the
[current physical receipt](devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md).
Phase 2 Local/Node, 32 actual clients and final acceptance are not measured.
Next: attribute the GNS submission status, native feedback interval and abort
capture finalization before a correction or separately authorized physical
attempt. Preserve the service targets. KI-006 remains OPEN; Foundation 3L is
B — PARTIALLY READY; no 3M or merge. Historical updates below retain their
original scope.

Production-GNS Phase 1 on direct static fiber, 2026-09-24: **OPEN / STOPPED ON
INCOMPLETE FOUR-GRANT PROOF**. Four actual GameSession clients applied eight
512-KiB waves. Native feedback recorded exact accepted=retired 16,802,660 B,
first sent=ACKed 16,833,199 B and zero retransmission, with no below-floor
interval. Qualified four-grant overlap occurred in only three of eight waves;
the prescribed probe exited 1. This demonstrates no path-loss budget failure,
but does not establish repeatable required service. The 32-actual-client Local
and Node matrix, action/recovery and final Foundation sweep are not measured.
KI-006 remains OPEN; [physical receipt](devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md).
The later one-client physical readiness retry PASSED with bidirectional
captures. One separately authorized four-client readiness attempt then reached
four distinct GNS connections, server Ready/active 4/4 for the canonical
interval, and four clean client closes. It failed the capture gate: the client
raw pcapng ended mid-block and the worker miniport missed inbound traffic for
two of four source ports. A single later run independently qualified corrected
client finalization, worker NDIS capture and four-client coordinator labeling
before launching. Its physical results passed: four actual GameSession Ready
clients, four clean closes, both complete captures with every tuple in both
directions, and `FOUR_CLIENT_READINESS_ONLY` success at both endpoints and the
physical coordinator. The outer lifecycle adapter nevertheless returned FAIL
after its server agent ended `NEEDS_USER/MISSING_CAPABILITY` despite a successful
host result. The run was not retried; cleanup passed. Four-client physical
readiness evidence is recorded, while lifecycle wrapper completion and
strengthened Phase 1 funding remain open; see the
[physical receipt](devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md).
Next: reconcile the lifecycle wrapper status before treating the physical
prerequisite as fully closed. The bounded actual-GNS structural workload still
requires separate authorization and sustained qualified four-grant backlog with
complete host measurements; the unchanged
32-client matrix remains gated on Phase 1. The pre-existing static `/30` plan is retained as a
candidate deployment configuration; temporary scoped rules were removed.

Current funding review 2026-09-23: **OPEN / REAL-GNS PHYSICAL QUALIFICATION**.
The accepted [decision B](devdocs/CurrentArchitecture/PhysicalFundingGateReview3L.md)
establishes raw capacity and supersedes the synthetic zero-loss/NDIS-attribution
prerequisite in historical updates below. Preserve the fixed POOLED_SERVICE
profile; measure real transport cost, unique service and exact retirement under
static dedicated-link addressing. A healthy bounded four-grant actual-client
probe must precede both canonical 32-actual-client Local/Node runs. Those runs
remain required before closure. No merge, final 3L acceptance or 3M is implied.

Approved receive-handoff retry 2026-09-23: **OPEN / INSTRUMENTATION STOP**.
Expanded NBL/NDIS/TCPIP/WFP tracing reproduces 167 missing sequences after
the last observed filter edge and before TCP/IP. The verified retained suffix
contains 323,058 packets at each pre-TCPIP edge versus 322,891 at TCP/IP/application.
No native drop reason or distinct NBL metadata joins the loss. NDIS throttling
ends 120.6 ms before the burst and does not identify its owner. Two clean
three-second controls without a correction do not establish funding repeatability.
No supported correction was selected; DHCP, rules and captures were restored.
Next: Windows/NDIS or vendor-assisted indication/return ownership and queue
telemetry, then a justified correction and the unchanged funding matrix.

Direct fiber update 2026-09-23: **OPEN / PHYSICAL FUNDING NOT ESTABLISHED**.
The [10 GbE receipt](devdocs/CurrentArchitecture/FiberPhysicalPreflight3L.md)
confirms the installed direct Mellanox path and 9.246–9.471-Gbit/s TCP. WFP
tracing identifies WSH Default Inbound Block filter 147332 at IPv4 ALE
receive/accept on a local/raw path. Matched DHCP → static same-address → DHCP
profiles give 140.840 → 900.001 → 145.683 Mbit/s, NETIO shares
64.646% → 2.745% → 66.549%, and FindCacheMatch shares 44.356% → 0% → 46.308%.
AFD tracing directly observes DHCP service PID 2984 creating a raw UDP socket.
The default WSH block is not bypassed; static remains a reverted diagnostic
configuration pending a qualified physical profile and accepted address plan.
Elevated receiver tracing now localizes the final 2,244 missing sequences
after the last filter upper edge and before TCP/IP's capture point. Every
sequence observed at TCP/IP reaches the application. Removing Npcap's fiber
binding does not eliminate loss and is reverted. A 29.657-ms receiver gap is
29.644 ms blocked, then 6 microseconds runnable before scheduling: receiver
affinity/priority tuning is not supported. The exact upstream handoff queue
and drop reason remain unobserved; loss is intermittent and not causally bounded.
The requested stop applies; the final funding matrix was not run. No Engine
defect or hard fiber ceiling is established. Original DHCP, addresses, bindings,
NIC settings and cleanup are verified. Instrument that receive handoff's
queue/indication ownership, establish a supported correction, then demonstrate
repeatable
UDP funding against the unchanged 805.306368-Mbit/s
envelope, then qualify 32 actual clients through both Local and Node. Those
service/resource/cleanup gates remain **not measured**. Foundation 3L remains
**B — PARTIALLY READY**, final acceptance is deferred, and 3M remains blocked.
The September 15 deferred-cable statement below is historical.

Physical resumption update 2026-09-15: **OPEN / PREFLIGHT INCONCLUSIVE**.
The [current physical receipt](devdocs/CurrentArchitecture/PooledPhysicalQualification3L.md)
now records three independent TCP trials each way and sequence-accounted UDP
on the current 7950X3D/5900X LAN against the unchanged 96-MiB/s envelope.
Clean trials exceed the envelope, but reverse UDP loss, pacing variation and
one playback-window failure remain unattributed. Missing completed frames in
that failure are not exact network packet loss. The conservative gate is not
passed; no hard 1-GbE limit or Engine defect is established. The earlier local
firewall privilege blocker was avoided with client-initiated pull connections.
All four temporary worker rules and all benchmark processes are gone.

The operator confirms **no direct cable yet**; the planned **25 Gb fiber cable
is deferred until delivery**. Current intermediate hardware is unidentified;
future fiber capacity is not measured. Identify the installed path and repeat
attributable preflight, then, if it passes, implement the smallest missing
actual-client harness and run the unchanged Local/Node workload in the same
task. Both 32-actual-client runs and all their service/resource/cleanup metrics
remain **not measured**. Production Engine/loopback qualification is retained;
3L remains **B — PARTIALLY READY**, not ready to close, with no 3M or merge.

Name correction update 2026-09-14: **OPEN / WORKER PRODUCTION QUALIFICATION COMPLETE**. The
[integration receipt](devdocs/CurrentArchitecture/PooledReliableServiceIntegration3L.md#known-object-name-correction-2026-09-14)
records the acceptance-safe Name suffix correction at `555354bb5`, with recovery
measurement corrected at `ab3f0d61d`. MSVC, Clang sanitizers, the established full
GNS scope and qualified Local/Node 32/200 simulated-peer regressions pass.
Structural/mixed convergence is 1.638/1.682 seconds on MSVC and 1.980/1.986 seconds
under sanitizers; service recovery is separately below 1.12 seconds, with exact
debt conservation and zero raw journal remainder at 20 seconds. The accepted
profile, gameplay guarantees and barrier/acceptance semantics are unchanged.

The receipt requires terminal-green hosted checks for the executable source and
consuming publication before final qualification handoff. Once those gates pass,
the exact next task is the **separate funded 32-actual-client Local/Node physical
qualification**; that evidence is **not measured** here. Production code and
loopback results do not close this physical gate. Foundation 3L remains
**B — PARTIALLY READY**, and 3M remains **BLOCKED / NOT STARTED**.

Historical production integration stop 2026-09-14: The isolated
checkpoint based on `222c5beb3` implements pooled credit/grants, exact native
retirement and generation cleanup, but its canonical structural and mixed
overload cases each retain 7,248 journal records after the fixed 20-second
recovery deadline. The [historical stop receipt](devdocs/CurrentArchitecture/PooledReliableServiceIntegration3L.md#production-recovery-conflict-2026-09-14)
records the 180-MiB workload versus 2-MiB/s peer-credit conflict, exact debt
conservation and limited passing gameplay evidence. The model's one pending
group does not cover the retained authoritative history. Reconcile that
contract before continuing integration or physical qualification; do not
retune the profile or discard history to obtain a PASS. This source is not
qualified by prior CI. Foundation 3L remains **B** and 3M remains blocked.

Retirement-attribution update 2026-09-14: **OPEN**. The
[current qualification receipt](devdocs/CurrentArchitecture/ReliableTransportFeedbackProof3LValidation.md#retirement-attribution-qualification-2026-09-14)
records exact sender-local native message retirement at `a5a182ff9`, 12/12 mixed
cases and the real-GNS identity proof on MSVC and Clang sanitizers. Native CI
passes 58/58 MSVC and 50/50 Linux sanitizer tests; GNS CI passes its five transport
tests and full established scope. Native attribution is **READY FOR POOLED-SERVICE
INTEGRATION**. The attribution blocker is closed; the
[historical integration stop](devdocs/CurrentArchitecture/PooledReliableServiceIntegration3L.md)
remains evidence that aggregate ACKs cannot identify structural retirement.
Production receipt propagation and `POOLED_SERVICE` admission are not implemented;
physical 32-client qualification remains not measured. Foundation 3L remains
**B — PARTIALLY READY**; 3M remains blocked.

Native feedback update 2026-09-14: **OPEN**. The narrow pinned-GNS feedback
boundary is implemented; its [receipt](devdocs/CurrentArchitecture/ReliableTransportFeedbackProof3LValidation.md)
records native ACK/payload semantics, generation/teardown, checked overflow,
real-GNS fixtures and sanitizer gates. This supplies observations for the next
separately authorized pooled-admission task. It does not implement
`POOLED_SERVICE`, fund a physical path or qualify 32 actual clients. Foundation
3L remains **B — PARTIALLY READY** and 3M remains blocked.

Update 2026-09-14: **OPEN**. The selected Option C design/model now passes all
33 existing model and nine registered hardening cases (**42/42**) in the
networking-contract test. The [proof receipt](devdocs/CurrentArchitecture/PooledReliableServiceProof3LValidation.md)
records debt conservation, the unchanged FIFO gameplay bound, feedback freshness,
slow-peer regrant protection and the corrected pending-versus-committed test
expectation. No model/profile retuning or production networking change occurred.
The production pooled-service implementation remains **NOT IMPLEMENTED**;
actual 32-client physical qualification remains **NOT MEASURED**. A model-target
1 GbE envelope is not measured/funded service. Implementation readiness also
requires matching docs validation and required current-source terminal-green CI.
Foundation 3L remains **B — PARTIALLY READY**; no 3M. Earlier receipts below
retain their source/profile scope and do not close these remaining gates.

Update 2026-09-13, physical preflight: **OPEN** for the intended 32-actual-client
profile. The [hardware receipt](devdocs/CurrentArchitecture/PhysicalDeploymentPreflight3L.md)
finds a 1 Gbps active link on the inspected worker, below the accepted 32-peer
2.147 Gbps application reservation / 4.295 Gbps aggregate backend ceilings.
The task stops before client execution; this is a physical funding shortfall,
not an attributed engine defect. No new physical scale or headroom is measured.
A 10 Gbps dedicated host/path is only a candidate. Hypothetical 200/500-client
support is not a product requirement or the reason this issue remains open.

Update 2026-09-13, disposition **B — narrowed, OPEN**:
[qualified recipient-service evidence](devdocs/CurrentArchitecture/ContentClientScaleQualification3L.md#recipient-service-qualification-2026-09-13)
passes Local and Node with 32 and 200 protocol peers, eight root-motion
Characters/eight recipients each and a contract-compliant Remote/action mix.
All qualified due/accepted states are observed, with zero unresolved due work,
lateness or scheduler rejection. RPC/Event/action targets, structural
convergence, all-interval message/byte budgets and measured shutdown ownership
pass. Due→observation maxima are 72.5193/72.2049 ms at 32 and 76.4958/65.6693
at 200 (Local/Node); the 200-peer raw cadence guard can still exceed 250 ms.
No production correction or historical guard relaxation is retained.

This has one actual gameplay client per case. New Character-fanout service with
32 actual GameSession clients and a funded physical deployment's actual-path
capacity remain not measured. Historical 200/50 remains over-limit;
its later reload also leaves 199 unresolved forecast records, without missing
produced/accepted observations. Do not claim whole-stress-trial cadence closure
from the passing load join. These limitations are explicit, not a new renderer
latency requirement. KI-006 is not resolved and Foundation 3L is not Ready.

Update 2026-09-13, joined attribution: [per-state evidence](devdocs/CurrentArchitecture/ContentClientScaleQualification3L.md#joined-recipient-attribution-2026-09-13)
classifies the ~650-ms 200/50 gap as mixed cadence/shared-fixture timing. The
next due state reaches observation in 52.2210/50.1847 ms Local/Node; the raw
656.3057/649.8674-ms interval includes 12 correctly scheduled ticks and
250.5823/245.2356 ms of serial observer work. All 45,120 recipient-state keys
match through acceptance and observation. Complete forced-fanout accounting
also fails this diagnostic's peer/global reliable message-count envelope.
No single production owner is established; no production optimization or guard
relaxation is retained. Qualified real-client scale remains open, not disproved
or closed by this unqualified shared-harness result. See diagnostic `7b299ab61`.

Update 2026-09-13: the [current-source client/scale receipt](devdocs/CurrentArchitecture/ContentClientScaleQualification3L.md)
at `543cc0de0` passes official single-Player Local/Node RPC and lifecycle service,
but reproduces the retained 200/50 streaming diagnostic failure: Local/Node
Character/root observation gaps 662.822/642.989 ms, versus 202.476 ms control.
Those fixture guards are not a universal product contract. Complete high-scale
real-client/fanout and physical-capacity qualification remains not measured;
no conclusively attributed production correction is retained. KI-006 stays open.

The current [SEC-3L-001 closure](devdocs/CurrentArchitecture/ContentAvailabilitySecurityClosure3L.md)
passes the reviewed Foundation 3L security gate after a generation-safe package
ownership correction. Client/scale, aggregate Event/action fanout, physical-link
capacity and rendered-client qualification remain open; KI-006 is not closed.
Earlier security-gate statements below are historical checkpoints.

The former KI-008 combined aggregate failure is corrected; its
[attribution and scoped overload/recovery evidence](devdocs/CurrentArchitecture/GnsPacketSequenceAttribution3L.md)
does not close the independent KI-006 qualification gates.

The September 13 [workload contract](devdocs/CurrentArchitecture/ReliableGameplayWorkloadContract3L.md)
selects conservative engine-owned defaults and implements canonical real
GameSession/GNS qualification fixtures. The missing-number stop is superseded.
The sustained fixture demonstrated accumulated grounded downward velocity in
the shared locomotion policy, eventually preventing compact Character state
encoding; the policy now resets downward velocity while grounded before
applying gravity. The [validation ledger](devdocs/CurrentArchitecture/ContentAvailabilityFoundation3L_3Validation.md)
separates measured workload results from remaining client, scale, journal and
security gates. KI-006 remains open; a passing subset does not close it.

The [overload qualification Part A review](devdocs/CurrentArchitecture/ReliableOverloadQualification3L.md)
at `5ada43a5773a96b1f0a97e9e6299baa6f420b762` stops at the missing qualified
gameplay payload/arrival envelope. Codec maxima and reserve arithmetic do not
define supported ordinary large requests, responses or Events. Overload,
recovery and production journal margin remain unmeasured in that checkpoint;
no production defect is attributed. Native and complete GNS sanitizer CI are
verified green at that HEAD. The contract gap and independent client/scale and
security gates keep Foundation 3L partially ready.

The [profiled GNS sanitizer attribution](devdocs/CurrentArchitecture/GameSessionReliableAdmissionAttribution.md)
closes the previously blocked GNS sanitizer progression at code revision
`8fa332416`: four base fixtures, profiled GameSession, and the executed 12-case
production byte-admission matrix all pass with ASan/UBSan/LSan enabled. The
mandatory-deferral assertion was a test-contract error; production admission is
unchanged. This does not close the gameplay/client, overload, journal, scale or
security qualification requirements of KI-006.

Update 2026-09-12: the user approved RPC p95/p99/max 150/250/500 ms and
Event/action max 250 ms, >=25% gameplay reserve, no large-group exception and
unqualified low-rate compatibility. Hierarchical reliable-byte admission is now
implemented and targeted-validated: elapsed-time per-peer/global
credit, finite backlog feedback, exact pre-acceptance sizing, encoded reuse and
generation-safe cleanup. The candidate 8 MiB/s application / 16 MiB/s backend
profile passes official near-max Local/Node 100-RPC cases at p99/max
72.058/73.316 and 74.723/75.801 ms with no timeout/error. This does **not** close
KI-006: full gameplay burst/request-path and client qualification, overload,
production journal margin, security and current-source CI remain open until
measured. The statements below that admission is absent describe the earlier
published checkpoints, not this implementation. See the
[current implementation contract](devdocs/CurrentArchitecture/NetworkingReliableDeploymentContract.md).

The published envelope assessment (`108200d07`) now has a
[deployment/atomic-group contract follow-up](devdocs/CurrentArchitecture/NetworkingReliableDeploymentContract.md).
One actual dependency group can occupy all 524,256 GRPL application bytes of
the 512 KiB GNS complete-message ceiling. At 256 KiB/s that necessarily means
two seconds of ideal FIFO serialization. The proposed finite-credit compatibility
class cannot be called a subsecond gameplay guarantee; reject an impossible rate/
latency profile rather than silently changing content or ordering. Production
admission remains unimplemented, and KI-006 remains open.

- Status: Open; Foundation 3M remains gated.
- Priority: High
- Area: Structural materialization and gameplay latency under peer scale.
- Evidence: [Foundation 3L.2 measured validation](devdocs/CurrentArchitecture/ContentAvailabilityFoundation3L_2Validation.md).
- Relevant code: `src/network/ReplicationCoordinator.cpp`,
  `src/network/ReplicaApplier.cpp`, `tests/GameSessionBenchmark.cpp`.

The reliable-service envelope follow-up is diagnostic/design only. At the current
256 KiB/s backend rate, 50/75% byte pacing reduces controlled RPC maxima to about
53 ms while 1.45 MB of structure takes 10.97/7.32 s to converge. This is not an
official-path fix. Valid planned hard-reference work can encode to 123,183 bytes
in two operations; a universal small burst cap needs an explicit oversized-group
policy. Production implementation stops for the trusted deployment/aggregate rate
and compatibility decision. See
[NetworkingReliableServiceEnvelope.md](devdocs/CurrentArchitecture/NetworkingReliableServiceEnvelope.md).
No rates, lanes, wire, buffers or semantic bounds changed; KI-006 remains open.

Latest post-publication latency attribution (`latency-v3`) distinguishes due-tick
cadence from accepted-packet delay. Healthy Local/Node p99 is 31.00/33.88 ms;
raw Character/root observer gaps are 645.85/658.24 ms, throughput loss 9.02%/9.56%,
and all sampled ordinary publications are produced on their due tick. The worst
Local root sample spans 12 dilated shared-fixture iterations, not scheduler
rejection. The observer precedes real-client application; it is not a presentation
guarantee. A 512-operation client callback costs 24.03 ms including 8.89-ms native
preflight and 9.18-ms live application. No transaction redesign is implemented.

The official reliable service follow-up (2026-09-12) identifies fixed GNS
capacity plus same-lane FIFO backlog: configured minimum/maximum/effective send
rate are all 262,144 bytes/s. The fuller capture peaks at 1,449,887/1,449,885
pending reliable bytes, dominated by four large GRPL frames. Correlated RPC 6
has a 5.32–5.34-second estimated backend wait, microsecond engine handoff, and
5.52–5.54 seconds of client-local request-to-response availability; Player polling
and callbacks add milliseconds, not seconds. Smaller messages with identical
queued bytes do not help; test-only 1/4 MiB/s rates reduce backend drain by about
4x/16x. No production rate/buffer/lane/order/wire change is retained. The next owner
is **Networking Reliable Service Envelope**: supported rate/capacity policy,
dependency-complete/KI-007 atomic-group byte bound and bounded backlog admission,
before considering separate lanes. See the
[transport attribution ledger](devdocs/CurrentArchitecture/ContentAvailabilityFoundation3L_3Validation.md#official-reliable-service-attribution-2026-09-12).

The preceding official near-max GNS test FAILS for both Local and Node: one RPC
times out at approximately five seconds; Remote receive gaps reach 5.544/5.529 s
while Player event-loop gaps remain below 54 ms. Sampled pending reliable bytes
reach 1,420,323; GRPL and reliable application use the same backend connection
stream. Backend rate/capacity versus head-of-line service is the next dedicated
owner; precise per-RPC backend wait remains unmeasured. No lane, wire, budget or
priority change is justified here. See the
[post-publication ledger](devdocs/CurrentArchitecture/ContentAvailabilityFoundation3L_3Validation.md#post-publication-due-to-recipient-attribution-2026-09-11).
Client preflight, official transport guarantees, overload/recovery, the 170-entry
startup journal margin, current-source security and terminal CI remain open.

The preceding attribution/lookup checkpoint (`derive-lookup-v3`) preserves bounded
planning and reduces healthy Local/Node tick p99 to 30.82/30.74 ms by making
existing 3E examinations cheaper. Recipient maximum gaps remain 652.59/649.24 ms,
and throughput losses remain 9.15%/9.05%. 500-peer reload p99/max is 43.27/52.41 ms,
convergence 4.257 s / 158 ticks; load still has an 846.56-ms recipient gap.
3E dominates average derivation, but saturated structural publication and
client/observer service dominate the recorded gap windows. No jobified phase
was added: current spatial queries lack a safe pinned/read-only contract.
The [assessment](devdocs/CurrentArchitecture/ReplicationDerivationAssessment3L.md)
defines the proposed boundary and measured scaling ceiling. MSVC 6/6 and targeted
Clang 19 ASan/UBSan/LSan 7/7 pass; health, official delivery, client preflight,
overload, startup journal, security and CI remain open.

The preceding dependency-complete checkpoint (`planning-boundary-v13`) bounds private
planning at 65,536 charged steps/tick, rotates after at most 2,048 peer steps,
and exposes only complete, revision-valid, costed batches to unchanged 3J.
Maximum measured 500-peer discovery examinations are 56,007; global planning
reservations peak at 4,574,325 credits under an 8,388,608 ceiling. Reload tick
p99/max improves from 95.95/178.46 to 48.89/56.29 ms and Character/root max gap
from 1,147.77 to 459.61 ms, but convergence slows from 3.233 s / 33 ticks to
4.743 s / 158 ticks. Healthy 200-peer Local/Node recipient gaps worsen to
686.79/703.88 ms, despite lower maximum ticks and RPC tails. Streaming health
still fails. Recurring 3E query/hysteresis and post-selection publication/apply
cadence remain measurable owners; the new work cap is not an end-to-end latency
guarantee. One acquisition/initial admission, exact 8,192 cap and KI-007 semantics
are preserved. Startup journal observed margin improves to 170 entries but
remains unproved as a production bound. Client preflight, official reliable
transport, overload, current-source security and CI remain open. See the
[current validation ledger](devdocs/CurrentArchitecture/ContentAvailabilityFoundation3L_3Validation.md)
for exact final validation and unfavorable results as well as improvements.

The following paragraphs retain **historical checkpoints**. Statements that
general planning was unbounded describe those older sources, not v13.

The original 2026-09-11 attribution reproduces a 488.404-ms reload tick: three
whole-peer fixup passes examine 8,517,714 properties in 433.633 ms. The first
pass alone has 2,840,760 examinations, 90.36% scalar; encode retries are zero.
After the separately validated KI-007 correction, a canonical immutable
reference-property index reduces worst-tick examinations to 99,692 and combined
fixup CPU to 170.473 ms. Reload tick maximum is 221.427 ms and Character/root
gap is 1,200.67 ms; convergence remains 33 ticks (3,312.04 ms). Healthy Local/Node
load p99 remains 40.079/40.327 ms, with 404.849/397.816-ms recipient gaps and
approximately 11.47% throughput loss. Streaming health still fails. General
planning remains unbounded by a narrow per-tick work cap: 138,752 discovery
examinations and over half a million referrer visits occur in measured ticks.
The measured index is not the previously discarded, unvalidated candidate;
exact source and validation checkpoints are separated in the 3L.3 ledger.

The next instrumented attribution separates 138.570 ms of Desired/referrer work
(133.467 ms outside reference-cost construction), 6.628 ms accepted-edge work and
31.194 ms restoration. A call-local ordered map join reduces combined fixup work
from 176.494 to 128.261 ms with the same scopes/counts. Reload maximum becomes
178.456 ms, recipient gap 1,147.77 ms and convergence 3,233.36 ms / 33 ticks.
It retains no frontier and does not bound the 138,752 discovery examinations or
536,510 Desired referrer visits. KI-007 semantics and the exact cap are unchanged.
Dependency-complete resumable planning requires a larger reviewed completeness/
revision barrier; adding a partial scan cap is not a safe closure of this issue.

The [3L.3 diagnostic checkpoint](devdocs/CurrentArchitecture/ContentAvailabilityFoundation3L_3.md)
now retains the settled-static physics, typed Character-inspection and bounded
retired-catalog reclamation corrections. Retirement formerly made 143,447,770
peer-Known probes in the instrumented 500-peer eviction; private prepared/accepted
identity leases now permit at most 4,096 ownership checks/tick without peer
searches. Retirement CPU is 8.569 ms total / 0.604 ms maximum, and the previous
1.055-second eviction Character/root gap falls to 449.639 ms. Eviction converges
in 1.457 seconds / 32 ticks; load remains 2.690 seconds / 33 ticks.
The first new 200-peer control/Local/TLS Node load p99 is
4.571/39.901/42.373 ms, with Local/Node recipient gaps 621.101/680.095 ms.
General pre-3J discovery still reaches 68,258 examinations (106,176 at 500 peers)
and is not covered by the retirement cap. Complete Desired/Known/dependency/
fixup planning and recurring 3E query/hysteresis work remain the owning paths;
the general bounded-discovery objective is not completed. The historical
26-entry journal margin is startup-cumulative, not
the observed eviction margin; its whole-session bound remains unproved.
The separately rebuilt official Player keeps polling while RPC handlers return
promptly and replies wait behind reliable structural backlog: Remote service
gaps historically reach 3.785/3.796 seconds. That official transport path was not
rerun by the loopback correction. No new discovery queue, selection-cap increase,
wire change or legal content-limit change was made. The new retired-catalog
cardinality ceiling and ownership memory cost are explicit in the ledger;
full overload/security/CI closure, B and the 3M gate remain unchanged.
The 2026-09-11 reviewed-owner slice confirms that scalar payload changes already
avoid dependency-plan invalidation and retains only a shared per-step send
allowance to service late Character/Remote production after drained structural
work. Actual recording-transport Send now occurs in the production step, but
Local/Node load p99 remains 40.42/41.89 ms. Their max recipient gaps are
398.41/403.00 ms; the unchanged completion-driven RPC fixture now runs 301 rather
than 401 ticks, so raw cumulative measurements are not equal-duration comparisons.
500-peer reload still exposes a 478.171 ms server tick dominated by structural
selection/fixup work, 138,752 discovery examinations and a 1.508-second recipient
gap. The exact 8,192 selection cap and convergence remain intact, but surrounding
work is not bounded by it. One changed client property at 8,193 identities still
preflights the entire candidate world (roughly 89--101 ms total across retained
runs); localizing that requires an accepted transaction-validation design.
Official transport, client, overload, security and CI gates remain open. See the
current ledger for unfavorable 500-peer eviction/reload tails as well as the
isolated handoff improvement. No general frontier or readiness upgrade is claimed.
Historical measurements follow.

The candidate 32-peer 512-object workload converges within the existing 3J cap,
but RemoteFunction p99 rises from about 26 ms without the streamed region to
about 382 ms with it Resident. A 100-peer run similarly converges but records
one owner-action submission failure during reload. The first grouped 500-peer
fixture missed client-code hydration before the workload Script materialized;
that attempt produced zero RPC samples and is not an RPC failure. Establishing
the gameplay client first exposed a separate full-rate receive overload:
500 input commands/tick exceed the official host's one 128-event Poll call/tick,
and reliable gameplay traffic is disconnected when the simulated queue fills.
An explicitly staggered 12 Hz input profile converges without raising the host,
transport, or 3J limits, but the 500-peer baseline already has roughly one-second
RPC latency and streaming raises p99 to about 1.9 seconds; owner-action submission
failures remain. These are measured
performance/availability failures, not evidence that the previously fixed
RemoteFunction access violation has returned. Concurrent bounded build activity
is recorded and isolated confirmation is still required.

Resolution requires attributed baseline/resident/streaming measurements, healthy
owner/action/root-motion and Remote traffic, and clean 32/100/500 convergence
without raising limits, bypassing 3E/3J, or weakening replica validation. Full
semantic snapshot validation during incremental replica application is an
inspection lead, not a proven exclusive root cause.

The original 512-object / 1,048,197-byte official network-first Player case exceeded
its three-minute deadline with both Local and TLS Node providers. A non-invasive
main-thread stack, symbolized against an identical executable text section,
passes through string allocation, property/subtree encoding, SetParent,
LoadSnapshot, ReplicaApplier::ApplyFrame and GameSession::Poll. The Server reaches
Resident/reload while Player does not finish. Async output drainage excludes the
previous redirected-pipe explanation. Preserve this as a client materialization
regression target; do not bypass semantic validation to make it pass.

The final-correctness candidate removes discarded validation-world journal work
and reuses native preflight for exact repeats of already validated native
properties, while retaining live setters and changed-state preflight. The
isolated official Local/TLS Node maximum-content runs now have maximum frame
intervals of 35.193/35.911 ms and event-loop gaps of 44.184/45.566 ms. Both fully
materialize, evict and reload the 512-object hierarchy, but their 100-call RPC
gates still fail with two/one timeouts. Server handler traces show prompt
execution and replies queued behind large reliable structural messages. This is
a remaining transport-service problem, not evidence of a multi-second client
CPU freeze or a RemoteFunction access violation.

The 500-connected / 100-Character spectator baseline is also unhealthy before
streaming (tick p99 322.706 ms; RPC p99 691.378 ms). The fixture broadcasts a
diagnostic Attribute update to all 500 peers for every RemoteEvent. A controlled
follow-up uses the existing accepted-ACK counter instead of that diagnostic
broadcast; no Character, Remote, relevance, transport or 3J limits are raised.
The broadcast-heavy results remain capacity evidence, not an isolated
Character-only or content-streaming attribution.

The final-correctness follow-up now has an isolated, healthy deterministic
200-connected / 50-active-Character control (five Characters per spatial
neighborhood, 12 Hz staggered owner input). Correcting Windows relative-sleep
overshoot and keeping root-motion crossings on the fixture ground makes all
four no-streaming phases pass their health gates. Local/TLS Node baselines also
pass, but their load, eviction and reload phases fail. During load, Character
wall throughput falls from 8,218 states/s in the equivalent no-streaming phase
to 2,778/2,707; maximum publication gaps rise from 205.696 ms to
1,075.46/1,133.61 ms. RPC p99 rises from 52.780 ms to 424.622/430.907 ms.
This is now a genuine streaming differential failure, distinct from dense
500-Character baseline saturation.

A separate 3J correctness regression reproduced replay of pre-publication
Attribute history after an accepted complete Enter, rolling a replica backward.
Per-object prepare-time journal watermarks prevent that replay while retaining
post-prepare mutations and the existing structural cap. Load selections in the
200-peer fixture fall from approximately 1.26 million to 104,840 operations;
the remaining gameplay degradation is not thereby resolved.

After that correction, all four near-maximum/property-heavy official Player
Local/TLS Node profiles complete load/evict/reload and 100 RPCs without timeout,
error or crash. Maximum frame intervals are 46.783/46.567 ms for the near-limit
payload and 95.473/88.960 ms for property-heavy content; event-loop maxima are
57.888/60.417 and 108.894/101.981 ms respectively. Nevertheless continuous
Character/Remote handler service has 3.49–3.81 second maximum gaps and RPC p99
is 3.56–3.68 seconds. CPU responsiveness improvement does not close reliable
transport/service latency. These are headless official Windows hosts, not a
graphical GPU-present latency proof. See the final-correctness closure report
linked from the measured validation document for exact revisions and remaining
gates. Foundation 3M remains blocked.

## Maintenance rules

- Record only issues verified against the current branch.
- Include evidence, affected paths, and concrete resolution criteria.
- Link an upstream issue or commit when it contributed evidence, but verify the
  local implementation independently.
- Remove resolved entries in the same commit as the verified fix, relying on
  Git history for the completed record.
- Keep planned features in the roadmap and security findings in their security
  workflow rather than duplicating them here.
