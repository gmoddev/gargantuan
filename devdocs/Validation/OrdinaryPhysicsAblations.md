# Ordinary Workspace physics comparisons

Each adjacent comparison runs the same native Windows Release EditorHost with one change to physics preparation. The scene contains 24 anchored Parts, the stock avatar, locomotion and camera. The comparison follows the existing paired whole-step protocol. Earlier work measured an average physics increase of 1.47 µs across individual ticks and 7.64 µs on ticks in the 94th–96th percentile of whole-step duration. [Shipped phase attribution](SpatialOrdinaryAttribution.md)

| Build | Change |
| --- | --- |
| A · `d28a2ad74` | Released Windows Engine. The normal Windows C++ production sources match the `5b6db615b7` campaign baseline. |
| B · `9489330c6` | Resolve transfer protection at the first live Part with an actual pending command in each fixed substep. |
| C · `060446f63` | Skip ordered motion readback when the accepted dynamic-body count is known to be zero. Unknown counts and requested recounts follow the usual scan. |
| D · `fd59e1851` | Poll terrain for pending edits, version-tombstone retirement or an unreported provider failure. |
| E · `1f28dd2ca` | Observe asset changes when at least one MeshPart is tracked. The ordinary first AssetService materialization and service replacement stay visible. First mesh admission resumes observation from its current asset snapshot. |

The primary comparisons use four balanced four-process blocks, 192 warmup ticks and 384 measured-window ticks per process. All comparisons use the stock diagnostic API. The measurements of interest are the average native physics phase, and the same physics phase on ticks at the 94th–96th percentile of whole-step duration. The paired whole-step p95 has a separate, larger historical qualification. The moving-body control uses two balanced blocks with the same 192/384 budgets, adds six unanchored bodies and sets their upward velocity every 30 ticks. Each fresh session asserts that all six moved by tick 12. [Acquisition protocol](SpatialOrdinaryPrecision.md)

An initial same-executable check used two blocks, 64 warmup ticks and 192 measured-window ticks per process. It recovered 1,524 individual ticks of 1,536. The average physics difference was −0.36 µs (two-sided 95% paired interval −0.77 to +0.06). On the 94th–96th percentile ticks the difference was −7.71 µs (−13.55 to −1.87) despite identical executable hashes. The whole-step p95 difference was −7.25 µs (−255.66 to +241.16). That control illustrates the variability of a small tail sample on this shared desktop. [Processes](OrdinaryPhysicsAblations/AA/Processes.csv) · [Native counters](OrdinaryPhysicsAblations/AA/Polls.csv.gz) · [Paired analysis](OrdinaryPhysicsAblations/AA/Comparisons.csv)

## Measured changes

The table gives later minus earlier in microseconds, with two-sided 95% paired intervals across independent blocks. Each fresh process contributes one value. The rank band uses the complete step duration to select ticks, then measures physics on those same ticks. The raw process receipts record missing ticks and exact-sample p95. Each linked directory includes the compressed individual poll counters and their checked comparison.

| Change | Exact/attempted ticks | Mean physics | Physics at step ranks 94–96 | Whole-step p95 |
| --- | ---: | ---: | ---: | ---: |
| [A → B · command guard](OrdinaryPhysicsAblations/AB/) | 6,010/6,146 | +1.64 [−3.40, +6.67] | +4.57 [−15.35, +24.49] | +19.28 [−63.86, +102.41] |
| [B → C · anchored readback](OrdinaryPhysicsAblations/BC/) | 6,118/6,144 | −0.46 [−1.37, +0.44] | +4.76 [−6.56, +16.09] | +3.09 [−26.84, +33.01] |
| [C → D · idle terrain](OrdinaryPhysicsAblations/CD/) | 6,053/6,145 | −1.366 [−2.726, −0.006] | −1.78 [−18.62, +15.06] | +3.31 [−3.95, +10.58] |
| [D → E · mesh observation](OrdinaryPhysicsAblations/DE/) | 6,082/6,144 | −1.078 [−2.138, −0.018] | −1.96 [−11.22, +7.31] | −22.26 [−52.66, +8.14] |

The first two changes have small implementation scopes and inconclusive individual timing. The latter two resolve small average physics improvements with intervals close to zero. Each individual whole-step p95 interval spans zero. The timings were acquired in separate experiments and do not establish a cumulative p95 by addition.

The stock character also reaches mesh asset observation during PreSimulation. Its admission path resolves kinematic motion after asking the world to apply pending physics changes. D → E reduced mean PreSimulation from 189.50 to 172.38 µs (−17.12, interval −23.03 to −11.22) and from 363.36 to 325.15 µs on the rank band (−38.21, interval −53.85 to −22.58). These are inclusive phase durations. The stock counter does not time the asset observer in isolation. [All phase comparisons](OrdinaryPhysicsAblations/DE/Comparisons.csv)

A separate instrumented comparison recorded 640 ordinary ticks per build. Both builds entered collision asset observation 1,280 times. The cumulative native treatment made zero service lookups or change-journal reads there, down from 1,280 of each. Ordered stationary readback visits fell from 8,950 to zero, transfer-protection lookups for pending impulses from 358 to zero and idle terrain polls from 640 to zero. The existing fixed-step traversal still made 8,925 Part visits in the cumulative build and 8,950 in stock. In the independent six-block instrumented timing, the cumulative change reduced mean physics by 1.89 µs (interval 0.61 to 3.16) and by 7.00 µs on the whole-step rank band (1.51 to 12.49). That comparison also contains the concurrent character and render changes. [Operation counts](SpatialOrdinaryAttribution/ExactOperations/OperationSummary.csv) · [Matched native phases](SpatialOrdinaryAttribution/NativeTreatment/Comparisons.csv)

With six moving bodies, A → E recovered 3,062 of 3,072 ticks across eight fresh sessions. All six bodies moved in each session. Mean physics went from 47.25 to 49.44 µs (+2.18, interval −10.75 to +15.12). Whole-step p95 went from 620.55 to 639.23 µs (+18.68, interval −19.13 to +56.48). Rank-band physics changed by +16.64 µs (−174.41 to +207.68). This small control demonstrates successful publication of dynamic motion under repeated commands and leaves smaller performance changes unresolved. [Process receipts](OrdinaryPhysicsAblations/RigidAE/Processes.csv) · [Native counters](OrdinaryPhysicsAblations/RigidAE/Polls.csv.gz) · [Checked intervals](OrdinaryPhysicsAblations/RigidAE/Comparisons.csv)

The four treatments use the original Release link graph, recompile the changed translation units, and retain the same 35 runtime, shader and DLL hashes. A control relink preserves every byte of the 10,500,096-byte executable code section and its address. Two read-only data bytes changed. A separate full normal build exercises header dependencies and the native regressions. [Build identities and section comparison](OrdinaryPhysicsAblations/Builds.json)

## Native lifecycle checks

The normal Windows MSVC Release build at cumulative `4af419f00` passed `gargantuan_physics_backend` in 0.90 s and `gargantuan_spatial_transfer` in 0.04 s. The backend fixture replaces a materialized AssetService, publishes while no mesh exists, attaches a mesh whose collision body is still pending, publishes again, then detaches, readmits and destroys the mesh. The transfer fixture queues an impulse, holds the actor for two fixed substeps, cancels, unanchors, verifies that the impulse applies once, and reanchors it. The imported mesh and both spatial session transport suites also passed. [Backend fixture](../../tests/PhysicsBackendTests.cpp) · [Transfer fixture](../../tests/SpatialTransferTests.cpp)

The same build passed `gargantuan_terraincoordinatortests` and `gargantuan_terrainruntimetests`. The coordinator test keeps a cleared chunk's version alive through an outstanding reader, releases that reader, and verifies reclamation on the following idle poll. Existing tests exercise admission, conflict, cancellation and provider failure. [Terrain coordinator fixture](../../tests/terrain/TerrainCoordinatorTests.cpp)

## Reproduction

Each capture uses a new directory. The two commands exercise one adjacent pair and verify each recorded tick against the native phase counters. The process order alternates ABBA or BAAB within balanced randomized blocks. [Capture](../../tests/SpatialConstructionStepProfile.py) · [Analysis](../../tests/SpatialConstructionStepAttribution.py)

The captures used the stock counter path during the attribution revisions from `58ecfa851` through `e4bb74c52`. The later revisions extended optional instrumented modes. The commands here use the latter pin, whose Windows SHA-256 hashes for capture and analysis were `f09f4a31a3659d5c778871c6a6899f8a623be1511f1cf2f4e4d740a7b131ecc5` and `1676a219934e5b323183be38201e2f26041c5b6f53d44feb9c85ee17a9f80ed4`. Reanalysis at that pin reproduced all 45 metric rows of every ordinary comparison exactly. Use `--scene rigid --blocks 2` for the moving-body control.

```powershell
python -B tests/SpatialConstructionStepProfile.py --baseline $Earlier --current $Later --output $Raw --scene ordinary --blocks 4 --frames 384 --warmup 192
python -B tests/SpatialConstructionStepAttribution.py --input $Raw --output "$Raw/Comparisons.csv"
```
