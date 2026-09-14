---
status: validation-evidence
owner: runtime-networking-and-runtime-host
last_verified: 2026-09-14
---

# Foundation 3L reliable transport feedback proof validation

## Source and scope

Starting published source: `e29128950d51d669057129c5408dd2ea0c558a10` on
`foundation/3l-content-availability`. The accepted 42-case Option C pooled-service
model is treated as PASS. This task changes only test/reference-model and
architecture/validation documentation. Production `ReliableServiceProfile`,
`ReliableByteAdmission`, `GameSession`, scheduler, GNS behavior, wire protocol and
reliable ordering remain unchanged.

Implementation decision: **B — NARROW GNS ADAPTER EXTENSION REQUIRED**.
Production pooled service remains **NOT IMPLEMENTED**.

## Source-level GNS probe

Pinned GameNetworkingSockets revision:
`2cb93a06350bb065db53abdb0d87cf297e0bfd34`.

Direct inspection of pinned `steamnetworkingsockets_snp.cpp/.h` establishes:

- unique reliable stream ranges exist in pending, in-flight/sent-unacked, retry,
  or Acked state;
- first send moves a range from pending to sent-unacked;
- loss/NACK moves that same range from sent-unacked back to pending retry;
- retransmission moves it back to sent-unacked;
- ACK removes it from whichever outstanding state owns it and marks it Acked;
- the reliable message holds `m_cbHdr` and a sent-segment reference count;
- after the final Acked segment reference retires, the reliable message is
  unlinked/released;
- shutdown purges sender state and zeros pending/unacked counters.

Therefore pending alone is nonmonotonic and cannot be integrated as delivered
bytes. Pending + sent-unacked is a truthful instantaneous unique reliable-stream
outstanding quantity, but GNS private reliable headers mean that stream quantity
cannot exactly retire Gargantuan complete-message debt without a narrow message-
retirement signal.

Current upstream master was inspected at
`a424b7db649438acafb60c99cae6667587c42732`. It retains the same internal sender
state/message-retirement design; no sufficient public cumulative application-
payload ACK statistic was found. A dependency upgrade is not selected.

## Reference model execution

`tests/ReliableTransportFeedbackModelFixture.hpp` is deterministic, bounded,
network-free and registered in `gargantuan_networking_contract_tests` beside the
accepted Option C model/hardening tests.

The standalone development form was compiled as C++23 with warnings enabled and
passes **19/19** cases:

1. `UniqueFirstSendDrain`
2. `OneRetransmission`
3. `RepeatedRetransmissions`
4. `DelayedAck`
5. `DuplicateAck`
6. `AckAfterRetransmission`
7. `QueueBytesReenterPending`
8. `PeerDrainsAtFloor`
9. `PeerDrainsBelowFloor`
10. `IdlePeer`
11. `FirstGrantBootstrap`
12. `DrainedSlowPeerRequalification`
13. `StaleFeedback`
14. `MissingFeedback`
15. `ConnectionTeardown`
16. `ReconnectNewGeneration`
17. `CounterResetWrap`
18. `TransportFailure`
19. `LogicalDebtPhysicalCostSeparated`

The repeated-retransmission cases prove that physical bytes may exceed logical
created bytes while verified logical drain never exceeds the one admitted
payload. Terminal release remains separate from ACKed drain.

## Proof mapping back to Option C

The native feedback refinement does not change N=32, G=524,288 B, the 96 MiB/s
usable model envelope, 64 MiB/s structural pool, 8 MiB/s gameplay reserve,
four structural grants, 16 MiB/s active-grant floor, or 50-ms freshness bound.
It replaces only the abstract meaning of `verified service` with implementable
native observations:

- exact logical debt retirement: delta `ReliablePayloadBytesAcked`;
- sender queue-service qualification: delta `UniqueReliableStreamBytesFirstSent`
  at >=16 MiB/s over a fresh <=50-ms sample window;
- remote transport progress: positive delta `UniqueReliableStreamBytesAcked`;
- physical retransmission cost: delta `ReliableStreamBytesRetransmitted`;
- queue occupancy: current pending + sent-unacked reliable stream bytes;
- terminal reconciliation: explicit generation teardown only.

First service uses one bounded qualification grant. A drained peer that previously
failed the floor cannot regain ordinary-grant eligibility merely because debt
reaches zero; it may obtain a new qualification grant only after a one-second
per-generation cooldown. These grants consume the same existing grant/debt
bounds, so the feedback mechanism does not introduce an unbounded probe channel.

## Validation disposition

| Gate | Result |
| --- | --- |
| Option C abstract service model | **PASS**, retained 42/42 evidence |
| Transport feedback reference model | **PASS**, standalone 19/19 |
| Pinned GNS source-level semantics probe | **PASS**, exact lifecycle identified |
| Current upstream comparison | **B**, same internal capability; no sufficient public signal |
| Registered networking-contract CTest at final source | hosted CI pending/record separately |
| Real GNS implementation of new counters | **NOT IMPLEMENTED** |
| Real-GNS loss/retransmission counter probe after extension | **NOT MEASURED** |
| Physical 32-client pooled qualification | **NOT MEASURED** |
| KI-006 | **OPEN** |
| Foundation 3L | **B — PARTIALLY READY** |
| Foundation 3M | **BLOCKED / NOT STARTED** |

## Exact next task

Implement only the narrow GNS telemetry boundary first. Add checked sender
counters at existing first-send, ACK, complete-message retirement and retry-send
transitions, expose them through one private lock-safe Gargantuan bridge, wrap the
sample with generation-safe `ConnectionId` and monotonic observation time, and
prove the counters under real loss/retransmission/delayed/duplicate ACK plus
teardown/reset on Windows Release and Linux sanitizers.

Do **not** connect the counters to production pooled admission in that telemetry
task. Once the native signal itself is green, a subsequent separately authorized
task may implement `POOLED_SERVICE` admission against this contract and re-run
Option C plus actual GNS qualification.
