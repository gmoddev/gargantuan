---
status: accepted-core-qualified-physical-deferred
owner: runtime-networking
reviewed: 2026-10-20
supersedes: D01/F1 hard packet timing and implied continuous delivered capacity
---

# D03 — Product service, exact structural permits and independent progress

## Decision scope

### Final adoption — 2026-10-20: non-real-time correctness contract

The product decision for core Foundation3L is **POOLED_CORRECTNESS_V1**.
This section supersedes release-blocking timing requirements in the historical
D03 checkpoints below. It does not recertify a historical run. General-purpose
OS execution has no unconditional wall-clock service promise.

Mandatory correctness includes exact ACK/reference retirement, native-positive
generation/token/byte attribution, reliable FIFO, no duplicate capacity/credit
return, finite512-KiB/peer and16-MiB global retention, four native leases, actual
wire/funding conservation including retries and partial positives, safe resource
exhaustion/backpressure, terminal purge, security and causal recovery. Invalid,
incomplete, retrograde or contradictory evidence cannot certify completion.
Elapsed time never forgives reliable ownership. Existing request, handshake and
cancellation/lifetime policies remain bounded failure policies, not delivered
service guarantees.

Algorithmic fairness stays mandatory under continuing eligible execution and
available funded resources. The bounded native request FIFO preserves age across
suspension, excludes pending/native-rate-blocked heads, and provides retained
selection after at most seven successful protected packets when both budgets are
ready. Main retains its last-granted rotation and one retained group per peer;
ACK holders cannot consume another peer's retention quota. These are opportunity
and conservation properties, not OS deadlines.

| Measurement | Unchanged optimization target | Core release effect |
| --- | --- | --- |
| Native structural BODY |37.3243865966796875ms | Diagnostic; exact complete BODY/token chronology stays hard. |
| Protected ordinary DATA |50ms combined queue/native budget | Diagnostic; enrollment, FIFO and actual arbitration stay hard. |
| Completed Main opportunity |100ms | Diagnostic; completed-pass ownership stays hard. |
| Historical combined turnover |150ms | Diagnostic; only exact ACK/terminal evidence releases retention. |
| Healthy-peer admission | conditional1092.5207061767578125ms capacity-separation derivation; historical1181.25ms comparison | Diagnostic; rotation, eligibility and no software starvation stay hard. |
| RPC/Event/action and service/convergence recovery | existing150/250/500ms,250ms,20s and workload-derived comparisons | Diagnostic; accepted results, exact convergence and causal replay stay hard. |

The21/3-MiB/s DATA subdivision stays beneath the existing24-MiB/s parent and
single-packet burst; four18-MiB/s structural allocations retain existing funding.
No target is enlarged. Structured `[Qualification:Performance]` records retain
values/comparisons. Explicit `HYBRID_F1_V1` and finite-grant invocations retain
their original semantics.

`PooledServiceNonRealtime()` selects separate native capacity and diagnostic
timing. The standard Server `--reliable-mode POOLED_SERVICE` composes it with the
existing exclusive native-worker domain and actual DATA arbitration. Explicit
`PooledService(false)` / `POOLED_SERVICE_LEGACY_V1` preserve the old fallback.
`RequireLatencyCompatibility` rejects the new profile rather than promising
real-time service. Raw native transport flags remain explicit.

Qualification is compositional: deterministic production accounting/fairness and
real native collector/worker fixtures cover bounds and fault races; shared-process
GameSession workloads cover ledger, replication, gameplay and recovery in their
compatibility transport arrangement; the existing packaged GameSession test
executes actual worker/arbitration composition in separate Server/Player processes.
The exclusive-domain invariant is not weakened for shared-process fixtures.

The existing final reader's new-contract core branch requires three exact-source
CI jobs, original artifacts/JUnit, the packaged session case and completed
correctness records from all five Release workloads. Missing completion at a
bounded capture's end is incomplete qualification, never a warning-only PASS.
Corruption, resource, ownership, FIFO and invalid-evidence assertions remain fatal.

Local32 physical certification, real-TLS Node32 deployment/provider parity and
physical resource/performance envelopes are deferred integration/release work.
They are excluded from core closure and are not claimed complete. Existing
physical readers remain strict and reject this core identity; Farm mode requires
the explicit legacy profile until those readers are separately migrated. Current
protocol/provider correctness regressions remain mandatory. No physical campaign
is started here.

**Core closure — 2026-10-21: KI-006 CLOSED; Foundation3L COMPLETE within
CORE_ENGINE_CORRECTNESS.** The [original final reader receipt](../../tests/Foundation3LCoreAcceptance20261021.json)
qualifies execution source `b420a7fd1567632b376760e65fc71386c9bddcf4`, with
reviewed offline analyzer `70088e119e5f4f68cb030035d025d5068da63240`.
Windows97/97, Linux headless53/53, GNS sanitizers11/11 and all five complete
Release workloads pass. Actual separate-process worker/arbitration composition,
focused fault/ownership races and32-peer production workload coverage pass.
The [acceptance ledger](../../tests/Foundation3LFinalAcceptance.md#current-core-contract--2026-10-20)
records exact provenance and retained failed preflights. The analyzer-only
job-log input cap does not change the tested runtime payload. These scoped
results do not establish an unconditional timing promise or a physical release
certificate. PR6 remains draft; no merge, release or3M is authorized.
Historical failures remain unchanged, including Local32 turnover164.511ms,
Main317.170ms/turnover317.724ms and poll267.649ms, original native F1 failures,
the66646us permitted-service counterexample, and source4bf317c04's mixed
Main152.287ms/turnover164.753ms failure. That run's41.4886ms poll leaves41.4788ms
outside named native scopes. Neither delay is attributed or relabeled; further
attribution is not a core acceptance prerequisite under this product decision.

### Native DATA arbitration implementation — 2026-10-20: qualification pending

The follow-on candidate adds `EnableNativeDataArbitration=false` to the trusted
native transport configuration. It requires the existing exclusive worker
domain (at most32 connections), permits only the existing single GNS lane,
and remains independent of the standard profile selection. Neither this flag
nor capacity separation is enabled by default. V1 health, readers and historical
results retain their original identities. This is implementation evidence,
not permission to activate a replacement contract.

The pinned SNP collector now makes an actual packet selection:

- Incomplete active structural BODY uses the existing structural native service
  allocation. It skips already-positive retained ranges and stops fresh FIFO
  collection at the active structural message. Failed never-positive ranges
  remain ahead of new first-send cursors.
- Protected ordinary collection stops at the last pending protected native
  message number (the maximum of seven bounded origin tails). It skips positive
  retransmission ranges. Prefix messages retain their original FIFO position;
  protected metadata never moves a message ahead of an earlier fresh message.
- Retained packets carry positive retry ranges. While a protected obligation
  exists they cannot also advance fresh cursors or consume never-positive
  suffixes. GNS keeps explicit reliable stream offsets, packet numbers, nonce
  assignment, ACK/reference owners and application delivery order.

The existing24-MiB/s residual wire allocation is divided into21MiB/s protected
unique DATA and3MiB/s retained/bulk DATA. Gameplay and control share the former;
there is no extra control source allowance. Four18-MiB/s structural native
allocations plus21+3 retain the96-MiB/s aggregate ceiling. The existing24-MiB/s
parent bucket remains mandatory, preserving its original **one-packet aggregate
burst**, rather than adding two independently available child bursts. Each DATA
packet reserves both parent and selected child before collector mutation; both
constraints retain actual native bytes plus48B. They are nested constraints,
not additive funding. Once serialization proves a packet contains no DATA,
both reservations refund **before native queueing**, so a pending ACK/control
result cannot exhaust another peer's DATA credit. Negative DATA attempts refund
on their outcome without releasing reliable ownership.
Partial positive native results are charged, fail health, retain/requeue reliable
references, and receive no unique first-send credit. The worker previously
discarded that positive byte count on an incomplete native result; this is
corrected without treating the partial result as a complete datagram. Physical
packet counters continue after invalid health; unique BODY and ACK certificates
remain frozen. Invalid evidence cannot retrospectively become valid retirement.

Each class has a fixed32-entry FIFO of generation-bearing native requests with
eligibility bits. A pending result, depleted native connection credit, failed send
or native yield suspends readiness without resetting request age. The other class
is suspended when a packet reserves service, and both classes suspend for every
queued native packet, including ACK-only packets. Ineligible peers cannot own
the shared ready head; resuming their original age prevents repeated protected
packets from resetting retry fairness. Reservations remove one request.
Mode changes, structural service and terminal
purge remove stale requests. A continuously eligible retry class gets a packet
selection after at most seven successful protected DATA packets on its connection
when both class budgets are ready. A depleted class cannot block the other.
ACK-only, failed and partial attempts do not advance this quantum. This is an
opportunity bound under actual actor/worker progress, not an unconditional OS
latency guarantee. No deadline releases reliable ownership. Resource saturation
denies DATA; it never certifies completion or erases debt.

The original permitted-service model remains66646us/FAIL. With identical BODY,
native and wire inputs, the production split-bucket fixture completes its20480B
ordinary flow at38462us after the same37324-us prefix. A conservative shared
source calculation uses176948B and154 messages over50ms, at most296 packetization
allowances and206548 wire bytes in the specified1248-B BODY/1300-B native model.
Enumerating every positive wire size49..1348B, including tiny retry packets,
bounds parent blocking by3/17 of retained inter-packet pacing time. With one
initial parent-packet allowance and integer refill rounding, the allocation
service bound is11815us; adding the ceil structural prefix gives49140us.
The21/3 subdivision provides this analytic envelope within unchanged24-MiB/s
funding; it is not selected from observed throughput or a changed F1 constant.
These calculations
establish allocation feasibility only. They do not measure selector execution,
native worker latency, native packetization under ACK/control interference,
fairness under actor stalls, or physical50-ms completion.

Focused native fixtures exercise the pinned encrypted UDP collector with
continuous NACK/retry demand, two protected origins, actual packet class
boundaries, structural completion and receiver byte/FIFO checks. The existing
pending-worker fixture additionally exercises arbitration with failed and partial
native results, reference retention, reconciliation and terminal generations.
The Windows Release focused run passes all three native targets (selector,
pending ownership and worker). The raw native unit arrangement supplies31
continuously retained retry producers and a20480B ordinary producer through the
actual collector, encryption and positive UDP results. With declared synthetic
65-us actor opportunities, ordinary selection completes at38819us including the
same37324-us prefix, and all31 retry producers obtain service. This is a native
selector regression with synthetic opportunity timestamps, not a native32
GameSession workload or a measured physical F1 result. Another case sends two
protected origins alongside continuous retries (19 protected and2 retry packets),
then completes4000B structural BODY without consuming positive retries; all four
messages arrive byte-exact and in FIFO order.

These tests exposed and corrected an open-gate bug: an eligibility query could
advance `LastRefill` beyond the actor's captured packet timestamp even with full
credit, making every old packet snapshot appear ineligible. Full ready credit
now returns the query timestamp; depleted credit still cannot refill from stale
time. A focused boundary regression preserves that distinction. The pending
fixture also filters its pause for DATA and completes deferred receive replay
before asking for complete feedback. No mandatory health predicate is weakened.
Hosted source qualification remains required. No candidate activation or reader
migration follows from these regressions or the39-case admission/accounting proof.

The focused pending-owner regression also forces ready requests while a real
native result is paused and verifies that another generation can immediately
obtain both class turns. A native-credit-negative case exercises the same
production planner. These correct an algorithmic shared-head blocking defect;
they do not bound OS scheduling or external GNS lock holders.
An actual small-payload STOP_WAITING packet with retained retry ranges that
cannot fit is paused after its positive OS result. Both DATA budgets remain
fully available while that control-only result is pending; unique/retry counters
stay unchanged and terminal reference purge remains exact.

Source556033eca's20/4 child-only version is superseded before qualification:
review identified its extra initial packet burst. Its original hosted runs
39485392757/39485392755 were cancelled before Windows timing execution, and
remain cancelled evidence. The parent cap is a demonstrated conservation
correction, not a retry of unchanged failed service. A focused negative proves
that independently ready children cannot consume two initial parent packets.

#### Windows initiating interval and scoped observation

The preserved baseline first fault is structural-overload step3, phase5
(ServerSession), at3477855339us: Main317170us and turnover317724us. Immediately
before it, ClientPoll step3 spans3477587675700..3477855324200ns (267649us),
ending about14.8us before detection. Later548-ms intervals are post-fault and
cannot explain this initiating delay. The coarse CPU samples do not distinguish
blocked from runnable time. No original lock owner or scheduling cause is proved.

The existing workload now borrows a thread-local callsite observer only around
Client.Poll. It captures existing adapter-mutex, callbacks, observation, receive,
reconciliation and native API table/connection-lock scopes. The bounded128-record
ring retains the preceding poll at the first execution fault; eight longest
named scopes survive ring wrap. Current nested scopes may unwind, while later
polls and teardown cannot overwrite the frozen event. An explicitly enabled
full native observer retains precedence. No worker-wide observer, ETW session,
new scheduler, changed acceptance timestamp or deadline is introduced.
Clock/record overhead is unmeasured and reported as such; nested intervals must
not be added as disjoint time. Source identity and existing clock anchors remain
required. A new unreproduced failure cannot be inferred to fix the original
interval. Until causal execution evidence and all mandatory service predicates
qualify, **activation remains blocked; KI-006 OPEN; Foundation3L NOT_COMPLETE**.

### Capacity separation implementation checkpoint — 2026-10-20: PARTIAL / BLOCKED

The authorized Main-owned capacity/ACK correction is implemented behind the
explicit native `PooledService(true)` / `SeparateNativeCapacity` candidate
selection. **It is not the standard profile and is not latency compatible.**
`PooledService()` and `PooledService(false)` retain accepted `HYBRID_F1_V1`;
latency-required configuration rejects the candidate. No V2 contract or
acceptance-reader migration is adopted. Historical Local32 `7efff6f1` is FAIL.

#### Implemented ownership and resource transitions

- COMMIT takes one of four native-send leases and one retained per-peer grant.
  Only a complete, fresh, generation/token/byte-bound native BODY certificate
  returns execution capacity. Debt, references and credit do not change.
- Exact ACK retirement plus fresh reconciliation, or independently verified
  terminal purge/cancellation, releases retained ownership. One retained group
  per peer prevents ACK holders from replenishing indefinitely. Duplicate,
  partial, stale, future, wrong-token and wrong-generation receipts cannot
  double return capacity or forgive debt.
- Candidate service funding charges `NativeCapacityBytes` rather than completed
  structural ACK debt. Retention is independently bounded by G per peer and
  32G=16777216B globally. Existing ordinary funding, FIFO-prefix denial,
  transport reserves, generation limits and native transport ceilings remain.
  Resource exhaustion stops new exposure; it never proves service or retirement.
- The candidate reuses the existing100-ms engine allocation for native BODY
  completion through actual capacity return **and a completed Main admission
  pass**. A mid-pass return alone cannot publish that certificate. The derived
  outstanding local lease budget is37324.3865966796875+100000us; integer137324
  passes and137325 fails. Remote ACK retention has no candidate timer release.
  Accepted V1 still enforces its original150-ms turnover.

The39-case production fixture retains all V1 negatives and exercises actual
admission/feedback code for all32 maximum groups before any ACK, the ninth
maximum grant,31 stalled holders beside repeated healthy tiny/maximum demand,
true saturation, delayed/incomplete feedback, late ACKs after capacity reuse,
terminal/generation transitions, funding/FIFO bounds and exact conservation.
An early-return/late-pass counterexample distinguishes the engine endpoint.
These are deterministic native inputs, not a measured native32/physical PASS.

The first hosted source (`86f53ae36`) passed GNS sanitizers but failed the
existing 500-peer compact-frontier memory regression (52/53 Linux headless
tests passed). The added ownership flags occupied a separate alignment word.
Packing them into the existing flag padding removes that word per peer without
removing state or changing the 128-KiB gate. The focused native rerun includes
the existing 200/500-peer admission fixture: 500 peers use 128576 logical bytes
under MSVC, below 131072, and all 39 pooled cases plus the pacer regressions pass.
The failed CI artifacts are retained; the layout correction requires fresh
hosted checks and does not qualify protected DATA service.

Under the qualified source/read/rotation assumptions and independently available
ordinary/retained resources, the **admission component** has this conditional
replacement bound:

```text
100000us initial completed opportunity +31250us initial four-G credit warmup
+7*max(37324.3865966796875+100000us local lease,31250us four-G refill)
=1092520.7061767578125us <1181250us
```

Refill overlaps subsequent service waves. The one-grant-per-peer restriction,
unchanged last-granted cursor/global funding turn, and at most31 other retained
holders prevent structural ACK debt alone from occupying a healthy peer's
retention quota. If all32 identities own debt, none is eligible for another
grant. Ordinary/native resource exhaustion remains a genuine denial, not an
unconditional fairness promise. This argument assumes timely native service;
it does **not** prove the remaining protected-traffic or actual capacity gates.

#### New concrete blocker: retained retries share protected first-send capacity

The current native `GargantuanOrdinaryDataIsPaced` / `SNP_SendPacket` gate uses
one24-MiB/s bucket for post-completion DATA, without a protected-first-send
versus retained-retransmission allocation. With only four ACK-owned grants,
returning to this old behavior was part of V1's bounded execution topology.
With32 retained grants, Main-owned separation alone is insufficient:

1. Leaving the existing gate off between waves (`AnyRunning=false`) permits
   completed retained retry senders outside that shared budget. Four active
   leases alone no longer bound those senders.
2. Merely keeping every retained sender on the same bucket restores a finite
   DATA budget but does not establish the existing50-ms ordinary FIFO guarantee.

The existing wire-pacer fixture now supplies an exact counterexample to option2
using the **actual production bucket**:31 ACK-retained retry flows, one protected
20480B ordinary flow accepted behind a legal37324-us native structural prefix,
1248B BODY quantum,1300B native maximum and48B IPv6/UDP charge. Fair one-packet
rotation completes the ordinary message at **66646us**, exceeding50000us.
Only657696 retry BODY bytes are supplied, below the31G retained bound; every
packet is positively charged and the shared rate/burst conservation holds.
No scheduling overhead is added. Thus neither memory safety, timely structural
completion nor a fair shared bucket proves protected ordinary service.

This is a **deterministic permitted-service counterexample**, not a captured
GNS actor schedule, native OS failure or platform-impossibility proof. It shows
that the current undifferentiated DATA gate cannot supply the missing service
argument. The new test prints `model_only=1 safe_to_enable=0`. The optional
retained-pacing predicate is not wired into production; old call sites keep
their behavior. No rate or deadline is tuned to pass the example.

The smallest remaining implementation scope is native DATA arbitration that
funds retained retries while independently protecting ordinary unique first
sends, preserving reliable segment/packet order, native references and eventual
retry service. Changing ACK/debt bookkeeping cannot implement that policy.
The existing worker's ACK/control/retry dispatch reserve is not a proof of
ordinary DATA selection/bandwidth. No unqualified native segment-selection or
transport scheduling rewrite is started at this bounded stop.

**Disposition: BLOCKED on protected first-send/retained-retry service.** The
capacity correction is retained as an opt-in, unqualified implementation; no
contract, workflow, version marker, provider reader or source pin is relaxed.
The abandoned V2 migration patch is preserved outside the checkout with SHA256
`8b237bd7cf48643fd3762df7d333b9792eaf7028010a362751f5cddd0c6cb59f`,
not adopted. Required CI qualifies regressions, not activation of this candidate.
No physical campaign is launched. KI-006 stays OPEN; Foundation3L NOT_COMPLETE;
PR6 stays draft. The next step is the specific native service correction above,
not another qualifier layer or a replay of unchanged physical evidence.

### Authorized revision review — 2026-10-19: BLOCKED on healthy-peer progress

The authorized review separates the proposed service contract into the five
predicates below. **No replacement contract is adopted or implemented:** the
existing shared ACK-owned admission and funding architecture cannot establish
the required independent healthy-peer progress. Merely removing the150-ms
failure would conceal that contradiction. `HYBRID_F1_V1`, its production
health and all acceptance readers keep their current semantics. Historical
Local32 `7efff6f1-8d59-4fa8-8cef-c1f27cfa9b80` remains FAIL.

| Predicate for a replacement | Boundary, assumptions and failure condition | Review result |
| --- | --- | --- |
| Engine service | Original acceptance/activation through complete positive native unique BODY; exact37324.3865966796875us structural and50000us protected ordinary limits. A completed Main reconciliation/admission opportunity remains100000us when ready work or timed ownership exists. Incomplete native feedback cannot certify success. | Retain the established predicates; CI/subset evidence is scoped to its original source and contract. |
| ACK/reference ownership | Accepted generation/token/bytes through exact attributed ACK/reference retirement, or independently proved irreversible terminal purge and cancellation. Native completion, elapsed time and a disconnect request cannot release this ownership. | Retain exact accounting; no unconditional ACK completion deadline follows from the available network/execution assumptions. |
| Stalled-peer containment | Detect failure to satisfy an independently specified progress obligation; stop new exposure and isolate that peer while retaining references/debt until exact terminal evidence. Failure to purge remains fail-stop. | No existing bounded containment path proves continued healthy service: synchronous disconnect can wait250ms on Main and can fail with ownership retained. No replacement stall deadline is selected. |
| Resources | Preserve peer/global pending limits, finite connection count, accepted=ACK-retired+terminal-released+outstanding accounting, and existing ordinary/transport reserves. Missing ACKs cannot generate credit or a retirement receipt. | Bounds survive retaining debt; bounded memory alone does not imply admission progress. |
| Healthy-peer fairness | Start at independently established, continuously eligible complete source demand; end at actual admission. Preserve original wait through candidate replacement; distinguish remote ACK completion and source-empty endings. Stalled peers must not occupy all healthy service capacity indefinitely. | BLOCKED: the existing1181250-us derivation requires seven bounded ACK-owned turnovers and has no independent replacement proof. |

#### First causal contradiction: four exact owners can block28 healthy peers

`ReliableByteAdmission::Allowance` refuses a new grant when all four
`MaximumDrainGrants` are owned. `RefreshService` returns an owned grant only
after exact debt retirement and fresh feedback; `TerminalRelease` requires
GameSession's terminal-purge path. Thus four peers may complete native sending
on time, keep fresh valid counters, and withhold ACK retirement. The other28
peers can have complete ready groups, full credit and timely Main opportunities
but never receive a grant. Advancing time does not resolve the contradiction.

This counterexample already exists in
`ReviewSourceCompetitionDetectsAckHeldSlotsWithoutTurnoverTimer` in
`tests/PooledReliableServiceProductionFixture.hpp`. The adjacent
`WithheldAckCannotReleaseDebtOrEvadeTurnoverDeadline` and
`ReviewLateTurnoverPreservesExactReceiptAndProductWait` separately establish
that timely native service neither retires ACK debt nor justifies the present
combined deadline. No new replay layer or duplicate regression is needed.

The former fairness equation remains conditional arithmetic:
`100000 + 31250 + 7*150000 = 1181250us`. The initial completed opportunity and
four-G credit warmup remain established allocations; the seven finite slot
turnovers no longer follow after removing the combined deadline. Peer credit,
fair rotation, or a direct timeout on the waiting peer cannot create a free
slot. Disconnecting that healthy waiting peer is failure, not fair service.

#### A drain-slot-only correction is insufficient

`ReliableByteAdmission::FundedRoom` also subtracts **all outstanding structural
ACK debt**, including already-native-sent bytes, from the shared50-ms funding
window. With the existing minimum ordinary reserve, its exact integer budget is:

```text
floor(96MiB/s * 50000us / 1000000) = 5033164B
- floor(8MiB/s * 50000us / 1000000) = 419430B transport
- (160KiB + 64KiB) = 229376B ordinary
= 4384358B structural room before outstanding debt
4384358 - 8*524288 = 190054B < one maximum grant
```

Even a hypothetical change returning drain slots at native completion would
allow eight unretired maximum groups to prevent a ninth maximum group from
admission. Fresh feedback and fully refilled credit do not remove that debt.
The32-G pending cap is a separate bound; it does not override `FundedRoom`.
This is a source counterexample to that proposed correction, not a measured
failure or an implemented change.

#### Smallest complete architecture path and disposition

The smallest identified path is to separate bounded **native-send capacity**
from **ACK-owned retained resources**, addressing both the drain permit and
shared funding charge. Retain the exact per-peer grant/token/reference until
retirement or terminal purge; do not release the existing ACK-owned permit on
a timer. Any distinct send-capacity lease needs native-completion truth,
explicit memory/funding conservation, continuing ordinary protection and a
production-source fairness proof covering stalled holders and repeated waves.
The existing Main100ms and credit allocations are inputs to that proof, not
proof that it already holds. No new fairness number or lease is adopted here.

The alternative is bounded peer isolation using the existing exact terminal
path, but its synchronous250-ms wait and fail-stop outcome do not establish
the retained Main100ms or healthy-peer continuation guarantees. There is no
source-backed small timer or reader correction that closes either gap.

**Disposition: BLOCKED, KI-006 OPEN, Foundation3L NOT_COMPLETE.** This review
does not establish that every replacement architecture is impossible. It
establishes that a timer-only D03 revision cannot meet the authorized ownership
and healthy-peer requirements with the current shared resources. Do not adopt
a new contract identity or make readers accept historical evidence until a
complete correction is implemented and proved. No native/physical campaign is
launched. Existing CI, four-client and native32/recovery receipts remain useful
only for their recorded source, predicates and trust boundaries. A future
change to these production/physical predicates requires fresh matching
software/package, Local32, real-TLS Node32, provider/parity/resource/cleanup and
final-conjunction evidence; no prior FAIL becomes PASS.

### Final physical attribution checkpoint — 2026-10-16

One fresh bounded Local32 at nativef30/toolingcf45 fails the unchanged150-ms
turnover predicate at164.511ms. The initiating grant completes native BODY
in24.319ms and its Main opportunity is15.738ms. OS evidence does not attribute
the remaining ACK/reference-retirement interval to long runnable starvation.
No correctable defect, mathematical impossibility or unavoidable platform limit
is established. Full Local32 remains FAIL; Node32 and final acceptance are unrun.

Independent recommendation is **REVISE D03**, particularly the combined exact
turnover rejection, with independent eligible fairness, ownership liveness and
resource proofs. This is **not an accepted amendment**: all current hard gates
remain binding. Feasibility is **NOT DEMONSTRABLY GUARANTEED**, not QUALIFIED or
PROVEN IMPOSSIBLE. [Exact measurements, uncertainties and ranked alternatives](../../devdocs/FutureArchitecture/Foundation3LNativeEgressOwnershipDecision.md#32-final-bounded-local32-attribution-and-architecture-decision-2026-10-16)
record the bounded stop. Historical native32/recovery evidence is unchanged.

### Qualification-scope amendment — 2026-10-13

The product decision at this checkpoint removes the earlier exception that
retained raw D01 packet-scale running service as a mandatory standard-profile
release gate. For explicitly identified `HYBRID_F1_V1` evidence, legacy peer
and common-pool running curves are retained diagnostics, not independent
release predicates. The raw four-grant test remains mandatory under its
contract-aware mode; it must still reject incomplete/late native structural
completion, false first-send attribution, invalid chronology/frontiers,
packet/reserve violations, unreliable/incorrect order, lifecycle defects or
inexact accepted/first-sent/ACK/retired/terminal accounting.

Explicit legacy `F1`/`finite_grant_v1` profiles keep their original finite and
running-service semantics. Missing, unknown or mixed contract identity cannot
select HYBRID semantics. Historical e792 manual CI and earlier physical/native
failures retain their original source, contract and FAIL verdicts. A fresh
source identity and fresh evidence are required; no archived result is relabeled.

The four-grant native and four-client physical checks qualify a structural
transport subset. They cannot substitute for independently qualified protected
ordinary FIFO, Main opportunity, exact turnover, all32-peer fair admission,
gameplay, resources or recovery/convergence. Standard-profile final acceptance
must explicitly join matching HYBRID identities across CI, qualified package,
fresh four-client prerequisite and both Local32/real-TLS Node32 providers.

All D03 hard bounds remain exact: native completion37324.3865966796875us,
ordinary FIFO50000us, completed Main100000us, turnover150000us and qualified
32-peer eligible admission1181250us. All security, reliability, conservation,
reserves, bounded debt and canonical recovery obligations remain mandatory.
This amendment supersedes only the retained raw-running-curve exception in the
historical records below; it does not change production networking or grants.
Native qualification of the coordinated migration passes at the A7 checkpoint
below; the physical provider and final conjunction remain pending.

### A7 native qualification checkpoint — 2026-10-13

Source `5a2e735c96dd2a7983681a090a4610b90772d368` (A7), tree
`03f22a072ccb36bb0b7b1a2700b7b5de7a8c4218`, passes all eight original
exact-source hosted jobs and canonical CI/package replay. Fresh same-host
native32 run `6aa1c66f-35e2-45c9-8f92-1c7761705641` completes in284.656s;
independent whole replay is `MEASURED_SUBSET_PASS`, with no failed conjunct.
Its native D03 maxima are Main63.671ms, turnover112.213ms and ordinary FIFO
8.636ms. All32 peers conserve2092432666B accepted=first-sent=ACKed=retired;
terminal release, outstanding debt and open grants are0.

All96 healthy recovery cohorts retain ten RPC/Event results and the immutable
deadline. Maximum server cessation-to-receipt times are5.449543s gameplay,
2.620571s structural and5.420156s mixed, below the unchanged20s limit.
Gameplay/mixed first cohorts remain UNPROVEN and their second cohorts qualify;
no failed attempt is relabeled. Structural ordinary notices arrive33.663–
62.304ms after cessation, while exact client final Names complete15.208642–
17.156190s later. The separate758425155B structural reference has a32.848300s
bound; causal first complete-prefix convergence is17.143725s. GRPL marker
arrival and notice submission/native dispatch are not separately timestamped.

All7302 recorded eligible episodes complete, with maximum96.729ms. The broad
admission request-wait counter1.346241s includes time before independent
eligibility and does not replace that proof. This run contains no source-empty
eligible endings or carried replacements: those branches retain their focused
deterministic/production-fixture qualification, not invented native coverage.
A5 and all older failures remain unchanged. Exact hashes and scope are in
[ownership decision section28](../../devdocs/FutureArchitecture/Foundation3LNativeEgressOwnershipDecision.md#28-a7-native-d03-qualification-checkpoint-2026-10-13).

This qualifies the same-host native subset only. The incoming qualifier-only
analyzer/pin child is separate from native source A7. Same-source four-client,
canonical physical Local32, real-TLS Node32 and final acceptance remain
unqualified. Controller RAM around4.97GiB is below the full32 client8GiB floor;
it does not prevent the bounded four-client prerequisite. **KI-006 OPEN; PR6
draft; Foundation3L not complete; no3M or merge.**

### Independent service-recovery notification — 2026-10-13

The following A5/A6 checkpoint precedes A7's qualification above; its original
failed verdicts and then-unqualified corrections are retained.

Source `19f9f57a11dd7627b20bf4d5f42e0358618eabeb` (A5), run
`541d1719-2a70-4655-b9f4-afce45e15521`, remains **FAIL**. Its original107
members/153188597B plus17543B index are sealed by
`2c41f505808ea7fd9826836208d35ef759d242a598dca10bdd415acf6b03063a`.
All32 structural service receipts exceed the unchanged server20s maximum:
first22.895943s (peer12:1), last24.346420s (peer19:1). The first client waits
for a replicated `recover_*` marker behind the structural journal prefix,
then starts its cohort20.5930431s after cessation. Its fence-crossing frame is
prepared20.548006s, accepted385us later and fully ACKed at observed feedback
29.275ms later. Ten healthy probes take2.2793538s; final Event-to-end15.2786ms
confirms that the final200ms idle defect was corrected. These are source,
feedback and callback measurements, not native OS-call timings. Observer
perturbation is not separately measured; ordinary service before20s is not
proved because the required early probes were never offered.

The contract separates20s service restoration from structural convergence.
A5's immutable1000421668B reference permits36.109713s; its exact causal prefix
converges22.079024s. Its larger remaining work than A4's786571448B is not by
itself evidence of a production regression. Native HYBRID/FIFO and five-phase
subsets pass, while full qualification also retains INCONCLUSIVE fairness:
one generic `no_work` disposal does not prove that eligible source demand ended.

Select a fixture correction using existing protected ordinary ScalePhaseControl:
one case-bound cessation notice after actual server publication, marked before
submission and never replayed after partial failure. The client accepts only
its matching active case after the offered handshake with no pending overload
RPC; first receipt owns its immutable clock. Wrong/stale/duplicate notices
cannot reset it or create duplicate cohorts. The GRPL marker is no service
fallback. Exact final Names/source identities, C3/C5 coverage/retirement and the
original convergence deadline remain mandatory. The independent server20s
deadline, ten probes, nine inter-offer waits, errors and accepted ownership are
unchanged. Actual embedded client/server pinned-Luau regressions pass locally;
fresh native/CI/provider qualification remains required. A5/A4 are not relabeled.

The independent fairness correction is also unqualified: a source-owned
`HasPendingStructuralWork` query distinguishes real empty demand from planning
or zero journal-budget refusal, preserving candidates through the latter.
Explicit admission evidence V3 may end an eligible interval only with truthful
`source_empty`, checked against the original fairness deadline before disposal.
Source-empty endings are counted separately from actual grants and accepted
bytes. Generic `no_work` remains inconclusive; old V1/V2 keep their original
semantics. This prospective proof cannot reinterpret A5's unknown disposal.

A read-only pre-publication audit of unqualified A6 `751e4eeaa` identifies a
funding-turn coupling: preserving a candidate through unfinished planning also
preserves `GlobalWait`. After its funding credit refills, a peer still unable
to prepare can block other ready peers for the entire planning interval. This
is a source counterexample, not a measured A6 native32 failure; A6 was not pushed
or used for a native32 campaign. Its affected23-test preflight remains separate.
Select a source-deferral operation that releases only this funding turn,
retaining the exact candidate, original eligibility, required bytes and credit.
Exercise the real two-peer/planner interaction before fresh qualification.
No timer, admission deadline, accounting allowance or empty-source claim changes.
The reviewed follow-up also requires fresh monotonic time after the owner query
for source-empty disposal; cached step time cannot forgive a deadline breach.
Missing/backward time fails before mutation. Empty proof additionally requires
valid source/generation/cursor ownership and equality with the committed raw
journal frontier, not just cached catalog lag. Detached quotes use their frozen
frontier. These scalar checks add no catalog scan or live quote replay; focused
regressions cover actual mutation with cached lag0 and an end1us beyond the bound.

The coordinated scope candidate `4a2e8d6112a5720f8908f26dd61ee79adf9f487e`
subsequently passes all eight original exact-source hosted jobs and independent
canonical CI/package verification. Its fresh native32 run remains FAIL: a
correctly versioned structural recovery ACK arrives20.149647s after cessation,
while its ten RPC/Event results were complete by19.928129s. The fixture's
unconditional200ms sleep after the final probe withholds the receipt; the
selected correction waits only between probes and still requires final Event
settlement. All D03 and recovery deadlines remain unchanged. Exact candidate
coverage remains incomplete after legitimate replanning; the selected peer
eligibility proof below preserves its original wait across atomic replacement.
Full qualification is pending; the detailed preserved-source account is in
[ownership decision section25](../../devdocs/FutureArchitecture/Foundation3LNativeEgressOwnershipDecision.md#25-hybrid-scope-qualification-and-final-probe-withholding-2026-10-13).

The authorized Foundation 3L follow-up adopts D02's B/C direction: retain
funding, exact delivery and hard player outcomes; qualify actual source-shaped
capacity. The coordinated deterministic/native Windows preflight passed eight
affected suites; this record is explicitly adopted with implementation.
Adoption is not Foundation 3L PASS.
Historical failures retain their original source, contract and verdict.

## Execution-gate review — 2026-10-12

### Final checkpoint — 2026-10-13

**Retain D03 V1; required supplementary raw service remains unqualified.**
The100000-us Main and150000-us turnover predicates are unchanged. Candidate
`b65c3528e83e` passes these predicates across32 peers/7089 completed grants,
but its full native32 conjunction fails separately. Candidate
`e792482e8a9f326bb0f2861576e87e163d0c7537` contains the source-backed
qualification enrollment and publication diagnostic corrections, with focused
deterministic and affected native preflights passing.

Its mandatory original manual CI `38346195711` fails the explicitly retained
stronger raw four-grant running-service probe. All four first sends are prompt
and finite completions pass; this failure does not establish a D03 execution
predicate defect. The one final diagnostic passes without reproducing it, loses
early bounded history and measures no overall observer overhead. It cannot
replace the failed original run or qualify the official package.

The bounded attribution track therefore stops with an execution/qualification
scope decision required. Neither a production defect nor platform impossibility
is demonstrated. No raw gate is demoted, no D03 V2 is adopted, and no current
native32 or physical run is launched. The exact evidence, ranked alternatives
and conditions for a justified next experiment are recorded in
[ownership decision section21](../../devdocs/FutureArchitecture/Foundation3LNativeEgressOwnershipDecision.md#21-final-required-raw-service-attribution-and-qualification-boundary-2026-10-13).

**Selected A: retain D03 V1 and continue bounded production qualification.**
The 100000-us completed Main opportunity, 150000-us exact turnover and all
other hard predicates below remain unchanged. No V2 production health or
source-class change is adopted.

Separate deterministic schedules exhibit a 110-ms Main opportunity or one
170-ms exact turnover with timely native BODY completion, protected ordinary
service, gameplay responses, all 32 source admissions, exact ownership and
finite recovery. They do not assert that repeated 170-ms turnovers satisfy
the fair admission deadline. The timers are stronger than the remaining
player outcomes; that alone does not justify deleting them.

Counterexamples reject removal without an independent replacement: four
ACK-held slots can hide 28 waiting peers, unfair rotation can repeatedly serve
the same peers, and tiny grants can satisfy admission fairness while missing
the immutable convergence deadline. First producer visitation cannot prove
the readiness of an unvisited source. Cancellation of unrelated newer work
cannot count as service of an older obligation.

A proposed direct 1181250-us source-admission guard was investigated but is
**not adopted**. Supporting metadata/credit prototypes passed focused tests;
their diffs and failed fixture premises are preserved outside the candidate.
They do not qualify a runtime health replacement. A safe replacement would
need original-unit first-ready provenance for all expected peers, exact
complete cost and historical credit, bounded read feasibility, matching
COMMIT/cancellation and unresolved/disconnected-unit rejection. It must also
distinguish legitimate non-slot funding/FIFO denial during declared overload
from slot-induced starvation, without restarting existing obligations.
Adding a copied/replayed source observer solely to remove gates that the
current corrected workload passes is not the smallest justified correction.

Archived prototype patch SHA-256 identities (not applied to the candidate):

- Source ownership/runtime: `70afdd9cfc859ab2a0276074fb93cd793717a0e9e72f9abb9b956604c6a4528e`.
- Source-unit tests: `9c412be9d2b0300d528a1dc1977e3b80f97f7d75d6899702a72f64a100b9eafa`.
- Historical credit API/tests: `627aa62805ff98b87d1d24255f7781f3d61e2172f35a78fb5e8aa3c355ed966d`.

All six original hosted jobs at current `0571f3b210028` pass. The fresh
loss-free fixed PooledFull run below also passes. These facts justify one
normal exact-source native32 qualification after preflights; they neither
attribute the original E8 failure nor prove it disappeared. Keep the bounded
first-fault discriminator available. If qualification fails, retain its
actual first predicate/obligation and compare a source-supported correction
with the direct-outcome alternative; no deadline relaxation or blind retry.

### Bounded attribution result

The first-fault diagnostic now retains the pre-check obligation, distinguishes
Main/turnover failure from invalid accounting, covers the whole workload lifetime
and records the first post-fault lifecycle event. Its opt-in native history uses
the existing 8192-record bounded store and fixed named call sites. History may
be read or freed only after positive native quiescence; failed bounded shutdown
retains callback storage and reports incomplete.

The fresh instrumented fixed PooledFull execution at base `0571f3b210028` plus
the recorded 14-file diagnostic/static overlay passed all eight cases. The
mixed-overload maximum step interval was **24.101 ms**. Exactly **38693126 B**
were accepted and retired, with zero terminal release and outstanding debt.
No actual D03 breach was retained. Kernel capture was loss-free and cleanup was
verified. **The original E8 CI breach remains unattributed**; this execution does
not prove that it disappeared. Lock owners/full stacks and observer perturbation
remain unmeasured. An earlier fresh staging failure omitted the executable-local
runtime font, aborted before networking, and is separately preserved.

Local retained attribution summary:
`foundation3l-d03-first-breach-0571-v1/attribution-summary.json`, SHA-256
`f8cc8a66ffc5d1655f525a3ddf5c4bb5d59844d0c4b07cac62ca8effee6523fa`.
The archived source-unit/credit preflight subsequently passed the actual
replication and networking suites; it is not part of the selected V1 candidate
and qualifies no V2 health migration. Native32 and physical gates still require
the selected final source.

The selected V1 source was restored after prototype archival and rechecked:
all14 diagnostic/static normalized Git blobs match that measured overlay;
the five prototype-only production/test paths match base0571. Remote jobs4
incremental preflights pass GameSession, networking contracts and real transport
(3/3;23.16s), plus capture/callsite deterministic self-tests. JUnit SHA-256:
`307c87169d22b1b27a792b14bcb09fe3aeca8cbcfbfb0ddcf4d811ae3216726f`.
This is supporting native verification, not final-source hosted CI or native32.

### First retained-V1 native32 and focused fixture correction

At `00239d940527`, original Windows96/Linux53/GNS11 CI and the official
package pass. The single fresh native32 run retains no server F1/Main/turnover
breach across32 peers and6752 qualified grants (maximum73064/123475us), but
its overall verdict is FAIL. The canonical recovery reader rejects structural
offer cadence, and the ordinary join rejects nonzero coverage for clients that
emitted no prospectively enrolled normal demand. Passing subsets cannot repair
either missing conjunct. Ownership decision section19 preserves exact custody.

The corrective slice changes qualification evidence and the controlled source
handoff, with no D03 deadline or transport ownership change:

- Preserve mutation-completion `monotonic_us`; add explicitly versioned
  opportunity-start time from the existing native16667us pacing boundary.
  New readers prove start cadence, ordered completion and no overlap; legacy
  rows retain their original strict interpretation.
- Use the already received client control phase, apply prospective source
  membership and trace phase, then publish a local host-applied phase marker.
  Controlled phase traffic waits for that marker on opening and closing;
  initial ready and declared overload retain their existing protocols.
  The phase clock is retained before waiting, and the100-RPC task starts once.
  No accepted message is retimestamped, and credit is not reset.

Focused existing fixtures pass: cadence positive/nine negatives; actual pinned
Luau opening/closing handoff,100 RPCs/four clock probes and unchanged recovery;
exact historical missing-tick negative; ordinary-demand Python30/30. A new
test-slice error was corrected and preserved separately. The corrected jobs4
native build and three affected suites pass; fresh canonical Player/Server
packages pass Validate/Load and byte-closure checks with the current script.
Use supported short staging paths: the failed output projects a243-character
temporary member beyond the unchanged240 guard, while the successful output
projects238. The old generic failure did not expose its exact diagnostic item;
both roots/results remain preserved. Bounded generator failure diagnostics now
expose existing errors without changing acceptance.

Native preflight JUnit SHA-256:
`79a99df0ec9aabb6f12b136632a3ef86509186c2a4da640332037e2e5431185e`.
Package preflight receipt:
`39999377635cf6dbdf54b9d0dd9ac6b169dce6fe258f2e48e68c799d5f5a4d73`.
Exact-source hosted CI and a fresh native32 identity remain mandatory before
claiming the complete conjunction.

### Corrected native32: execution passes, recovery receive attribution pending

At `b65c3528e83e`, all original hosted Windows96/Linux53/GNS11 jobs and the
official package pass. Fresh run `9196af39-8da8-4200-95da-a07e604a92a2`
passes native F1 for32 peers/7089 completed grants, with maximum Main61208us
and exact turnover106758us. All64 nonzero normal ordinary certificates pass
(maximum9075us); exact admission/conservation, fixed32-peer fairness and960
versioned cadence records pass. **Full qualification still FAILS** recovery
client RPC latency and the publication join. Retain all hard predicates.

Every first gameplay/mixed recovery RPC exceeds500ms. Client0 measures
618219/549290us, with604891/536267us between GNS API handoff and server
receipt. Scheduler/handler work is short and server frames continue. The
evidence does not separate native sending/retry/reassembly from completed
receive waiting for polling or adapter dispatch. Recovery phase0 is intentional;
normal H2 PASS cannot repair failed recovery probes. Historical recovery
correction and original E8 failure retain their source/verdict.

The join separately finds repeated compact-invalid candidates recorded as
`StateBuilt` before caller rejection. A focused diagnostic correction records
only encodable snapshots while preserving candidate return, rejection counters,
sequence advancement and due/requeue. It neither ignores duplicate successful
states nor changes publication behavior.

The next bounded discriminator adds only fixed RPC receive metadata to the
existing publication tap: GNS dequeue timestamp, age since complete ordered
message became receivable and adapter event depth before enqueue. Pinned GNS
assigns `m_usecTimeReceived` at full ordered-message reception, not first UDP
arrival. Joining that record to existing RPC receipt separates completed receive
queue residence from adapter dispatch. It cannot attribute earlier native send
or reassembly on its own. No payload retention, log I/O, new scheduler or health
change is needed. Focused regressions, affected native tests and exact-source
CI precede one fresh diagnostic identity. Observer perturbation remains
unmeasured. Ownership decision section20 retains exact receipts and alternatives.

Independent contract review then establishes a separate fixture enrollment
mismatch: the canonical service-recovery promise is return to qualified behavior
within20s, not zero settling time after overload. The already qualified
`PooledReliableServiceIntegration3L.md` recovery-probe correction and actual
`ReliableGameplayWorkloadFixture.hpp` keep failed latency attempts unproven,
try at most one fresh group per second, retain every attempt and never forgive
accepted errors. The farm's immediate one-shot10-probe cohort lacked that
established enrollment distinction.

The prospective `bounded_recovery_cohort_v1` correction uses at most20 fresh
cohorts, one in flight, at least1s between starts. Every invocation/ACK retains
its actual integer clock endpoints and cohort identity. Pending Events cannot
be abandoned to restart, and actual RPC/Event errors remain fatal. All completed
positive but slow cohorts remain visible as UNPROVEN; only a complete qualified
10-RPC/10-Event cohort can produce the final summary/recovery acknowledgment.
The existing150/250/500ms RPC and250ms Event gates and original server20s
cessation deadline remain unchanged. Exact server acknowledgment timestamps
must independently satisfy that deadline; client recovery-entry clocks only
bound the controlled producer. Legacy evidence retains its strict original
interpretation. This is alignment with the existing recovery contract, not
retrospective PASS for b65 or a new production exemption. Focused actual Luau,
reader negatives, native preflights and fresh hosted CI remain required.

Corrected source preflight subsequently passes: actual Luau recovery/cohort/
pending-transition tests, exact missing-tick negative, strict recovery-reader
positive/negative/legacy/boundary cases, publication Python12, all four affected
native suites and fresh closed Player/Server packages. Source input and native
JUnit SHA-256 are
`f9f151c753c26e7bd3300a6bdca1f1322fca87f981f6fd4249707e7b897947b2` /
`23051315e6c4828d133dfa98c888cd1da43a7bcb5b6184a108f18790f1f18124`.
The earlier Character-fixture promotion premise and PowerShell nonce-lookup
failure remain preserved, with source-supported minimal corrections. This
preflight is not hosted CI, native32 or physical qualification.

## Hard conjunction

1. **Native structural completion.** For every Ready, generation-valid accepted
   complete grant, `0 < W <= G=524288 B`, preserve its scheduler acceptance and
   activation boundary. Require all unique structural BODY bytes at positive
   native send within the exact uniform budget:

   ```text
   R=16777216 B/s; Q=1248 B
   R*(t_complete-t_activate) <= R*6000+(G+Q)*1000000
                            =626199296000 byte-us
   T_max=37324.3865966796875 us
   ```

   Integer 37324 passes; 37325 fails. Tiny grants receive this same maximum.
   Missing first send and missing last bytes fail at a complete native evidence
   frontier. An unresolved worker result cannot publish PASS or an exact native
   lateness claim. Reconciliation uses the actual worker completion timestamp.
   Earlier incomplete evidence must not be retrospectively repaired.

2. **Protected ordinary FIFO.** A prospectively protected ordinary reliable gameplay/control
   message must complete unique native BODY first-send within **50000 us of its
   original scheduler acceptance**, including any scheduler and GNS queue delay.
   Direct control has no scheduler prefix: its successful submission starts at
   transport entry. WouldBlock before acceptance creates no delivery obligation;
   WouldBlock after scheduler acceptance preserves the original timestamp.
   Retries do not count as new service. All phases 1–5, both directions, complete
   exposed prefixes and continuing arrivals must be covered. RPC p95/p99/max
   remain 150/250/500 ms; Event ACK/action maxima remain 250 ms. Dependency waits
   never restart invocation clocks. No stage receives another 50-ms allowance.

   Membership uses the existing combined ordinary source envelope: per peer
   64 messages/s with burst 16 and 32768 B/s with burst 20480 B; per session
   512 messages/s with burst 128 and 262144 B/s with burst 163840 B. Complete
   message bytes include the 32-byte adapter envelope. Gameplay and control
   share these allocations. Control funding/reserves are not additional source
   arrival allowances. Ready pooled servers enroll this class automatically;
   clients require the trusted native startup declaration. Bootstrap and an
   explicitly declared workload overload do not enroll new protected work.
   Membership is immutable at acceptance; later scope changes cannot remove
   an obligation or recreate source credit. Excess traffic retains existing
   acceptance, funding, backpressure and overload treatment. The classifier
   adds no pacing, rejection, scheduler or public API.

3. **Exact turnover and completed opportunity.** Select **150000 us** as a new
   combined structural turnover entitlement, from activation through exact
   structural ACK/reference retirement, fresh Main reconciliation, actual
   permit return and completion of the admission pass. Select **100000 us**
   for a completed Main reconciliation/admission opportunity when source work
   is ready or timed ownership remains. These are explicit product class
   allocations using the existing 50-ms queue plus 100-ms nonqueue allocation,
   not measurements or inferred GNS ACK/OS guarantees. No extra 100 ms follows
   the 150-ms turnover. First opportunity is armed before preparation.
   A missing ACK never retires debt, regardless of elapsed time.

4. **Fair opportunity.** For the declared 32-peer, four-slot Ready workload,
   with complete groups ready to prepare under the qualified production read
   and operation budgets, require admission of every independently eligible
   peer within:

   ```text
   completed initial opportunity 100000 us
   + conservative 4G global-credit warmup 31250 us
   + seven exact slot turnovers 7*150000 us
   =1181250 us
   ```

   Empty peer credit has its separate maximum G warmup of 250000 us.
   Existing incomplete/invalid feedback and insufficient funded room never
   become eligibility by retimestamping. Source preparation, accepted grant,
   first native send, ACK, retirement and client application remain distinct.
   This explicitly replaces the old mechanically derived 220500-us promise.
   A different valid source configuration is not automatically qualified:
   offered-work feasibility and peer-by-peer progress require source evidence.

   HYBRID qualification must prove continuous eligible **peer** wait across
   replacement of an unaccepted encoded candidate. Distinct candidate IDs and
   exact byte counts remain distinct; replacement is not acceptance, delivery
   or payload equivalence. Carry the earliest eligibility only through the
   source's contiguous `replaced -> exact_demand -> credit_eligible` records
   for the same connection generation and timestamp, then end at an actual
   grant. Both candidates must fit the unchanged pre-reservation peer credit;
   peer/global credit, active grants and all four deferral counter witnesses
   remain identical throughout that atomic bridge. Repeated replacement cannot
   restart the1181250-us clock. Any
   observed overdue interval fails, including one ending in interruption or
   disposal. Missing/noncontiguous bridges, delayed successor eligibility,
   feedback interruption, eligible withdrawal, generation change or unfinished
   ownership cannot prove coverage. The independently required
   `ContinuousEligiblePeerEpisodeCoverage` must pass; the original
   `ExactDemandEpisodeCoverage` remains a diagnostic for distinct candidate
   episodes and retains its mandatory original semantics under legacy F1.
   This clarifies the existing peer guarantee without changing production
   admission or promising delivery of a candidate that was never accepted.

5. **Independent finite recovery outcomes.** Keep the 20-second service
   recovery maximum and the existing C3/C5 immutable cessation reference,
   exact quote, source/semantic coverage, finite accepted cut, moving relevance,
   causal replay and original numerical convergence deadline. Its old rate
   terms become an independent reference policy, not a universal delivered
   lower bound derived from credit. The 150/100-ms class does not mathematically
   imply that byte-only deadline for every legal tiny/mixed count distribution.
   All hard canonical outcomes must still pass on the adopted source.
   Live quote frame count is not a future count bound under legal divergence.

## Funding and capacity

Keep all existing rates, credit caps, reserves and resource limits:
96-MiB/s backend funding; 64-MiB/s structural funding/refill; 8/4/8-MiB/s
gameplay/control/transport reserves; 2-MiB/s peer refill; four structural
permits; one accepted grant per peer; G peer pending and 32G global pending;
4G accepted structural debt; 50-ms feedback freshness and one-second bounded
requalification. Credit and configured send rate are not delivered capacity.

The 16-MiB/s design value continues to fund prospective serialization and
defines the uniform maximum above. There is no additional default 31.25-ms
post-first G gate or continuous 16/64-MiB/s delivered entitlement. A 14-MiB/s
peer can serialize G in 35.714286 ms and G+20 KiB in 37.109375 ms; these are
feasibility arithmetic, not qualification. It may qualify only if every actual
hard outcome passes. A 14-MiB/s **aggregate** sender needs 142.857143 ms for four
simultaneously activated G groups and fails the uniform deadline. This is a
meaningful capacity negative without inventing a minimum legal grant size.

Repeated waves must include actual Main opportunities, group sizes, preparation,
fair rotation, ACK/reference lifetime, ordinary traffic, credit and regrant.
Offered demand below the required source workload, withheld final bytes/ACKs,
growing backlog, excluded peers or a late immutable recovery prefix cannot PASS
through averages or a favorable configured rate. Exact byte conservation and
resource bounds are checked throughout and at cleanup.

## Structural ownership correction

`ReliableByteAdmission::RefreshService` returns a permit only after exact
generation/token/byte retirement and complete, fresh Available feedback.
Unrelated ordinary ACK debt no longer owns that permit. Ordinary bytes remain
independently funded; native first-send, timeout, stale/incomplete evidence,
mismatched tokens and unaccounted disconnects do not return capacity.
Terminal cancellation and accounting remain explicit and separate.

Ordinary native-unsent bytes use a new exact unique BODY frontier, not ordinary
ACK debt. The accountant reuses its existing exposure word for this prefix;
its compact-state bound is unchanged. Before accepting a structural grant it
reserves the uniform G completion budget plus 20 KiB gameplay and the complete
64 KiB control burst in the 50-ms prospective peer window:

```text
ordinary_unsent_limit
=floor((R*50000-626199296000)/1000000)-20480-65536
=126645 B
```

This conservative resource eligibility check is not proof of actual FIFO
service. A large native-unsent prefix denies new structural exposure even if
global funding fits; the same already-first-sent ordinary ACK debt does not.
The independent global ordinary funding and reliable FIFO order stay intact.

## Independent journal and grant rotation

A shared cursor can be redirected by nonemitting journal reads while all slots
are held. The deterministic supported 128-record peer/512-record global pattern
uses the actual geometric read tail. Its acceptance step advances 12 positions;
six held steps advance 14 each. `12+6*14=96=0 (mod 32)`: every seven-step
turnover returns to the same four peers, starving 28 despite finite ACKs.

POOLED_SERVICE now rotates admission only after the last accepted grant.
An independent cursor rotates the existing nonemitting journal pass, using the
same bounded connection list and unchanged read caps. No payload queue or new
scheduler is introduced. The supplied corrected schedule serves all 32 peers
by 816683 us with exact retirement, four permits, 308 B debt high and identical
84*512 journal charges. This is a deterministic schedule, not a host measurement.
The previously qualified recovery encoder correction remains intact and its
historical receipt remains valid at its recorded revision.

## Health and evidence migration

Keep legacy F1 arithmetic and histories intact as nonfatal diagnostics only
under the explicitly adopted `HYBRID_F1_V1`. Native completion integrity,
native-positive truth, chronology, security, reliability and generation/lifetime
errors remain hard. Native, adapter and GameSession use the separate hard
completion fields together; qualified FIFO failures fence new structural service.
Execution/turnover breaches use existing structured failure and terminal
accounting, never time-based retirement.

Evidence readers require explicit contract identity. Legacy `finite_grant_v1`
and admission V1/V2 remain strict. New admission V3 identifies HYBRID_F1_V1.
Grant certificates require exact counts, completion, retirement and completed
Main opportunity/turnover identities; zero or missing evidence is not success.

The missing H2 acceptance fact is supplied by existing positive segment hooks:

Runtime protection is independent of an optional publication observer. The
original acceptance timestamp is captured before observer callbacks and follows
the existing scheduler message through WouldBlock. Direct protection starts
at transport entry and charges its prospective class only after successful
native acceptance. A nonzero queued timestamp also prevents bootstrap or
out-of-class queued work from being reclassified at a later Ready handoff.

Existing GNS message lifetime owns remaining unique BODY and non-owning links
in seven monotonic origin lists (six scheduler priorities and direct sends).
Positive never-sent BODY intersections reduce remaining bytes; retries, ACKs
and cursor advancement cannot discharge that obligation. At a complete native
feedback frontier, seven list heads suffice to detect an overdue outstanding
message, including provisional packet references. Completion/unlink is O(1).
Terminal purge detaches links before native release. Unsupported metadata or
unexpected early release invalidates protection evidence without fabricating
delivery, corrupting reliability ownership or replaying a successful send.
Missed protected deadlines remain sticky health failures for that connection
lifetime, even if a subsequent message is timely. Diagnostic queue failures
and diagnostic table overflow do not set production protection health.
The added native metadata footprint must be included in actual resource gates;
the compact structural accountant bound is unchanged.

Separately, the qualification certificate retains its bounded event table:
qualified origin/timestamp POD follows the existing reliable message; unique
BODY intersections advance only after positive native send. A fixed 40-entry
qualification metadata table retains up to 20 timely completed messages in
one unresolved packet and 20 currently awaiting completion under the declared
16-message burst/64-per-second/50-ms class. It assumes no actor reconciliation
bound, owns no payload/reference and cannot change delivery. Overflow makes
qualification incomplete/invalid. Bootstrap and legal deliberate overload
remain outside fixed ordinary phases, observable under their existing gates.

All 32 server connection certificates and 32 client certificates, across
33 processes, must join exactly to the existing successful ordinary demand
denominators, including direct controls and all original qualified queued
work. Native pending results and absent/unknown contracts cannot certify FIFO.
Source and observer overhead remain measured separately where available;
unmeasured instrumentation perturbation must be disclosed.

## Required qualification before closeout

1. Deterministic ownership, invalid evidence, boundary, fairness, FIFO,
   source/capacity and recovery negative regressions.
2. Affected Windows native, pinned real GNS and Linux sanitizer suites;
   all required exact-source hosted jobs.
3. One fresh bounded native32 campaign with actual offered shapes and all hard
   conjunctions. Correct only demonstrated defects; retain failed evidence.
4. Clean resource/link/capture/credential preflight, canonical Local32 and
   real-TLS Node32, both endpoint capture/resource receipts and verified cleanup.
5. Independent original Foundation 3L acceptance ledger and provider parity.

Until these receipts pass, KI-006 stays OPEN and Foundation 3L remains partially
ready. Keep PR #6 draft. No merge, release, Foundation 3M or historical relabeling.

### Initial coordinated preflight receipt

Base source `4eea2307ce8bf78e3f7a8354d5cb672dcd677dc8` plus the task changes:
MSVC Release, existing pinned GNS/cache, four build jobs; eight affected CTests
PASS in 71.48 s. Networking production has 28 cases, the historical/corrected
capacity schedules 13, native finite curves 19 and feedback 20. The first
preflight failed the unchanged compact accountant memory bound after adding
an exposure word; the implementation now reuses the existing word and the
same bound passes. Both logs and JUnit are retained at
`C:\Sandbox\Codex\Artifacts\foundation3l-permit-ownership` and the corresponding
local `C:\Users\aiden\.codex\artifacts\foundation3l-permit-ownership` directory.
This dirty-source preflight does not replace exact-source hosted CI or a physical
receipt. Additional queue-watchdog regressions and final source gates follow.

### First committed-source CI and supervisor correction

Native source `7c1be009fdbfcbe0857824393f8615a9c01a4225` preserves the prior
deterministic commits. Both original GNS sanitizer workflows passed:
push `38192457367` and PR `38192459467`. The push collection verified the
exact checkout marker and 11 original JUnit cases. This is partial CI evidence.
Windows push `38192457355` passed 95/96 CTests; the endpoint fixture failed
with `redirected output did not drain` after its harmless 32-child sampler
observation. Its dependent Linux job was skipped, so the required CI
conjunction did not pass. Native32 has not launched on this revision.

A bounded isolated Windows counterexample occupied the shared worker pool,
exited the owned child and retained its real anonymous pipes. `CopyToAsync`
missed the existing 5000-ms drain wait; the existing dedicated
`PhysicalFarmPipeDrain` completed both streams within that unchanged wait and
preserved exact stdout/stderr bytes. This demonstrates the implementation
mechanism; the original CI did not instrument pool saturation, so attributing
its precise scheduling state remains inference. The endpoint correction reuses
that helper and pins/stages its dependency. It changes no networking contract,
shutdown timeout or native implementation. A new exact-source CI conjunction
is required before the single fresh native32 campaign.

Correction preflight: endpoint fixture PASS in 21.49 s; isolated starvation
and the existing 33-child/66-stream pipe test PASS in 9.22 s; analyzer inventory
PASS; final acceptance reader 39/39 PASS; existing outer staging mocks 48/48
PASS. The isolated fixture waits for completed pipe writes before occupying
the pool and uses a child blocked on its own redirected stdin, avoiding an
unrelated child-start or descendant-spawn race. Its intermediate fixture
quoting failure remains retained. This observer-process correction does not
attribute the historical native initial-service failure.

Logs and original JUnit files are retained at
`C:\Users\aiden\.codex\artifacts\foundation3l-endpoint-pipe-attribution`.

Retained failure log SHA-256:
`7f6a83570750e1b0c5639d2a2554d96d044b3d9568c6db5dc26b925f4dc29ce9`,
at `C:\Users\aiden\.codex\artifacts\foundation3l-permit-ownership\native-push-38192457355-failure.log`.
Historical failures and the qualified recovery correction remain unchanged.

### Sink-independent ordinary health correction (2026-10-12)

All six original hosted jobs passed on baseline
`561baba1d7e948bedc52430d40123fdd657226bc`: Windows 96/96, native Linux
53/53 and GNS sanitizer 11/11 in each event. Official package custody also
verified both 58-file runtime closures. These receipts do not qualify the
subsequent ordinary-health correction.

A source counterexample found that no optional publication sink produced no
ordinary diagnostic tag, so even a tiny message completing at 51 ms could
escape production H2 while structural completion, opportunity and turnover
remained timely. No physical reproduction is claimed. The correction separates
runtime membership/health as specified above and keeps the existing observer
as certificate evidence. Independent review additionally removed a diagnostic
queue-failure dependency and froze acceptance before optional callbacks.

Combined MSVC Release preflight, four build jobs, production pause hooks OFF:
eight affected suites PASS in 71.88 s. Production accounting has 30 cases,
capacity schedules 13, causal recovery 32, native finite curves 19 and feedback
23. Coverage includes no-sink native lateness, 50000/50001 boundaries, partial
BODY/retries, sticky late-then-timely health, crossed scheduler origins,
diagnostic overflow, policy churn, bootstrap, WouldBlock and observer clock
perturbation. The first compile failed a missing fixture include and the
second link failed the omitted private transport-policy forwarder; both
failures and corrected snapshots are retained. Neither is a measured service
failure. Actual pinned allocation uses new/delete without whole-object memset;
the added message member's constructor/destructor participates in that lifetime.

Receipts and source-member hashes are retained under
`C:\Users\aiden\.codex\artifacts\foundation3l-h2-runtime-protection`
and the corresponding worker artifact directory. The separate MSVC build with
test-only pause hooks ON passed both native pending-ownership and worker suites
in 0.39 s. It covers unresolved feedback, negative-result retry, late positive
worker completion without a diagnostic sink and terminal detach/release.
Fresh exact-source hosted CI and the bounded campaign remain separate gates.
No native32, Local32 or Node32 result is inferred from these tests.

### First sink-independent source CI (2026-10-12): FAIL retained

At `7f30ed98670f94a22c4ecf6d604725a4fec28396`, both original GNS sanitizer
jobs pass 11/11. Native PR and the official dispatch pass 96/96 native tests,
then reject two stale source-text assertions: successful submission recording
is no longer immediately adjacent to the OK case, and the client phase is now
named before scope publication. The corrected assertions retain the successful
GNS submission boundary and the independent private/diagnostic scope update
before the gameplay step. This is not a native OS-completion measurement.
The focused ordinary-demand tests pass 28/28.

The original Native push separately passes 95/96 and fails funded ACK stats.
A 77-byte token 3 completes native first-send in 126 us. Accepted, unique BODY
first-send, BODY ACK, token/byte retirement and cleared ownership all match.
One 79-byte stream retry makes the fixture's combined `Conserved` false.
Its receiver schedules ACK at +50000 us but prepares it at +75892 us and
positively sends it at +75943 us. Pinned retry timeout is smoothed ping times
3000 plus 60000 us; retry is compatible with this delayed execution. The exact
scheduling/lock cause is uninstrumented and no correction to it is claimed.

The accepted [physical funding clarification](../../devdocs/CurrentArchitecture/PhysicalFundingGateReview3L.md)
explicitly treats nonzero retransmission as wire cost, not failure alone.
D01 charges retries/duplicates independently and D03 preserves that rule.
Logical conservation must therefore remain separate from retry telemetry.
The full stats/funding observation aborted before its terminal checks; neither
the observed equalities nor this source analysis relabels the run as PASS.
The reviewed fixture correction preserves exact delivery/retirement and all
funding, control-coverage, prompt-isolation and service gates. Fresh source
verification is required. No production timer, priority, rate or acceptance
constant is adjusted.

Both original dependent Linux jobs and the manual Linux job are skipped.
The original CI conjunction fails and no official package is produced.
Original index SHA-256:
`e0527807b93ec5d3b859106b869883be818c629214a57fb951fa83282d268ac4`;
checkout reconciliation:
`f6537182cb64c9e20a3034cc8f232862317c2140241293690ce2975c8cd66bc9`.
The failed push's unchanged whole 96-case JUnit SHA-256 is
`ade7ffe95dbc338154776a0e68b3de8080b13ae97a3d23a950dd80958cbe6407`.
These remain historical failures; staged native32 plan
`4c49856c-783e-45c0-b6a1-9ab9a2c3fc3d` has not executed.

Correction preflight: all 279 existing Farm32 tooling tests PASS in 197.120 s.
The focused real native stats boundary passes in 64.29 s: both original
48-grant arms, all completed-prefix wire ceilings, exact payload/stream
ACK/retirement, tracer and instantaneous send/receipt coverage, and prompt
isolation. Control has zero prompts; funded mode has 17. Each accepts
6694471 B; whole IPv6 wire upper bounds are 7204128 and 7204179 B, below
the unchanged 7587727 and 7587729 B ceilings. Both endpoint retry totals
are zero in this fresh run, so it does not reproduce or explain the late ACK.
The deterministic observed-value counterexample and nine invalid-accounting
negatives qualify the changed conservation definition. Native production
code, timers, rates and hard acceptance constants are unchanged.
Preflight JUnit SHA-256:
`569fe54dc4b9e6d0ddc28e28decc523b5a25a95e68c794d6e7b2d27e419e0387`.
Fresh original hosted CI is still required before the unlaunched campaign.

## Stats-boundary D03 predicate migration (2026-10-12)

Original PR CI at `e8c89f4aa575a00510ba8db32544fb12c4a618aa` fails
`gargantuan_gns_funded_ack_stats`; its dependent Linux job is skipped.
The control arm's token 34 has 393652 BODY bytes and exact first-send, ACK,
retirement and cleared ownership; retries are zero. Its 347 retained positive
native segment records sum to those exact bytes. First-send is 653 us after
activation and completion is 21794 us after activation. The exact completion
cost is `365642645504 <= 626199296000` byte-us. Legacy running deficit is
`27736588736 > 18025216000` byte-us and is the shown failed predicate.
The failed log does not print the separate hard completion flags, so their
values are not inferred. This arithmetic is not a complete D03 qualification.
Original Windows job `114687586999` log SHA-256:
`e2682c0f74e78633d189bd05b8cc7915b4a0c4ba8ca5b19b2d00b7edc62e58bc`.
The failed verdict remains unchanged; native32 run
`eae1f917-1755-4f14-92ce-bb370dd0a9bd` has not executed.

The stats fixture still fatally checked the legacy running/size-finite flags,
despite the adopted nonfatal diagnostic semantics above. Its correction replays
the existing bounded native BODY events through `NativeGrantCompletionCurve`,
checks exact token/bytes/original activation, completed sequence, hard evidence
flags and complete timestamp chronology, and rejects missing final bytes.
The same 37324/37325-us integer boundary applies to tiny, medium and maximum
grants. Invalid, incomplete, misidentified, reversed, overflowing and explicitly
failed certificates remain rejected. Legacy failures retain their detailed
trace output as diagnostics. Wire-prefix/terminal funding, stats/tracer coverage,
prompt limits, payload/FIFO and exact ACK retirement checks are unchanged.
This fixes the demonstrated stats-test migration only; separate stronger
four-grant probes and physical prerequisites are unchanged. No production
timer, scheduling, rate, acceptance constant or runtime behavior changes.
Fresh focused preflight and exact-source hosted CI remain required.

Focused fixture preflight passes in 64.30 s on the explicitly dirty e8 base
with only the revised stats header transferred. Both 48-grant arms conserve
6694471 B. Control has zero prompts and whole IPv6 cost 7204128 B below
7587728 B; prompt mode has 17 prompts and cost 7204179 B below 7587730 B.
Both arms observe tracer and instantaneous send/receipt coverage. The new
completion/conservation definitions pass. No runtime legacy-only deficit
reproduces; the deterministic legacy-failed/native-complete case executes.
The preflight is not clean-source CI or native32 qualification.
JUnit SHA-256:
`7b09565fa0355c6808155f9425161e1b295946061a5cdb728dcbb8168d9e3ef5`.

## Original e8 execution-opportunity failure (2026-10-12)

The same source's original manual packaging workflow passes 96/96 native tests
but fails the subsequent POOLED single-peer mixed-overload product workload.
The server rejects peer 1:1 with the execution-opportunity budget diagnostic;
that source predicate combines Main 100-ms and turnover 150-ms failures.
The output does not retain the distinguishing bits. Service recovery,
convergence and application failures follow disconnection; they are not an
attributed native sender defect. The preceding structural-overload case
recovers in 93.9855 ms and converges in 363.169 ms. Its 20.0094-s drain
intentionally observes the 20-second journal snapshot.

The terminal mixed-overload `Client.Poll` phase spans 214.7994 ms on the shared
fixture Main thread (`3931549734000..3931764533400` steady ns). Coarse sampled
thread/process CPU deltas are zero. This phase includes native polling and
application dispatch and may include disconnect cleanup after an earlier
rejection. It is not established as the initiating delay, lock contention,
OS scheduling or native execution. Source bounds received work (1024 native
messages / 2 MiB per connection, 128 dispatched events), but public pinned-GNS
table/connection lock acquisition retries have no total deadline. Callback and
terminal-store mutex acquisition also has no proven wall-time bound.

The original six-job conjunction remains FAIL: Native push Windows 96 and
Linux 53 pass; Native PR Windows 95/96 fails stats and Linux skips; both GNS
events pass 11. The manual Linux job skips and no qualified package exists.
Original index SHA-256:
`47bcc9592fc604ed31436501b57f12ada0bb2121c4329489b19d3f40944e27fd`;
checkout reconciliation:
`2b97d621876250c5e773e2e6c3c0c2de4d008a8ccb222679d6b5da3201bfda46`;
manual failed log:
`07eb18991f12fe5df33c4c41adb5e5ee072fdd801cd9a350c19d7b57c874f55a`.

One bounded existing POOLED workload preflight retains the first existing
`PooledServiceRecord.Result` execution failure with flags, maxima, timestamp,
step and debt identity. The scalar witness forwards all original evidence
callbacks and changes no clocks or health rules. Independently, the fixture
declares its 16-KiB/concurrency-16 gameplay/mixed ordinary overload sources,
preserves normal ordinary protection during structural overload, and restores
normal scope before new recovery probes without resetting credits or accepted
memberships. The single-peer pooled client explicitly declares its ordinary
source. These fixture corrections do not establish the cause or fix the
retained execution rejection. No deadline exemption or runtime correction is
justified by this evidence. Native32 remains unlaunched.

The one focused preflight passes all eight cases in 125.828 s with every
first-failure witness `observed=0`. Created/retired bytes both equal 38668514;
terminal/outstanding bytes are zero. Process/port cleanup is verified.
Summary SHA-256:
`c1a914ce3ffe3bd153aa244060367775e2694c41cb1854de8f9da9fac446e8d8`.
The original failure does not reproduce and remains unresolved.

Independent offline replay of the original manual scheduler trace proves
loss-free anchor-window coverage. The 214.7994-ms terminal client-poll span
contains 4.8864 ms Running, 129.0823 ms Waiting/blocked and 80.8307 ms Ready.
All four waits report state 5/reason 6; the first wait is 120.7932 ms.
Wait object/stack/owner, first-breach time and Main-versus-turnover bit are
not measured. That span may include cleanup after the actual initiating
failure. QPC resolution is 100 ns; observer perturbation is unmeasured.
Attribution SHA-256:
`a141251b9a0a5adf86ae140ad7ddfdd32d79027e6ab93165de1490d2df5e8365`.

No source-supported production correction is demonstrated. Further campaign
launches stop for the [execution architecture assessment](../../devdocs/FutureArchitecture/Foundation3LNativeEgressOwnershipDecision.md#17-hybrid-execution-opportunity-qualification-decision-2026-10-12).
Retain the exact 37.3243865966796875-ms completion, 50-ms ordinary, 100-ms
Main and 150-ms turnover requirements and all other canonical guarantees.
This is not a platform impossibility proof or a D03 qualification PASS.
