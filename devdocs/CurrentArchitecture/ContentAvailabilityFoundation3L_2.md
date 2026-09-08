# Content Availability Foundation 3L.2

Status: validation in progress, 2026-09-08. This document does not grant the
Foundation 3M readiness gate. Measurements below are candidate-working-tree
evidence, not terminal publication CI results.

## Baseline and scope

Engine `main` and Node `master` were pulled and verified against their intended
remote branches before modification. Starting revisions:

- Engine: `b54256805b1e4af8f8ad60a8672507ce26bb0be2`.
- Node: `4e6bc0a8ff37c58a8a122262d746dd8934b71835`.
- No intervening remote commits were found at that baseline check.

Local working trees remain the source of truth. Only scoped changes are copied
to disposable validation trees under `C:\Sandbox\Codex` on `dockerbox`.
No commit or push is used merely to synchronize the worker. Existing untracked
build artifacts and unrelated worker services are preserved.

No GRPL, GCHR, handshake, Remote protocol, package semantic version, ordinary
Luau API, Studio API, provider authority, or automatic residency policy changes
are introduced. Trusted explicit demand still feeds ContentAvailability;
3K/3H project/index Resident objects, 3E selects relevance, and 3J alone commits
structural materialization. Node supplies verified immutable bytes only.

## Narrow production corrections

### Decoded ownership and bounds

The pre-3L.2 encoded cache/completion budgets did not charge parsed JSON retained
by Available records. A 64-unit, 512-Part, failed-dependency fixture retained
87,718,245 tracked allocation bytes despite a 16,825,728-byte encoded cache.

The service now charges every retained decoded document to a shared atomic
accounting object. The RAII document wrapper is shared by completion, Available,
and Main-drain owners without double charging. It releases the document before
returning its charge. The default and hard maximum is 16 MiB, with trusted
configuration validated between 1 MiB and 16 MiB. Existing input, worker,
in-flight, encoded completion, cache, admission, and eviction limits are not
raised. Object count is checked against the manifest before Instance creation.

An individual document larger than this decoded limit fails closed. Aggregate
pressure drops the DOM and retains verified immutable bytes, then defers parsing
through the existing bounded worker queue when admission dependencies permit.
It does not refetch content solely because the decoded budget was full. Failed
dependencies do not retain an Available DOM indefinitely. Main clears the local
completion vector before admission so it does not prolong otherwise dead DOMs.

Accounting includes conservative STL-owned capacities and wrapper overhead.
It excludes allocator headers/fragmentation and parser temporaries. Temporary
parsing remains bounded by existing per-input limits and at most eight workers;
the 16 MiB limit must not be described as a total-process heap/RSS ceiling.

Added aggregate metrics report decoded current/high-water bytes, decoded
deferrals, Available decoded current/high-water bytes, detached-object
high-water, resident-origin metadata estimates, and retained record count.
There are no production per-peer or per-content metric labels.

### Final world-owner retirement

`ContentAvailability::Stop` cancels/joins work, destroys resident roots, releases
cache/decoded/completion storage, and clears manifest, records, dependency, and
index tables. The authoritative world's final change records remain available
while consumers still own the DataModel.

The final `DataModel` destructor now privately retires only that existing
ObjectId scope from ChangeJournal and RenderDirtyAccumulator. It never creates
an identity, globally clears another world's state, or adds a public reset API.
Destroy and final shared-owner release are deliberately distinct. A 100-world
regression keeps another world live and verifies its journal/dirty state is
unchanged throughout retirement of the other worlds.

The global unscoped journal and reusable ObjectRegistry capacity are separately
attributed process infrastructure; they are not decoded content or live world
graphs. A test-only global journal clear is used solely after fixture teardown
for allocation attribution, never in production shutdown.

### FullyResident startup backlog

Bootstrap no longer treats zero active provider requests as failure while
verified Available units await bounded admission. It continues draining that
backlog until Resident, a real content failure, or the existing deadline.

### Negotiated structural byte bounds

GameSession passes the peer's existing negotiated reliable-message byte limit
to 3J production. A frame larger than that limit is retried with a smaller
transition budget before scheduler acceptance. Critical bootstrap and hard
reference groups remain indivisible; an indivisible over-limit operation fails
closed. No hard reference is truncated, no Known/cursor/sequence commit occurs
for a rejected oversized candidate, and the global 8,192 transition cap is
unchanged. Existing 8 MiB protocol and 512 KiB transport ceilings are not raised.

## Validation fixtures and interpretation

The coarse-cell range check also now divides/floors in double precision. The
previous float comparison rounded `INT32_MAX` to 2^31 and accepted that
out-of-range coordinate. The new regression reproduced both a Windows test
failure and Clang UBSan float-to-int overflow before correction. The signed
32-bit coordinate range itself is unchanged.

- `ContentAvailabilityBenchmark --decoded-pressure` and `--decoded-overload`
  attribute blocked and draining 64-by-512-Part cases, including Stop and final
  world-owner release. `--lifecycle` runs 100 same-process 512-object, 1 MiB
  acquisition/admission/Stop lifetimes.
- `GameSessionBenchmark --content-scale` uses real Engine, 3E, 3J, and one real
  network-client Engine, with remaining peers using the existing simulated
  transport and ordinary GRPL/GCHR observers. One shared region is requested
  once; eviction/reload checks fresh runtime identity and exact properties.
- Gameplay runs ongoing RemoteEvent, 100 ordinary Luau RemoteFunction calls per
  phase, owner actions, and ten real Animator root-motion tracks. Spatial
  consistency is checked at the relevance safe point, before later authority
  motion intentionally dirties the index. The extra full validation traversal
  is excluded from the measured production tick samples.
- Trusted test focus includes the initial peer neighborhood and content site.
  The grouped 500-peer case has 20 neighborhoods of 25 Characters, with the
  common site also relevant. The all-to-all 500-peer diagnostic remains separate;
  it already overloads its no-content baseline and is not silently discarded.
- Official RSS uses relocated production Server distributions and Local/TLS
  Node providers, 100 ms RSS/PrivateUsage/thread/handle sampling, and explicit
  load/evict/reload. `--content-churn-cycles` is a bounded internal test option
  (1–10,000, OnDemand only); default production execution is unchanged.
  Standalone churn uses existing startup-smoke composition without a peer and
  exits after a verified final drain. Network-first churn and standalone churn
  are reported separately; the latter is not a Player vertical pass.
- The analyzer reports the second half of completed cycles, before final drain,
  including distributions, OLS bytes/second slope, and first/last quartiles. A
  positive short-window slope alone is not a leak verdict, and a final low sample
  alone is not a plateau verdict.
- Official full-process lifecycle uses the same Node TCP and Server UDP endpoints
  across repetitions, fresh processes/providers, ordinary gameplay, and final
  process waits. Existing Windows Node process teardown is forced by the fixture;
  it proves process/port reclamation, not graceful Node signal handling.

## Candidate measurements so far

These timing samples overlap bounded worker build activity and require isolated
confirmation before a performance-readiness claim.

| Case | Measured result |
| --- | --- |
| Failed-dependency decoded pressure after fix | Decoded retained current 0; high-water 3,230,336 bytes; tracked peak 3,416,424 bytes |
| Independent decoded overload after world retirement | 64 Resident units, 64 acquisitions/admissions; decoded high-water 16,151,680 bytes; 53 decoded deferrals; no refetch/failure |
| Same overload allocation attribution | 183,614,121 live tracked bytes Resident; 19,481,746 after Stop; 19,474,635 after service destruction; 6,726,241 after world destruction; 1,147,098 after test-only global journal clear |
| 100 same-process lifetimes | Tail tracked range about 11,872,300–11,875,423 bytes, peak 19,730,919 bytes; no continuing per-cycle growth after warm-up |
| World-retirement regression | Passed; surviving world's journal and render scope preserved |
| 32-peer Local load | One acquisition/admission; all peers converge in 4 ticks / 292.318 ms maximum; pending high-water 8,192; cap 8,192; lag 6,139; no journal failures |
| 32-peer Local reload | Cache reuse: one cumulative acquisition, two admissions; fresh lifetime; convergence 3 ticks / 338.652 ms maximum |
| 32-peer gameplay | 100 RPC calls/phase, no errors; baseline p99 25.838 ms, load p99 381.812 ms, reload p99 377.322 ms; no action submission/rejection errors |
| 100-peer Local load | One acquisition/admission; convergence 8 ticks / 1,240.71 ms maximum; pending 43,008; cap 8,192; lag 6,148; no journal failures |
| 100-peer Local reload | Two admissions, one cumulative acquisition; convergence 8 ticks / 1,151.6 ms; one action submission failure observed; RPC p99 754.511 ms versus 161.155 ms baseline |

The measured RPC/tick degradation is not a non-starvation readiness pass merely
because structural convergence succeeds. No access violation occurred in these
completed 100-call phases. Investigation and the remaining matrix are pending.

The first near-limit network-first churn attempt exceeded its five-minute Go
test deadline. It is a failed validation attempt, not evidence of a memory
plateau or successful Player completion. A bounded script-side network timeout
now allows diagnostics/cleanup before the outer test deadline.

## Remaining acceptance work

Long-run official RSS/private plateau, Local/Node 32/100/500 gameplay distributions,
100 official process cycles, complete sanitizer/CTest coverage, scoped security
review, candidate publication CI, and documentation deployment are not yet all
terminal. The final measured report must answer every requested gate and preserve
failed attempts alongside corrected reruns. Foundation 3M remains gated.
