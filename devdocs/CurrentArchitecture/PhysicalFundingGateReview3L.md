---
status: accepted-qualification-clarification
owner: runtime-networking-and-runtime-host
last_verified: 2026-09-23
related_code:
  - include/gargantuan/network/ReliableServiceProfile.hpp
  - src/network/PooledReliableServiceFeedback.hpp
related_adrs:
  - ../FutureArchitecture/Foundation3LServiceCoverageDecision.md
---

# Foundation 3L physical funding-gate review

## Accepted decision

**B — RAW CAPACITY ESTABLISHED; PROCEED TO BOUNDED REAL-GNS PHYSICAL
QUALIFICATION.** The operator approved this review against published
`4d27238553fdc27b9f6772b438c49393eac3a8eb` on
`foundation/3l-content-availability`. This clarifies the accepted contract; it
does not amend `POOLED_SERVICE`, qualify the application, close KI-006, authorize
a merge, or begin Foundation 3M.

The [fiber diagnostic receipt](FiberPhysicalPreflight3L.md) establishes direct
10 GbE, MTU 1500, and 9.246–9.471 Gbit/s bidirectional TCP. Static addressing
removes the independently observed DHCP/APIPA WSH/NETIO sender pathology and
permits approximately 900 Mbit/s synthetic UDP generation. Residual synthetic
UDP loss remains unattributed within Windows. Those observations are retained;
they are not evidence of an Engine failure or a hard physical-capacity ceiling.

Synthetic UDP is diagnostic/preflight evidence. **Zero unexplained UDP loss is
not a pooled-service invariant.** Additional Windows/vendor NDIS attribution is
not a prerequisite for the bounded production-GNS probe. Historical instructions
in the receipts to require such attribution or zero-loss repeatability before
application testing are superseded by this review, not silently deleted.

## Unchanged funding and service contract

The [canonical profile](../../include/gargantuan/network/ReliableServiceProfile.hpp)
remains authoritative: BackendCap 96 MiB/s; StructuralPool 64; GameplayReserve 8;
ControlRealtimeReserve 4; RequiredTransportReserve 8; unallocated slack 12.
Four active structural grants require 16 MiB/s **unique first-send** service
each. Their configured transport ceilings total 72 MiB/s: the existing
`BackendSendRate()` funds 18 MiB/s per grant from structural plus transport pools.
Configured rate and sampled wire rate are not measured unique service.

Retain the [pooled proof](PooledReliableServiceProof3L.md), production admission,
exact attributed retirement, all credit/grant/fairness and debt-conservation
rules, qualified feedback/freshness semantics, gameplay gates and recovery
semantics. Do not translate the transport reserve or slack into an invented
acceptable UDP-loss percentage. Retransmitted bytes are physical cost and
never unique logical drain; nonzero retransmission alone is not failure.

Each connected peer still earns 2 MiB/s structural eligibility credit. Four
actual clients can exercise repeated concurrent four-grant bursts, but cannot
establish sustained 64 MiB/s eligibility. Report the tested demand and active
intervals explicitly; do not equate whole-run offered average with an active
grant's drain floor.

## Ordered qualification gates

1. Establish exact-source build/dependency hashes and static dedicated-link
   addresses, routes, selected interfaces, link speed, MTU and host/NIC state.
   No mixed-age inventoried binary qualifies. Do not weaken Windows security
   policy or retain unrelated diagnostic NIC tuning.
2. Run a short actual GameSession/GNS probe through production pooled admission
   and native feedback. Exercise all four grants simultaneously under structural
   demand with representative ordinary gameplay/control traffic. Obtain repeated
   fresh observations, including unique first-send/ACK progress, exact structural
   retirement, retransmission, pending/unacked bytes, queue time, configured and
   effective rates, wire rates, RTT, grants/debt, gameplay and host resources.
3. Stop on a canonical service/funding, freshness, debt/fairness, latency,
   recovery, resource or lifecycle violation. Preserve failed evidence. If actual
   GNS loss consequences prevent service, return to path attribution with that
   evidence; do not automatically restore a synthetic zero-loss gate.
4. Only after a healthy probe run the unchanged physical matrix with **exactly
   32 actual GameSession clients**, independently for Local and Node. Neither
   simulated peers nor protocol observers qualify. Preserve per-peer attribution,
   canonical content/Character/root/RPC/Event/action demand, load/evict/reload,
   Known/journal behavior, and complete resource/lifecycle measurement.
5. Keep [fixed 20-second service recovery distinct from workload-derived complete
   structural convergence](PooledReliableServiceRecoveryContract3L.md). Retain
   [due-to-observation Character/fanout semantics](RecipientServiceWorkload3L.md#measurement-contract),
   not obsolete shared-fixture raw-cadence gates.
6. Verify cleanup after every run. Only both fully passing physical provider
   runs make KI-006 eligible to close and permit the final existing 3L acceptance
   sweep. Missing required evidence is **not measured**, not PASS. No new closure
   gates or relaxed contract are introduced.

Static addressing is a **candidate physical deployment requirement** to avoid
the known sender pathology. A successful qualification must record its address
plan and verify persistence with the smallest safe interface lifecycle test.
Configuration presence alone is not deployment qualification.

## Receipt and status

The [physical qualification receipt](PooledPhysicalQualification3L.md) owns
execution results. KI-006 remains OPEN until that actual-client matrix passes;
Foundation 3L remains **B — PARTIALLY READY** until all existing gates pass.
Unaffected qualified production/CI evidence retains its original scope.
