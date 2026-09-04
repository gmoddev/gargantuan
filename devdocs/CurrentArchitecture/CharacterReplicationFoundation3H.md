---
status: current
owner: networking
last_verified: 2026-09-04
related_code:
  - include/gargantuan/runtime/SpatialRegionIndex.hpp
  - src/runtime/SpatialRegionIndex.cpp
  - include/gargantuan/network/ReplicationRelevance.hpp
  - src/network/ReplicationRelevance.cpp
  - include/gargantuan/network/GameSession.hpp
  - src/network/GameSession.cpp
  - tests/SpatialRegionIndexTests.cpp
  - tests/SpatialRegionIndexBenchmark.cpp
  - tests/ReplicationRelevanceTests.cpp
related_adrs:
  - docs/src/content/docs/developing/networking-architecture.mdx
  - devdocs/FutureArchitecture/SpatialRegionsAndPortalTopology.md
---

# Character and replication foundation 3H

Foundation 3H promotes the private cell lookup introduced by 3E into one
canonical, generic runtime spatial-address and region-index layer. It organizes
the already-loaded authoritative world for bounded candidate discovery. It does
not stream world content, control simulation, grant authority, or add a client
region protocol.

```text
authoritative transform/bounds
    -> derived SpatialAddress and sparse region memberships
    -> bounded candidate regions and ObjectIds
    -> 3E exact relevance, hysteresis, owner/global policy
    -> dependency-closed structural materialization
    -> 3F desired Character cadence
    -> 3G bounded actual Character publication
```

The former 3E implementation already used one sparse ordered 128-unit 3D grid
per `ReplicationRelevance`. It point-indexed Character and BasePart roots, polled
every Character on each relevance update, and performed fresh bounded cell
queries. 3H deliberately retains the measured uniform-grid shape. It replaces
the private 32-bit `CellAddress` and cell maps with the reusable
`SpatialAddress`/`SpatialRegionIndex`, adds generic bounds and large-object
handling, and makes transform membership updates dirty-driven. There is no
second competing spatial index and no octree.

## Canonical address

`SpatialAddress` is `{ uint32 Space, int64 X, int64 Y, int64 Z }`. Space zero is
invalid; space one denotes the current DataModel world. The field is a reserved
multi-space seam only: 3H supports one active space and rejects other query
spaces. Portals, interiors, world travel, server sharding, and floating origins
remain deferred.

The address is derived runtime metadata. It is not persisted in project files,
written to `ChangeJournal`, encoded in GCHR or structural replication, or sent
to a client. The authoritative CFrame/bounds remain the sole location source.
`ObjectId { slot, generation }` continues to answer which object exists;
`SpatialAddress` answers where it currently belongs. A region crossing changes
no ObjectId, control epoch, materialization epoch, authority, or gameplay state.

Coordinates use mathematical `floor(position / regionSize)`, including for
negative values. Regions are half-open intervals `[N*S, (N+1)*S)`. Bounds use
the same rule and apply `nextafter(max, -infinity)` on a non-degenerate maximum,
so a box ending exactly on a boundary does not acquire the next region.
Conversion happens through finite `long double` arithmetic followed by an
explicit signed-64-bit range check; NaN, infinity, and overflow fail instead of
wrapping. Equality and ordering are structural. The stable diagnostic hash is
an explicit byte-wise FNV-1a mix of the space and three signed coordinates; it
is distribution metadata, not a security primitive or identity.

## Spatial roots and bounds

The current canonical roots remain intentionally narrow:

- a `Character` owns one point membership at its authoritative Character
  transform; all Character descendants belong to that replication root;
- outside a Character, a `BasePart` owns its rotated world-axis-aligned bounds
  and its non-spatial descendants;
- Player/session objects, Remotes, services, Models without a canonical
  aggregate bounds owner, and other non-spatial Instances stay global under
  3E policy.

This avoids recursively recomputing Model bounds and avoids indexing every
Character descendant. Standalone Parts, Character-based NPCs, scripted motion,
physics-published Part CFrames, kinematic movement, teleports, and root-motion
Character commits all converge on the same CFrame dirty signal. The region
index observes those committed transforms at the next relevance safe point; it
never mutates a transform or invokes Luau.

Bounds overlapping at most 64 regions receive one sorted membership per
region. Their primary address is the region containing the bounds center and
may change without bucket churn when the overlap set is unchanged. Larger
bounds enter a separate bounded large-object set and are conservatively offered
to every query. That set is capped at 1,024 in replication and 4,096 by the
generic index hard ceiling. This favors bounded extra 3E work over a false
negative and prevents one large assembly from manufacturing unbounded region
memberships.

## Ownership, storage, and lifecycle

`ReplicationRelevance` owns one `SpatialRegionIndex` for its authoritative
server world/session. All peers share its region buckets; no peer receives a
copy of region contents. The reusable runtime type is networking-neutral, but
3E is its only production consumer in 3H. A server GameSession creates the
index during manager initialization and destroys it during `Stop`, failure, or
normal shutdown. Offline/client runtimes create neither relevance nor region
query state.

The index contains ordered sparse maps from generation-bearing `ObjectId` to
one current entry and from `SpatialAddress` to an ordered ObjectId set. It
allocates nothing for empty theoretical regions. Removing the final membership
erases the bucket immediately. A 100,000-unique-region teleport test leaves one
live region while moving and zero after removal; there is no historical path or
tombstone collection.

Registration derives and validates the complete candidate membership, checks
all object/region/membership/large-object limits, inserts bucket membership, and
only then publishes the object entry. Allocation failure rolls back every
inserted membership and empty bucket. Update first derives a complete new
entry, validates the final live capacity, inserts new membership while the old
entry remains queryable, removes retired membership, and commits the entry.
Validation/resource failure retains the prior membership. At most one object's
bounded overlap can exist transiently during the atomic old/new swap.

Same-membership movement only replaces current bounds/primary address and
increments a diagnostic; it does not erase or insert a bucket. Dirty roots use
one pre-reserved ObjectId vector plus an intrusive `Dirty` bit in the existing
root entry, so repeated transform signals coalesce and ordinary same-region
motion allocates no dirty tasks. Stale dirty handles validate through the full
ObjectId generation. Reparent/remove disconnects signals and incrementally
removes the old root; no raw Instance pointer is retained in a bucket.

## Candidate query and 3E separation

The query accepts at most four replication focus spheres and enumerates the
complete axis-aligned region envelope of each leave-radius sphere. Region
geometry is deliberately conservative. For a single focus, ordered `(X,Y,Z)`
ranges use lower-bound scans across only populated Z buckets; multiple focus
ranges use sorted region-address dedup. Object candidates use a 64-bit query
generation in the object entry, a reusable vector, and final ObjectId sort.
Multi-region roots and overlapping focuses therefore appear once.

The returned region list and ObjectId list are candidates only. 3E separately:

1. retains an already relevant root through the 320-unit leave radius;
2. admits a new candidate through the exact 256-unit enter radius;
3. pins the authoritative owner Character outside every region query;
4. includes global/session objects; and
5. hands the desired/required roots to `ReplicationCoordinator`, which alone
   adds ancestors and hard-reference dependency closure.

Thus a region boundary has no semantic enter/leave meaning. A relevant Part was
moved from region `(0,0,0)` to `(1,0,0)` in the integration test without a 3E
enter/leave counter change or structural operation. An actual 3E radius leave
still emits bounded `Unpublish`, and re-entry sends current complete state. A
global remote Player and its hard `Player.Character` shell can materialize even
when the Character is outside spatial candidates; nullable soft RootPart
references remain nil until their target becomes relevant. Region proximity
never creates an Instance or control grant.

Fresh bounded queries remain simpler than a per-peer region subscription and
measured adequately. Consequently 3H adds no per-peer candidate refcount,
region revision, subscription memory, callback fanout, or cache invalidation
state. The existing six-tick 3E refresh cadence and trusted focus dirtiness
remain authoritative.

## Limits and failure behavior

Production replication defaults and hard validation are:

| resource | limit |
| --- | ---: |
| region size | 128 units; accepted range 1 to 1,048,576 |
| spatial roots | 65,536 |
| populated regions | 262,144 |
| memberships | 262,144 |
| memberships per ordinary root | 64 |
| large roots | 1,024 |
| peer focus volumes | 4 |
| enumerated query regions | 4,096 total |
| query candidates / peer desired objects | 65,536 |
| candidate membership visits | 262,144 |
| peers | 512 |

The standalone generic index additionally caps test/benchmark configurations at
one million roots/regions, four million memberships/visits, 4,096 overlaps and
large roots, 64 volumes, 65,536 query regions, and one million candidates.
Configuration is native trusted server/session state, immutable for the index
lifetime, and unavailable to client packets or ordinary Luau.

A query that exceeds an enumeration, membership-visit, or candidate limit
returns an explicit failure and no partial candidate list. 3E preserves its
existing fail-closed session/resource behavior rather than silently truncating
potentially relevant objects. No region failure changes authoritative world
simulation. Aggregate saturating metrics expose regions, memberships, large
objects, moves, same-membership updates, queries, enumerated regions, membership
visits, candidates, dedup hits, query/candidate limit failures, bucket creation/
removal, occupancy, relevance transitions, and relevance CPU. There are no
coordinate or per-peer metric labels.

## Measured selection and scaling

The MSVC Release microbenchmark uses lightweight generation-bearing roots. Its
fixed-interest scene has 9,261 roots on a 64-unit lattice, 515 exact roots in a
320-unit sphere, and the remaining population far away. Each query is repeated
100 times after warmup. One local run measured:

| region | build 100k | regions | query regions | candidates | false positives | query |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 60.58 ms | 100,000 | 1,331 | 1,331 | 816 | 0.150 ms |
| 128 | 57.27 ms | 92,070 | 216 | 1,728 | 1,213 | 0.153 ms |
| 256 | 55.92 ms | 90,955 | 64 | 4,096 | 3,581 | 0.447 ms |
| 512 | 57.04 ms | 90,803 | 8 | 4,096 | 3,581 | 0.414 ms |
| 1,024 | 58.28 ms | 90,747 | 8 | 9,261 | 8,746 | 0.961 ms |

64 units slightly reduced candidates in this lattice, but used 1,331 regions
per focus, makes four disjoint current focus envelopes exceed the 4,096 query
bound, creates more buckets, and crosses boundaries more frequently. 256 units
and above increased false-positive filtering substantially. The existing
128-unit default therefore remains the best measured balance and avoids a
behavioral configuration migration.

At the selected 128 units, fixed-interest query cost remained effectively flat
as distant world population grew:

| roots | build | requested build bytes | query | query allocations | candidates |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 10,000 | 4.02 ms | 2,248,400 | 0.118 ms | 0 | 1,728 |
| 100,000 | 57.08 ms | 31,048,400 | 0.131 ms | 0 | 1,728 |
| 1,000,000 | 689.87 ms | 319,048,400 | 0.193 ms | 0 | 1,728 |

World construction remains intentionally linear and occurs once; peer query
work follows populated queried regions/candidates rather than total distant
objects. The million-root fixture is an index-only scale proof, not one million
heavyweight Parts or physics bodies.

For 10,000 roots, same-region movement took 1.61 ms with zero allocations and
10,000 same-membership updates. Crossing one boundary took 5.68 ms and 30,000
bounded allocator calls; teleporting directly to 10,000 distant final regions
took 4.84 ms and the same 30,000 calls, proving no distance/path traversal.
Dense lookup of 10,000 roots in one region took 0.833 ms and zero query
allocations. Four overlapping focuses took 0.290 ms for 2,592 candidates;
four disjoint focuses took 0.037 ms for 666. Both used preallocated scratch.

The dense build requested 2,000,120 bytes for 10,000 point roots. Combined with
the unique-region fixture, the MSVC allocator request sizes imply approximately
160 bytes per index object record, 40 bytes per point membership, and 120 bytes
per populated region node. This excludes allocator bookkeeping and the 3E
root/member/signal records. One million sparse point roots requested 319.0 MB.
Default reusable query capacity is about 640 KiB (4,096 32-byte addresses plus
65,536 8-byte ObjectIds); internal membership scratch is bounded by 64
addresses. Per-peer region subscription memory is zero.

The end-to-end one-peer fixed-interest admission fixture measured 1.39, 1.91,
and 1.72 ms at 1,000, 10,000, and 50,000 distant Parts while materializing the
same 16 objects. Allocator calls stayed at 14,120/14,288/14,288. Repeated
500-peer mixed stationary/moved/teleported-focus relevance cycles measured
1.97--2.29 ms, 10,830 allocations, 500 queries, 4,782 candidates, 300 enters,
and 940 leaves. This matches the pre-3H 2.176 ms CPU result while reducing the
reported 16,113 allocations.

The complete 500-peer admission fixture measured 5,302.46 ms and 67,609,981
allocator calls, compared with the same local pre-change run at 5,871.23 ms
and 67,611,524 calls. Its inclusive phase counters were 1,507.37 ms session
acceptance, 367.20 ms Player creation, 114.88 ms graph synchronization, 7.90 ms
baseline snapshot capture, 865.01 ms baseline discovery, 118.47 ms baseline
encoding, 123.33 ms gameplay registration, 12.46 ms relevance initialization,
35.09 ms relevance updates, and 2,994.96 ms materialization. These overlapping
counters identify structural discovery/materialization as the remaining scale
cost; they must not be summed into a separate wall-clock total.

## Verification and unchanged boundaries

`gargantuan_spatial_region_index_tests` covers origin/positive/negative and
near/exact boundaries, extreme finite coordinates, overflow/NaN/Inf, stable
hash/order/diagnostics, point and multi-region bounds membership, exact maximum
overlap, large-object fallback and limit, same-membership motion, boundary
crossing, teleport, invalid-update atomicity, remove/reuse generation safety,
overlapping/disjoint multiple focuses, corner/neighbor/vertical queries, dense
limits, deterministic candidate order, randomized point and bounds brute-force
zero-false-negative comparisons, and 100,000-region churn/reclamation.

`gargantuan_replication_relevance_tests` retains 3E owner/global/hard/soft
dependency, hysteresis, Player shell, NPC, two-peer differential, actual leave,
re-entry, and bounded structural transition coverage. It adds generic BasePart
bounds, large roots, same-region movement, still-relevant boundary crossing,
runtime Character movement, reparent, destroy, and consistency checks. Existing
GameSession and Character networking suites continue to cover cross-control,
stale generations/epochs, `FailPeer`, reconnect, 100-cycle lifecycle, GCHR
malformation, semantic action/teleport bypass, 3F cadence, and 3G scheduler
acceptance/fairness/budget behavior.

No GCHR, structural, handshake, package, persistence, authoring, or Remote wire
format changed. There is no new Luau callback or API. Client code has no region,
address, query, tier, budget, or candidate knowledge and cannot name a region to
request contents. Studio, Node, MCP, and Telemetry require no source change.
Renderer culling and physics broadphase remain non-authoritative consumers;
the index depends on neither and runs headlessly.

3H explicitly does not implement terrain/content paging, disk region manifests,
simulation LOD, PVS/occlusion, portals, region servers, cross-server travel,
region wire messages, region revisions, structural serialization caches,
runtime region-size rebuilds, or a game-facing spatial API.

Measured candidate discovery is now small and stable, while the admission
profile remains dominated by global Player/Character shells, dependency closure,
peer-view construction, and structural encoding. The next foundation should
therefore pursue **region-backed structural materialization optimization** only
where immutable template ownership and invalidation can be proven; adding a
public spatial API or production world streaming before that profile-driven
boundary would be premature.
