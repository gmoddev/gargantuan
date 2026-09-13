---
status: qualified-simulator-workload
owner: runtime-networking
last_verified: 2026-09-13
---

# Foundation 3L recipient-service workload

## Pre-run arithmetic

This fixture derives a smaller combined mix from the unchanged
[accepted contract](ReliableGameplayWorkloadContract3L.md). It does not replace
the upper-size, concurrent, overload or physical GNS cases already qualified.
The historical `--content-differential ... 50 5 200` remains unchanged unless
the explicit test-only `GARGANTUAN_QUALIFIED_SCALE=1` switch is present.

The switch requires exactly 32 or 200 connections, eight active Characters,
eight-recipient neighborhoods, and one ordinary Remote/action producer. All
eight Characters have looping authoritative root-motion tracks. Neighborhoods
start at x=2048, separate from the shared content site at x=0. Trusted relevance
includes the local neighborhood and content site. Normal relevance and accepted
materialization determine recipients; no Desired/Known membership is injected.
At 32 peers there are two Characters per neighborhood; at 200, eight populated
neighborhoods among 25. Each Character has eight recipients, including its owner.

| Traffic | Offered maximum and fanout | Complete-message allowance |
| --- | --- | --- |
| Ordinary Character state | 8 producers, 20 Hz full-rate upper cadence per relationship, 8 recipients each: 1,280 recipient states/s. Staggered recipient phases can require a fresh snapshot each tick: up to 480 distinct authoritative states/s | 74 B compact; up to 146 B with active action, plus 34 B frame header and 32 B adapter: conservative 212 B per unbatched delivery |
| Semantic promotion | Up to 60 Hz while promoted; conservative continuous upper bound 480 unique states/s, 3,840 deliveries/s | At most 814,080 B/s aggregate; 32-peer neighborhood at most 25,440 B/s/recipient (two Characters); 200-peer populated neighborhood 12,720 B/s/recipient |
| Owner input | Seven protocol owners every five ticks plus one live client, at most 60 Hz each as conservative bound | 60 B payload plus 32 B allowance: <=44,160 B/s aggregate; unreliable, outside reliable buckets |
| Event and matched ACK | One producer every four callbacks, <=15/s; fanout one each direction | <=80 B frame plus 32 B allowance: <=1,680 B/s/direction |
| RPC request/response | One sequential producer, 100 calls/phase, wait >=100 ms after completion: <=10/s; one outstanding | <=80 B frame plus 32 B: <=1,120 B/s/direction |
| Action request/result | One owner every 120 callbacks, <=0.5/s, one pending | <=256 B frame plus 32 B: <=144 B/s/direction |
| Forced action state | Budget four semantic state publications/action, each to eight recipients: <=16 deliveries/s | <=212 B/delivery: <=3,392 B/s server egress |

Callbacks are paced at least 16.667 ms; work overruns lower offered rates.
The four-publication action allowance covers start/result/end/terminal refresh;
the trace must verify actual forced traffic rather than assume that bound.
No repeated bind or connection churn is offered during the measured phases.
Bootstrap and initial spectator retirement precede those phases.

Reliable totals: ingress <=25.5 messages/s and 2,944 B/s; egress <=41.5
messages/s and 6,336 B/s including every forced recipient. The busiest recipient
is <=27.5 messages/s and 3,368 B/s. Conservatively bunch all four semantic
publications with one Event, one RPC and one action: 35 global messages/7,296 B,
seven peer messages/1,072 B. At each phase boundary, repositioning the eight
roots can also force a publication to all 64 relationships: allow another
64 messages/13,568 B globally and two messages/424 B to a 32-peer recipient.
The combined 99-message/20,864-B global and nine-message/1,496-B peer burst
still fits. Phase boundaries are more than 13 seconds apart. These are below 64 messages/s +16 burst and
32,768 B/s +20,480 B burst per peer/direction; at N=32, below 512 messages/s
+128 burst and 262,144 B/s +163,840 B burst; at N=200, below 1,024 messages/s
+256 burst and 524,288 B/s +327,680 B burst. RPC concurrency one is below four
per peer/direction and 64 manager-wide. The one ordinary producer is below
eight (N=32) and 16 (N=200). The 16,384-B encoded-frame ceiling is unchanged.
The fixture audits actual all-interval demand, not just these average bounds.

Unreliable input/state still consume host and path capacity. Even continuous
promotion plus the reliable mix is <865 KiB/s aggregate before structural work.
The configured simulator backend is 16 MiB/s per connection. The existing
candidate admission profile uses R=8 MiB/s, A=N*R, S=0.75, 25% gameplay reserve,
and backend ceiling 2R. N=32 means A=256 MiB/s; N=200 means A=1,600 MiB/s.
These are **simulator reservation assumptions, not funded physical capacity**.
The simulator does not prove a NIC or WAN service lower bound. No 500-peer run
or physical 200-client support claim is authorized by this arithmetic.

Structural demand is one shared 512-object/273,032-B provider unit, replicated
to every peer through normal load, steady resident, eviction and reload. Keep
65,536 planning work, 8,192 selection work, 524,288-B complete-group credit,
Qp=1,048,640 and Qg=524,288+524,352*N; no bounds are increased. Local and Node
use the same engine fixture; the Node source remains untouched.

## Measurement contract

Raw observation gap and authoritative tick gap remain historical diagnostics.
Due service begins at FrameBegin for the authoritative desired due tick, then
joins production, accepted state and observation by full recipient/ObjectId,
control epoch, authoritative tick and state sequence. Future scheduled ticks
are not latency. Importance refresh may supersede a forecast before wheel
discovery; it is counted as `forecastRescheduled`. After `CharacterDue`
confirms discovery, rescheduling alone never counts as service. Explicit GRPL
unpublish/destroy retires that full recipient/ObjectId relationship, with a
separate count of confirmed due work retired. A different generation cannot
cancel it. Unchanged suppression is counted separately. Missing accepted
or observed production, overdue scheduled relationships and unavailable origins
invalidate completeness; changing the metric cannot erase those failures.
Forced reliable states are included in traffic accounting and require separate
acceptance/observation conservation from ordinary cadence samples.
Their separate built-to-observation distribution starts at actual forced state
construction; it is not mislabeled as a cadence-due sample.

The bounded trace is analyzed after measured phases, with a 4,194,304-record
cap, full overflow/decode accounting and no payload retention. Normative RPC
p95/p99/max 150/250/500 ms and Event ACK/action 250 ms remain unchanged.
Due-service percentiles describe these measured runs, not a new universal
Character latency guarantee. Rendered/GPU visibility is not measured.

The unchanged historical `phaseHealthy` result still includes tick and raw-gap
guards. The qualified mode separately gates normative Remote/action service,
zero Character scheduler rejection, complete due/accepted-state accounting and
all-window workload budgets. A diagnostic raw-gap failure remains visible and
does not become a claimed production starvation failure by itself.

## Reproduction

Build the existing `gargantuan_game_session_benchmark` target (or the unchanged
Node integration target `gargantuan_node_content_scale`). Set
`GARGANTUAN_CONTENT_TRACE=1` and `GARGANTUAN_QUALIFIED_SCALE=1`, then run:

```text
gargantuan_game_session_benchmark --content-differential <512-object-package> local 8 8 32
gargantuan_game_session_benchmark --content-differential <512-object-package> local 8 8 200
```

For Node, retain the existing sibling's `TestContentDifferentialRealTLS/node`
driver and set `GARGANTUAN_CONTENT_DIFFERENTIAL_ACTIVE_PEERS=8`,
`GARGANTUAN_CONTENT_DIFFERENTIAL_NEIGHBORHOOD=8`, and
`GARGANTUAN_CONTENT_DIFFERENTIAL_PEERS=32` or `200`, plus its existing executable
paths. The driver inherits the qualified switch; no sibling source edit is needed.
For the historical comparison remove that switch, set
`GARGANTUAN_DUE_SERVICE=1`, and retain arguments `50 5 200` for both providers.
Its exit 1 remains expected and must not be recorded as a qualified pass.

Only one fixture runs at a time. Native compilation and Linux sanitizer work
run separately from timed qualification. Archive all result/phase lines, not
only successful RPC summaries. The qualified log must contain
`[Content:ServiceQualification] complete=1`, healthy normative service for
every phase, and explicit zero transient ownership after shutdown.
