---
status: current
owner: networking
last_verified: 2026-09-04
related_code:
  - include/gargantuan/runtime/SpatialRegionIndex.hpp
  - include/gargantuan/network/ReplicationRelevance.hpp
  - include/gargantuan/network/ReplicationCoordinator.hpp
  - include/gargantuan/network/GameSession.hpp
  - include/gargantuan/network/CharacterProtocol.hpp
  - src/network/ReplicationRelevance.cpp
  - src/network/ReplicationCoordinator.cpp
  - src/network/GameSession.cpp
  - src/network/CharacterNetwork.cpp
  - tests/ReplicationRelevanceTests.cpp
  - tests/GameSessionTests.cpp
  - tests/CharacterNetworkingTests.cpp
  - tests/GameSessionBenchmark.cpp
related_adrs:
  - docs/src/content/docs/developing/networking-architecture.mdx
---

# Character / replication foundation 3E

## Implemented boundary

Foundation 3E makes server-owned peer relevance the common upstream boundary
for structural replication and realtime Character state:

```text
authoritative DataModel
    -> accepted ConnectionId / Player identity
    -> 3H SpatialAddress/region candidates
    -> ReplicationRelevance policy
    -> peer desired objects + owner-required objects
    -> ReplicationCoordinator ancestor/hard-reference closure
    -> peer KnownObjects materialization view
    -> structural Publish / Unpublish and soft-reference fixups
    -> the same spatial Character membership gates GCHR publication
```

Relevance answers whether an object is currently materialized. It does not
select a state cadence or detail level. Foundation 3E adds no network LOD,
adaptive Character cadence, public streaming API, region protocol identity,
client camera protocol, or Luau per-object callback.

`ReplicationRelevance` is an internal server mechanism. Ordinary client Luau
has no pin, focus, radius, group, region, or arbitrary ObjectId request surface.
The native `GameSession::SetTrustedReplicationFocus` seam is server-role-only
and exists for trusted hosts and deterministic fixtures; no client message can
reach it. Without an explicit trusted focus, the current controlled Character
position is the default focus.

## Policy and spatial lookup

Foundation 3H now supplies the renderer- and physics-independent canonical
uniform region index behind this policy. A spatial root is a `Character`, or otherwise the nearest ancestor
`BasePart`; its descendants share that root's membership. Objects without a
spatial root remain global. Remote Instances are global because their identity
and application semantics are not transform-owned.

The defaults are:

| policy | bound |
| --- | ---: |
| region size | 128 units |
| enter radius | 256 units |
| leave radius | 320 units |
| normal recomputation interval | 6 simulation ticks |
| trusted focus points per peer | 4 |
| indexed roots / populated regions / memberships | 65,536 / 262,144 / 262,144 |
| ordinary memberships per root / large-root fallback | 64 / 1,024 |
| desired objects per peer | 65,536 |
| regions visited by one peer query | 4,096 |
| structural enter + leave objects per incremental frame | 4,096 |

The distinct enter and leave radii provide deterministic hysteresis. Object
and peer changes mark selection dirty. Character and BasePart CFrame changes
coalesce into a preallocated dirty-root list and update region membership at
the relevance safe point; unchanged membership mutates no bucket. All index,
candidate, desired, and transition order
uses `ObjectId` ordering. Pointer identity and hash iteration never define wire
results.

`SpatialAddress` is derived locality rather than physics, render, or network
identity. Regions return a conservative bounded candidate superset while 3E
retains exact policy ownership:

```text
world membership/address -> relevance policy -> desired ObjectIds
```

See `CharacterReplicationFoundation3H.md` for address semantics, multi-region
bounds, large-object fallback, lifecycle, limits, and scale measurements.

## Mandatory relevance and Player semantics

Required ancestry is always materialized. The accepted local `Player`, its
current `Player.Character`, and every member of the currently controlled
Character subtree are owner-required. Changing or revoking control changes the
owner reason through the same relevance path; control does not bypass it.

Current public `Players` behavior remains intact: all connected `Player`
Instances remain globally visible, so `PlayerAdded`, `PlayerRemoving`, and the
trusted `Players.LocalPlayer` association retain their existing semantics.
Player visibility does not force every Character descendant to remain visible.

`Player.Character` is declared as an internal hard materialization dependency.
Consequently a remote Player retains a lightweight Character shell even when
that Character is spatially irrelevant. `Character.RootPart` remains a nullable
soft reference. While the subtree is absent it reads as nil; on enter it is
fixed up after the target publishes, and on leave it is cleared before the
target unpublishes. The Character shell and `Player.Character` identity remain
stable, so spatial churn does not invent `CharacterAdded` or
`CharacterRemoving`. This is safe for Luau and never leaves a dangling native
pointer.

The client intentionally reuses ordinary replica destruction for a peer
unpublish, so streamed descendants emit the existing local ancestry/removal
signals and may materialize as fresh client objects on reentry. That local
destruction cannot travel upstream. Retaining the remote Character shell is
what keeps authoritative `Player.Character`, `CharacterAdded`, and
`CharacterRemoving` semantics distinct from those descendant streaming
signals; no public `StreamedOut` signal is introduced.

## Dependency closure and references

The coordinator computes closure iteratively from policy-selected ObjectIds:

1. include every selected and owner-required object;
2. include its complete parent chain;
3. include replicated Object references marked `Hard`, plus any non-nullable
   Object reference;
4. repeat until no new object is found.

The visited `ObjectId` set terminates cycles. Depth is limited to 64 and total
closure to 65,536 objects. Exceeding a bound fails the peer/session coherently;
it never silently produces an orphan or invalid graph. Incremental
materialization is separately budgeted at 4,096 enter/leave objects per frame;
larger valid changes remain desired and drain deterministically over later
frames. Enter groups are atomic for ancestry and hard references. Leave groups
traverse descendants and hard-reference dependents so a referrer leaves before
its target across a budget boundary; a dependency group larger than the
transition ceiling fails coherently. Owner-required objects sort ahead of
ordinary spatial entries. A newly entering hard target of a remaining known
referrer has the same safety priority; if those critical dependency groups do
not fit yet, unrelated leaves defer until the reference graph can remain valid.
This is dependency safety, not an importance/LOD system. Arbitrary Attributes,
extensions, custom properties, and nullable soft references do not recursively
pull targets into interest, preventing one reference edge from expanding into
the whole DataModel.

`InstanceProperty::MaterializationDependency` is frozen schema metadata, not a
second object identity or public game API. Schema validation permits `Hard`
only on replicated Object-reference properties. `Player.Character` is the only
new hard edge in 3E.

## Desired, known, enter, leave, and reentry

Each peer keeps policy `DesiredObjects` separately from the replication
`KnownObjects` view. The coordinator owns one generation-safe authoritative
structural template catalog, refreshes affected immutable object revisions from
the journal, and advances each
peer cursor even when records are off-interest. It does not build a complete
DataModel snapshot for every admission or every peer update.

`KnownObjects` advances only for the bounded part actually queued. The remaining
desired-versus-known difference is an observable materialization backlog; the
same selection is coalesced and reconsidered next step instead of appending
duplicate transition jobs. Dense enter and leave tests use 4,101 objects and
complete as 4,096 plus 5 lifecycle operations while keeping peer-unpublished
server objects alive; a separate in-flight target is destroyed to prove the
wire/bookkeeping distinction.

Enter publishes current authoritative objects after closure is complete.
Parents precede children and all objects in a frame are constructed before
references apply. Leave first clears remaining soft references, then
unpublishes children before ancestors. A retired authoritative object uses the
existing `DestroyReplication` intent; an otherwise-live object leaving only
this peer uses `UnpublishReplication`. A peer unpublish changes only that
receiver's replica; the server Instance remains alive and may reenter. Server
destruction retains tombstone structure only while a peer still knows the
identity, allowing deterministic child-before-parent retirement without
confusing unpublish with authoritative destruction.

Off-interest journal records are skipped rather than queued. Reentry therefore
publishes one fresh current snapshot and never replays the object's historical
backlog. Native property changes in one journal batch coalesce to current state;
this is necessary for `Player:LoadCharacter()`, whose intentional intermediate
nil must not destroy a replacement already published by the relevance frame.
If a new hard-reference target remains behind the transition budget, its
property delta is coalesced and the eventual dependency enter emits the current
fixup only after the target is known.

Structural frames retain the peer `ReplicationEpoch`, reliable sequence, and
generation-bearing `ObjectId`. Foundation 3E.1 widens the GCHR v4 peer
materialization epoch from 16 to 64 bits. The server and client advance it on
every accepted Character materialize/unmaterialize transition. A delayed frame
from before leave/reentry is consumed and counted as stale, but cannot mutate
the new replica or restore prediction, interpolation, or action state. The
state-frame header is now 34 bytes and retains the 15-state/1,200-byte batch
ceiling. See `CharacterNetworkingFoundation3E1.md` for exhaustion and reliable
commit behavior.

## Production session integration

`GameSession` registers Character and Remote descendants incrementally from
DataModel signals. Peer readiness no longer rescans every descendant and every
already-ready peer. Each server step updates owner pins and advances relevance.
Dependency closure and structural reconciliation run when that peer's selection
is evaluated (normally every six ticks, immediately when dirty) or while a
bounded transition backlog remains; they are not rebuilt for every peer at 60
Hz. The session then consumes the journal, synchronizes the GCHR and Remote
materialized sets, runs Character authority, and flushes the existing bounded
scheduler.

On both endpoints, a Character shell enters GCHR publication,
prediction/presentation only while both the shell and its `RootPart` are in the
committed peer view. Leave clears prediction and interpolation; reentry starts
a fresh presentation lifetime. Remotes still require structural
knowledge. The Remote manager's argument-materialization set is initialized
from the accepted view and then advanced from the same structural frames on
both endpoints, so an Instance argument may target a known replica but cannot
smuggle or retain an off-interest ObjectId. Remote publication identities and
pending RemoteFunction lifetimes remain separate from unrelated relevance
churn. No secondary repository changes were required: production
connection, accepted peer, Player, LocalPlayer, and control ownership already
live in the engine `GameSession` established by 3D.

## Measurements and tests

The reproduced pre-3E Release benchmark at 500 peers was 15,803.2 ms and
43,649,262 allocator calls. Instrumentation attributed approximately 11.60 s
to repeated `SynchronizeServerGraph`, 0.782 s to baseline snapshot capture,
0.319 s to discovery, 0.200 s to encoding, and 0.374 s to Player/Character
creation. The dominant cause was repeated whole-graph/whole-peer gameplay
synchronization, not GCHR batching.

The 3E sparse fixture places controlled Characters 1,024 units apart. A final
local MSVC Release run measured:

| peers | admission ms | allocator calls | materialized objects (sum) |
| ---: | ---: | ---: | ---: |
| 1 | 0.408 | 2,325 | 16 |
| 32 | 19.538 | 283,711 | 2,496 |
| 100 | 169.925 | 2,658,035 | 21,400 |
| 500 | 4,191.74 | 68,838,783 | 507,000 |

The 500-peer time fell 73.5% from the reproduced 15,803.2 ms and completed in
four handshake ticks. Repeated gameplay graph synchronization fell from about
11.60 s to 21.1 ms; relevance initialization was 44.7 ms, bounded candidate
query/update work was 11.1 ms, and dependency-closed baseline discovery was
801.0 ms. The 507,000 sum is about 1,014 objects per peer: globally visible
Player identities, their hard-dependent lightweight Character shells, and the
owner subtree/mandatory services. It is not the full authoritative descendant
set per peer.

Allocator calls remain above the 3D number because preserving global Player
visibility requires repeated Player/Character-shell baseline values and the
implementation deliberately owns catalog, closure, known-view, and Remote
reference-materialization sets. That remaining cost is explicitly measured and
is a Foundation 3F optimization input, not hidden as completed work.

Holding one peer's interest at 16 objects while increasing distant indexed
world Parts from 1,000 to 10,000 to 50,000 measured 1.247/1.543/1.526 ms and
14,280/14,324/14,445 allocations for admission. Global index construction is a
world-lifetime cost outside that per-peer admission window. Holding the world
fixture shape fixed while materializing 50/500/5,000 ordinary objects measured
0.142/1.679/25.596 ms and 1,975/28,352/393,922 allocations, demonstrating that
materialization work follows actual interest/closure size.

A 500-peer relevance cycle with 250 stationary, 150 normally moved, and 100
teleported trusted focuses measured 2.176 ms, 16,113 allocations, 500 bounded
queries, 4,782 candidates, 300 enters, and 940 leaves. It never scanned the
authoritative object catalog per peer.

Those figures preserve the pre-3H 3E baseline. The same post-3H fixtures and
the 10k/100k/1M index-only scale results are reported in
`CharacterReplicationFoundation3H.md`.

Memory is bounded by the same ceilings. Per peer, relevance owns at most four
focus points, 65,536 desired/required ObjectIds, the spatial-root residency
set, and the coordinator's desired/known/relevant ObjectId sets; GCHR and
Remote maps contain only currently materialized identities. Each spatial root
owns one weak Instance reference, current position, bounded region membership,
its descendant-member set, dirty bit, and CFrame/Size signal connections. The
shared region index owns one current generation-bearing root entry plus at most
64 ordinary memberships, or one bounded large-object fallback entry. The
authoritative catalog stores one current revisioned structural publication
template per world object globally, not per peer. In the 500-peer
fixture the measured known-object sum was 507,000, about 1,014 per peer; there
is no per-peer storage for the distant 50,000-object world fixture and no
off-interest delta queue.

Normal admission is proportional to global/mandatory objects plus the queried
interest set and its dependency closure. A focus update visits bounded regions
and candidates, then diffs the peer's desired/known sets; object movement
updates only changed old/new memberships in logarithmic ordered-container work
and same-membership motion mutates no bucket. Catalog refresh
is journal-driven and shared across peers. Enter/leave serialization is
proportional to the bounded transition slice. Only initial index construction
or explicit journal-overflow resnapshot scans the complete world, neither of
which is a per-peer steady-state scan.

The original all-relevant 500-moving-Character, 20 Hz GCHR v3 fixture produced
759,040 state bytes/s and 762,640 bytes/s including input in 740 messages/s.
Foundation 3E.1 retains the same batching/cadence and adds six header bytes per
state frame for the widened generation. The current measured v4 totals are
recorded in `CharacterNetworkingFoundation3E1.md` and the milestone report;
sparse relevance still removes unnecessary Character states.

`gargantuan_replication_relevance_tests` covers owner pinning, initial
near/far selection, dependency closure, global remote Players, stable
`Player.Character`, soft `RootPart` nil/fixup, enter, leave, reentry,
hysteresis, composed spatial/owner reasons, shared-ancestor retention,
Player-independent NPC enter/leave/reentry, deterministic bounds, coherent
query-limit failure, off-interest journal retirement without a delta backlog,
bounded 4,101-object enter/leave backpressure, dependency-cycle termination,
and authoritative destruction during queued enter/leave work. The 64-edge
closure-depth ceiling aligns with the native hierarchy-depth limit; deeper
ancestry is rejected before it can become a valid replicated world.
It also places a hard `Player.Character` edge across the 4,096-operation
boundary to prove dependency-safe enter and leave ordering, and replaces that
hard Character while 4,097 objects leave to prove the new target is
materialized before the old replica can retire.
`gargantuan_character_networking_tests` covers stale pre-reentry GCHR state and
resuming an active action at its current authoritative phase.
`gargantuan_game_session_tests` retains Player/LocalPlayer/control isolation,
replacement, disconnect, Remote, action, and production lifecycle coverage and
adds two-client differential spatial materialization with owner pins and remote
Character reentry. `gargantuan_game_session_benchmark` covers 1/32/100/500
sparse peers, fixed-interest worlds at 1,000/10,000/50,000 spatial objects,
50/500/5,000-object materialization, and a mixed stationary/moving/teleporting
500-peer relevance update.

The existing FirstCompleteGame packaged headless-server plus headless/graphical
client proof now also drives `RootMotionOpenNpc` far/near/far/near through
ordinary authoritative server Luau and requires the client to observe
absence/enter/leave/reentry before success. The optional localhost GNS
game-session test repeats that lifecycle with a Player-independent NPC and
asserts RootPart closure, current transform, server survival after peer
unpublish, and current-state reentry. Deterministic two-client coverage remains
the differential LocalPlayer/control oracle because two-client GNS is not a
reliable CI requirement.

Earlier MSVC Release acceptance passed all 44 registered CTest tests. That
matrix includes the packaged/relocated FirstCompleteGame, 100-cycle production
session composition, the 100-connection churn test, protocol malformation
coverage, Remotes, offline runtime, editor/Studio-facing runtime tests, and
package validation. The optional Vulkan FirstCompleteGame GPU proof also passed
(`rendererRestart=PASS`), as did localhost GNS Remote, Character, and complete
game-session lifecycle executables. The exact final source then passed 42/42
Linux tests in 522.43 seconds under Clang 19 ASan/UBSan with
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and strict UBSan halt behavior;
there were no address, undefined-behavior, or leak diagnostics. Post-push Native
CI remains the cross-platform publication authority. TSAN was not run because
this repository has no reliable TSAN job; no TSAN-clean claim is made.

The existing Studio production Play boundary was inspected and requires no
protocol change: it launches the engine-owned packaged session that now owns
relevance. Node remains outside realtime simulation and materialization; MCP
has no generic integration gap; Telemetry requires no separate repository
contract because the new bounded counters are exposed by native session,
replication, and relevance metrics. No secondary or tertiary repository was
modified for 3E.

## Foundation 3F integration

Foundation 3F now consumes the exact resolved 3E trusted focus set and ranks
only Characters in each peer's materialized set. The 3E boolean desired set,
structural materialization lifetime, owner pin, and control authority remain
unchanged and are never recomputed by importance. The resulting private
20/10/5 Hz cadence is documented in
`CharacterNetworkingFoundation3F.md`. No Player visibility, dependency closure,
or structural transition was moved into GCHR.

Foundation 3H changes only the upstream candidate implementation. Region
crossing alone does not change this boolean 3E result, and 3F/3G see no
relationship change unless 3E actually commits materialization enter/leave.

Foundation 3I changes only the downstream structural description ownership.
Template existence does not grant relevance or materialization; 3E still owns
the desired/known transition. Hard closure, soft peer fixups, materialization
epochs, trusted LocalPlayer, and terminal scheduler admission remain separate.
See `ReplicationFoundation3I.md`.

## Explicitly deferred

- simulation, AI, physics, renderer, or region LOD;
- a public Luau streaming/relevance API;
- client camera or focus hints;
- region wire identity, world paging, portals, and transfer epochs;
- occlusion or visibility prediction;
- changing globally visible Player identity semantics;
- shared mutable wire buffers or protocol identity tied to grid cells.
