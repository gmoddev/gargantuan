# Runtime Foundation 6 realistic publication probe

This source-bound, headless CPU probe is
`tests/Runtime6RealisticPublicationProbe.cpp`, built as
`gargantuan_runtime6_realistic_publication_probe`. It supplements the existing
asset, animation, soft-body, and replication qualification targets. It does not
replace their correctness gates or claim GPU frame time.

The shared-MeshPart arm imports one canonical OBJ through `SourceMount` and
`AssetService`, constructs ordinary authoritative `MeshPart` Instances, and
publishes one initial immutable full resync. Timed steady frames use real
`SetCFrame` mutations and alternating `SetColor` mutations, then measure
`RenderPublisher::Publish` and `RenderProjection::Apply` separately. An asset
reimport changes the canonical geometry and must produce one mesh replacement
and an update for every exact dependent MeshPart. The retained incremental
`RenderItem` values are compared exactly against a new authoritative full
resync and a fresh projection. One MeshPart then moves under a nested Folder,
outside WorldRoot, and back; it must update, remove, and recreate the same
generation-safe identity in those respective publications.

The replica arm constructs one server DataModel and an ordinary
`ReplicationCoordinator` peer. `ReplicaApplier` creates the live receiving
DataModel through the real baseline. Timed frames separate source setters,
incremental production, receiver apply, receiver render publication, and
projection. Every changed source Part must resolve to the same live receiver
Part, the receiver transform must match, and one projected update must exist
per changed identity. A final authoritative full resync must reproduce every
retained receiver `RenderItem`. A final source destroy and replacement must
remove the old local replica identity and create one distinct local identity,
with a matching projected remove/create and another full-resync parity check.

One world per arm is constructed outside the timed frames. Both arms use the
same source and count gates in baseline and candidate binaries. The CLI bounds
are 1–512 objects, 1–512 changed objects (at most the object count), and 1–60
frames. `--mode=mesh|replica|both` selects arms; `--count`, `--changed`, and
`--frames` select their common fixture. Raw CSV rows report each stage and
exact create/update/remove and mesh-resource counts. The CTest smoke uses
16 objects, four changed objects, and two frames. No native timing result is
asserted here before controlled A/B execution.
