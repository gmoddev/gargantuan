---
status: implementation-under-qualification
owner: engine-content-policy
related_code:
  - include/gargantuan/content/ResidencyDemandPolicy.hpp
  - src/content/ResidencyDemandPolicy.cpp
  - src/content/ContentAvailability.cpp
  - src/Engine.cpp
  - src/network/GameSession.cpp
---

# Foundation 3M trusted default-space residency demand

3M decides why immutable package content should be resident. It owns bounded
policy state and requests/releases one direct 3L demand per aggregate content
key. `ContentAvailabilityService` retains acquisition, verification, dependency
closure, admission, eviction, cache, cancellation and provider ownership. 3E
retains peer relevance; 3J retains accepted structural materialization.

## Composition and authority

The official server enables policy for ordinary `OnDemand` content. Its explicit
manual lifecycle/stop and historical Farm qualification paths retain manual
request ownership. Native Engine composition opts in with the internal
`EnableResidencyDemandPolicy` setting in `NetworkServer` mode. Existing 3L
fixtures retain their original composition. Once enabled, the Engine prepares
one policy after the verified manifest becomes available. Fully resident and
client compositions retain their existing behavior. Preparation copies immutable
manifest metadata and constructs its index on one bounded asynchronous task;
Main polls for completion without waiting. The task retains one immutable
manifest snapshot, independent of provider and residency state; even a direct
native service stop cannot invalidate that metadata. Publication checks that
the exact snapshot is still current. Destruction joins preparation before
stopping its content owner. Failed preparation emits a structured diagnostic
and disables automatic policy; no partial index or demand is installed.

GameSession supplies only ready peers' current authoritative Character position
and controller-owned accepted velocity. Full Player ObjectId slot and generation
form the native focus token. Character absence, invalid facts and peer teardown
retire that token. Session replacement creates fresh Player identities. This
feed observes committed server state and does not consume 3E resolved focus,
client transforms, camera messages or Luau-provided content keys.

The policy and its requirement/inspection API are internal native composition,
unreflected and unserialized. Server gameplay can declare an exact-key critical,
gameplay or presentation requirement through the native owner; there is one
requirement slot per content key. Independent gameplay producers aggregate
upstream. Requirements cannot name an absent manifest key or select another
package/provider. No client or ordinary Luau acquisition API is added.

## Candidate policy

The immutable package coarse index locates unloaded content. Runtime 3H indexes
only already resident ObjectIds, and 3K continues to derive live DefaultSpace
projections after 3L admission. 3M does not insert ghost objects into either.
Only logical package space `default` participates in the first policy.

Queries have separate budgets for cells, memberships and emitted candidates.
A cursor is bound to an immutable index epoch, exact space and query volume.
Invalid, exhausted or incomplete lookup is explicit; an incomplete page is never
represented as a complete spatial miss. Dense buckets resume without scanning
the full manifest per focus. Duplicate memberships coalesce by manifest identity.

The evaluator ranks a bounded retained candidate subset by semantic class,
optional deadline, distance, declared byte cost and stable content key. Critical
native requirements bypass spatial candidate competition. Priority governs
3M's request publication; existing 3L dependency and acquisition scheduling is
unchanged and may acquire a dependency before a higher-ranked dependent.

Position and accepted velocity define a finite movement segment with bounded
time and distance. Segment-to-AABB distance selects predictive corridor content.
Entry and leave radii differ; bounded tick grace retains an owned request after
its last validated reason ends. Desired/grace is policy state, not a second
acquisition or residency state machine. Release does not promise immediate
eviction when another owner, dependency or runtime-created child retains content.

## Revision, work and lifetime

Focus updates stage newest facts. Bounded evaluation revalidates candidates
before a superseded focus can publish new demand. Teleports invalidate unrelated
query continuations. Within a conservative query envelope, moving focuses can
continue a sweep and publish only candidates valid under current facts. Such a
moving sweep is a bounded approximation: earlier discarded candidates may have
become better after movement. Moving/saturated evaluation is observable, and a
fresh sweep follows to converge after movement settles. A quiescent complete
sweep selects the deterministic best retained subset.

Policy state contains only bounded focus candidates, aggregate reasons, owned
request bits, pending deltas and grace expiry. Request acceptance changes the
ownership bit; equivalent repeated demand never increments 3L again. Removal
and stop release only that policy's accepted counter contribution. The policy
binds to one exact service and manifest identity, including content digests and
dependency declarations, and cannot publish into a replacement session/package.

Each simulation tick has one bounded evaluation/publication opportunity.
Requests and releases both receive bounded opportunities when both are pending.
Invalid query results preserve the prior safely validated set and are retried
or exposed as unhealthy; they do not fabricate an empty complete result.

The coarse index additionally caps total memberships at 1,048,576 and populated
cells at 262,144. These are internal safety ceilings, separate from the 65,536
manifest-unit fixture ceiling and the per-unit 4,096-cell overlap ceiling.

## Qualification scope

Focused tests cover deterministic policy, query bounds, revisions, denial and
ownership. The local-package integration writes real manifests and immutable
payload files, then uses `LocalPackageContentProvider` and production 3L
dependency/admission/eviction. A GameSession fixture drives the actual default
server policy from authoritative Character lifetime.
That fixture also continuously updates 500 native policy focuses while a real
accepted GameSession publishes Character state and reliable Remote round trips;
progress and exact replies are mandatory, and observed frame cost is diagnostic.

The benchmark separates index/policy construction, isolated query/update cost,
allocations and retained state, a linear reference, and live 3L service cost.
Representative fixtures are 1/10/100/500 focuses and 1K/10K/65,536 units; they
are stress points, not adopted product limits. Existing 3L per-manifest service
loops remain visible in live integration timing. Policy-only percentiles do not
establish end-to-end Character/Remote wall-clock service guarantees.

The accepted D03 `POOLED_CORRECTNESS_V1`, KI-006 core closure and historical
evidence remain unchanged. Physical/provider certification and historical
Local32/real-TLS Node32 campaigns are separate gates. See the campaign's
validation receipt for measured results and outstanding qualification.
