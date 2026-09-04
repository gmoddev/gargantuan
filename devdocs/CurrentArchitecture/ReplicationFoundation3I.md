---
status: current
owner: networking
last_verified: 2026-09-04
related_code:
  - include/gargantuan/network/Replication.hpp
  - include/gargantuan/network/ReplicationCoordinator.hpp
  - src/network/Replication.cpp
  - src/network/ReplicationCoordinator.cpp
  - src/network/ReplicationProtocol.cpp
  - src/network/ReplicaApplier.cpp
  - src/network/GameSession.cpp
  - tests/ReplicationTests.cpp
  - tests/GameSessionBenchmark.cpp
related_adrs:
  - docs/src/content/docs/developing/networking-architecture.mdx
---

# Replication foundation 3I

Foundation 3I reduces repeated structural materialization work without changing
relevance, authority, the GRPL wire format, or the reliable scheduler contract.
The server now owns one immutable, revisioned structural publication description
per authoritative object and lets many peer plans reference it. Each peer still
owns its own relevance, known-object state, sequence, materialization lifetime,
and final scheduler outcome.

```text
authoritative DataModel + scoped ChangeJournal revision
    -> immutable object StructuralMaterializationTemplate
    -> 3E peer selection and dependency closure
    -> small peer plan + soft-reference nil patches
    -> ordinary GRPL v1 Publish bytes
    -> reliable NetworkScheduler admission
    -> peer materialization commit or terminal peer failure
```

This is semantic reuse, not a cached final packet. The receiver still decodes an
ordinary GRPL v1 `Publish`; no template identifier, revision, relevance bit,
materialization epoch, or cache protocol crosses the wire.

## Pre-3I profile and allocation cause

Before 3I, `ReplicationCoordinator` already maintained one journal-refreshed
authoritative Snapshot catalog. Baseline discovery did not recapture the whole
DataModel for every peer. The dominant repeated work was later in the path:
`MakePublish(const SnapshotObject&)` deep-copied class/name strings, property and
Attribute maps, extension/custom-property maps, Tags, and nested string values
once for every peer-object relationship. The frame then encoded that copy and
the scheduler retained its own contiguous payload.

The isolated 500-peer 3H reproduction measured 4,859.56 ms, 67,609,981 allocator
calls, 820.005 ms baseline discovery, 2,720.293 ms materialization, 99.914 ms
baseline encode, and 1,439.499 ms session acceptance. Candidate relevance was
already small: 1,768 queries/candidates for 257,500 relevant objects. Allocation
sampling and source tracing therefore identified repeated peer `Publish`
construction, not 3H region lookup, GCHR, transport scheduling, or a fresh
per-peer world Snapshot, as the safe target.

## Structural field classification

The shared immutable description contains only authoritative, peer-independent
state:

- full generation-bearing `ObjectId`;
- class `SchemaId`, definition version, canonical class name, and Instance name;
- authoritative parent `ObjectId`;
- replicated native properties;
- Attributes, extension overrides, custom-class overrides, and Tags; and
- an estimated retained-byte count used only for bounded diagnostics.

The following remain outside the template because they are peer/session state:

- `ConnectionId`, desired/relevant/known-object membership, and transition
  backlog;
- GRPL frame epoch and reliable sequence;
- GCHR materialization epoch and Character control epoch;
- trusted `LocalPlayer` selection and accepted Player identity;
- soft-reference visibility substitutions for this peer;
- scheduler admission, transport delivery, and peer failure; and
- client interpolation, prediction, or Character publication tier/budget state.

`Player.Character` remains an authoritative replicated property in the Player
template and follows the same revision invalidation as every other replicated
reference. The peer's trusted `LocalPlayer` binding is GSES/GameSession metadata
and is never stored in a structural template. Runtime Character CFrame remains
GCHR-only and never enters the template or authoring journal.

## Granularity, identity, and revision

The granularity is one complete structural `PublishReplication` description per
authoritative object. A subtree or region-sized blob would force unrelated
objects to invalidate together and would make heterogeneous peer selections
expensive. Property fragments would add indirection and ordering complexity to
the already bounded maps. The object-sized boundary matches the protocol's
existing lifecycle operation and the coordinator's generation-safe catalog.

The key is:

```text
{ world/scope ObjectId, object ObjectId, structural ChangeJournal sequence }
```

Both ObjectIds include their generation. The world/scope prevents accidental
cross-world reuse. The structural revision is the last scoped journal sequence
that affected the object's replicated structural projection. During a full
resnapshot it is the captured cursor's preceding sequence. Modulo indexes or
pointer addresses do not participate. An object created after slot reuse has a
different generation and cannot match the retired object's key.

The existing runtime schema registry remains process-global and frozen; 3I adds
no second schema cache. Schema identity/version is retained in every template
and validated by the unchanged receiver compatibility manifest.

## Ownership and lifetime

One `ReplicationCoordinator`, owned by one server GameSession/world lifetime,
owns the ordered map of current template pointers. There is no process-static,
cross-session, cross-world, Studio, Node, MCP, or Telemetry cache. A prepared
frame holds a `shared_ptr<const StructuralMaterializationTemplate>` so replacing
the catalog entry cannot invalidate already-prepared work. The referenced
description and its string views stay alive until that frame is encoded or
discarded.

Templates are immutable after construction. The catalog retains only the
current live revision plus a destroyed object's final revision while an
existing peer still needs the normal terminal destroy transition. Replaced old
revisions survive only through bounded in-flight frame references; there is no
history, LRU, tombstone-value, or deferred-frame cache. `RemovePeer`, terminal
peer failure, GameSession `Stop`, world destruction, and coordinator destruction
release their owners normally.

## Transactional build and invalidation

`RefreshCatalog` reads the existing scoped `ChangeJournal`. Replicated property,
reparent, Attribute, extension, custom-property, Tag, create, and destroy records
mark the affected object. Non-replicated property records do not. Multiple
records for one object in one read collapse to its latest sequence and one
current Snapshot extraction.

All replacement templates for the read are built in a temporary ordered map.
Only after every extraction/allocation succeeds are current catalog pointers
replaced and new objects merged. Failure returns an error without publishing a
partially refreshed catalog or falling back to an older revision. A resnapshot
similarly builds a complete replacement map before swapping it into ownership.
This is a prepare/commit boundary for the catalog, not peer materialization.

Invalidation is precise to the affected object. Relevance enter/leave, a 3H
region crossing, focus movement, peer join/leave, and GCHR cadence/budget changes
do not invalidate authoritative structural state. A hierarchy or reference
mutation rebuilds the referrer/child whose encoded state changed, not an entire
region or subtree. Destroy retires the full generation-bearing identity. Reentry
and reconnect select the current revision and never replay an off-interest or
prior-session template.

## Peer materialization plans

3E continues to produce the peer selection. The coordinator expands ancestors
and schema-declared hard/non-nullable references exactly as before, orders
parents before children, and produces a small peer plan. Every planned publish
references the immutable template. Nullable soft references to objects outside
the resulting peer view are represented by sorted property-name patches.

The common zero-, one-, and two-patch cases use inline bounded storage. Larger
patch lists may allocate only for the overflow and remain bounded by the
Snapshot property ceiling. Encoding walks the authoritative map and writes nil
for a listed patch; it never mutates the template. Attributes, extension state,
and custom-class state retain the existing fail-closed reference validation.
Different peers can therefore encode different legal bytes from the same
template without sharing or modifying peer visibility state.

The final wire bytes remain peer-specific and are encoded once per coordinator
validation/submission path. They are not cached because frame epoch/sequence,
selection, soft-reference visibility, batch shape, and scheduler outcome differ.
The scheduler continues to own a copied contiguous payload after admission; it
never retains a pointer into a template or mutable DataModel object.

An internal constructor switch disables prepared-template reuse and materializes
ordinary owning `PublishReplication` intents. It is test/benchmark-only, is not a
host, client, protocol, or Luau setting, and preserves identical bytes. Template
build failure is a coherent resource failure; the coordinator never uses stale
data. The explicit uncached path proves that shared prepared intent is an
optimization rather than a wire-semantic dependency.

## Publication and failure transaction

The peer transaction remains:

```text
authoritative revision committed
    -> template/catalog refresh committed
    -> peer relevance/dependency plan prepared
    -> current template + peer patches encoded as GRPL v1
    -> reliable scheduler submission
    -> accepted: GameSession commits dependent Remote/GCHR visibility
    -> rejected: 3E.1 destroys the affected peer; unrelated peers/templates live
```

The coordinator's candidate view may advance while it constructs a frame, but a
live GameSession can observe that view only if scheduler admission succeeds. A
terminal reliable rejection runs `FailPeer`, so no surviving peer derives later
work from an undelivered cursor or known-object set. Multiple frames and peers
remain independent: acceptance for peer A cannot commit peer B. The scheduler
owns already-submitted bytes, so later template invalidation cannot rewrite an
accepted batch. Structural replication has no nonterminal partial reliable
batch commit.

`ReplicaApplier` never receives a prepared operation from the network. Direct
in-process tests normalize one through the production encoder/decoder first;
the decoded operation is the same ordinary `PublishReplication` a real client
receives.

## Memory and diagnostics

Persistent memory scales with live authoritative catalog objects plus live peer
views, not `peers × objects` template copies. The 500-peer fixture retained an
estimated 1,132,423 template bytes for 1,517 live descriptions (about 747 bytes
per description, excluding allocator bookkeeping). The old peer-side deep
copies were transient but produced much higher allocator pressure. A prepared
operation retains one shared pointer and an inline patch list only until frame
encoding; the existing variant's owning `PublishReplication` remains its largest
alternative. Peak measured operation-vector scratch was 745,472 bytes. There is
no per-peer template map or historical revision store.

Saturating aggregate diagnostics expose template builds, hits, misses,
invalidations, estimated live bytes, peer plans, peer patch operations,
reference patches, structural bytes encoded/reused, and scratch high-water
bytes. Existing metrics continue to own relevance transitions/backlog and
scheduler admission. No coordinate, ObjectId, player, or peer labels are added.

## Correctness evidence

The focused deterministic suite proves:

- two unchanged peers reference the same immutable object revisions and encode
  byte-identical baselines;
- a replicated mutation replaces only the current revision while an old
  prepared frame remains memory-safe and semantically unchanged;
- non-replicated `Archivable` mutation does not invalidate;
- hierarchy, Attribute, Tag, native reference, name, create/destroy, and
  reentry paths preserve existing complete-state semantics;
- hidden and visible peers encode nil/reference variants without template
  mutation or cross-peer identity leakage;
- the explicitly uncached path is byte-identical;
- 64 deterministic pseudo-random create/destroy, reparent, property,
  Attribute, Tag, native-reference, `Player.Character`, and heterogeneous
  relevance iterations remain byte-identical between prepared and uncached
  coordinators; and
- the existing malformed frame, hard dependency, reliable queue exhaustion,
  disconnect, reconnect, stale epoch/sequence, Remote dependency, Character,
  and GameSession tests remain authoritative.

3I adds no threading. Catalog refresh, plan construction, encoding, and
GameSession submission remain on the existing server safe point. Immutable
ownership would remain lifetime-safe if encoding moves later, but 3I does not
introduce parallel work.

## Performance evidence

The direct identical-world A/B fixture uses 500 peers and 128 unchanged Folder
objects plus the world root (64,500 publishes). Five-run local MSVC Release
medians were:

| path | total | per peer | allocations | baseline discovery | materialization |
| --- | ---: | ---: | ---: | ---: | ---: |
| shared prepared | 74.06 ms | 0.1481 ms | 1,633,129 | 54.83 ms | 71.05 ms |
| uncached owning | 109.50 ms | 0.2190 ms | 2,739,500 | 81.89 ms | 104.58 ms |

That is 32.4% less CPU and 40.4% fewer allocator calls with exactly identical
6,475,500 encoded bytes. The shared run built/missed 129 templates once, served
64,371 hits (99.8%), retained an estimated 34,067 bytes, and reported 16,999,433
reused structural bytes.

The mutation fixture used 256 peers, 128 objects, and one object mutation every
four peers. It built 192 templates (129 initial + 63 replacements), recorded 63
invalidations, served 32,832 hits, and completed 33,024 publishes in a median
37.18 ms / 837,603 allocations. Build work followed authoritative revisions rather than
the peer-object product.

The heterogeneous fixture used 500 peers, 4,096 objects, and 16 rotating objects
per peer. It published 8,500 objects in a median 16.52 ms / 174,797 allocations. It built
4,097 descriptions, then reused only the overlapping 4,403 requests; sparse
peer selection did not manufacture a dense peer-template matrix.

Across five end-to-end 500-peer runs, allocator count was deterministic at
53,441,661, down 14,168,320 (21.0%) from the reproduced 67,609,981 baseline.
Median total was 3,957.47 ms versus 4,859.56 ms before 3I. The median baseline
discovery phase was 594.82 ms (27.5% lower), median baseline encoding was
68.13 ms (31.8% lower), and median materialization was 2,158.16 ms (20.7% lower).
Total samples ranged 3,886.36--4,042.85 ms. The five-run mean was 3,969.17 ms;
the measurements support a material allocation reduction and a lower median
wall time without treating a single host run as a cross-platform guarantee.

That end-to-end run built 1,517 templates, recorded 1,513 first-use misses and
505,487 hits (99.70% hit rate), made 1,764 peer materialization plans, encoded
122,939,234 structural bytes, and reused an estimated 246,376,260 bytes of
description storage. No authoritative structure changed in this admission
fixture, so invalidations correctly remained zero.

## Security and API audit

Only authoritative DataModel state and its scoped journal can build or replace a
template. The client cannot name a template, revision, patch, object, peer, or
priority. Full ObjectId generations, frozen schema checks, 3E server relevance,
hard dependency closure, and receiver validation remain mandatory. Sharing is
read-only; peer-specific visibility never mutates shared state.

There is no GCHR/GRPL version change, client protocol field, ACK, ordinary Luau
API, callback, Studio surface, Node routing, MCP operation, or required
Telemetry dependency. Studio, Node, MCP, and Telemetry repositories are
intentionally unchanged. Offline runtimes with no GameSession construct no
coordinator/template state; headless server operation is unchanged.

## Explicit deferrals and next recommendation

3I does not cache final encoded frames, compress GRPL property names, implement
content paging, expose spatial/materialization controls, parallelize encoding,
or add a generic cache framework. Per-peer known/relevant sets, dependency
closure sets, frame buffers, reliable scheduler bytes, and receiver
materialization remain real peer-specific costs.

The next measured target should be **structural replication work budgeting**.
3I removed the unsafe reason to rebuild identical semantic descriptions, but a
synchronized dense enter still legitimately performs peer-specific closure,
encoding, and reliable submission. A future foundation can budget atomic
dependency groups without delaying mandatory owner/session state or changing
3E relevance. Production structural streaming should wait until such burst
control and package-backed availability have independent evidence.
