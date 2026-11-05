---
status: current
owner: engine-content-policy
last_verified: 2026-11-04
related_code:
  - include/gargantuan/content/ResidencyDemandPolicy.hpp
  - src/content/ResidencyDemandPolicy.cpp
  - include/gargantuan/content/ResidencySpatialMetadata.hpp
  - src/content/ResidencySpatialMetadata.cpp
  - include/gargantuan/content/StreamingDiagnostics.hpp
  - src/content/StreamingDiagnostics.cpp
  - src/Engine.cpp
  - src/editor/EditorHost.cpp
---

# Foundation 3M streaming readiness integration

This is the current Engine-side startup, metadata-consumer and diagnostic slice
on top of the [qualified 3M demand policy](TrustedResidencyDemandFoundation3M.md).
The [3M qualification receipt](../Validation/TrustedResidencyDemand3M.md) records
measurements at its pinned earlier source. This slice has no replacement measured
startup or service latency claim until its own qualification is recorded.

## Startup and immutable ownership

`ContentAvailabilityService` validates one package manifest and exposes a
`shared_ptr<const PackageContentManifest>` through its internal
`GetManifestSnapshot()` seam. For official on-demand server composition, Engine
captures that exact snapshot after publication, constructs the coarse index and
policy on one `std::async` worker, and polls the future without waiting on Main.
The worker retains immutable metadata only: no provider, DataModel, residency
record, or acquisition task. Main installs the prepared policy only while the
same service still publishes the same pointer **and shared ownership control
block**. A stopped or replaced service cannot accept an old preparation. Engine
joins an outstanding worker before its content owner is destroyed. Preparation
failure disables automatic policy and releases Engine's saved snapshot; no
partial index or demand is published.

The shared-snapshot policy constructor retains that immutable owner. Its first
service binding is an O(1) pointer and owner-equivalence comparison; equal
manifest bytes from another service do not qualify. This removes the prior
one-time O(manifest entries) comparison on Main for official composition. It
does not make manifest fetch, parsing, index construction, or the rest of the
first Engine step constant time. The existing `const PackageContentManifest&`
constructor makes a defensive value copy and compares every entry on its first
service binding, including namespace, digest, dependencies and bounds. It
remains a native compatibility path. `ManifestBindingEntriesCompared` exposes
the value-comparison work; it is zero for an exact shared snapshot.

This changes neither 3L provider ownership nor its request, dependency,
admission, eviction, cache and cancellation semantics. The historical 3M
receipt measured policy construction, first binding, provider readiness and
manifest encoding separately; compare new measurements at those same
boundaries before attributing a practical startup improvement.

## Conservative metadata consumer boundary

`ResidencySpatialMetadata` version 1 is a detached **native compatibility
input**, not a serialized package extension or a geometry producer. A record
names the package namespace, sorted unit key, content digest, logical package
space and finite ordered conservative AABB. The report merges at most 65,536
records against a 3L-validated, sorted manifest. It checks namespace, version,
identity, space and that the manifest AABB contains the supplied conservative
AABB. The aggregate must already be in the same manifest package-space
coordinates; its producer performs all geometry transforms outside 3M. A
mesh-local bound is not substituted for that aggregate. `Compatible` requires
coverage for every spatial manifest unit; a dependency with no manifest bounds
needs no row. It returns `Compatible`, `Incomplete` or `Rejected`, aggregate
counts and at most 64 issue details with an explicit truncation bit. The issue
cap limits only details, not verdict validation. Missing evidence is
`Incomplete`; malformed, mismatched or underbounded evidence is `Rejected`.

The adapter does not calculate MeshPart bounds, transform geometry, amend a
manifest, request content, or grant a producer qualification. There is no
qualified asset/package metadata producer in the independently qualified PR15
slice. The separately composed
[cross-foundation integration](CrossFoundationPackageSpatialIntegration.md)
implements package production/verification using this same adapter; its new
combined qualification is recorded separately from PR15 evidence.
[KI-3M-001](../../KNOWN_ISSUES.md#ki-3m-001-packaged-mesh-coarse-bounds-need-conservative-geometry-metadata)
remained open at the pinned PR15 source. The separately qualified PR16 package
producer resolves that defect for its supported static geometry; unsupported
subtrees remain in bootstrap. Spatial policy still consumes only bounds in the
validated manifest. PR15's independent receipt does not qualify the later
producer or the combined Input/streaming mainline source.

## Opt-in observation

`Engine::SetStreamingDiagnosticsEnabled` is trusted native configuration. It
allocates a fixed 128-sample policy timing ring when a policy exists; enabling
before async publication carries the opt-in to the installed policy. Collection
is disabled by default. Disabling drops the timing history. Reads run on Main,
are read-only, and do not call `Step`, change demand, mutate the DataModel or
advance project history. The Engine view reports `Disabled`, `NotConfigured`,
`AwaitingManifest`, `Preparing`, `Active`, `PreparationFailed` or `Stopped`.

`GetStreamingDiagnostics` validates `StartIndex <= 65,536`, `MaximumUnits <=
128` and `MaximumTimingSamples <= 128` (defaults 0, 64 and 32). Unit pages
walk existing demand records from `lower_bound(StartIndex)` in manifest-index
order; `NextIndex` is the next record or null. Zero-sized pages and zero timing
samples are allowed. A read never scans all manifest entries. The policy view
contains focus/candidate/desired/owned/pending/grace counts, revision, limits,
request/release and stale/failure/budget counters, bounded unit rows, and a
fixed chronological timing sample ring. A unit row distinguishes desired
policy demand, an accepted policy-owned 3L request, grace, pending release and
current 3L availability state. It does not imply client Known state.
The projection counts attached committed focus contributions by summing at
most 512 focus-map sizes, without a manifest or demand-map scan for that gauge.
If 3L has stopped, `Content.Stopped` is true and Engine immediately reports
`Stopped` without exposing stale policy rows.

`EvaluationNanoseconds` covers policy evaluation; `ReconciliationNanoseconds`
covers expiry and request/release application. Their timestamp interval ends
before observer metric assembly, ring write and diagnostic serialization.
The wire field `Policy.TimingAccounting` is
`CorePhasesExcludeObservation` for this boundary.
`LastContentStepNanoseconds` measures one 3L `Step` call with diagnostics
enabled. `Content.Timing` reports selected 3L accepted-to-queue,
queue-to-provider, provider, verification, worker preparation,
preparation-to-commit, commit, end-to-end and step durations as sample count,
sum and maximum in microseconds. These are distinct boundaries: provider time
can include provider/client-side latency; policy and 3L timing do not measure
remote client materialization, Studio presentation or physical service by
themselves. No P50/P95/P99 is inferred from a sum and maximum.

Memory fields use `LogicalPayloadV1`: policy structure/value estimates, shared
manifest entry vector capacity times entry size, and index membership payload.
`FocusContributionRecords` counts attached committed contributions; the
policy-state estimate includes both their demand-map value pairs and ranked
focus-set pair payloads.
They exclude allocator headers, STL node and map-cell overhead, separately
allocated strings/dependencies, provider/cache data and process working set.
These are diagnostic gauges, not a complete retained-memory or allocation
census. The existing benchmark remains the source for allocation and process
attribution.

EditorHost exposes this as an optional version-1, exact-Play-session diagnostic
extension under its launch token and Studio read capability. Its configure
operation changes only collection state; reads return bounded value snapshots.
The [EditorHost protocol](EditorHostProtocol.md#streaming-diagnostics-extension-version-1)
specifies the wire shape and denial behavior. No Studio or LUI presentation is
implemented here, and no request can set a focus, content key, priority,
provider or residency state.
