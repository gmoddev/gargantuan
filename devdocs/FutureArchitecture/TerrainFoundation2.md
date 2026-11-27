# Terrain follow-on implementation assignment

Close Terrain Foundation 1 before promoting terrain for persistent offline
projects. Treat the scale and multiplayer work below as separately qualified
milestones. Each capability retains its own readiness verdict.
This is the original assignment. The
[campaign ledger](../Validation/TerrainCampaign.md) records completed work
and current acceptance boundaries.

## Complete Foundation 1 acceptance

1. Capture an immutable terrain snapshot with the committed project revision.
   Seal its canonical GTCH and GTMF artifacts with the existing content
   digest and root confinement. Verify the whole authenticated closure and
   stage immutable bytes before one durable GAS root replacement. Supersede
   the current edited-world save guard only after save as, interrupted save,
   prior-version load, malformed artifacts, and Windows/Linux parity pass.
2. Reopen against the verified source artifacts. Reconstruct the editable
   field, rebuild or validate derived meshes and collision, and gate movement
   until required collision is active. Keep unavailable pages distinct from
   authoritative air. Verify exact round trips, fresh scope identities,
   local Play isolation, undo and pending edit behavior on close.
3. Run the actual offline Engine sample and integration fixtures on current
   Windows Release and Linux sanitizer builds. Capture scenes and native GPU
   and gameplay metrics, including stock and custom Character contacts on
   seam and acute cave geometry, dynamic rigid objects during an edit,
   full material behavior, frame phases, and invalidation under memory pressure.
4. Finish project-defined PBR material resolution, texture and blend budgets,
   and provider/query surface-material behavior. Exercise paint across a
   seam with no geometry extraction and explicit handling of any physical
   material change. Qualify GPU upload denial and resource retirement with
   renderer acknowledgements and a recovery policy.

## Large-world and multiplayer work

1. Select a regular/transition contour pair that agrees on face decisions,
   material and normal seams. Then qualify balancing, hysteresis, protected
   openings and fallback for thin features during camera traversal. Visual
   LOD revisions remain independent from collision readiness.
2. Drive activation through the existing spatial relevance and content
   systems, with distinct active terrain records and inactive canonical
   directory state. Verify admission budgets for movement, isolated cave
   dwell, teleport, teardown and enough retained resources to avoid holes
   during warmup or reload.
3. Define versioned peer terrain delivery under authorized package/audience
   policy. Use authenticated immutable base artifacts and revisioned edit
   records with exact-precondition deltas and full replacement fallback.
   Demonstrate late join, reconnect, malformed frames, stale revisions,
   backpressure and separated players using matching server and client builds.
   Require active collision before admitting movement in dependent regions.
4. Add versioned EditorHost brush transactions, selectors, previews and
   undo/redo against the same Engine Field and mutation gateway. Coordinate
   tool ownership with Studio Foundation 12. Use immutable source artifacts
   for heightmap import and procedural recipes with explicit provenance.

Scattering, navigable tiles, virtual texture residency, projected decals and
spline-generated terrain can reuse bounded active projections and field revision
dependencies. Their resource lifetimes and public APIs require subsystem
decisions. Water remains outside the terrain representation.

### Terrain delivery integration

The package pins GTMF and GTCH in its integrity manifest. The first
[terrain session bootstrap](../../docs/adr/TerrainNetworkBootstrap.md)
uses those pins for bounded delivery and collision admission between
matching Development packages on loopback. A future provider namespace
will require the existing package audience and Node authorization policies.

During `GameSession` bootstrap, the server needs to select the required
spawn-region closure from an authoritative terrain index and bind it to the
session epoch, package identity, dataset and terrain revision. The client
can prepare physics against verified immutable bytes. Its readiness
acknowledgment should name the exact bootstrap revision and region before
`ReadyServerPeer` can admit movement. Reconnect allocates a fresh epoch.
An edit that invalidates an in-flight closure requires new admission or an
explicitly pinned replacement. A missing required chunk leaves the region
unavailable.

Runtime updates need ordered revisions and separate resource admission.
Reuse the existing reliable backpressure and content budget controls, and
prioritize active player collision ahead of wider visual residency. Keep
render LOD independent from physics and share the single authoritative
field with runtime edits. Qualify real load, cancellation and stale
completion across two matching production hosts before removing the startup
guard.
