---
status: attribution-resolved-integration-pending
owner: runtime-networking
last_verified: 2026-09-14
related_code:
  - cmake/gns/ApplyReliableServiceFeedback.cmake
  - tests/ReliableServiceFeedbackFixture.hpp
related_adrs:
  - ../FutureArchitecture/Foundation3LServiceCoverageDecision.md
---

# Foundation 3L pooled production integration stop receipt

## Current disposition

The original aggregate-feedback insufficiency remains a valid finding. Decision B
now adds exact sender-local native message attribution in `62c663335cfd36498869fde3d8e85406f102e281`,
with compilation corrections in `a5a182ff94ae7373e0cdcdcc6232b434696926fe`.
The [qualification receipt](ReliableTransportFeedbackProof3LValidation.md#retirement-attribution-qualification-2026-09-14)
owns the current gate. Local mixed-retirement and real-GNS attribution pass,
and required Native engine and GNS sanitizer CI are terminal green on the corrected
source. **The retirement-attribution stop is resolved: READY FOR POOLED-SERVICE
INTEGRATION.** Production pooled admission remains **NOT IMPLEMENTED**.

## Preserved historical stop

The local stop checkpoint `ec4c103dd12410664e5d4d99fb18134630c27cc4` was based on
published aggregate feedback `c4c46345242a595e550f352cde04751bf81d3031`. It was not
published to the working branch. This document carries its finding forward;
the historical test/source and measurements below retain that exact scope.

Its `ReliableRetirementAttribution` fixture in `GnsServiceFairnessTests.cpp`
constructed two ordered reliable messages, each with 129 payload bytes and three
private GNS header bytes. Two legal histories retired opposite messages through
the actual pinned `SSNPSenderState::RemoveRefCountReliableSegment` implementation:

| After one retirement | Structural first | Gameplay first |
| --- | ---: | ---: |
| Unique stream first-sent | 264 | 264 |
| Unique stream ACKed | 132 | 132 |
| Aggregate payload retired | 129 | 129 |
| Retransmitted / pending stream bytes | 0 / 0 | 0 / 0 |
| Sent-unacked stream bytes | 132 | 132 |
| Actual structural payload outstanding | **0** | **129** |

Both histories have the same aggregate accounting and Connected state. Neither
send order nor timestamps recover the missing identity. Receiver delivery order
does not require FIFO sender retirement; retry references can delay retirement
independently. Subtracting every aggregate payload ACK delta from structural debt
therefore credits unrelated traffic as structural service.

Historical MSVC Release and Clang 19 ASan/UBSan/LSan probes both reproduced
`IndistinguishableClassRetirement=PASS`; the seven aggregate-feedback fixtures,
19-case feedback model and 42-case pooled model remained green. These measurements
proved insufficiency, not successful production integration. The old stop
checkpoint's own hosted CI was not measured. Its logs remain under
`C:\Sandbox\Codex\Logs\gargantuan-3l-pooled-integration`.

## Resolution and remaining work

The [native contract](ReliableTransportFeedbackProof3L.md#exact-retirement-attribution)
matches final native message retirement to its sender-local token. Gameplay or
control retirement cannot clear another message's active obligation. No wire,
pin, reliable lane or ordering change is needed. The aggregate counter remains
available for aggregate accounting; it is not a substitute for the exact receipt.

Resume production `POOLED_SERVICE` admission
integration using exact attributed structural retirement. That separate task
must attach the token, carry native receipts and terminal ownership through the
existing `ConnectionId` generation boundary, and prove the accepted model against
production admission. It must not infer those fields from the current aggregate
adapter value.

KI-006 remains **OPEN**. Foundation 3L remains **B — PARTIALLY READY**.
Physical 32-actual-client pooled qualification is **NOT MEASURED**.
Foundation 3M remains **BLOCKED / NOT STARTED**.
