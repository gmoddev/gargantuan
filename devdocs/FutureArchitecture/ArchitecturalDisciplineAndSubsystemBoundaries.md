---
status: planned
authority: non-normative
owner: cross-repository-architecture
---

# Architectural Discipline and Subsystem Boundaries

## Status

Future architecture guidance, imported from the supplied comparative-review design.

Examples and proposed interfaces are non-normative; this document does not claim
that the proposed work is implemented or replace current contracts. The supplied
Atomic comparison is design provenance, not an independently reproduced audit.

As of the 2026-09-12 roadmap review, Foundation 3L is **partially ready** and 3M
remains blocked on its independent correctness, gameplay/client, overload,
journal-margin, scale/physics, and security gates. The GNS sanitizer gate is
closed; that does not close the foundation. See the
[3L validation ledger](../CurrentArchitecture/ContentAvailabilityFoundation3L_3Validation.md),
[roadmap evidence review](RoadmapEvidence20260912.md), and
[delivery roadmap](Roadmap.md).

This document records architectural improvements identified through comparative review of other engine implementations, particularly Atomic Game Engine, while preserving Gargantuan's existing semantic ownership model.

It is not an instruction to reproduce another engine's architecture.

The intended direction is:

> Borrow proven mechanical discipline where it strengthens Gargantuan's existing architecture without creating duplicate authorities, truth sets, lifecycle models, or resource schedulers.

The strongest identified opportunities are:

1. mechanically enforced subsystem dependency boundaries;
2. authenticated admission before authoritative gameplay allocation;
3. first-class CPU, allocation, and residency attribution;
4. explicit resource/service envelopes across every amplifying pipeline stage;
5. trusted spatial residency policy in Foundation 3M;
6. later semantic anti-entropy and networking hardening after Foundation 3L stabilization.

Several ideas that may appear to be missing features are already established Gargantuan architectural patterns and must not become duplicate systems.

These include:

- semantic DataModel authority;
- generation-safe lifecycle identity;
- detached preparation followed by validated publication;
- capability-based script authority;
- canonical runtime schema metadata;
- bounded Character prediction/reconciliation;
- authoritative Studio transaction history;
- renderer residency ownership;
- 3E peer relevance authority;
- 3J accepted structural materialization authority;
- Foundation 3L content acquisition/admission lifecycle;
- 3K semantic spatial state versus derived runtime projection.

The core architectural rule for future borrowing is therefore:

> **Borrow mechanical discipline; preserve semantic ownership.**

---

# 1. Architectural Principles

Gargantuan's existing ownership model remains normative.

Any future restructuring, optimization, or imported design must preserve the following boundaries.

---

## 1.1 Semantic authority

`Instance` / DataModel state remains authoritative semantic truth.

Indexes, caches, ECS-like stores, renderer state, physics participation, spatial projections, replication preparation, content caches, and other runtime structures remain derived or subsystem-owned representations.

A derived representation may:

- accelerate lookup;
- cache validated information;
- provide runtime participation;
- expose bounded diagnostic information;
- prepare future authoritative work.

It may not become semantic truth merely because it is faster or more convenient to query.

A performance optimization must not become a second authority.

---

## 1.2 Projection ownership

Derived runtime state remains owned by the subsystem that consumes it.

Examples:

```text
DataModel semantic spatial state
        ↓
SpatialRuntimeProjection
        ↓
SpatialRegionIndex
```

```text
DataModel renderable state
        ↓
renderer-neutral publication
        ↓
renderer residency
```

```text
accepted semantic replication state
        ↓
transport preparation
        ↓
backend queues
```

```text
content demand
        ↓
ContentAvailabilityService
        ↓
runtime residency
```

A projection may observe or derive from semantic state.

It may not mutate the semantic source merely because it owns a lower-cost representation.

---

## 1.3 Generation-safe identity

Reusable runtime identities must remain generation-safe.

A stale lifetime must never silently resolve to a replacement object.

This requirement applies to:

* Objects;
* Players;
* Characters;
* replication continuations;
* spatial runtime identities;
* content operations;
* worker results;
* future plugin/author leases;
* future topology/runtime projections.

Future region, networking, plugin, content, or authoring identities should reuse existing durable or generation-safe identity mechanisms wherever possible.

Do not create parallel string-key identity systems unless the persistent/project architecture demonstrates that existing identities are insufficient.

---

## 1.4 Single authoritative commit boundary

Fallible or asynchronous work may prepare detached results away from the authoritative owner.

Authoritative mutation still occurs through an explicit validated commit boundary.

Worker completion order is not authoritative order.

The intended shape remains:

```text
bounded input / authoritative snapshot
        ↓
detached or read-only preparation
        ↓
complete validation
        ↓
revision/lifecycle validation
        ↓
explicit authoritative commit
```

---

## 1.5 Relevance authority

Foundation 3E remains the semantic peer-relevance authority.

Future systems may provide:

* topology candidates;
* spatial candidates;
* residency demand;
* predicted movement corridors;
* visibility hints;
* content availability information.

Those systems do not create a competing peer-relevance set.

---

## 1.6 Materialization authority

Foundation 3J remains the authority for accepted peer structural materialization and `Known` state.

Future:

* transport admission;
* anti-entropy;
* residency policy;
* portal traversal;
* content acquisition;
* planning caches;
* replication diagnostics

must not directly mark peer state `Known`.

`Known` remains acceptance-only.

---

## 1.7 Failure atomicity

A failed fallible boundary must not leave false authoritative state behind.

Where an operation is defined as dependency-complete or correctness-atomic, all required semantic work must be:

1. discovered;
2. validated;
3. costed;
4. accepted as one valid group.

Existing KI-007 behavior is an important example:

```text
required reference clear/replace
        +
target removal
        =
one fully charged correctness group
```

The system must not publish an invalid prefix merely because a resource limit was reached.

---

## 1.8 Independent resource envelopes

A bound on one resource must not be treated as a bound on another.

For example:

```text
bounded candidate count
≠
bounded planning work
```

```text
bounded planning work
≠
bounded structural operations
```

```text
bounded operations
≠
bounded encoded bytes
```

```text
bounded encoded bytes
≠
bounded backend queue residence
```

```text
bounded backend queue residence
≠
bounded client application work
```

```text
bounded client application work
≠
bounded cleanup/recovery work
```

Every stage capable of amplifying work must expose its own finite service/resource envelope.

---

## 1.9 Capability authority

Runtime domains, script names, plugin names, client-supplied fields, network claims, provenance metadata, or package identifiers cannot manufacture authority.

Capabilities remain host-created and explicitly granted.

Future plugin/project trust extends the existing capability model rather than replacing it.

---

## 1.10 Transport separation

GameNetworkingSockets or another backend does not define:

* Character semantics;
* Remote semantics;
* replication semantics;
* DataModel semantics;
* gameplay authority.

Transport configuration and service policy may constrain delivery, but semantic ownership remains above the backend.

---

# 2. Mechanically Enforced Module Boundaries

## 2.1 Motivation

Gargantuan currently documents several subsystem boundaries more strongly than the build graph mechanically enforces them.

Large portions of engine implementation remain reachable through broad core compilation targets.

That allows accidental dependency violations to remain legal C++ even when they violate documented architecture.

The long-term goal is:

> Important architectural dependency violations should become build or CI failures rather than review discoveries.

This is the strongest architectural discipline identified through the Atomic comparison.

---

## 2.2 Do not begin with a full target rewrite

Do not immediately split the engine into a large number of physical libraries.

That would create substantial mechanical churn involving:

* source lists;
* include paths;
* transitive dependencies;
* test linkage;
* compile definitions;
* export visibility;
* build ordering.

Begin by measuring and enforcing the current dependency graph.

Suggested progression:

```text
Architecture 0A
    dependency graph extraction

Architecture 0B
    declared allowed/forbidden edges

Architecture 0C
    CI enforcement

Architecture 0D
    low-risk physical target extraction

Architecture 0E
    progressive subsystem isolation
```

This allows architectural rules to become enforceable before physical decomposition is complete.

---

## 2.3 Dependency graph extraction

The first phase should determine the current architecture mechanically.

Potential sources include:

* target ownership;
* include relationships;
* linked targets;
* generated source dependencies;
* public/private header use;
* external dependency leakage.

The output should be machine-readable.

For example:

```text
runtime_semantic
    -> foundation
    -> schema

network_runtime
    -> runtime_semantic
    -> network_protocol

network_gns
    -> network_runtime
    -> GameNetworkingSockets
```

The initial CI mode may report violations before enforcing them.

---

## 2.4 Candidate long-term module bands

A possible destination is:

```text
gargantuan_foundation

gargantuan_runtime_semantic

gargantuan_script_api
gargantuan_script_luau

gargantuan_spatial_semantic
gargantuan_spatial_runtime

gargantuan_network_protocol
gargantuan_network_runtime
gargantuan_network_gns

gargantuan_render_publication
gargantuan_render_sdl

gargantuan_editor_host
```

These names are illustrative rather than normative.

The actual dependency graph and ownership should determine the final physical targets.

---

## 2.5 First enforceable prohibitions

Initial architecture CI should reject at least:

```text
runtime semantic -> Studio/editor host
```

```text
runtime semantic -> renderer backend
```

```text
runtime semantic -> GameNetworkingSockets
```

```text
network protocol -> GameNetworkingSockets
```

```text
network protocol -> Studio
```

```text
render publication -> SDL/GPU backend
```

```text
script semantic API -> Luau C API
```

```text
spatial semantic types -> spatial acceleration implementation
```

Product hosts may compose lower layers.

Lower subsystem layers may not depend upward on product hosts.

---

## 2.6 Recommended first physical extractions

Implementation note from the 2026-09-12 review: `CMakeLists.txt` already defines
`gargantuan_gns_transport` and separate runtime/player/server host targets.
Treat the GNS candidate below as a boundary/transitive-dependency audit and
incremental tightening, not a request to create a duplicate adapter library.
Neutral protocol/publication extraction and a complete forbidden-edge graph
remain future work.

After graph enforcement is useful and stable, prioritize clean leaf boundaries.

Recommended first candidates:

```text
network protocol / codecs
```

```text
GameNetworkingSockets adapter
```

```text
renderer-neutral publication
```

These provide clear architectural benefit because the desired dependency direction is already obvious.

For example:

```text
network protocol
    X-> GameNetworkingSockets
```

and:

```text
render publication
    X-> SDL renderer backend
```

are easy rules to understand and enforce.

---

## 2.7 Build architecture acceptance

A subsystem-boundary milestone should prove:

* machine-readable intended dependency graph;
* actual graph extraction;
* forbidden-edge CI;
* no vendor/backend leakage across neutral interfaces;
* product hosts compose downward;
* lower layers do not depend on product hosts;
* target extraction does not change runtime semantics.

The purpose is architecture enforcement, not faster compilation alone.

---

# 3. Authenticated Admission Before Gameplay Authority

## 3.1 Motivation

Local development transport establishment is not Internet account authentication.

Before public untrusted deployment, Gargantuan requires an admission boundary between:

```text
transport connectivity
```

and:

```text
authoritative gameplay allocation
```

The invariant is:

> An unauthenticated transport peer may own bounded transport/admission state, but it owns no gameplay authority.

---

## 3.2 Pre-admission state

Before successful admission there must be no:

```text
authoritative Player
Character control
3E relevance peer
3J Known/materialization state
Remote gameplay peer
content pin
gameplay capability
```

Bounded transport state may necessarily exist because GNS owns a connection.

Bounded session/admission bookkeeping may also exist to exchange admission messages.

Those are not gameplay authority.

---

## 3.3 Proposed lifecycle

```text
GNS connection established
        ↓
bounded PendingAdmission
        ↓
GSES protocol compatibility
        ↓
server/session identity validation
        ↓
join-grant validation
        ↓
authenticated principal
        ↓
GameSession peer acquisition
        ↓
authoritative Player
        ↓
baseline / relevance / materialization
        ↓
Remote + Character
        ↓
Ready
```

---

## 3.4 Join-grant ownership

The external control plane should establish identity and authorization.

It should not become authoritative for engine protocol semantics unnecessarily.

A join grant should minimally bind:

```text
target server/session
authenticated subject
nonce/replay identity
issued time
expiry
experience/session authorization
signature
```

GSES should continue independently validating:

```text
engine protocol compatibility
schema compatibility
build compatibility
session expectations
```

unless a future architecture explicitly requires coarse compatibility information to be signed into the grant.

---

## 3.5 Cryptography

Do not invent bespoke application cryptography merely because another implementation uses a custom challenge protocol.

Use established mechanisms for:

* signatures;
* short-lived grants;
* key rotation;
* expiry;
* replay protection.

The engine should consume a trusted validation interface rather than own an unnecessary general identity platform.

---

## 3.6 Failure behavior

A rejected grant produces:

```text
TransportConnected
        ↓
AdmissionRejected
        ↓
close peer
```

and explicitly:

```text
NO Player
NO Character
NO 3E peer
NO 3J state
NO Remote registrations
NO gameplay capability
```

---

## 3.7 Roadmap condition

Authenticated admission is:

> **P0 before public Internet hosting.**

It is not inherently a prerequisite for Foundation 3M or private/local development.

---

# 4. Per-Stage Resource and Service Envelopes

Foundation 3L demonstrated that bounding final output is insufficient.

A pipeline may still fail because an earlier or later stage performs unbounded work.

A standing architecture review must therefore examine:

```text
discover
    ↓
derive
    ↓
plan
    ↓
prepare
    ↓
encode
    ↓
scheduler admission
    ↓
transport admission
    ↓
backend queue / service
    ↓
client validation / application
    ↓
cleanup / recovery
```

---

## 4.1 Required questions per stage

For each stage answer:

```text
What is the resource unit?
```

```text
What is the finite bound?
```

```text
Who owns the bound?
```

```text
How is overload represented?
```

```text
How is fairness maintained?
```

```text
How is progress guaranteed?
```

```text
What retained memory can accumulate?
```

```text
What happens after overload ends?
```

---

## 4.2 Example from Foundation 3L

Foundation 3L established several independently necessary bounds:

```text
pre-3J planning
    65,536 charged planning steps
```

```text
3J selection
    8,192 accepted structural operations
```

These did not bound:

```text
encoded reliable bytes
```

The reliable transport investigation then demonstrated that bounded structural operations could still generate enough reliable bytes to create multi-second GNS FIFO backlog.

That led to an independent transport byte service envelope.

This distinction should become permanent architecture policy.

---

## 4.3 Overload is part of the contract

A bounded system must define behavior when incoming demand exceeds sustainable service.

Valid overload behavior may include:

* bounded deferral;
* bounded pending state;
* bounded rejection;
* explicit degraded compatibility;
* backpressure;
* finite recovery.

It must not silently become:

* unbounded queue growth;
* unbounded memory;
* starvation;
* false semantic progress.

---

## 4.4 Legal input versus qualified service

A parser ceiling defines legal representation, not supported production demand.
Legal input remains subject to authority, lifecycle and resource admission.

A qualified service claim must identify the complete workload: encoded message
sizes, request/response combinations, arrival count and byte rates, finite burst
and replenishment windows, concurrency, active peers and recipient fanout. It
must also identify the deployment capacity and host/path assumptions that fund
that workload in each direction. Engine-generated control traffic shares those
finite resources and cannot be omitted from the accounting.

Overload is demand outside that accepted envelope or above sustainable service.
Its ordinary latency targets may lapse, but bounded resource use, explicit
failure, correct ownership and defined recovery remain required. A workload
cannot be relabeled overload after failing a qualified service test.

Product/platform owners select the supported application behavior; subsystem
owners cost and validate it, and trusted host/operator policy funds it. Codec
maxima, denial ceilings, reserve percentages and passing microbenchmarks cannot
substitute for that decision. Missing requirements must remain explicit before
fixtures or implementation turn them into an accidental product promise.

---

# 5. Parallel Derivation Policy

## 5.1 General rule

Parallelism should be introduced only after:

1. unnecessary work has been removed;
2. work is independently bounded;
3. the read/ownership contract is explicit;
4. serial versus parallel crossover has been measured.

Do not parallelize an inefficient algorithm merely because worker threads are available.

---

## 5.2 Preferred execution model

Where worker execution is justified:

```text
coherent authoritative revision
        ↓
bounded read-only worker derivation
        ↓
private worker result
        ↓
revision / lifecycle validation
        ↓
serialized authoritative acceptance / commit
```

Worker completion order is not semantic order.

---

## 5.3 Spatial/relevance prerequisite

Production worker-side 3E derivation requires an immutable or pinned read contract for 3H/3K spatial state.

A possible model:

```text
Main:
    complete spatial refresh
    establish read epoch
    capture bounded peer inputs

Workers:
    independent query scratch
    derive relevance candidates

Main:
    validate peer identity
    validate revision
    deterministic install

Barrier:
    release read epoch
```

Current spatial query implementations that mutate shared deduplication/query state cannot simply be called concurrently.

---

## 5.4 Current priority

The immutable read-epoch concept remains useful future infrastructure.

Actual 3E jobification is conditional.

Foundation 3L attribution found limited Amdahl headroom relative to the larger delivery/service costs under investigation.

Therefore:

> 3E jobification is not a prerequisite for Foundation 3L closure or Foundation 3M.

Revisit it when measurements demonstrate sufficient benefit.

---

# 6. CPU, Allocation, and Residency Attribution

Wall-clock timing alone is insufficient for many engine scaling failures.

Future profiling should correlate bounded work scopes with resource consumption.

Possible work categories:

```text
ReplicationRelevance
ReplicationPlanning
ReplicationEncoding
ReplicationAdmission

ContentDecode
ContentMaterialize

SpatialQuery

PhysicsStep

RenderExtraction

AssetDecode
AssetUpload
```

Use bounded static or interned tags.

Never create profiler tag identities from:

* ObjectId;
* Player ID;
* peer ID;
* content key;
* asset name;
* arbitrary user string.

---

## 6.1 Stage 1 — subsystem-owned accounting

Begin with explicit subsystem facts.

Examples:

```text
planner retained bytes
```

```text
journal bytes
```

```text
content cache bytes
```

```text
transport queued bytes
```

```text
asset canonical bytes
```

```text
decoded CPU bytes
```

```text
GPU residency
```

```text
physics residency
```

These are easier to make accurate because the owning subsystem already knows the resource.

---

## 6.2 Stage 2 — allocator integration

Where allocator architecture permits it cleanly, `RuntimeWorkScope` or equivalent may gather:

```text
CPU duration
allocation count
allocated bytes
freed bytes
live-byte delta
peak live bytes
```

Do not begin with invasive global allocator interception solely for profiling.

---

## 6.3 Stage 3 — worker attribution

Worker execution should preserve bounded attribution identity.

Example:

```text
owner captures WorkTag
        ↓
job executes under tag
        ↓
bounded aggregate produced
        ↓
owner merges result
```

Do not require persistent per-job trace history merely to attribute aggregate resource usage.

---

## 6.4 Telemetry boundary

Detailed runtime allocation/profiling information is a local diagnostic surface by default.

Do not automatically publish it to Gargantuan Telemetry.

External telemetry remains a separate privacy, security, and deployment contract.

---

# 7. Asset Residency Diagnostics

Asset Foundation 2B should establish measurable residency before selecting normative eviction policy.

Existing subsystem ownership remains:

```text
AssetService
    canonical/cache facts

renderer
    GPU residency

physics
    collision residency

animation
    animation-runtime residency

EditorHost / profiler
    bounded joined diagnostic projection
```

The profiler owns none of those underlying truths.

---

## 7.1 Example diagnostic record

```cpp
struct AssetResidencySnapshot {
    AssetId Asset;
    uint64_t ContentRevision;

    uint64_t CanonicalBytes;
    uint64_t DecodedCpuBytes;
    uint64_t GpuBytes;

    uint32_t RenderConsumers;
    uint32_t PhysicsConsumers;
    uint32_t AnimationConsumers;

    uint32_t PinCount;

    uint64_t LastUseEpoch;
    uint64_t Loads;
    uint64_t Evictions;

    AssetResidencyState State;
};
```

Exact fields should follow the eventual Asset 2B implementation.

---

## 7.2 Studio view

Potential Studio diagnostics:

| Asset | Canonical | CPU decoded | GPU | Render users | Physics users | Pins | Last use | Loads | Evictions |
| ----- | --------: | ----------: | --: | -----------: | ------------: | ---: | -------: | ----: | --------: |

Package-level aggregation may expose:

```text
Package
    Meshes       180 MiB
    Textures     410 MiB
    Animations    34 MiB
    Audio         92 MiB
```

These remain diagnostic projections.

---

## 7.3 Asset 2B acceptance

Before a general eviction policy becomes normative, Asset 2B should demonstrate:

* canonical asset memory attribution;
* decoded CPU attribution;
* GPU attribution;
* consumer/pin counts;
* load/eviction counts;
* last-use information;
* bounded profiler storage.

Eviction policy should follow measurement rather than precede it.

---

# 8. Foundation 3M — Trusted Spatial Residency Policy

## 8.1 Purpose

Foundation 3M owns:

> **Why content should be resident now.**

Foundation 3L owns:

> **How content is acquired, verified, staged, admitted, retained, and evicted.**

Foundation 3E owns:

> **Which semantic objects are relevant to a peer.**

Foundation 3J owns:

> **Which relevant structural changes have been accepted for the peer.**

3M must not duplicate any of those authorities.

---

## 8.2 Policy pipeline

The intended shape is:

```text
trusted gameplay / spatial focus
        ↓
bounded coarse content query
        ↓
ResidencyDemand candidates
        ↓
semantic policy class
        ↓
deadline
        ↓
distance / cost
        ↓
stable deterministic tie
        ↓
bounded request / release delta
        ↓
ContentAvailabilityService
        ↓
existing 3L lifecycle
```

3M generates demand.

It does not directly load content.

It does not directly instantiate semantic content.

It does not mark a peer relevant.

It does not mark a peer Known.

---

## 8.3 Trusted focus sources

Potential trusted focus sources include:

* authoritative Player/Character position;
* accepted movement intent;
* server-authorized camera/view interest where applicable;
* travel destination;
* server gameplay objective;
* portal/topology traversal;
* server-owned AI focus.

Client or Luau input does not automatically become trusted residency authority.

---

## 8.4 Priority semantics

The core rule is:

> Semantic importance may outrank pure geometric distance.

For example:

```text
collision/gameplay content
100 m ahead in the accepted movement corridor
```

may outrank:

```text
decorative presentation content
10 m away
```

Potential internal policy categories might include:

```text
Critical
Gameplay
Predictive
Presentation
Speculative
```

More specific reasons such as:

* collision safety;
* bootstrap;
* travel destination;
* audio;
* imminent movement

may be represented as policy metadata.

Avoid prematurely freezing a large public serialized priority enum.

---

## 8.5 Deadlines

Demand may carry deadlines where semantically justified.

Examples:

```text
must be available before accepted travel completes
```

```text
should be available before movement corridor reaches region
```

Deadline is an ordering input, not a guarantee that impossible capacity can be exceeded.

---

## 8.6 Hysteresis

Residency demand should not flap at spatial boundaries.

Use separate entry and exit criteria where applicable.

Conceptually:

```text
enter volume
leave volume > enter volume
```

A bounded grace period may also be appropriate.

---

## 8.7 Lookahead

Movement prediction may inform content demand.

Lookahead must remain:

* bounded;
* derived from trusted movement state;
* non-authoritative;
* finite in spatial range/time horizon.

Speculative lookahead does not make content semantically required.

---

## 8.8 Policy revisions

Asynchronous policy work must be revision-stamped.

Example:

```text
revision 41 query begins

focus changes

revision 42 becomes current

revision 41 completes

discard revision 41
```

Do not rely on perfect cancellation timing.

This extends existing generation/revision-safe patterns.

---

## 8.9 Do not duplicate 3L lifecycle

Foundation 3L already owns content state such as:

```text
Unavailable
Requested
Acquiring
Available
Admitting
Resident
Evicting
Failed
```

Foundation 3M must not create another lifecycle over the same content units.

3M should retain only policy state necessary to represent demand.

A possible shape:

```cpp
struct ResidencyDemand {
    ContentKey Content;
    ResidencyPriority Priority;
    Deadline Deadline;
    PolicyRevision Revision;
};
```

If additional state is required, keep it minimal and policy-local.

---

## 8.10 Demand cancellation

When policy no longer desires content:

```text
Desired
    ↓
Grace
    ↓
NoDemand
```

may be preferable to immediate release where hysteresis is required.

3L remains responsible for actual eviction state and lifecycle.

---

## 8.11 Spatial query requirements

3M should avoid:

```text
for each focus:
    scan entire content manifest
```

Instead use bounded coarse lookup/index structures.

Normative requirements:

* bounded cells/regions examined;
* bounded candidate count;
* bounded demand delta;
* bounded retained policy records.

---

## 8.12 Determinism

Equivalent inputs must produce stable ordering.

Potential ordering:

```text
priority class
    ↓
deadline
    ↓
distance / cost
    ↓
stable content identity
```

Do not allow unordered-container iteration to determine publication/request priority.

---

## 8.13 Foundation 3M acceptance

Representative scale testing should include increasing focus and content counts.

Example stress points may include:

```text
1 focus
10 focuses
100 focuses
500 focuses
```

against:

```text
1K content units
10K content units
65,536 content units
```

Those values are representative stress fixtures unless separately adopted as product limits.

Normative acceptance properties:

* no full manifest scan per focus;
* bounded query work;
* bounded candidate generation;
* bounded request/release delta;
* bounded retained policy memory;
* deterministic priority/tie ordering;
* hysteresis;
* stale-revision rejection;
* exactly one provider acquisition under equivalent demand;
* current 3L dependency safety preserved;
* Character/Remote service envelope preserved;
* no raw content authority granted to client/Luau;
* policy cannot mark peer state Known;
* policy cannot create semantic Instance truth.

---

# 9. Future SpatialRegion Architecture

## 9.1 Semantic identity is distinct from runtime spatial identity

A future `SpatialRegion` is semantic DataModel/schema state.

`SpatialSpaceId` remains an internal runtime projection identity.

Relationship:

```text
SpatialRegion semantic identity
        ↓
runtime topology projection
        ↓
SpatialSpaceId
        ↓
3K/3H runtime structures
```

Do not expose or persist `SpatialSpaceId` merely because semantic regions become public.

---

## 9.2 Avoid duplicate durable identity

Do not automatically introduce:

```text
SpatialRegion string key
+
SpatialRegion ObjectId
+
SpatialSpaceId
```

as three parallel identities.

Prefer the existing persisted object/reference model for semantic region identity.

Introduce a region-specific durable key only if persistent architecture demonstrates a need that ordinary object references cannot satisfy.

---

## 9.3 Region state remains decomposed

Never introduce a universal semantic property such as:

```cpp
SpatialRegion.Loaded = true;
```

because the following are distinct:

```text
semantic region existence
content bytes available
Instances resident
simulation active
peer relevance
peer structural materialization
physics readiness
renderer residency
audio readiness
```

Existing subsystem owners remain authoritative for their respective state.

---

## 9.4 Semantic region membership

A semantic spatial root may have an authoritative current region relationship.

Conceptually:

```text
CurrentSpatialRegion
```

That represents topology/simulation ownership.

It is distinct from content provenance and runtime coverage.

---

## 9.5 Content provenance

Where content was authored or packaged is not necessarily the same as where the object currently exists physically.

Keep provenance separate from current spatial membership where needed.

---

## 9.6 Coverage

Spatial/query/render coverage should be derived where possible.

Do not automatically add another mutable authoritative `Coverage` property.

Make coverage authored semantic state only where creators require explicit semantic control.

---

# 10. Portals and Shared Topology

Future portals/links should provide bounded topology candidates.

They should not own independent relevance or residency truth.

---

## 10.1 Residency use

```text
SpatialPortal / SpatialLink
        ↓
bounded topology traversal
        ↓
candidate regions/content
        ↓
3M demand
```

3M decides demand policy.

3L owns content lifecycle.

---

## 10.2 Networking use

```text
portal/topology candidate set
        ↓
3E relevance
        ↓
3J materialization
```

The portal never marks peer objects relevant or Known directly.

---

## 10.3 Rendering use

```text
portal topology
        ↓
bounded render traversal
        ↓
renderer publication/view work
```

The renderer consumes topology but does not own semantic topology.

---

## 10.4 Consumer-specific budgets

Do not define one universal:

```text
MaxPortalDepth
```

as the complete resource contract.

Different consumers require independent envelopes:

```text
RenderPortalBudget
ReplicationPortalBudget
SpatialQueryTraversalBudget
AudioTraversalBudget
InteractionTraversalBudget
AIVisibilityTraversalBudget
```

Rendering pixels, replication bytes, spatial queries, and audio graph traversal are different resource domains.

Shared topology does not imply shared service limits.

---

# 11. Studio Authoring Provenance

Gargantuan already has authoritative bounded transaction history.

Do not create another command/history architecture.

Future work should extend existing transaction history with engine-assigned mutation provenance.

---

## 11.1 Proposed provenance

Potential categories:

```cpp
enum class AuthorSource {
    LocalUser,
    Plugin,
    Agent,
    Importer,
    RemoteCollaborator,
    SystemMigration
};
```

Potential additional identity:

```text
AuthorLeaseId
```

where the lease is host-created and generation-safe.

Caller-supplied provenance does not grant capability.

---

## 11.2 History presentation

History may expose:

```text
Move Part                      LocalUser
Generate marketplace           Agent
Reimport character.glb         Importer
Set material defaults          Plugin
```

This is diagnostic/authoring provenance over one authoritative history.

---

## 11.3 Existing transaction semantics

The current model is commit-only grouping.

It is not a general rollback transaction.

If an external author:

```text
BeginRecording
apply valid mutations
fails or disconnects
```

already committed valid mutations remain authoritative.

Plugin-facing documentation must not call this model atomic rollback.

---

## 11.4 Future truly atomic external-author transaction

If required later, a different mechanism would be needed:

```text
external operations
        ↓
bounded staged candidate
        ↓
complete semantic preflight
        ↓
single authoritative commit
```

This should be designed only if a real plugin/agent workflow requires it.

Do not retrofit false rollback semantics onto the existing history API.

---

## 11.5 Undo

Maintain one authoritative history.

Do not silently skip later foreign edits when the user requests undo of an older local edit.

Selective undo beneath subsequent mutations can violate:

* dependencies;
* references;
* object lifetime;
* ordering assumptions.

Collaborative/selective undo is separate future architecture.

---

# 12. Capability and Plugin Trust

Capability-based authority already exists.

Do not create another permission framework.

Future Studio/plugin work should extend the existing capability system with:

```text
plugin grants
scoped capabilities
project trust
consent
revocation
persistent grant policy
```

A plugin name, package ID, script domain, author provenance, or project filename does not itself grant authority.

---

## 12.1 Project trust

Persistent project trust must be:

* explicit;
* scoped;
* revocable;
* host-owned.

Do not make trust transitive merely because one trusted plugin loads another component.

---

## 12.2 Agent authority

Agents should use the same capability framework as other external authors.

Do not create privileged `Agent` authority merely because an operation originates from an AI workflow.

Provenance and capability remain distinct.

---

# 13. Canonical Schema and API Metadata

`RuntimeSchemaRegistry` remains the canonical schema source.

Do not create a second scripting metadata registry.

Future API stabilization should audit whether the following surfaces derive from canonical metadata:

```text
properties
methods
signals
service APIs
arguments
return values
capability/domain requirements
documentation
Luau types
MCP introspection
```

Where gaps exist, extend the existing registry/class-generation pipeline.

The goal is:

```text
one canonical API/schema source
        ↓
runtime
Studio
documentation
Luau tooling
MCP/tooling
```

not parallel handwritten registries.

---

# 14. Prediction and Reconciliation Reuse

Character already implements the important bounded prediction/reconciliation behavior.

Do not build a generic prediction framework merely for architectural symmetry.

Extract reusable machinery only when a second real consumer exists.

Likely future consumers might include:

* vehicles;
* another locally controlled actor type;
* other authoritative movement systems.

---

## 14.1 Reusable primitive scope

A reusable primitive might own:

```text
bounded sequence history
acknowledgement retirement
pending iteration
reset
```

For example:

```cpp
template<typename Entry, std::size_t Capacity>
class ReplayWindow {
public:
    RecordResult Push(Sequence, Tick, Entry);
    AckResult AcknowledgeThrough(Sequence);

    std::span<const Entry> Pending() const;

    void Reset();
};
```

It should not know about:

* Character;
* physics;
* ObjectId;
* transport;
* animation;
* smoothing.

---

## 14.2 Overflow remains consumer policy

Character's current policy remains:

```text
prediction lineage overflow
        ↓
invalidate lineage
        ↓
suspend prediction
        ↓
wait for authoritative baseline
```

Do not replace this with:

```text
drop oldest and continue
```

unless a future consumer explicitly supports incomplete replay lineage.

---

# 15. Semantic Anti-Entropy

Semantic anti-entropy is later networking hardening.

It is not part of Foundation 3L closure.

Potential audit domains:

```text
StructuralIdentity
AuthoritativeProperties
AuthoritativeReferences
CharacterAuthoritativeBaseline
```

Only audit state both endpoints should already semantically agree upon.

---

## 15.1 Explicit exclusions

Do not audit derived state as semantic truth.

Exclude:

```text
SpatialCellAddress
spatial acceleration buckets
3K dirty state
renderer residency
GPU state
predicted pose
visual animation pose
interpolation buffers
transport sequence state
backend queue state
```

---

## 15.2 Eligibility

Only audit an object/group when:

```text
peer accepted lifetime matches
AND
no structural group affecting it is awaiting acceptance
AND
the chosen representation is receiver-normalized
```

This avoids false divergence while normal replication is still in progress.

---

## 15.3 Mismatch behavior

A mismatch should result in:

```text
semantic digest mismatch
        ↓
bounded recovery requirement
        ↓
normal planner / relevance validation
        ↓
normal complete 3J recovery group
        ↓
scheduler acceptance
        ↓
Known advances normally
```

Anti-entropy does not mutate `Known` directly.

---

## 15.4 Priority

This is a P2 networking-hardening feature unless production evidence demonstrates unexplained divergence that existing recovery cannot address.

It should follow reliable-service stabilization.

---

# 16. Coverage-Guided Fuzzing

Fuzzing should expand by trust/risk boundary rather than through one monolithic "Fuzz Foundation."

Existing fuzzing should be extended systematically.

Priority surfaces include:

```text
network protocol codecs
content manifest / unit parser
snapshot / journal codecs
project persistence
package / artifact formats
bounded asset importers
future admission grant parser
future SpatialRegion / topology formats
```

---

## 16.1 Priority order

Risk-weight fuzz effort.

Prefer externally supplied and persistent boundaries first:

```text
network
content
snapshot/journal
project persistence
package/artifact
asset importer
future auth tickets
future topology
```

---

## 16.2 Seed corpora

Seed corpora should include:

* valid protocol examples;
* historical regression vectors;
* minimum legal input;
* maximum legal input;
* known malformed cases;
* previous crash inputs.

Run under sanitizer-enabled configurations where practical.

---

# 17. Explicitly Rejected or Deferred Directions

The following should not be borrowed merely because another engine uses them.

| Direction                                        | Gargantuan decision | Reason                                      |
| ------------------------------------------------ | ------------------- | ------------------------------------------- |
| ECS as complete semantic truth                   | Reject              | Conflicts with DataModel authority          |
| Grid/cell as semantic space authority            | Reject              | Grid/index remains acceleration             |
| Multiple script VMs without demonstrated need    | Reject/defer        | Large lifecycle/security/tooling surface    |
| JavaScript/TypeScript runtime                    | Reject/defer        | No demonstrated requirement                 |
| C# runtime                                       | Reject/defer        | Same                                        |
| Renderer-owned semantic portals                  | Reject              | Topology must remain semantic/shared        |
| One-world physics assumption baked into topology | Avoid               | Future regions may require isolated physics |
| Giant compositor/material graph immediately      | Defer               | Renderer-neutral semantics first            |
| Generic node canvas as runtime primitive         | Defer               | Studio infrastructure later                 |
| Full DAW immediately                             | Defer               | Runtime audio architecture first            |
| Native `.blend` parser                           | Avoid               | Prefer exporter/intermediate representation |
| Figma/Photoshop import immediately               | Defer               | Target GUI/asset semantics first            |
| Full terrain authoring suite immediately         | Defer               | Runtime representation/streaming first      |
| AI/model-provider authority in gameplay core     | Reject              | Provider must not become semantic authority |
| Second schema registry                           | Reject              | Duplicate truth                             |
| Second capability model                          | Reject              | Duplicate authority                         |
| Second 3M content lifecycle                      | Reject              | 3L already owns lifecycle                   |
| Second peer relevance/materialization authority  | Reject              | 3E/3J already own them                      |

---

# 18. Roadmap Integration

This document intentionally does not impose one global P0/P1/P2 queue across unrelated workstreams.

Roadmap priority should be scoped by architectural track.

---

## 18.1 Core multiplayer / foundations

```text
Finish Foundation 3L
        ↓
Foundation 3M — Trusted Spatial Residency Policy
        ↓
later SpatialRegion / topology foundations
```

Foundation 3L must close its current correctness/service/resource gates before 3M begins.

3M then builds policy over the completed 3L lifecycle rather than reopening it.

---

## 18.2 Architecture enforcement

Parallel workstream:

```text
dependency graph extraction
        ↓
forbidden-edge CI
        ↓
low-risk leaf extraction
        ↓
progressive subsystem isolation
```

This does not need to block 3M unless a specific 3M dependency would otherwise violate architecture.

---

## 18.3 Network security

```text
authenticated admission
        ↓
public Internet qualification
        ↓
later semantic anti-entropy
```

Authenticated admission is required before untrusted public hosting.

It is not inherently a 3M prerequisite.

---

## 18.4 Performance infrastructure

```text
per-stage service-envelope discipline
        ↓
subsystem-owned memory/resource attribution
        ↓
optional allocator attribution
        ↓
conditional immutable read epoch
        ↓
conditional jobification after measured crossover
```

Parallelism remains measurement-driven.

---

## 18.5 Assets

```text
Asset Foundation 2B
        ↓
residency attribution
        ↓
measured normative eviction policy
```

Eviction policy follows measurement.

---

## 18.6 Studio

```text
authoring provenance
        ↓
plugin grants / project trust
        ↓
optional staged atomic external-author transactions
```

Do not replace the existing transaction history.

---

## 18.7 Large-world / topology

```text
Foundation 3M default-space residency policy
        ↓
SpatialRegion semantic foundation
        ↓
region-local physics/topology
        ↓
semantic portals
        ↓
renderer/query/audio/network topology consumers
```

Do not introduce SpatialRegion complexity before default-space residency policy has been validated.

---

## 18.8 Security / fuzzing

Continuous workstream:

```text
boundary inventory
        ↓
coverage-guided fuzzing
        ↓
sanitizer execution
        ↓
regression corpus retention
```

New hostile parsers should add corresponding fuzz coverage as part of feature maturity.

---

# 19. Recommended Roadmap Entries

The following concise entries should be added to the appropriate roadmap sections.

---

## 19.1 Cross-cutting architecture

### Mechanically enforced subsystem boundaries

Progressively make Gargantuan's documented ownership boundaries mechanically enforceable.

1. Extract the current dependency graph.
2. Define machine-readable allowed dependency direction.
3. Make forbidden edges CI failures.
4. Physically extract low-risk subsystem boundaries beginning with transport-neutral protocol/codecs, the GNS adapter, and renderer-neutral publication.
5. Continue target isolation where it materially strengthens ownership without unnecessary mechanical churn.

---

## 19.2 Foundation 3M

### Foundation 3M — Trusted Spatial Residency Policy

Foundation 3M owns trusted demand policy for why content should be resident now.

It does not own:

* content acquisition/admission lifecycle;
* peer semantic relevance;
* peer materialization;
* runtime cell identity;
* future portal topology.

Initial requirements:

* bounded coarse content queries;
* semantic residency priority;
* deadline ordering;
* deterministic distance/cost tie-breaking;
* enter/leave hysteresis;
* bounded movement lookahead;
* revision-stamped policy results;
* bounded retained demand state;
* stable request/release deltas into Foundation 3L.

3L remains content-lifecycle authority.

3E remains relevance authority.

3J remains materialization/Known authority.

---

## 19.3 Network security

### Authenticated admission before public Internet hosting

Before untrusted public deployment, establish authenticated admission between transport establishment and authoritative gameplay allocation.

Successful admission is required before allocating:

* Player authority;
* Character control;
* relevance/materialization state;
* Remote gameplay state;
* content pins;
* gameplay capabilities.

Use short-lived server/session-bound join grants issued by a trusted control plane.

GSES independently validates engine protocol/schema/build compatibility.

---

## 19.4 Architecture invariant

### Per-stage resource/service envelopes

Every asynchronous or externally driven pipeline must identify finite resource/service envelopes for each stage capable of work amplification, including where applicable:

```text
discovery
derivation
planning
preparation
encoding
scheduler admission
transport admission
backend service
client application
cleanup/recovery
```

A bound on final selected output is not evidence that upstream or downstream work is bounded.

---

## 19.5 Performance infrastructure

### Runtime resource attribution

Extend performance diagnostics beyond wall-clock timing with bounded subsystem-owned resource attribution.

Begin with resources already owned explicitly by subsystems.

Do not introduce invasive global allocator hooks without demonstrated need.

### Immutable spatial read epoch

Investigate a same-tick immutable 3H/3K read contract as prerequisite infrastructure for safe worker-side spatial derivation.

Do not parallelize 3E merely because the read epoch exists.

Retain jobification only when measured crossover and Amdahl headroom justify it.

---

## 19.6 Asset Foundation 2B

### Residency attribution

Before normative general eviction policy, Asset 2B must expose bounded diagnostic attribution for:

* canonical asset bytes;
* decoded CPU bytes;
* renderer GPU residency;
* physics/collision residency where applicable;
* animation runtime residency;
* consumer/pin counts;
* last-use information;
* load/eviction counts.

Diagnostic projections do not become residency authority.

---

## 19.7 Studio

### Authoring provenance and external-author policy

Extend existing authoritative transaction history with engine-assigned provenance for:

* local users;
* plugins;
* agents;
* importers;
* remote collaborators;
* migration/system operations.

The existing history model remains commit-only grouping rather than abortable rollback.

Plugin/project authority extends the existing capability system through scoped grants and project trust.

---

## 19.8 Networking hardening

### Semantic replication anti-entropy

After reliable-service stabilization, investigate bounded periodic semantic audits over authoritative state both endpoints should already agree upon.

Derived runtime, renderer, predicted, spatial acceleration, and transport state are excluded.

Mismatch feeds normal bounded recovery and does not directly mutate `Known`.

---

# 20. Work That Should Not Become New Roadmap Foundations

Do not add independent projects for:

```text
generic prediction
```

because Character already implements the important behavior and extraction should wait for a second consumer.

Do not add:

```text
parser -> validate -> publish
```

because it is already an architectural invariant.

Do not add:

```text
capability framework
```

because capability-based authority already exists.

Do not add:

```text
schema metadata registry
```

because `RuntimeSchemaRegistry` already owns canonical schema truth.

Do not add:

```text
Studio command/history architecture
```

because authoritative bounded history already exists.

Do not add:

```text
3M content lifecycle
```

because Foundation 3L already owns content lifecycle.

Do not add:

```text
Atomic-style generic streaming system
```

because its useful demand-policy ideas belong inside 3M and later SpatialRegion work.

Do not pursue:

```text
ECS conversion
```

as a consequence of this review.

---

# 21. Immediate Engineering Order

After current Foundation 3L closure:

1. Begin Foundation 3M Trusted Spatial Residency Policy using the ownership boundaries in this document.
2. Start dependency-graph extraction on a separate architecture branch.
3. Define forbidden dependency edges and CI reporting.
4. Add subsystem-owned memory/resource attribution where current diagnostics lack it.
5. Fold asset residency diagnostics into Asset Foundation 2B.
6. Physically extract low-risk networking/render boundaries after graph evidence is available.
7. Before public Internet hosting, implement authenticated admission and join grants.
8. Expand fuzzing around externally supplied network/content/persistence boundaries.
9. Add Studio author provenance when plugin/agent workflows mature.
10. Revisit immutable spatial read epochs and 3E jobification only when profiling again demonstrates meaningful headroom.
11. Consider semantic anti-entropy after reliable service is operationally mature.
12. Begin SpatialRegion/portal architecture only after default-space 3M residency policy is validated.

---

# 22. Final Architectural Direction

The objective is not to make Gargantuan resemble another engine.

Atomic and other engines are useful references for mechanisms that have already survived implementation pressure.

Gargantuan should borrow those mechanisms only when they strengthen its own architecture.

The useful synthesis is:

```text
mechanically enforce subsystem dependencies
        +
authenticate peers before gameplay authority
        +
make resource cost observable
        +
bound every amplifying pipeline stage
        +
keep semantic truth singular
        +
keep runtime projections derived
        +
keep 3E relevance ownership singular
        +
keep 3J materialization ownership singular
        +
make 3M policy-only
```

In short:

> **Borrow mechanical discipline while preserving Gargantuan's semantic ownership model.**

That principle should guide future architecture comparisons as well as the specific Atomic-derived recommendations recorded here.
