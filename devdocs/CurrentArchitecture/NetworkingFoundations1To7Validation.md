---
status: current
owner: networking
last_verified: 2026-08-16
related_code:
  - include/gargantuan/network
  - src/network
  - src/scripting
  - tests/SimulatedTransportTests.cpp
  - tests/SchedulerContractTests.cpp
  - tests/ReplicationTests.cpp
  - tests/RemoteTests.cpp
  - tests/RemoteLuauTests.cpp
  - tests/GameNetworkingSocketsTransportTests.cpp
related_adrs:
  - docs/src/content/docs/developing/networking-architecture.mdx
---

# Networking Foundations 1–7 adversarial validation

## Verdict

`NETWORKING FOUNDATIONS 1–7 VALIDATED`

This checkpoint independently reviewed the composed protocol-input,
backend-neutral contract, simulator, scheduler, GNS, basic-replication, and
bounded-Remote layers. It is evidence for the Windows x64 implementation at
this revision, not a claim that deferred multiplayer systems exist.

## Ownership and authority map

| State or action | Owner | Security boundary |
| --- | --- | --- |
| connection identity and negotiated limits | host session plus backend-neutral transport contracts | backend handles never escape the adapter |
| transport delivery mechanics | `IGameTransport` implementation | carries bytes and observations, never DataModel authority |
| queued traffic admission and selection | `NetworkScheduler` | validates semantic intent and bounded queues, never interprets payloads |
| replication relevance and publication operations | `ReplicationCoordinator` and per-peer `ReplicationView` | only server-produced `ReplicationOperation` values reach the replica path |
| replica application | transactional `ReplicaApplier` | validates a complete group before mutation and has no upstream authority path |
| application messaging | `RemoteManager` | decodes, validates, admits, correlates, and dispatches application data only |
| gameplay authorization | developer server code | successful engine validation does not authorize a purchase, door, inventory slot, or other game meaning |
| script execution authority | host-created `ScriptSecurityContext` | payloads cannot encode capabilities, trust, execution domains, or mutation authority |

Replication and Remotes share scheduler and transport infrastructure but never
share semantic operations:

```text
Replication -> ReplicationCoordinator -> ReplicationOperation
            -> NetworkScheduler -> IGameTransport

Remotes     -> RemoteManager -> RemoteMessage
            -> NetworkScheduler -> IGameTransport
```

No general causal order is promised between those branches. The only
cross-branch dependency is explicit Remote Object-reference materialization.

## Mixed composition evidence

The simulator and GNS tests now exercise one real session, one production
scheduler, ordinary replicated Remote Instances, structural replication, a
reliable Remote event containing an Object reference, and a RemoteFunction
request/response together.

| Scenario | Expected invariant | Evidence |
| --- | --- | --- |
| new Object plus reliable Remote reference | publication/baseline drains before handler sees the reference | mixed simulator and localhost GNS tests |
| hidden/unpublished/destroyed reference | rejected without handler dispatch or identity resurrection | Remote adversarial tests |
| property update plus unrelated Remote | no incidental cross-channel causal promise | scheduler and mixed-composition assertions |
| unreliable message before materialization | dropped/rejected instead of waiting | Remote dependency tests |
| replication baseline under Remote traffic | structural work retains precedence; reliable application receives bounded service | scheduler eight-to-one fairness regression |
| old pending request, disconnect, reconnect, late response | old response cannot complete new request | Remote epoch and real-GNS reconnect tests |
| one peer terminates | other peer queues, requests, and sequences remain intact | multi-peer Remote tests |

The deterministic simulator covers latency, forced loss, duplication,
reordering, bandwidth pressure, congestion drop, timeout, cancellation,
disconnect, stale sequencing, reconnect, and request exhaustion. The simulator,
scheduler, replication, Remote, and Remote Luau set passed 20 consecutive runs
each. The Debug localhost GNS Remote composition passed five consecutive runs.

## Confirmed defects and remediation

The validation found five reportable cross-layer defects. All were fixed and
received focused regressions before this verdict:

1. **Luau continuation authority restoration (High).** Task and resume paths
   could resume a restricted Remote handler under ambient `CoreTrusted` state.
   Scheduled/deferred work now stores the exact `ScriptSecurityContext`; all
   relevant resumptions restore it, inbound RemoteFunction `task.wait` is
   rejected, and the shared task queue is bounded to 65,536 entries.
2. **Reentrant Remote lifecycle teardown (High).** Request completion callbacks
   could reenter peer removal while containers were being traversed. Structural
   state is now detached first and callbacks run from a separate completion
   list. Recursive `RemovePeer` completes exactly once.
3. **Stale Remote publication/session state (High).** `ObjectId` alone could not
   distinguish two peer-specific publications of the same Remote. Strong
   `RemotePublicationId` identity, Remote protocol v2, GNS envelope v2, retired
   connection-generation rejection, and publication-scoped sequence state now
   close replay and stale supersession paths.
4. **Manager-wide Remote amplification (Medium).** Per-peer limits composed to a
   larger unbounded manager workload. Aggregate call, handler, request,
   dispatch, deferred, and generated-reliable ceilings now apply before work is
   created. A 33-peer test admits exactly the 8,192-call manager budget.
5. **Lost terminal scheduler state (Medium).** A terminal reliable-backlog
   rejection could be reduced to an ordinary Remote rejection and strand an
   earlier request. `RemoteSendResult` now carries terminal disconnect data;
   `RemoteManager` removes the peer, resumes pending work exactly once, and
   notifies the host.

The scan also evaluated whether GNS `Authenticating` incorrectly represented
application authentication. This was suppressed as non-reportable because the
accepted current contract explicitly defines it as transport cryptography only,
rejects application authority in payloads, and defers host-owned join tickets.
No public host or ticket claim was introduced by this validation.

## Resource and fairness policy

The composed hard ceilings include:

| Resource | Ceiling |
| --- | ---: |
| scheduler aggregate queued reliable bytes | 256 MiB |
| consecutive structural messages while reliable application waits | 8 |
| Remote frame / arguments / string | 256 KiB / 32 / 16 KiB |
| Remote calls per peer / Remote per second | 1,024 / 256 |
| Remote calls per manager per second | 8,192 |
| incoming handlers per peer / manager | 64 / 4,096 |
| outgoing requests per manager | 8,192 |
| queued Remote dispatch and deferred work | 8,192 messages / 32 MiB |
| generated reliable terminal traffic per second | 4,096 messages / 32 MiB |
| logical broadcast admission per second | 4,096 submissions / 16 MiB |
| script task queue | 65,536 tasks |

Per-session `NetworkLimits` continue to bound message sizes, unreliable
datagrams, per-connection reliable queues, in-flight requests, and per-tick
decode/send/receive work. Aggregate scheduler exhaustion is terminal for the
submitting connection and releases its charge. Unreliable work is dropped or
superseded rather than accumulated.

## Protocol and lifecycle results

Remote protocol v2 has a fixed 52-byte language-neutral header carrying
`ObjectId`, `RemotePublicationId`, `RemoteRequestId`, and the independent Remote
event sequence. The private GNS envelope is v2 and 32 bytes; the conservative
unreliable application ceiling is 1,168 bytes. Unknown versions and malformed
publication metadata fail closed.

On unpublish, destroy, disconnect, or a new session epoch, pending requests
terminate, queued/deferred state is removed, rate and sequence state is cleared,
and stale frames cannot resurrect a Remote or resume a new coroutine. Reliable
visible references may wait only on an explicit bounded materialization
dependency. Hidden references reveal no authoritative identity. Unreliable
references never wait for publication.

## Verification record

Verified on Windows x64 with MSVC Debug and Release builds:

- native CTest: 12 of 12 passed in Debug and 12 of 12 passed in Release;
- Foundation, PreRun, physics backend, networking contracts, simulator,
  scheduler, serialization benchmark smoke, replication, Remote, and Remote
  Luau tests passed;
- localhost GNS: real transport and mixed Remote transport passed in Debug and
  Release, two of two in each configuration;
- malformed Remote corpus, rate/abuse, lifecycle, reentrancy, publication,
  request timeout/cancellation/disconnect, and multi-peer isolation passed;
- `git diff --check` passed; and
- `vendor/sdl_image` and its nested zlib checkout remained clean.

The complete renderer `all` target was not used as networking evidence: its
existing `matc` shader invocation rejects the current raw GLSL command line.
All supported headless networking targets build independently, and physics and
renderer backend isolation tests passed. No sanitizer preset was available.

## Performance sanity

The Release Remote benchmark measured 160,000 scheduler submissions and
10,920,000 output bytes:

| Work | Measured time |
| --- | ---: |
| codec encode | 0.617312 microseconds/message |
| codec decode | 0.196664 microseconds/message |
| reliable admission | 0.737170 microseconds/message |
| unreliable admission | 0.733084 microseconds/message |
| sequenced admission | 0.741006 microseconds/message |
| request start/response/dispatch | 1.497760 microseconds/request |

The Release 1,000-Object baseline benchmark produced 81,700 bytes and measured
1.7248 ms generation, 0.2729 ms encode, 0.5350 ms decode, 0.2358 ms scheduler/
transport, and 3.0040 ms apply. These measurements are sanity evidence from one
local run, not a throughput guarantee, and no limit was weakened for them.

## Security checkpoint

Codex Security scan `948ce3ff-848d-4daa-8888-6a18b40d32d4` reviewed the Remote
codec, `RemoteManager`, Luau/native bridge, request lifecycle, scheduler
integration, `ReplicationView` reference boundary, and GNS session envelope.
It recorded five validated findings at the pre-remediation revision: three High
and two Medium. All five were remediated and their affected surfaces rerun. The
sealed report has complete scoped coverage, no deferred finding, and no known
unresolved reportable defect in the changed attack surface.

## Deliberately deferred

This checkpoint does not implement a complete `Players` service, matchmaking,
Node integration, Studio play mode, realtime transform replication, physics
ownership, interpolation, prediction, rollback, streaming/interest management,
voice, HTTP/service networking, or backend service messaging.

Subsequent Character / Animation Foundation 3B implements the narrow
Character realtime/prediction slice as a sibling protocol. That later evidence
is recorded in `CharacterNetworkingFoundation.md`; it does not retroactively
expand this Foundations 1–7 checkpoint into general realtime physics or
ownership.

The next milestone is `PLATFORM FOUNDATION 1 — HOST / INPUT EVENT BOUNDARY`.
