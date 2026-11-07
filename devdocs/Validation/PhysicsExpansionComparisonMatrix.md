# Comparison contract after the physics expansion

This is a workload and responsibility inventory for the next campaign, not a
Jolt migration decision. Box3D and Gargantuan XPBD remain production backends.
The previous experimental branch and its unequal-quality measurements are
unchanged. Exact expansion qualification and measurements belong to the
campaign receipt; pending rows must not be promoted to qualified behavior.

| Capability | Current implementation / Engine responsibility | Jolt upstream starting point | Required comparison |
| --- | --- | --- | --- |
| Rigid motion, force, angular dynamics | Box3D; BasePart queues and WorldRoot safe points | Rigid bodies | Equal units, force duration, mass/inertia, sleep and motion transitions |
| Groups, sensors, touch | One neutral mutual-mask rule; Box3D filters; owned event normalization | Collision filtering and sensors | Runtime edits, same-world ownership, sensor event and retirement parity |
| Rays, sphere/capsule/box casts, overlaps | Box3D convex queries; bounded semantic owner resolution | Rays, shape casts and overlap detection | Stable ties, chunk deduplication, filtering, finite hits, overflow and latency |
| Fast rigid movement | Existing static CCD plus opt-in moving-body CCD | Rigid continuous collision detection | Same projectiles, obstacle thickness, speed and step; record unsupported pairs |
| Articulated mechanisms | Box3D ball/hinge/slider/distance; semantic Attachment frames | Point, hinge, slider, distance, fixed and other constraints; motors | Door limits, slider travel, spring extension, drift and motor work |
| Dense imported contacts | Immutable Engine mesh resources, chunk accounting and compatibility patches | Triangle mesh collision | Same geometry, support surfaces and adjacency quality; no omitted contacts |
| Cloth and rubber | Owned XPBD topology, constraints, materials and bounded jobs | Soft bodies with bend/volume constraints and rigid collision | Equal sag, extension, volume error, impulses, pins and collision quality |
| Moving pins and inspection | XPBD revision-safe updates; WorldRoot copied diagnostics | Soft attachment facilities | Preserve deformed state during changes; map exact authoring semantics |
| Rigid-to-soft interaction | Main snapshots rigid primitives after rigid step; XPBD discrete contact | Soft/rigid collision | Equal contact quality and moving-collider behavior |
| Soft-to-dynamic-rigid feedback | Unsupported | Must be demonstrated for the chosen Jolt configuration | A deformable rope/body must move a dynamic rigid body with stable exchanged work |
| Authority and persistence | DataModel, reflection, MutationGateway, ObjectId, schema, Luau and EditorHost | Engine-specific integration still required | Save/reopen, capabilities, journal/replication, stale-ID denial and rollback |

The upstream feature column is a capability inventory from the pinned
[Jolt v5.6.0 README](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/README.md#features),
not proof of Gargantuan semantic parity or complete bidirectional coupling.
The next experiment must select and record its exact dependency commit and
verify those APIs again. Existing upstream features do not remove the need for
an Engine adapter, bounded admission, public contracts or qualification.

## Equal-workload evidence

Retain raw P50/P95/P99 samples, compiler/build mode, machine, worker count,
solver iterations and quality thresholds. Use the same WorldRoot scenes for
stacks, dense contacts, door/slider/spring mechanisms, body churn, collision
group changes, query limits, fast projectiles, moving pins, rubber point
impulses and 4K/16K cloth. New capabilities have no pre-feature zero-time
baseline. The legacy coincident Weld workload is not a spaced joint chain.
Report quality failure before performance and do not trade solver quality for
an apparent speedup. CPU/RSS and estimated solver bytes are separate metrics;
missing allocation counts remain unavailable.

## Consolidation value is a separate decision axis

For both architectures, inventory the owner, source paths, tests, failure modes
and ongoing engineering responsibility for:

- step synchronization, collider snapshots, contact exchange and future feedback;
- geometry translation, native resources, IDs, cancellation and destruction;
- diagnostic capture, queue/storage admission and failure recovery;
- solver-specific fixtures, sanitizer coverage and feature implementation;
- upstream patch count/size, isolation, regression tests and measured rebase work.

Classify each current responsibility as removed, reduced, relocated, or retained.
Do not count code moved into a Jolt fork as eliminated maintenance. A small,
isolated patchset with measured upstream rebase cost is materially different
from rewriting a solver inside the fork. Keep this ownership comparison
independent of raw frame times, and require actual deformable-to-rigid feedback
before crediting a unified backend with solving the cross-solver problem.
