---
status: selected-native-feedback-contract
owner: runtime-networking-and-runtime-host
last_verified: 2026-09-14
related_code:
  - src/network/GameNetworkingSocketsTransport.cpp
  - src/network/ReliableServiceFeedback.hpp
  - cmake/gns/ApplyReliableServiceFeedback.cmake
  - tests/ReliableServiceFeedbackFixture.hpp
  - tests/ReliableTransportFeedbackModelFixture.hpp
  - tests/NetworkingContractsTests.cpp
related_adrs:
  - ../FutureArchitecture/Foundation3LServiceCoverageDecision.md
---

# Foundation 3L reliable transport feedback proof

## Verdict

**READY FOR POOLED-SERVICE INTEGRATION:** native feedback and exact sender-local
retirement attribution are implemented and qualified at `a5a182ff9`. The
[receipt](ReliableTransportFeedbackProof3LValidation.md#retirement-attribution-qualification-2026-09-14)
records terminal-green MSVC, Linux sanitizer and GNS workflow results.

The accepted Option C pooled-service model remains valid as an abstract service proof. Upstream public GNS status alone cannot supply its exact drain/debt evidence. The pinned integration now observes existing native first-send, retry, ACK and final-message retirement transitions and exposes a private generation-safe snapshot. This does not implement production pooled admission. Foundation 3L remains **B — PARTIALLY READY**, KI-006 **OPEN**, and 3M blocked.

The implemented boundary preserves GNS send, ACK, retransmission, ordering, queue and congestion behavior. No dependency upgrade, wire change, second reliable lane or application ACK is introduced.

## Implemented private boundary

### Exact retirement attribution

Decision B retains the narrow private GNS integration. Connection-wide
`ReliablePayloadBytesAcked` is truthful aggregate retirement, but cannot identify
structural versus gameplay/control retirement. Reliable delivery order does not
imply sender retirement order. The [integration stop receipt](PooledReliableServiceIntegration3L.md)
preserves the counterexample; aggregate ACK deltas must never pay structural debt.

The private `GargantuanBeginReliableRetirementAttribution(Token)` /
`GargantuanEndReliableRetirementAttribution()` scope marks exactly one synchronous
reliable submission on the calling thread. In the existing SNP send path,
immediately after assigning `m_nMessageNumber`, the reliable branch consumes the
token and binds it to that native message number in the connection's sender
state. The token is never attached to a packet or serialized. GNS's existing
message number supplies identity; the Gargantuan token supplies sender-local
obligation correlation. The current single reliable lane/order domain is unchanged.

At `RemoveRefCountReliableSegment`'s final ACKed message reference, before
unlink/release, `AckMessage(MessageNumber, MessageBytes, PrivateHeaderBytes)`
updates the aggregate counter. Only an exact active-message match advances
`AttributedRetirementSequence` and publishes `LastAttributedRetirementToken`,
`LastAttributedRetirementMessageNumber`, and `LastAttributedRetiredPayloadBytes`.
It then clears the active token/message pair. Unrelated gameplay/control
retirements cannot change that receipt. Retry references can delay final release;
retry bytes remain physical cost, never a second logical retirement.

The fixed storage permits **one active attributed message per native connection**
and one last-retired receipt. It adds six `uint64_t` fields (48 bytes) and one
thread-local pending token, with no per-message map, packet history, queue or
allocation. Successive tokens must be nonzero and strictly increase within a
sender lifetime. Zero/negative native message identities, concurrent obligations,
token reuse/regression, sequence exhaustion and invalid byte arithmetic set
sticky invalid feedback. Zero/nested Begin calls reject. Callers must always end
the scope, including rejected submissions, and consume a receipt before granting
another obligation; the receipt is not an event backlog.

Shutdown marks `Purged` without advancing verified retirement or clearing the
unretired active identity. That identity describes remaining ownership for
terminal release, not ACK success. A new native sender starts with zero state.
The outer lifecycle key remains the full adapter `ConnectionId` generation;
message numbers or tokens alone must never identify a replacement connection.

The current production `ReliableServiceFeedback` adapter value still exposes
aggregate fields only. Exact fields are available in the private native
snapshot and exercised by the native fixture. The next pooled integration must
bind its accepted structural obligation at submission, preserve exact receipt
and terminal identity through the existing generation-safe adapter boundary,
and consume each matching receipt once. This task does not implement that
production policy or claim that the aggregate adapter value already carries it.

### Aggregate observation and capture

`cmake/gns/ApplyReliableServiceFeedback.cmake` verifies normalized source hashes at
the exact pin and applies idempotent hooks. Unknown edits fail configuration.
The hooks observe `SSNPSenderState` at these existing ownership points:

| Native point | Observation |
| --- | --- |
| First-send branch allocating an in-flight reliable segment | Add its unique stream range once to `UniqueReliableStreamBytesFirstSent` |
| Retry branch reusing an existing segment | Add physical retry bytes only to `ReliableStreamBytesRetransmitted` |
| Segment ACK processing before its existing state update | Add to `UniqueReliableStreamBytesAcked` only if it was not already Acked |
| `RemoveRefCountReliableSegment`, final Acked segment reference, immediately before message unlink/release | Add `m_cbSize - ReliableSendInfo().m_cbHdr` once to `ReliablePayloadBytesAcked` |
| `SSNPSenderState::Shutdown` | Mark sender ownership purged; retain cumulative counters and never create ACK progress |

The final-reference point may occur after the segment's first ACK when retry
packet references still exist. That delay is conservative: partial ACK and
duplicate ACK cannot retire the message early. The payload excludes only GNS's
private reliable header, retaining Gargantuan's complete 32-byte GGNS envelope.

All four counters use checked `uint64_t` increments. Overflow, negative native
ranges or ACK progress exceeding unique first-send sets sticky invalid feedback;
the bridge returns unavailable. Invalid counters cannot wrap or appear as valid
saturation. Only construction of a new native sender starts a zero baseline.

The bridge obtains GNS's existing per-connection lock through its existing API
handle lookup and copies all counters, pending, sent-unacked and native state
together, including a steady-clock microsecond timestamp taken while the native
lock still protects the copied values. A delayed adapter return cannot re-date
an older observation. It introduces no global lock. Gargantuan's existing adapter
ownership mutex serializes identity lookup and wrapping with `ConnectionId`;
the wrapper preserves the captured timestamp. Raw GNS handles never leave the
private bridge.

No-linger close captures the post-purge snapshot inside the existing native close
operation, using its existing lock order. One terminal snapshot per adapter slot
survives release with the old `ConnectionId`; slot reuse clears it and advances
the generation. This bounded snapshot is terminal evidence, not an admission
debt-release policy. If native feedback is invalid, no valid sample is exposed.

Owned checked arithmetic and snapshot/capture code compile in separate object
targets with full sanitizer instrumentation. Only existing GNS translation units
retain the established upstream function/alignment compatibility exclusions.
The adapter/private type is neither gameplay, Luau, wire nor creator-facing API.

## Pinned source and reliable-byte lifecycle

Gargantuan pins GameNetworkingSockets at `2cb93a06350bb065db53abdb0d87cf297e0bfd34` through `cmake/GameNetworkingSockets.cmake`.

One Gargantuan reliable send follows this source-verified lifecycle:

```text
Gargantuan payload
    -> 32-byte GGNS adapter envelope
    -> GNS CSteamNetworkingMessage
    -> GNS reliable-message header prepended to the reliable stream
    -> queued/pending reliable stream bytes
    -> unique stream segment first transmitted: pending -> sent-unacked
    -> packet loss/NACK: sent-unacked -> pending retry
    -> retransmit: pending retry -> sent-unacked
    -> ACK: owning stream segment becomes Acked and leaves pending/unacked ownership
    -> final ACKed segment reference retires/unlinks/releases the reliable message
    -> connection shutdown purges any remainder
```

Pinned `steamnetworkingsockets_snp.cpp` verifies that `m_cbPendingReliable` and `m_cbSentUnackedReliable` describe mutually exclusive ownership of unique reliable-stream bytes. First transmission moves bytes from pending to sent-unacked. A retry moves the same bytes back from sent-unacked to pending. ACK processing subtracts the segment from whichever state owns it and marks that segment Acked. Therefore:

```text
m_cbPendingReliable + m_cbSentUnackedReliable
```

is a useful instantaneous unique reliable-stream outstanding quantity. `m_cbPendingReliable` alone is not a drain counter.

GNS reliable stream bytes are not identical to Gargantuan application debt. GNS prepends its own per-message reliable header and increases the message byte size before assigning stream positions. The internal reliable message retains `ReliableSendInfo::m_cbHdr`, the original message object and its sent-segment reference count. When the last ACKed reliable-segment reference is removed, GNS still has enough information to retire exactly the original message payload as:

```text
GnsMessageBytes - ReliableSendInfo.m_cbHdr
```

For Gargantuan this payload already includes the 32-byte GGNS adapter envelope and therefore matches the complete-message byte unit charged by reliable-service admission.

## Current adapter telemetry classification

The existing adapter exposes or records these quantities:

| Existing quantity | Actual meaning | Suitable for logical debt retirement? |
| --- | --- | --- |
| adapter `BytesSent` | Gargantuan payload bytes accepted by GNS `SendMessage...` | **No.** Submission/acceptance only. |
| `m_cbPendingReliable` | reliable-stream bytes waiting to be sent/retried | **No.** Retries can increase it. |
| `m_cbSentUnackedReliable` | unique reliable-stream bytes currently sent but awaiting ACK | Not alone. Useful with pending as instantaneous outstanding state. |
| `m_nSendRateBytesPerSecond` | GNS rate/congestion estimate subject to configured min/max clamps | **No.** Explicit profiles set min=max, so this may report configured policy. |
| `m_flOutBytesPerSec` | recent physical output observation | No. Includes transport behavior and is not unique application retirement. |
| RTT/loss estimates | path-health observations | Supporting diagnostics only. |

The current explicit Gargantuan profile sets GNS SendRateMin and SendRateMax to the configured backend rate. Consequently `m_nSendRateBytesPerSecond >= 16 MiB/s` cannot prove that a peer actually received or ACKed 16 MiB/s of unique reliable service.

Likewise, summing positive decreases of pending reliable bytes is unsound: one stream range can leave pending on first send, re-enter pending after loss, then leave pending again on retransmission. The same logical bytes can therefore generate multiple positive pending decreases.

## Smallest truthful native extension

The four aggregate counters below remain implemented and useful. Exact class
attribution additionally requires the fixed identity fields described above;
these aggregate values alone are not structural retirement evidence:

```text
UniqueReliableStreamBytesFirstSent
UniqueReliableStreamBytesAcked
ReliablePayloadBytesAcked
ReliableStreamBytesRetransmitted
```

Semantics:

- `UniqueReliableStreamBytesFirstSent` advances only when a reliable stream range first transitions from pending to in-flight. Retry sends do not advance it.
- `UniqueReliableStreamBytesAcked` advances exactly once when a unique reliable stream segment becomes Acked, whether the successful ACK followed first send or retransmission.
- `ReliablePayloadBytesAcked` advances once when the final reliable segment reference for a message is ACKed and the message is retired. The increment is that message's complete payload bytes excluding only the private GNS reliable-stream header.
- `ReliableStreamBytesRetransmitted` advances for actual retry stream bytes transmitted. It is physical-cost evidence and never retires logical application debt.

All increments use checked arithmetic. Overflow is terminal/invalid feedback, never wrap.

The Gargantuan adapter combines those counters with existing instantaneous status and generation-safe identity:

```cpp
struct ReliableServiceFeedback {
    ConnectionId Connection;
    std::uint64_t ObservedAtMicroseconds;
    std::uint64_t UniqueReliableStreamBytesFirstSent;
    std::uint64_t UniqueReliableStreamBytesAcked;
    std::uint64_t ReliablePayloadBytesAcked;
    std::uint64_t ReliableStreamBytesRetransmitted;
    std::uint64_t PendingReliableStreamBytes;
    std::uint64_t SentUnackedReliableStreamBytes;
    ConnectionState State;
};
```

No raw GNS handle is a service identity. The adapter resolves the live handle through its existing `ConnectionId` generation mapping and stamps one monotonic observation time. Missing bridge/status data returns unavailable feedback rather than fabricated zero.

`ReliablePayloadBytesAccepted` need not be added to this feedback object: Gargantuan's existing synchronous reservation/scheduler-acceptance path is already the authoritative source of created structural debt. Adding another accepted-byte counter would duplicate ownership rather than strengthen conservation.

## Logical debt versus physical transport cost

Option C must keep two ledgers:

```text
Logical structural debt
    = complete Gargantuan reliable bytes accepted for structural service
      - payload bytes from matching, previously unconsumed attributed retirements
      - valid terminal release

Physical transport cost
    includes unique first-send stream bytes
           + retransmitted stream bytes
           + packet/encryption/ACK/protocol overhead not represented by logical debt
```

The exact conservation equation remains:

```text
CreatedLogicalDebt
= VerifiedPayloadAcked
+ TerminalReleased
+ OutstandingLogicalDebt
```

Retransmission can make physical cost exceed created logical debt by any bounded amount allowed by transport policy, but it cannot make `ReliablePayloadBytesAcked` advance twice for one message. Transport reserve and host/path qualification cover that physical cost; retransmission bytes never masquerade as successful logical drain.

## Slow-peer qualification

The [isolated production integration checkpoint](PooledReliableServiceIntegration3L.md#implemented-checkpoint-ownership)
propagates the exact receipt through queued intent metadata and retains each
closed native generation until GameSession consumes terminal evidence. That
checkpoint is stopped on overload recovery; native attribution's prior
qualification does not qualify the complete pooled production path.

The 16 MiB/s Option C `PeerDrainFloor` describes the sender-side queue serialization floor needed for the same-FIFO blocking proof. Whole-message ACK completion time is too conservative for this purpose because RTT/ACK delay is part of the accepted nonqueue/path allowance, and ACK-only burst rate can be distorted by delayed ACK arrival.

An ordinary new structural grant therefore requires two fresh samples for the same `ConnectionId` generation, separated by at most the accepted **50 ms** feedback window, with checked monotonic counters and:

```text
Delta(UniqueReliableStreamBytesFirstSent)
    >= ceil(16 MiB/s * DeltaTime)

and

Delta(UniqueReliableStreamBytesAcked) > 0
```

while the peer has no previous outstanding structural grant debt. The first condition proves actual local/backend unique serialization rather than configured rate. The second proves that the remote transport is making ACK progress rather than merely accepting bytes into an unacknowledged black hole. Exact structural debt is retired only by a matching attributed message receipt, never by aggregate payload ACKs or either qualification counter.

RTT, pending/unacked state and retransmission deltas remain supporting health/cost diagnostics. They can make a profile unqualified but cannot manufacture positive verified service.

An idle peer is not classified slow merely because both deltas are zero. It simply has no current ordinary-grant qualification evidence.

## First grant and drained-slow-peer requalification

Requiring service history before first service is circular. Resolve that with a bounded **qualification grant** rather than pretending the peer is already qualified:

- one complete group at most `G`;
- consumes one of the same four aggregate structural drain-grant slots;
- obeys the same FIFO, aggregate debt, credit, pending and generation limits;
- no second qualification grant while any old debt is outstanding;
- the first live generation may receive one qualification grant immediately after fresh backend/connection health and the normal admission checks;
- successful first-send-floor plus ACK progress enables ordinary grants.

If a peer previously drains below the floor, reaching zero debt does **not** restore ordinary eligibility. The selected proof contract permits another qualification grant only after a **1 second** per-generation cooldown, under the same bounded one-G/one-slot rules. This is a requalification probe, not ordinary service. A repeatedly slow peer therefore cannot mint unrestricted drain capacity or monopolize more than one bounded grant at a time.

A future production implementation may choose a longer cooldown, but shortening it below this proof value requires revalidation.

## Freshness, generation and contradictory feedback

Freshness applies to the complete atomic feedback snapshot. The two samples used for qualification must:

- carry the same live generation-safe `ConnectionId`;
- be observed monotonically and no more than 50 ms apart;
- have nondecreasing cumulative counters;
- come from a connected backend state with available instantaneous status.

Reconnect creates a fresh `ConnectionId` generation, zero baseline, no inherited qualification, no inherited ACK history and no inherited cooldown advantage. Backend handle reuse is irrelevant unless it resolves to the current generation.

Same-generation counter decrease/reset, timestamp reversal, impossible arithmetic, missing source data or bridge/status disagreement marks feedback contradictory. New structural grants stop. Outstanding logical debt is retained until truthful ACK retirement or terminal reconciliation.

Counters never intentionally wrap. Checked `uint64_t` exhaustion is a terminal feedback failure. A legitimate reset occurs only with a new connection generation.

## Terminal release

Terminal release is not successful drain. Outstanding logical debt may be released only when the owning connection generation is irreversibly torn down and GNS/adapter ownership has definitively abandoned/purged the remaining reliable work. A transient closing state or elapsed timeout does not itself erase debt.

Diagnostics must retain separate cumulative values for `VerifiedPayloadAcked` and `TerminalReleased` so a failed connection cannot appear as successful service.

## Application-level ACK decision

No Gargantuan wire/application ACK is required for this boundary. GNS transport ACK proves the property Option C needs: the remote transport has acknowledged the reliable bytes and GNS can retire its retransmission responsibility. Semantic application consumption is a later layer and is already measured by existing gameplay/client qualification where needed. Adding application ACK messages would unnecessarily change the wire protocol and ordering workload.

## Trust boundary

All positive service counters are local backend telemetry. A remote peer can influence ACK timing, loss, queue depth and connection lifetime, but that influence can only delay ACK progress, increase retransmission cost, retain existing debt or reduce future grants. It cannot increment the local first-send/ACK/payload-retirement counters except through actual local send/transport ACK processing.

## Executable feedback model

`tests/ReliableTransportFeedbackModelFixture.hpp` is registered in `gargantuan_networking_contract_tests`. It is deterministic and socket-free. It models pending/in-flight/retry/ACK transitions, unique first-send and ACK counters, exact payload retirement, retransmission physical cost, terminal release, generation reset, feedback qualification and bounded qualification grants.

The matrix covers:

1. unique first-send drain;
2. one retransmission;
3. repeated retransmissions;
4. delayed ACK;
5. duplicate ACK;
6. ACK after retransmission;
7. queue bytes re-enter pending;
8. peer drains at floor;
9. peer drains below floor;
10. idle peer;
11. first-grant bootstrap;
12. drained-slow-peer requalification;
13. stale feedback;
14. missing feedback;
15. connection teardown;
16. reconnect/new generation;
17. counter reset/wrap/regression;
18. transport failure;
19. logical debt versus physical cost separation.

The model asserts that retransmission may increase physical bytes while `LogicalDrained <= LogicalCreated` and the conservation equation remains exact.

## Pinned-source probe and current upstream comparison

Pinned-source inspection establishes the transitions above directly in `steamnetworkingsockets_snp.cpp` and `steamnetworkingsockets_snp.h`: pending/unacked ownership, retry migration, ACK state, per-message reliable header size, sent-segment reference count and final message retirement all already exist internally.

Current upstream master inspected at `a424b7db649438acafb60c99cae6667587c42732` retains the same sender-state/message-retirement design. Its public realtime status still exposes pending/unacked/rate-style fields rather than a cumulative application-payload-ACKed counter. Therefore a dependency upgrade does not remove the need for the narrow integration signal.

Classification: **B**, not C. The capability exists internally in both revisions; the missing piece is a narrow truthful exposure, not a broad dependency redesign.

## Production implementation boundary

The narrow native boundary is implemented in this checkpoint. Its release gate
is the real-GNS, native-transition, model, sanitizer and source-specific CI evidence
in the [validation receipt](ReliableTransportFeedbackProof3LValidation.md).
Only after those gates are green may a separately authorized task connect it to
production `POOLED_SERVICE` admission and re-run the accepted Option C service
proof against real GameSession/GNS feedback.

KI-006 remains **OPEN**. Production pooled service remains **NOT IMPLEMENTED**. Foundation 3L remains **B — PARTIALLY READY**. No 3M.
