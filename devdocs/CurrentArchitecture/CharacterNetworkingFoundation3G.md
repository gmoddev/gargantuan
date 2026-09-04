---
status: current
owner: networking
last_verified: 2026-09-04
related_code:
  - include/gargantuan/network/CharacterNetwork.hpp
  - include/gargantuan/network/GameSession.hpp
  - src/network/CharacterNetwork.cpp
  - src/network/GameSession.cpp
  - tests/CharacterNetworkingTests.cpp
  - tests/CharacterNetworkingBenchmark.cpp
related_adrs:
  - docs/src/content/docs/developing/networking-architecture.mdx
---

# Character networking foundation 3G

Foundation 3G bounds server work for recurring, replaceable Character state. It
selects which already-materialized, nominally due peer-Character relationships
may enter GCHR v4 assembly on a simulation tick. Selection is deterministic,
peer-specific, age-aware, and newest-wins.

The ownership boundaries remain separate:

- 3E alone owns relevance and materialization;
- 3F owns the desired Full/Reduced/Low cadence;
- 3G owns finite pre-assembly service of due replaceable state;
- Character, movement, action, root-motion, physics, and Luau policy remain
  authoritative simulation concerns; and
- `NetworkScheduler` still owns admitted-message lane, queue, packet, and
  transport bounds.

Deferral never unmaterializes a Character, changes its tier, reduces simulation
work, alters input freshness, or queues historical transforms. No GCHR schema,
protocol version, client message, ordinary Luau API, Node service, Studio API,
MCP surface, or required telemetry dependency was added.

Foundation 3H now provides server-private spatial candidates upstream of 3E.
It does not change 3G budgets, wheel membership, peer rotation, age, deadlines,
accepted-publication commit, GCHR batching, or reliable semantic bypass. Region
movement affects 3G only if 3E commits a real materialization transition.

## Configuration and selected unit

`CharacterNetworkConfiguration` owns three trusted, construction-time native
values:

| Value | Default | Valid range | Unit |
| --- | ---: | ---: | --- |
| `MaximumPublicationStatesPerPeerTick` | 512 | 1-4096 | selected recurring states per peer per 60 Hz simulation tick |
| `MaximumPublicationStatesPerTick` | 8192 | 1-65536 | selected recurring states over the server per simulation tick |
| `PublicationPeerQuantum` | 510 | 1-per-peer limit | states offered to one peer before rotation |

Invalid zero, overflow-sized, per-peer-over-global, or contradictory quantum
values reject manager construction. Tests use smaller valid values to create
deterministic overload; there is no runtime/client mutation route and no unsafe
production `unlimited` flag. A test-equivalent sufficient budget is obtained
with the documented hard maximums.

State count is the deterministic work unit. A compact state is 74 bytes without
the bounded action payload, while reliable action transitions already bypass
ordinary scheduling. Counting states bounds the snapshot, assembly, ordering,
and encoding work that dominated the 3F profile without encoding a state merely
to decide whether it fits. The negotiated byte limit, 1200-byte default
datagram, fixed frame array, and current maximum of 15 ordinary compact states
remain authoritative final bounds. The 510-state default quantum is divisible
by 15, preserves dense batch boundaries, and still rotates peers under global
pressure.

The defaults were calibrated above the measured healthy peaks. The 500-peer,
50-relevant production-shape fixture peaked at 4,141 selected relationships per
tick globally, while its per-peer set is only 50. The default therefore produces
zero budget deferrals in that fixture and in the 32-player/500-Character
representative fixture, but prevents an unbounded pathological assembly pass.

## Intrusive timing wheel

Every materialized relationship owns exactly one ordinary scheduling position:

```text
LastAcceptedPublicationTick
DesiredDueTick
HardDeadlineTick
effective 3F tier
queue kind / wheel slot
previous and next ObjectId
forced/deadline flags
```

Each peer owns a 64-slot fixed wheel and five intrusive lists: forced, owner,
FullRate, ReducedRate, and LowRate. Wheel modulo chooses storage only; the full
64-bit absolute due tick determines readiness. Links are generation-safe
`ObjectId` values resolved back through the peer's ordered relationship map, not
raw pointers. There is no heap node, encoded frame, revision history, bandwidth
debt, or tombstone per reschedule.

Normal accepted publication returns to the next canonical FNV phase after the
current tick. The absolute heartbeat deadline is the earlier of that phase and
`LastAbsoluteTick + 60`, so stationary suppression retains the exact heartbeat
bound. A delayed relationship remains due until accepted; missed samples are
never replayed. Recovery publishes the latest authoritative state and then
returns to its stable phase, avoiding permanent phase synchronization.

On a forward jump smaller than the wheel, only crossed slots are visited. A
backward tick or jump of at least 64 ticks scans the 64 slots once and validates
absolute deadlines. It never iterates once per skipped tick. Tests cover forward
jump, backward tick, slot reuse, and debugger-sized pause behavior.

## Due and priority policy

An accepted state establishes the next desired phase. Its escalation deadline
is one additional effective-tier interval after `DesiredDueTick`: 3 ticks for
FullRate, 6 for ReducedRate, and 12 for LowRate. This is the supported-overload
target, not a promise when configured capacity is mathematically insufficient.
Deadline misses are expected degradation and do not disconnect a peer.

Within a peer the auditable order is:

1. required/forced current state, outside ordinary budget;
2. the authoritative owner relationship;
3. heads whose hard deadline has arrived, ordered by hard deadline, desired due
   tick, then stable `ObjectId`;
4. FullRate due;
5. ReducedRate due; and
6. LowRate due.

FIFO service inside each tier preserves due age. Deadline escalation eventually
defeats ordinary tier preference, so a LowRate relationship cannot lose forever
to newly due FullRate work. Unchanged non-heartbeat work is detected and
rescheduled before snapshot construction and consumes no state budget; moving
or stopped-but-changed state therefore naturally wins over redundant heartbeat
work without an error-scoring system.

Peer service starts from a persistent `ConnectionId` cursor over the ordered
peer map. The first sweep reserves one global state slot for each still-unvisited
peer when capacity permits, then grants up to one quantum; later sweeps continue
round-robin until the global budget or all due work is exhausted. The cursor is
advanced across ticks. A low-ID dense peer therefore cannot permanently hide a
high-ID sparse peer, while each peer still assembles dense batches rather than
alternating one state per packet. When the global budget is below the number of
simultaneously due owners, the same rotation provides fair best effort and
metrics report owner deferral; no impossible owner guarantee is claimed.

## Required and reliable bypass

The forced list is intentionally separate from ordinary counters and limits.
Initial/re-entry current state and control-induced current state are serviced
before ordinary work. Reliable control bind/revoke, replacement, lifecycle,
action result, action start/end, teleport/discontinuity, and terminal state keep
their existing reliable GCHR/structural path. They do not become reliable merely
for scheduling priority, and ordinary low-priority state is not pulled into a
reliable packet.

A semantic burst can therefore exceed the 3G ordinary state cap. Foundation 3G
does not claim a total network CPU cap; the existing reliable scheduler limits,
action admission ceilings, Remote bounds, and 3E.1 terminal failure semantics
remain the defense for semantic traffic.

## Prepare, accept, and commit

The publication transaction is:

```text
absolute deadline becomes due
    -> select relationship under ordinary budget
    -> capture current Character snapshot once for the tick
    -> reuse it for selected peers
    -> sort each GCHR batch by ObjectId
    -> encode and submit to NetworkScheduler
    -> accepted: commit only that batch's peer relationships
    -> rejected: requeue compact due metadata, or enter FailPeer if terminal
```

Selection and encoding do not reset age. Only local scheduler acceptance updates
the fingerprint, last absolute tick, and `LastAcceptedPublicationTick`. If one
batch succeeds and the next fails, only the first batch commits. If a shared
snapshot succeeds for peer A and fails for peer B, A advances and B remains due.
The next B attempt captures the newest state; no second retry queue exists.

This age is local accepted-publication age. Unreliable loss after scheduler
acceptance is not visible to the server and does reset server publication age;
client received-sample age remains a separate loss/jitter quality measurement.
No per-state acknowledgement protocol was added.

## Lifecycle and memory

Relevance leave, Character destruction/replacement, and disconnect unlink the
relationship before erasing its map owner. Re-entry creates fresh age/deadline
state and a required baseline. Control changes retain accepted age but move the
relationship to the correct owner/tier queue. Tier promotion shortens the next
phase/deadline; demotion changes priority without pretending publication
occurred. Terminal failure cleanup remains deferred through `GameSession`'s
3E.1 pending-failure safe point and is idempotent.

On the measured 64-bit MSVC layout the relationship scheduling payload grows
from approximately 40 to 72 bytes before the existing ordered-map node. The
estimated complete map node grows from roughly 80-96 to 112-136 bytes. A peer's
64 wheel heads plus due heads cost about 1.7 KiB regardless of relationship
count. At 25,000 relationships and 500 peers the estimated 3G-specific increase
is about 1.5 MiB. Memory scales with active materialized relationships plus the
bounded per-peer wheel; there is no dense maximum-peer by maximum-Character
matrix and sustained deferral cannot grow historical storage.

## Diagnostics

`CharacterNetworkMetrics` distinguishes required selection, ordinary due,
due-transition, offered, selected, accepted, budget-deferred,
scheduler-rejected, overdue, deadline-escalated, and deadline-missed work. It
also reports:

- available/consumed budget and global exhaustion ticks;
- owner deferrals and owner accepted-state age;
- misses by Full/Reduced/Low tier;
- publication latency totals, maximum, and bounded 1-3, 4-6, 7-12, and over-12
  tick buckets;
- active/current due/current overdue/current forced relationship gauges;
- maximum current and historical state age;
- peer rotations and bounded large-jump rebuilds; and
- separate CPU totals for importance, due discovery, selection, change
  detection, snapshot capture, assembly, encoding, scheduler submission, and
  accepted commit.

`GameSessionMetrics` projects the operational aggregate subset. Labels are not
created per peer, Character, tier transition, or deadline. All counters use
saturating arithmetic. `GetPublicationSchedulingState` is a native diagnostic
and test seam; it is not replicated or exposed to Luau.

## Verification and measured behavior

The deterministic suite covers invalid configuration, synchronized overload,
dense/sparse and early/late peer identity, owner protection, sub-peer-count
global budgets, LowRate starvation, unchanged suppression, tier changes,
relevance leave/re-entry, destroy, disconnect, terminal failure, nonterminal
rejection, partial batch commit, shared snapshot partial acceptance, newest-wins
recovery, wheel jumps, and authoritative root-motion equivalence. Existing
action, teleport, control, prediction-disabled owner, input, loss/reorder,
presentation, malformed GCHR, and lifecycle suites continue to exercise the
unchanged surrounding contracts.

In the final local MSVC Release profile, default 3G reproduced the exact reported
3F 500-Character mixed traffic: 4,000 states/s, 305,860 state bytes/s, 309,460
total bytes/s, and 350 total messages/s. The 500-peer/25,000-relationship profile
retained 227,500 states/s and 30,020 messages/s. Compared with the validated 3F
baseline, fixed-wheel due discovery is approximately half the previous work and
mean/p95/p99 publication cost is lower in repeated local runs. Exact final-run
numbers and constrained/default age distributions are recorded in the 3G
completion report rather than treated as protocol guarantees.

With 500 synchronized FullRate relationships, a 256-state cap preserved each
relationship's three-tick accepted-state age by draining two stable groups on
adjacent ticks while reporting the intentional due-tick deferrals. A 32-state
extreme cap held accepted work to exactly 32 states/tick; state age reached 16
ticks, deadline misses were reported, memory remained fixed, and owner age
remained three ticks. Offered-work runs at 1,000, 2,000, and 4,000 relationships
all held snapshot/assembly/encoding work to 256 selected states/tick; only due
bookkeeping grew with the offered set.

The client remains unchanged. Its six-tick extrapolation and then hold policy is
deliberately not expanded under overload. Severe overload can therefore produce
visible holds; prompt semantics, bounded state, and current-sample recovery are
the correctness contract, not healthy-load smoothness under impossible capacity.

## Explicit non-goals

Foundation 3G does not add RTT/loss estimation, congestion windows, bandwidth
negotiation, simulation LOD, application Remote throttling, generic QoS,
parallel Character assembly, automatic host benchmarking, dynamic public budget
mutation, per-state ACKs, a historical snapshot cache, structural CFrame
fallback, or ecosystem tooling. Telemetry, Node, Studio, MCP, and client Luau
need no changes for this server-private layer.
