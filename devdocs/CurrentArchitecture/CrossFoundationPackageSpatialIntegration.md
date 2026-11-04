---
status: accepted
owner: cross-foundation-integration
date: 2026-11-04
related_code:
  - src/packaging/PackageBuilder.cpp
  - src/content/ResidencySpatialMetadata.cpp
  - src/content/ResidencyDemandPolicy.cpp
  - src/editor/StreamingDiagnosticsEncoding.cpp
related_adrs:
  - devdocs/CurrentArchitecture/AssetCollisionGeometryDecisions.md
---

# Geometry, package spatial metadata and streaming integration

This accepted integration design composes the exact independently qualified
dependencies in the [ledger](../Validation/CrossFoundationDependencyLedger.md).
Implementation and qualification are in progress; the design alone does not
close KI-3M-001 or qualify the merged source.

## One package producer and one spatial policy

PackageBuilder remains the owner of immutable package preparation. It derives
content-unit package-space conservative bounds from the same captured runtime
asset catalog and canonical artifacts that enter the package. Mesh identities,
content hashes, revisions and dependencies must describe one complete published
asset snapshot. An in-flight or failed reimport retains the prior publication;
an unfinished candidate is never used for package bounds.

Canonical Mesh local bounds are not content-unit package bounds. The producer
validates canonical mesh bytes through the existing artifact decoder and checks
their finite vertex coverage. It transforms the complete supported local envelope
by CFrame * Size, including an asymmetric local center, and unions every member
of the independently streamed subtree. The inherited Box envelope is included
because MeshPart can use Box collision. Collision availability alone cannot
prove render-geometry coverage. Double intermediates, conservative conversion
and finite validation must avoid shrinking the output or emitting infinity.

The existing manifest version-1 CoarseBounds field remains the serialized
representation, associated with its package namespace, logical unit key and
payload digest. There is no separate wire bounds registry or client metadata
channel. The producer rederives native ResidencySpatialMetadata v1 from actual
unit payloads/artifacts and uses PR15's consumer to validate full coverage and
containment. Build and full package validation reject underbounded or incompatible
streaming units even when their file hashes are internally self-consistent.
That also denies historical unsafe MeshPart packages before official OnDemand
configuration; disabling a policy alone is not a fallback for partitioned units.

## Conservative unsupported handling

Any missing, malformed, unresolved or unsupported geometry makes the entire
direct subtree ineligible for independent streaming. Skinned meshes have no
qualified animation envelope and remain in bootstrap. Initially unanchored
spatial members also remain bootstrap because startup bounds do not cover
subsequent physics motion. Unknown spatial members
and non-finite transforms cannot be silently skipped while another member gives
the subtree a small bound. Unindexable aggregates also remain bootstrap under
the existing bounded coarse-index contract. This preserves content visibility
at the cost of residency granularity. No payload repair or untrusted authority
inference is permitted.

## Distinct runtime ownership

3M remains the bounded candidate/demand owner. 3L alone acquires/verifies bytes,
prepares detached content and admits or evicts Instances. 3K/3H derive/index live
projections; 3E owns relevance and 3J owns accepted materialization. Geometry or
physics readiness cannot grant any of these states.

AssetService loads the immutable canonical asset snapshot for the package world.
Content-unit eviction destroys Instances and their normal world-owned physics
bodies; it does not evict the package AssetService's shared artifacts. Native
collision resources/shapes retire after their last consumer. Renderer object
unpublish and shared mesh-resource retirement are separately observed. Existing
physics safe-point replacement, rollback, chunk bounds and generation-safe
identity remain unchanged.

## Bounded authoring inspection

Existing geometry compiler diagnostics, AssetService metadata/residency and
physics residency APIs remain the native inspection seams. The existing opt-in
streaming diagnostic unit page additionally carries the manifest content digest,
package space and nullable conservative bounds, with one package namespace in
the policy snapshot. This is an additive version-1 response extension bounded by
the existing 128-unit cap. `BoundsAccounting = ManifestCoarseBoundsV1` identifies
the source, not a new producer-trust claim. Reads remain Main-thread projections,
accept no controls and do not advance document history. No speculative geometry
RPC, public modeling API, Studio implementation or LUI source change is added.

## Required integration evidence

Focused producer/consumer tests must cover actual imported and generated offset
geometry, mixed units, transform/scale, coherent reimport, missing/version/digest/
dependency changes, deterministic package output, malformed and underbounded
inputs, complete fallback and bounded work. Actual package/provider fixtures
must exercise movement/teleport, grace, release/reacquisition with fresh identity,
physics query/contact and resource teardown, save/package/reopen and revision
replacement. Bounds near extended geometry must prevent a false-negative demand
outside the former Size/2 box.

Import/compile, bounds preparation, serialization/validation, first binding,
query/reconciliation, CAS admission, physics creation and retirement are measured
as distinct boundaries. Existing geometry/3M baselines and historical failures
remain preserved; finite tests imply neither universal latency nor indefinite
resource guarantees. Hosted Windows/Linux/sanitizer qualification and independent
source/evidence review are required before marking this integration complete.
