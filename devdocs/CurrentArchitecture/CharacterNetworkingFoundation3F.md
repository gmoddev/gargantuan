---
status: current
owner: networking
last_verified: 2026-09-04
related_code:
  - include/gargantuan/network/CharacterNetwork.hpp
  - include/gargantuan/network/ReplicationRelevance.hpp
  - include/gargantuan/network/GameSession.hpp
  - src/network/CharacterNetwork.cpp
  - src/network/ReplicationRelevance.cpp
  - src/network/GameSession.cpp
  - tests/CharacterNetworkingTests.cpp
  - tests/CharacterNetworkingBenchmark.cpp
related_adrs:
  - docs/src/content/docs/developing/networking-architecture.mdx
---

# Character networking foundation 3F

Foundation 3F adds server-private, peer-specific publication importance to the
already relevant Character set. It changes neither the GCHR v4 wire schema nor
the 60 Hz authoritative simulation. Foundation 3E remains the only owner of
boolean relevance and materialization; 3F can only choose when replaceable
state for a materialized Character is next considered for publication.

Before 3F, every materialized Character used one global 20 Hz selection clock.
Foundation 3E.1 made each materialization lifetime failure-atomic and added a
64-bit materialization epoch, but deliberately left cadence global. 3F keeps
that lifecycle model and adds compact scheduling metadata to each existing
peer-Character relationship.

Foundation 3G now owns the current bounded due-discovery and fair-selection
implementation described in
`devdocs/CurrentArchitecture/CharacterNetworkingFoundation3G.md`. This document
remains authoritative for the 3F importance, cadence, phase, promotion, and
variable-rate client presentation policy.

Foundation 3H now supplies canonical bounded region candidates upstream of 3E;
it does not change this importance function, phase, desired cadence, or tier
history. A region crossing that remains 3E-relevant leaves the same 3F
relationship intact.

## Ownership and separation

The server-owned `AuthoritativeCharacterNetwork` owns importance. `GameSession`
passes it the exact resolved trusted focus points already owned by
`ReplicationRelevance`: up to four host-supplied focus points, or the owner
Character position when no explicit point exists. The minimum Euclidean
distance to those points is the current distance input.

The boundaries are strict:

- 3E relevance decides whether a peer knows and materializes a Character.
- 3F importance is evaluated only for entries in that materialized set.
- importance cannot materialize, control, destroy, replace, or simulate an
  object;
- simulation, collision admission, Luau policy, root motion, actions, and
  physics remain at their existing authoritative cadence;
- the client supplies no tier, priority, cadence, or arbitrary promotion API;
- Player shells remain structurally visible under the 3E policy even when a
  Character is irrelevant or low-rate.

NPCs use the same relationship policy without requiring a Player. Custom
Character movement policies affect authoritative transforms and velocities,
not tier ownership. Every locally controlled Character is full-rate regardless
of whether client prediction is enabled.

## Selected policy

| Tier | Distance policy | Replaceable state cadence | Interval at 60 Hz |
| --- | --- | ---: | ---: |
| Full | owner, temporary promotion, or distance at most 64 units | 20 Hz | 3 ticks |
| Reduced | between the full and low bands | 10 Hz | 6 ticks |
| Low | distance above 160 units | 5 Hz | 12 ticks |

Three tiers were selected because the candidate 20/15/10/5 Hz benchmark already
showed that 20 Hz preserved the proven owner path, 10 Hz gave a simple middle
band, and bounded extrapolation made 5 Hz numerically usable for distant
Characters. Extra adjacent tiers did not provide a clearer policy benefit.

Distance transitions use a 16-unit Schmitt-trigger band. Full does not demote
until distance exceeds 80. Reduced promotes below 48 and demotes above 176.
Low does not promote until distance is below 144. A direct full-to-low demotion
is allowed when safely beyond the outer band. Importance is recomputed every
six simulation ticks (10 Hz), or immediately after trusted focus,
materialization, control, or promotion state becomes dirty.

Materialization, control bind, motion resume, action transition, and
discontinuity establish a six-tick temporary full-rate window. Relevance entry
and re-entry additionally request an immediate baseline. Ordinary promotions
use the stable phase of the promoted cadence, avoiding synchronized movement
bursts; reliable semantic state bypasses the replaceable scheduler entirely.

## Publication scheduler and batching

There is no timer, task, callback, or heap object per Character. Each simulation
tick performs these bounded stages:

1. walk only each peer's materialized relationship map and mark relationships
   whose stable phase, immediate baseline, or absolute-age limit is due;
2. build and fingerprint one authoritative snapshot for each Character needed
   by at least one due relationship;
3. reuse that snapshot while comparing peer-private fingerprints;
4. preserve deterministic `ObjectId` ordering and the existing 15-state /
   negotiated-byte GCHR batching limit;
5. commit a relationship's fingerprint, age, and cadence history only after
   scheduler admission succeeds.

The stable phase is an FNV-derived hash of generation-safe `ConnectionId` and
`ObjectId`, modulo the tier interval. A deliberately focus-free forced-full
benchmark retains phase zero to reproduce the exact 3E.1 all-full compatibility
path. Production focus-bearing relationships, including reduced and low tiers,
are distributed. Movement does not randomize phase. Reliable control/action
traffic retains scheduler precedence; replaceable due states remain in
deterministic object order so GCHR canonical ordering and batching are intact.

If a replaceable batch cannot be admitted, its relationship history is not
advanced. Immediate baselines remain immediate until admitted, and the
60-tick absolute refresh becomes due on every later tick. Existing bounded
scheduler/backpressure policy remains the final burst bound. Reliable semantic
admission failure retains the 3E.1 peer-terminal behavior and is never silently
dropped.

Unchanged stationary state is suppressed exactly as in 3C. A healthy relevant
relationship still receives a full absolute state no later than 60 ticks after
its last publication. Starting motion receives a temporary full-rate window;
it is not assigned a permanent moving tier.

A paired deterministic NPC root-motion test runs identical authoritative
actions under full-rate and low-rate publication. Their transforms match at
every simulation tick while the relationships finish in different tiers,
directly proving that cadence does not alter authoritative simulation.

## Reliable semantics and lifecycle

The following never wait for the reduced or low replaceable phase:

- control bind/revoke and materialization/replacement structural state;
- `CharacterActionResult` and authoritative action start/end state;
- teleport or detected discontinuity state;
- peer-terminal reliable failures.

Action and teleport state use the existing reliable GCHR control lane. A
discontinuity is distance beyond the existing eight-unit hard-correction bound
plus expected travel from the previous/current authoritative velocity, so a
fast but coherent Character is not misclassified as a teleport.

Relevance leave erases the relationship and increments the materialization
epoch. Re-entry creates new cadence/fingerprint history and an immediate
baseline; it never revives old timers. Character destruction/replacement erases
the Character and all relationship entries. Peer disconnect destroys the whole
peer map. Prepared due work is transient to one `Step`, rechecks live
relationship maps during assembly, and cannot survive those operations.

## Variable-rate client presentation

The client does not receive tier identity. It continues to order samples by
authoritative tick and interpolates using the actual left/right tick span, not
an assumed 20 Hz interval. Each bounded snapshot now retains the already
replicated authoritative velocity.

When the delayed presentation target is newer than the newest sample, position
is extrapolated for at most six simulation ticks (100 ms) and rotation is held.
After that horizon, the newest transform is held until a newer state arrives.
The client never extrapolates velocity forever and never applies remote
presentation to gameplay semantics. Teleport, control epoch, materialization
epoch, stale sequence, and expected-travel-aware correction rules still reset
the buffer safely.

At 5 Hz in the deterministic 60 Hz quality fixture, measured position error was:

| Motion | Mean | p95 | Maximum | Result |
| --- | ---: | ---: | ---: | --- |
| constant 6 units/s | <0.001 m | <0.001 m | <0.001 m | exact within codec precision |
| acceleration, stop, reverse | 0.0128 m | 0.0584 m | 0.1189 m | bounded |
| jump/gravity | 0.0227 m | 0.0491 m | 0.5271 m | landing-only maximum, below 0.55 m gate |
| constant 120 units/s | <0.001 m | <0.001 m | <0.001 m | expected travel is not a teleport |
| constant 6 units/s, deterministic 20% sample loss | 0.2808 m | 1.50 m | 1.70 m | bounded hold after horizon |

The current capsule is four units high; the 0.55-unit jump landing bound is
13.75% of that height and is the explicit low-tier worst-case gate. Action and
teleport tests separately prove prompt reliable transitions and buffer reset.
No remote ballistic or gameplay prediction was added.

## Metrics, cost, and measurements

`CharacterNetworkMetrics` exposes importance evaluations/transitions, temporary
promotions, semantic bypasses, due relationships, shared snapshots and their
relationship uses, tier relationship gauges, states/bytes/age per tier,
maximum ages, batching, extrapolation/hold counts, and CPU for importance,
due-set construction, change detection, assembly, encoding, and scheduler
submission. `GameSessionMetrics` projects the production importance/state
counters without exposing policy to Luau.

The per-relationship payload is four 64-bit history/promotion values, two
one-byte tiers, and four transient flags, plus ordinary alignment and the
existing ordered-map node. On the measured MSVC build this is estimated at
about 80-96 bytes including tree-node links/allocation overhead. The
500-peer-by-50-relevant fixture therefore adds roughly 2.0-2.4 MiB for 25,000
relationships. No cadence state exists for irrelevant Characters. Each peer
also owns at most four focus vectors and one refresh tick/dirty flag.

MSVC Release measurements on 2026-09-03, 120 simulation ticks unless noted:

| Fixture | State B/s | State frames/s | States/s | States/frame | Total B/s incl. input | Total msg/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 3E.1-compatible all-full 500/500 | 763,120 | 680 | 10,000 | 14.706 | 766,720 | 740 |
| mixed 50 full / 150 reduced / 300 low | about 306,000 | about 290 | 4,000 | about 13.8 | about 309,600 | about 350 |
| all-full sparse 50/500 | 76,720 | 80 | 1,000 | 12.5 | 80,320 | 140 |
| mixed sparse 5 full / 15 reduced / 30 low | 31,640 | 60 | 400 | 6.667 | 35,240 | 120 |

The mixed 500 fixture reduces states by 60%, replaceable bytes by about 59.9%,
and total messages by about 52.7% from the all-full row. The sparse mixed row
adds a further 58.8% replaceable-byte reduction after 3E relevance savings.
Observed mean/maximum state ages were exactly 3/3, 6/6, and 12/12 ticks for
full/reduced/low moving state.

The 500-peer synthetic production-shape fixture owns 500 controlled Characters,
50 relevant relationships per peer, and 25,000 total relationships. It measured
250,000 importance evaluations/s, about 227,500 state relationship sends/s,
about 17.9 MB/s aggregate GCHR payload, and 5.86 ms mean Character network
`Step` in the final verification run. Importance evaluation
consumed about 17.9 ms total over the one-second run, due-set construction about
114 ms, change detection about 26.6 ms, assembly about 190 ms, encoding about
41.6 ms, and scheduler submission about 5.6 ms. The scheduler built 29,840
shared snapshots/s for 227,500 due
relationship uses/s, or roughly 7.6 relationship uses per snapshot.
The hot `Step` recorded 30,000 allocations (500/tick), not one allocation per
published state. The 32-peer/500-Character representative fixture contains 32
owners plus 468 NPC/other Characters and 1,600 relevant relationships.

The zero-peer/500-Character offline fixture produced zero importance
evaluations, due relationships, snapshots, messages, and Character-network
allocations across 60 measured ticks. Authoritative Character observation and
simulation remain available, while peer-private publication work returns
immediately when there are no peers.

Stable phases produced 3,791.7 mean states/tick, about 4,135 maximum/p95 in the
500-peer fixture (about 1.09x mean) when every relationship used a trusted
focus. Full compatibility mode intentionally retains the old synchronized
phase to prove exact 3E.1 output.

The full benchmark additionally exercises 500-Character all-reduced, all-low,
and dense semantic-promotion batches. Together with the synchronized all-full
compatibility workload and focus-bearing distributed-phase fixtures, these
cover homogeneous, mixed, potential burst, phase-distributed, and semantic
bypass batching behavior. All-reduced measured 382,240 state B/s, 360 state
frames/s, 5,000 states/s, and 13.889 states/frame; all-low measured 191,290
state B/s, 185 state frames/s, 2,500 states/s, and 13.514 states/frame. The
dense semantic fixture triggered 500 prompt reliable semantic publications,
then returned to its ordinary peer-private tiers.

## Admission audit

No Player/Character-shell or structural serialization optimization was made.
The remaining 500-peer cost is dominated by intentionally peer-specific
structural closure/materialization and baseline encoding; a safe immutable
cache was not evident because frames contain peer view/materialization state.
Changing Player visibility or `LocalPlayer`/`Player.Character` semantics for a
secondary allocation target was rejected.

The current local MSVC Release admission baseline remains: 1 peer 0.506 ms /
2,073 allocations / 16 summed materialized objects; 32 peers 18.558 ms /
263,779 / 2,496; 100 peers 160.012 ms / 2,528,715 / 21,400; and 500 peers
5,212.76 ms / 67,739,425 / 507,000. The representative 500-peer relevance
update measured 2.887 ms, 16,113 allocations, 500 queries, and 4,782 candidates.
These are admission costs, not steady-state cadence costs.

## Security, compatibility, and deferrals

GCHR remains version 4; cadence identity is not encoded. A client cannot select
or spoof a tier, promote arbitrary Characters, create relevance, gain control,
or change authoritative simulation. One peer's relationship state is stored
under that peer and cannot change another peer's cadence. Low rate cannot delay
revoke, destroy, replacement, ActionResult, action state, or teleport. Runtime
Character transforms remain outside structural replication and the authoring
journal. No mutable cross-peer encoded buffer or process-static session cache
was added.

3F explicitly defers simulation/AI/physics LOD, sleeping, distributed ownership,
congestion-control protocols, client bandwidth settings, renderer/PVS relevance,
regions/portals/`SpatialAddress`, authentication/matchmaking/Node tickets,
vehicle networking, combat rollback, generic abilities/Humanoid, animation
graphs, Studio multiplayer tooling, and telemetry redesign.

Foundation 3G has added the measured bounded publication budget, timing wheel,
age escalation, and deterministic peer fairness cursor. Production telemetry
should still validate the 5 Hz landing/loss and overload-hold bounds. Structural
admission allocation sharing remains deferred behind a proof of peer-specific
invalidation and unchanged Player semantics.
