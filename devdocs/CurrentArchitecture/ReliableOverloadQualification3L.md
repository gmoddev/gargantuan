---
status: blocked-on-gameplay-contract
owner: runtime-networking
last_verified: 2026-09-12
related_code:
  - include/gargantuan/network/RemoteProtocol.hpp
  - include/gargantuan/network/RemoteManager.hpp
  - src/network/RemoteProtocol.cpp
  - include/gargantuan/network/CharacterProtocol.hpp
  - src/network/CharacterProtocol.cpp
  - tests/OfficialNodeHostVertical.ps1
---

# Foundation 3L overload qualification: Part A contract gate

**B — FOUNDATION 3L PARTIALLY READY.** Inspected branch
`foundation/3l-content-availability`, HEAD
`5ada43a5773a96b1f0a97e9e6299baa6f420b762`. This is a documentation-only
qualification checkpoint. The requested Part A stop condition applies: the
existing contract does not specify an ordinary gameplay payload and arrival
envelope sufficiently to select representative large qualified messages or
distinguish qualified mixed overload from out-of-envelope gameplay demand.
Parts B–G were therefore not executed. No production defect was established.

## Legal representation versus qualified workload

The current [deployment contract](NetworkingReliableDeploymentContract.md)
specifies RPC p95/p99/max 150/250/500 ms, Event ACK and action-result maximum
250 ms, gameplay reserve >=25%, and no large-group exception. It expressly
excludes full-size requests on the unchanged 256 KiB/s Player request path,
sustained gameplay bursts, and overload/recovery from the existing official
fixture's qualification. Numeric profile compatibility is necessary capacity
arithmetic, not a qualified workload definition.

| Mechanism | Current legal representation ceiling | Existing ordinary evidence; large qualified envelope |
| --- | --- | --- |
| RemoteFunction request | 262,144-byte encoded Remote frame including its 52-byte header; at most 32 arguments and 16,384 bytes per string | Official fixture sends the 17-byte string `node-content-ping`; no declared larger qualified request ceiling |
| RemoteFunction response | Same frame, argument and string bounds | Official fixture returns the 17-byte string `node-content-pong` and numeric `os.clock()`; no declared larger qualified response ceiling |
| Reliable RemoteEvent | Same frame, argument and string bounds | Official materialization probe echoes the 21-byte string `materialization-probe` and numeric sequence; no declared larger qualified Event ceiling |
| Character action result | Fixed native `CharacterActionResult` fields, with optional authoritative action state; encoder bounded to 256 bytes, with no arbitrary gameplay argument payload | One matched accepted `PackageLunge` result per retained official run; sustained action-result rate and latency envelope not qualified |

Remote ceilings come from [RemoteProtocol.hpp](../../include/gargantuan/network/RemoteProtocol.hpp)
and [RemoteProtocol.cpp](../../src/network/RemoteProtocol.cpp). A maximum
262,092-byte argument encoding budget follows from subtracting the 52-byte
header; value tags and string lengths consume that budget. This is not permission
for a single 256 KiB string or evidence that every maximum-size value combination
is valid. Exact attainable maximum-size fixture execution is **not measured**
in this checkpoint. These sizes exclude the separate 32-byte GNS adapter envelope.
Action shape and encoder bounds come from
[CharacterProtocol.hpp](../../include/gargantuan/network/CharacterProtocol.hpp)
and [CharacterProtocol.cpp](../../src/network/CharacterProtocol.cpp).

The [official fixture](../../tests/OfficialNodeHostVertical.ps1) performs sequential
RPCs, waiting for completion and yielding before the next request. Its Event probe
is eligible every fourth simulation callback with at most one probe awaiting an
ACK. Other small lifecycle notifications are also sent. The retained near-max
run requests 100 RPCs and one action; its approximately 1 MiB payload is streamed
structural content, not a large Remote request or response. Eight churn cycles
do not mean eight connected-client pressure cycles: the remaining churn follows
the initial Player's disconnect. These facts define the measured fixture only,
not a general supported message-size or sustained-demand promise.

`GameplayBurst = 262,176` accounts for one codec-ceiling Remote plus adapter
framing. It does not specify the permitted distribution of overlapping requests,
responses, Events and actions or a qualified client-to-server burst. The
[RemoteManager limits](../../include/gargantuan/network/RemoteManager.hpp)
(including 1,024 calls/peer/s, 256 calls/Remote/s, 64 concurrent handlers/peer,
and finite manager queue/byte limits) are resource/denial ceilings, not latency
qualification at every legal combination. Neither those limits nor the 25%
reserve supply the missing ordinary arrival envelope.

## Contract needed before resuming

An accepted service-contract update must state:

1. Qualified encoded request, response and reliable Event sizes separately,
   including whether the counted bytes include Remote and adapter framing.
   Action results need an applicable supported mix/rate, not an invented variable
   payload field.
2. Per-peer and aggregate burst bytes, maximum overlapping calls/events/actions,
   sustained byte rates, measurement windows and directions. Define how gameplay
   plus control traffic shares the reserve, and what demand is out of envelope.
3. Qualified client request-path capacity and scheduling assumptions alongside
   server R/A/N, backend headroom, RTT/loss and host-service allowances. The
   existing 8 MiB/s server profile does not configure the Player's outgoing rate.
4. Which finite backpressure/terminal outcomes are acceptable when offered
   gameplay exceeds that envelope, preserving the existing reliable semantics
   and latency gates for in-envelope workloads.

No values for these missing decisions are selected here. Once accepted, the
canonical fixtures can use their upper qualified sizes while structural demand
exceeds production admission capacity, followed by aggregate fairness, recovery
and journal-margin measurement. A structural-only diagnostic could be useful,
but would not resolve Part A or establish qualified mixed-overload service.

## Bounds and evidence retained

The [deployment contract](NetworkingReliableDeploymentContract.md) documents
finite peer/global credit, one synchronous reservation, a bounded connection map,
scalar deferred-cost hints, and exposure thresholds rather than an exact total
GNS memory cap. The [3J contract](ReplicationFoundation3J.md) preserves the
8,192 structural selection cap, separate 65,536 planning bound, acceptance-only
Known and emitting journal progress, and peer-local failure if a consumer falls
beyond the 16,384-record journal window. No retention expansion, new queue,
semantic split or scheduler is proposed.

The [validation ledger](ContentAvailabilityFoundation3L_3Validation.md) retains
the observed 16,214-record startup lag and 170-entry margin, with zero failures.
That historical sample does not identify a measured oldest owner under production
overload, a journal production rate, a low-margin duration, or a safe recovery
window. Transport admission delay cannot be converted into retained-entry demand
without that evidence.

| Requested evidence | State at this checkpoint |
| --- | --- |
| Large qualified gameplay fixture | **not measured**; missing contract prevents selecting its envelope |
| Sustained single-peer, aggregate and mixed overload | **not measured** |
| Peer/global backlog, deferred groups, planning and memory high-water under overload | **not measured**; documented ceilings remain normative |
| Qualified-overload RPC/Event/action latency | **not measured** |
| Multi-peer service gaps, no-admission ticks, group ages, convergence spread | **not measured**; previous deterministic accounting is not production transport fairness |
| Drain, resumption, convergence, lifecycle and reservation recovery under overload | **not measured** |
| Journal production rate, minimum overload margin, low-margin duration, oldest owner and recovery | **not measured** |
| Ordinary official Local / Node | Prior unchanged-production evidence passes: RPC p99/max 72.058/73.316 and 74.723/75.801 ms; Event ACK max gap 100.787/83.469 ms; pending reliable peak 469,320 B; zero timeout/error/crash |
| MSVC Release / Clang ASan/UBSan/LSan | Existing evidence retained; no native source or fixture change and no new local/worker run |
| Published-HEAD CI | Native and complete GNS sanitizer workflows both independently verified terminal **success** at the inspected HEAD |

CI receipts: [Native engine CI 34724984881](https://github.com/gmoddev/gargantuan/actions/runs/34724984881)
and [GNS sanitizer CI 34724984872](https://github.com/gmoddev/gargantuan/actions/runs/34724984872).
The [profiled GNS attribution](GameSessionReliableAdmissionAttribution.md)
continues to apply; its former mandatory-deferral assertion was not a production
accounting defect. KI-007, dependency/bootstrap, acquisition/admission identity,
late-send, byte-accounting and planning/selection evidence is retained at its
recorded scope, not relabeled as new overload testing.

The proposed commit boundary is this report plus links in the current ledger
and KI-006. No commit or push is made by this checkpoint. Documentation checking
is limited to patch whitespace and local link resolution; no Astro source is
changed. Current-source security, required client/scale qualification, gameplay
envelope, sustained overload/recovery and production journal sufficiency remain
open Foundation 3L gates. No merge or Foundation 3M follows.
