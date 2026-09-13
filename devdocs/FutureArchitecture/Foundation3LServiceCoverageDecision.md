---
status: proposed-not-accepted
owner: runtime-networking-and-runtime-host
last_verified: 2026-09-13
---

# Foundation 3L physical service-contract decision

**Recommendation: C — REVISE SERVICE COVERAGE.**

Read-only assessment of `foundation/3l-content-availability` at
`4966f223467b12fc396f47ccb7736679c5dcbbc0`, 2026-09-13. This recommendation does
not amend the accepted contract, qualify hardware, authorize implementation,
close KI-006, or begin 3M. The original assessment changed no repository files; this publication records its
recommendation without accepting or implementing it.

## 1. Current contract semantics

The implemented profile and the current sections of the deployment/workload
documents take precedence over historical proposal sections in those files.

| Symbol | Current meaning | Category / important limit |
| --- | --- | --- |
| R | Reserved complete application-message service per connected peer; selected workload uses 8,388,608 B/s | Capacity entitlement. Structural credit refills at S times R. It is not measured throughput or merely an average offered-load cap. |
| A | Reserved application service for one Server process; selected 32-peer value 268,435,456 B/s | Aggregate entitlement; N times R must not exceed A. Not a host-wide allocator across multiple Server processes. |
| Backend ceiling | Explicit host CLI sets GNS send min=max to 2R, per listener/connection | Implementation headroom, including packet/realtime allowance. All N ceilings must be funded by the current deployment contract; a configured maximum is not a service lower bound. |
| G | 524,288 B complete reliable GNS message, including the 32-B adapter envelope; at most 524,256 B application frame | Legal supported complete-group/message ceiling. Not a sustainable rate or a deadline. Larger irreducible groups fail explicitly. |
| E | 262,176 B: one codec-ceiling Remote plus adapter framing | Conservative legal-frame gameplay admission headroom. Not the smaller ordinary qualified burst and not a hard total-memory cap. |
| Qp | At least G+2E = 1,048,640 B with default structural burst G | Per-peer admission-exposure threshold. Includes room for structural work, an existing gameplay burst and fresh gameplay headroom. |
| Qg | At least G+2NE; 17,303,552 B at N=32 with default global burst G | Aggregate admission-exposure threshold, not an exact GNS memory bound. |
| Bp / Bg | Finite structural credit caps, each at least G; defaults G | Burst credit, initialized at zero and replenished by elapsed time. Credits do not themselves reserve packets or create bandwidth. |

Hard resource safety is separately enforced by finite scheduler, backend,
message, request, planner, journal and decode limits. Profile Qp/Qg must remain
within the existing native 64-MiB peer / 256-MiB manager resource ceilings;
actual configured queues may be tighter. Qp/Qg describe admission exposure,
not all allocator or sent-unacknowledged memory. Exposure includes scheduler
unsent reliable bytes plus adapter framing and provider pending reliable bytes;
sent-unacknowledged data is not counted twice as another unsent FIFO prefix.

Latency compatibility is a necessary check: with the current 100-ms nonqueue
allowance, Qp/R and Qg/A must each fit the remaining 150 ms, and backend capacity
must cover 2R. Actual end-to-end percentile/max service still requires workload,
host, client and path qualification. Neither this arithmetic nor a token bucket
is a proof of a latency guarantee.

## 2. Source of the multi-gigabit requirement

The current docs intentionally require all connected peers to be funded,
including idle peers. Three constraints combine:

1. Preserve the maximum legal group and two E allowances in Qp.
2. Fit that possible FIFO exposure within the selected queue allowance.
3. Fund N per-peer entitlements and N backend ceilings simultaneously.

At the selected R, N=32 requires 32 times 8 MiB/s = 256 MiB/s =
**2.147483648 Gbps** application reservation. Backend funding is twice that:
**4.294967296 Gbps**. Do not add the application reservation to the backend
number again; the backend envelope already contains its application component.

Even the minimum integer R allowed by Qp/0.150 is 6,990,934 B/s. At 32 peers
that is 1.789679104 Gbps application and 3.579358208 Gbps backend capacity.
Changing only the configured rate cannot reconcile the existing promises with
a 1 Gbps path. The much smaller qualified gameplay offering does not change
these current reservation obligations.

## 3. Idle and low-traffic peers

**Current contract: full potential service entitlement must be physically
funded.** Idle peers do not transmit at R or consume continuously allocated NIC
time slots, but the deployment must be able to honor their entitlement without
overbooking A. Unused elapsed credit stops at a finite cap. Reconnect starts a
new generation with zero peer credit; the global bucket is not recreated.

**Recommended model:** bounded peer credit and a positive fair structural share
of one funded pool, plus ordinary gameplay coverage. Idle peers would not own
permanent peak maximum-group drain capacity. Logical credit could accumulate
only to its cap; spending it would additionally require an actual funded drain
grant and sufficient latency headroom. A future entitlement is not free capacity.

## 4. Maximum-group guarantee

Current legality permits an admissible complete group through the existing
planner/codec path, subject to schema, dependency, resource and message limits.
This is not permission for every legal request to be accepted immediately.

The implementation already waits for complete peer/global credit before
scheduler acceptance. It retains bounded planning state, exact byte hints,
rotating consideration and an eligible large-group earmark. Known and emitting
journal progress advance only after matching scheduler acceptance.

The approved no-large-group exception protects ordinary gameplay from an
already-admitted large FIFO prefix on a compatible, qualified path. It does
**not** establish a universal 250-ms deadline from initial structural demand to
final client application. Planning, initial credit, peer contention, provider
acquisition and application can add time. Current measured convergence already
exceeds 250 ms in some qualified simulator cases. No unconditional eventual
convergence exists under endless invalidation, provider outage or an unserviced
peer; those conditions retain bounded failure/overload behavior.

Likewise, N times R funding is not an assertion that all maximum groups enter
the scheduler at the same instant. The global burst defaults to one G and
admission is serialized on Main. There is no demonstrated physical all-peer
maximum-group completion-time qualification in the present evidence.

## 5. Explicit simultaneous-demand classification

For the recommended service model, a **finite wave of one eligible maximum
group per connected peer**, on a correctly provisioned and qualified profile,
with conforming gameplay and healthy endpoints, is:

**QUALIFIED BUT DEGRADED OPERATION.**

The degradation is longer, explicitly bounded structural admission/convergence.
Ordinary gameplay targets remain unchanged. This is proposed service coverage
requiring new proof and tests, not an assertion that this physical case has
already passed. It makes the previously implicit contention case explicit.

Repeated demand above the declared sustainable structural envelope, excessive
gameplay, endless invalidation or unbounded churn is **BOUNDED OVERLOAD**.
Running the unchanged 32-peer full-reservation profile on the inspected VPS
path is an **UNSUPPORTED DEPLOYMENT CONDITION**. These are different conditions;
ordinary failures cannot be relabeled overload after a test fails.

## 6. 1 / 2.5 / 5 / 10 Gbps consequences

Use the current R=8 MiB/s and backend=16 MiB/s. For this assessment, leave 20%
of nominal line rate outside the aggregate configured backend ceilings for
deployment margin. This is a conservative planning assumption, not a new
accepted profile, measured usable capacity, or a substitute for path tests.

N ceiling = floor(0.8 times LineBitsPerSecond / (16,777,216 times 8)).

| Nominal link | Budget after 20% margin | Arithmetic peer ceiling | Ideal ceiling with no margin |
| --- | --- | ---: | ---: |
| 1 Gbps | 0.8 Gbps | **5** | 7 |
| 2.5 Gbps | 2.0 Gbps | **14** | 18 |
| 5 Gbps | 4.0 Gbps | **29** | 37 |
| 10 Gbps | 8.0 Gbps | **59** | 74 |

These counts are not supported-client claims; CPU, memory, receive paths, loss,
RTT and service distributions may reduce them. Under this margin, 32 peers need
at least **5.36870912 Gbps** nominal capacity, making a genuinely funded 10 Gbps
class the straightforward choice among these four link classes.

The latest VPS evidence cannot be substituted for such a line: its best TCP
direction is about 1.001 Gbps, reverse about 108–174 Mbps, with significant UDP
loss below the required envelope. This is WireGuard-path evidence; untunneled
provider capacity remains unknown. No peer ceiling from the table is thereby
qualified on those VPSes.

## 7. Option A — full-envelope funding

This preserves the clearest existing semantics, implementation and safety
evidence. It requires no admission-policy redesign and is appropriate when
multi-gigabit service and compute are actually provisioned. Operational cost
grows with connected count regardless of ordinary offered load.

For a 32-client product target it still requires funded infrastructure and the
actual-client Local/Node matrix, including maximum-group coexistence, fairness,
resource use, recovery and cleanup. Existing simulator, security, overload and
journal evidence remains reusable within its source/workload scope. KI-006
cannot close from purchasing a nominal port or from this arithmetic alone.

This is defensible but a poor default product model when ordinary 32-client
service is the intended useful scale and permanent per-peer peak reservation
dominates deployment cost. It remains a valid legacy/high-capacity class.

## 8. Option B — smaller physical scale

With current selected rates, the largest conservative arithmetic candidate on
a genuinely available 1 Gbps path is **five peers**, not eight, twelve or sixteen.
Five requires A=40 MiB/s and aggregate backend funding=80 MiB/s, or 671.09 Mbps.
Seven fits only the ideal no-margin arithmetic; eight needs 1.074 Gbps before
additional deployment margin. Tuning to the minimum compatible R would be a
different numeric profile needing validation, not evidence for the existing one.

A smaller explicitly supported product could close 3L after actual-client
qualification and final acceptance. Thirty-two physical clients would then no
longer be a normative gate; 32/200 simulator results would remain larger-scale
boundedness evidence. Five actual clients cannot demonstrate the canonical
eight-recipient fanout: a separate small-profile workload and explicit product
scope would be necessary, not a relabeling of the 32-client case.

This option is the simplest implementation path but substantially narrows useful
multiplayer scale. It also does not make the current asymmetric VPS pair a funded
five-client deployment. I do not recommend shrinking the product to five clients
as the general service philosophy merely to avoid examining the reservation model.

## 9. Option C — exact proposed service separation

Add an explicit **pooled service coverage** mode while retaining the existing
full-reservation mode without silently reinterpreting its R/A fields.

Separate these quantities:

- Hard legal ceiling G and existing hard memory/count/lifecycle bounds.
- A small ordinary structural packing/turn quantum, which is not a new legal
  ceiling. A larger indivisible group accumulates credit and needs a whole-group
  drain grant; it is never semantically split or silently rejected as too large.
- Per-peer structural credit/fairness share, independent of peak drain service.
- One physically funded aggregate service pool and a bounded aggregate backend
  envelope including realtime, packet and retransmission headroom.
- Positive conditional structural service lower bounds and a separate bound on
  queue residence after admission.
- Qualified gameplay arrival/burst allowances, distinct from legal-frame resource
  headroom. Preserve 20,480 B +32,768 B/s and 16 +64 messages/s per peer/direction,
  the accepted aggregate budgets, frame sizes, fanout and concurrency.

Replace N times peak drain rate funding **only in the new mode** with funded
aggregate service plus controlled admission of active drain obligations. All
promised minimum sustained shares must still fit the pool. There is no economic
escape through oversubscribing guaranteed floors; only idle peak capacity is
shared. Static backend maxima alone must not become unbounded concurrent grants.

### Protect the existing reliable FIFO

Longer pre-admission waiting is safe for already-materialized gameplay only if
an admitted group's entire FIFO debt can drain within the remaining gameplay
budget. Accumulated credit alone proves none of this. Neither datagram
fragmentation nor a smaller nominal quantum removes head-of-line blocking.

Before complete-group scheduler acceptance, require both peer and aggregate
funding for existing pending exposure, the new complete group, and fresh
qualified gameplay burst. Maintain conservative physical drain lower bounds
for the duration of that admitted obligation. A sufficient end-to-end check
must allocate the queue budget across actual stages; checking each serial stage
against the entire 150 ms independently is not an end-to-end proof. One
conservative design criterion is:

    NonqueueAllowance + PeerQueueBound/PeerDrainFloor
                      + AggregateQueueBound/AggregateDrainFloor <= 250 ms

The modeled queue bounds include the prospective group and qualified gameplay
headroom. This deliberately conservative sum may count shared bytes at more
than one serial stage; a tighter service-curve proof may avoid that duplication.
It is a proposed proof obligation, not a completed guarantee. RPC percentiles
still require measurement, as do application work and client request service.

If a supported G cannot fit even an otherwise idle grant while preserving
gameplay, the profile is incompatible. It must not wait forever, declare all
large groups overload, or silently shrink G. The design must instead reject
that claimed profile or obtain adequate service. Unproven drain capacity keeps
the deployment unqualified. The current VPS pair is not made qualified by C.

### Distinguish hard resource exposure from qualified latency exposure

Keep all existing codec and hard memory ceilings. Preserve legal but
unqualified traffic's finite acceptance/failure semantics. Do not simply replace
E=262,176 with 20,480 in the existing validator.

Define additional, explicitly named ordinary-service exposure bounds using the
qualified peer and aggregate arrival envelopes. Already pending bytes count
regardless of whether they arose from ordinary or unqualified traffic. Structural
admission stops before these service bounds can be violated. Out-of-envelope
gameplay must be detected and contained using finite existing request/peer
failure paths before it can consume unbounded shared service. Preserve accepted
reliable order; never silently drop an accepted message to recover the envelope.

### Structural progress and completion

Each eligible, continuously backlogged healthy peer must receive a positive
service lower bound, with bounded scheduling latency. A formal profile can
express this as beta_i(t) = r_i times max(0, t-L_i), where r_i is guaranteed
structural service and L_i includes the proven grant/fairness latency. This is
different from a token refill rate, which is an upper bound on permission to send.

For finite eligible structural work W_i, this yields a conditional service bound
L_i + W_i/r_i, plus separately bounded preparation/application/path terms not
already included. The implementation design must derive finite r_i/L_i from
admission, whole-group service, active peer count and real drain assumptions;
it may not assume the token rate is automatically delivered service. Provider
acquisition and unready dependencies are separately timed, not erased from
end-to-end content measurements.

A finite all-peer maximum-group wave is covered by that structural bound, not
the 250-ms gameplay deadline. Sustainable mutation/input demand must fit the
declared structural envelope and existing journal/continuation capacity.
Unending overload, peer stalls or provider failures receive bounded explicit
failure/recovery, not a promise to converge an ever-changing infinite workload.

Gameplay targets remain RPC p95/p99/max **150/250/500 ms**, Event ACK max
**250 ms**, and action result max **250 ms**. New objects' dependent Remote calls
retain the existing dependency qualification boundary; an invocation waiting for
unmaterialized prerequisites cannot be reported as a fast ordinary call by
resetting its timer after materialization.

## 10. Abuse, DoS and fairness

Clients do not directly select arbitrary authoritative groups or content keys.
The adversarial case is repeated permitted movement/actions/reconnects causing
server-owned demand, or a trusted application producing excessive changes.

| Adversarial behavior | Required containment |
| --- | --- |
| Repeated maximum groups | One bounded pending requirement/continuation per existing owner; exact whole-group accounting; fair turns; sustained-demand classification established before tests. No serialized payload queue while waiting. |
| Small groups racing a large waiter | Preserve eligible large-group earmarking or prove an equivalent bounded-service discipline. Require progress bounds, not just eventual observed fairness in one trace. |
| Idle credit hoarding | Finite peer/global credit caps, zero initial credit, elapsed-time refill, no borrowed debt or unlimited catch-up. Credit does not bypass drain grants. |
| Reconnect churn | Generation-safe owner cleanup, zero new peer credit, persistent global budget, bounded bootstrap/join service; reconnect cannot reset global fairness or grant accounting. |
| Slow/stalled receiver | Ineligible peers cannot hold the global turn indefinitely. Already admitted debt remains accounted until drained or terminally released; a timer cannot make outstanding bytes disappear. |
| Gameplay or fanout abuse | Account every recipient, reply, action and generated control message; enforce bounded qualified admission/isolation and existing hard parser/request limits. One peer's overload must not silently consume another's promised service. |
| Missing feedback/path failure | No fabricated zero backlog or larger credit. Stop structural admission, record the service violation and use bounded recovery/terminal policy. |
| Stale plans/large deferral | Revalidate identity/revision/encoded size at preparation; only scheduler acceptance changes Known; retain journal/planning limits and explicit terminal failure before lost history. |

No finite system can guarantee ordinary latency under unlimited external flood
or arbitrary loss. State the protected workload and healthy-path assumptions;
do not use that fact to excuse starvation caused by admitted conforming peers.

## 11. Deployment-profile philosophy

Use a small set of named, versioned service-coverage classes backed by one
validator and exact numeric deployment records:

1. Development/unqualified: bounded correctness, no physical latency claim.
2. Ordinary pooled dedicated: the recommended production qualification target.
3. Existing full-reservation coverage retained for compatibility/high-capacity
   deployments; it is not automatically qualified by keeping its old numbers.

Publish only actually qualified host/path/N/workload combinations. Do not expose
an unrestricted N/R/A formula as an implied supported product, and do not build
a broad hosting SKU matrix now. The initial pooled production target should
remain 32 actual clients; larger support is a later independently funded claim.

## 12. Comparative decision

| Criterion | A: full funding | B: smaller scale | C: revised coverage |
| --- | --- | --- | --- |
| Correctness | Preserve current design | Preserve current design | Preserve invariants; new service proof required |
| Security/resource safety | Existing bounded model | Same model | Additional abuse/service isolation proof required |
| Simplicity | Simple entitlement | Simplest immediate implementation | More admission and validation complexity |
| Implementation cost | Low | Low | Highest; targeted Main-owned admission/profile work |
| Physical cost | High at 32 connected peers | Lower, but only a few clients on 1 Gbps | Pool sized to explicit workload/progress coverage, still physically funded |
| Useful scale | 32 only with funded hardware | About five candidate peers with chosen margin | Retain useful 32-client target without permanent per-peer peak entitlement |
| Future scaling | Linear peak reservation | Same limit at smaller N | Bounded shared service; progress bounds explicitly depend on N/demand |
| Existing evidence | Broad reuse, missing physical run | Broad reuse, new smaller workload needed | Unaffected correctness reuse; affected service matrices must be rerun |

**Choose C.** This choice is driven by accurate separation of legal support,
hard safety and workload-qualified service, not simply cheap hosting. It accepts
real engineering/proof cost and does not certify the current VPSes. A and B remain
analyzed alternatives, not co-recommendations or an automatic fallback policy.

## 13. Architecture delta and implementation owners

| Owner / code | Proposed responsibility |
| --- | --- |
| ServerHostConfiguration / ServerHost | Explicit service-coverage mode, immutable startup parameters, physical budget and supported N; retain legacy interpretation |
| ReliableServiceProfile header/validator | Separate hard ceilings, qualified bursts, fair sustained credit, peak drain and aggregate funding; fail impossible service claims |
| ReliableByteAdmission | Bounded peer/global credit, fair eligibility, active drain-obligation accounting and prospective service-exposure checks; no semantic/payload ownership |
| GameSession | Snapshot complete feedback, own lifecycle, account all admitted traffic/peer teardown, compose ordinary-service containment and exact reservation/acceptance |
| Replication planner/coordinator / 3J | Remain dependency/group owners; preserve bounded READY continuation, exact revalidation, acceptance-only Known, 65,536 planning / 8,192 selection |
| NetworkScheduler / GNS adapter | Preserve FIFO and wire behavior; supply bounded feedback and applicable backend controls. Do not introduce a second lane or semantic queue. |
| Remote/Character owners | Preserve authority and gameplay semantics; charge actual generated traffic/fanout and expose bounded overload outcomes through normal paths |
| Tests and architecture docs | Prove service model, quantify workload and resource state, then qualify numeric physical profiles |

Node remains a content provider and has no new authority or implementation role.
No wire fields, reliable lanes, order domains, group splitting, live runtime
authority, increased resource ceilings or unbounded queues are proposed.

## 14. Required validation

Before implementation, an executable service model must prove both the complete
maximum-group FIFO bound and a finite fair progress bound. Do not delete N times
R validation first and try to infer safety from a passing small-frame benchmark.

Required implementation validation includes:

- Exact G, one byte above G, near-max dependency groups, and variable encoded size;
  no false Known/cursor progress when insufficient credit, room or drain funding.
- Empty and overlapping gameplay bursts; all directions, largest ordinary frames,
  concurrency, generated fanout, forced states and all-window byte/message audits.
- All peers simultaneously demanding G, mixed tiny/large groups, continuous
  adversarial demand, reconnects, slow/nonreading peers and unavailable feedback;
  assert finite service bounds and debt conservation, not just nonzero totals.
- Real same-FIFO GNS tests with gameplay arriving immediately behind G, aggregate
  competition, delayed ACK/loss/retransmission cases within the declared path class,
  and startup rejection of a physically impossible group-drain profile.
- Long deferral, plan invalidation, KI-007 reference ordering, 3J exact selection,
  journal margin, complete-group reservation/rollback, overload drain and recovery.
- Zero peer owners, connections, pending RPCs, drain grants, reservations, stale
  journal readers, transient content and planner state after failure/disconnect.
- Affected MSVC Release, ASan/UBSan/LSan, dedicated GNS sanitizer and current-source
  CI; reuse unaffected exact-source security evidence with explicit scope.
- Canonical Local/Node simulator regression at 32/200 and actual 32-GameSession
  process qualification on a newly funded numeric profile: full receive/application
  path, due-service joins, gameplay, content lifecycle, fairness, CPU/RSS/network
  and shutdown. Recheck official single-client and upper/concurrent service when
  profile defaults or shared admission paths change.

This assessment changes no source, so it does not rerun builds or benchmarks.

## 15. KI-006 impact

KI-006 remains OPEN. Option A closes it only after the supported full-envelope
physical profile passes; Option B can close it for an explicitly adopted smaller
product profile after equivalent evidence; Option C adds implementation and
service-proof work before the physical qualification can close it.

Neither a design recommendation, smaller average workload nor a simulator pass
is closure. Retain the VPS stop. No hypothetical 500-client obligation is added.

## 16. Foundation 3L closure impact

Foundation 3L remains partially ready. With A or the recommended C and the retained
32-client product target, physical 32-client qualification remains mandatory.
B would permit smaller-scale closure only through an explicit product scope
decision and a suitable actual-client workload, not an unannounced reduction.

After the accepted supported profile, security/correctness, service, cleanup,
documentation and exact-source validation gates pass, 3L can close without future
200/500 physical qualification. Those larger counts are not current support
promises. No GPU/render gate or 3M work is added by this decision.

## 17. Exact next task

On acceptance of this recommendation: formalize the pooled service-coverage ADR
and produce its executable admission/service proof with concrete candidate
numeric profiles. Preserve current full-reservation behavior. Make the proof
exercise the unchanged maximum legal group, exact gameplay envelope and finite
all-peer contention; specify all ownership, overload and shutdown behavior.

Only after that design passes its FIFO/liveness gate should the scoped profile
and admission implementation proceed, followed by affected regressions and a
funded 32-actual-client Local/Node run. Stop if the proposed profile cannot
service G alongside ordinary gameplay without any prohibited lane/order/split
change. Do not implement Option C, change contracts or begin 3M in this assessment.

## Source references

- [Current deployment contract](../CurrentArchitecture/NetworkingReliableDeploymentContract.md)
- [Historical service-envelope evidence and current follow-up](../CurrentArchitecture/NetworkingReliableServiceEnvelope.md)
- [Gameplay workload contract](../CurrentArchitecture/ReliableGameplayWorkloadContract3L.md)
- [Canonical recipient workload](../CurrentArchitecture/RecipientServiceWorkload3L.md)
- [Reliable gameplay qualification](../CurrentArchitecture/ReliableGameplayQualification3L.md)
- [Client/scale qualification](../CurrentArchitecture/ContentClientScaleQualification3L.md)
- [Physical preflight](../CurrentArchitecture/PhysicalDeploymentPreflight3L.md)
- [Validation ledger](../CurrentArchitecture/ContentAvailabilityFoundation3L_3Validation.md)
- [KI-006](../../KNOWN_ISSUES.md)
- [Profile validation implementation](../../src/network/ReliableServiceProfile.cpp)
- [Admission implementation](../../src/network/ReliableByteAdmission.hpp)
- [Reservation and scheduler-acceptance composition](../../src/network/GameSession.cpp)
- [Established VPS measurements](../CurrentArchitecture/VpsPhysicalPreflight3L.md)
