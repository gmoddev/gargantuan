# Experimental rigid comparison

`PhysicsJoltRigidEvaluation` and its allocation-instrumented sibling use the
merged Engine's real `IPhysicsBackend` types. Box3D comes from the production
factory. Jolt v5.6.0 is confined to `RigidJoltBackend.cpp`; the production
factory, WorldRoot, public API, project files and defaults are untouched.

Run a case in a fresh process, for example:

```text
PhysicsJoltRigidEvaluation --backend=box3d --case=stack --workers=1
PhysicsJoltRigidEvaluation --backend=jolt --case=stack --workers=8
```

`--workers` is a total budget including the calling thread. Jolt uses zero,
one or seven background workers for budgets one, two or eight. Box3D's current
solver remains single threaded even when the argument is eight; its rows label
that fact. `--quick` shortens only smoke runs. Normal cases use 1/60 second,
four collision substeps, and retain every measured sample in JSON lines.
Run each backend, case and worker budget repeatedly on the same machine and
Release profile. Compare timings only where both corresponding quality rows
pass. The separate allocation binary is for counts/retention, not timing ranks.

| Case | Common geometry and principal gate |
| --- | --- |
| `static-world` | 400 static boxes; downward rays and teardown |
| `stack`, `dense-contacts` | 128 one-unit dynamic boxes over a plane; finite state, mass, floor and pair-penetration gates |
| `mostly-static` | 1,000 static and 10 dynamic boxes; the same contact gates |
| `body-churn` | 30 cycles of 32 create/step/destroy operations; stale IDs rejected |
| `sleep-wake` | Ball on plane; observed natural sleep then explicit wake |
| `mesh-contact` | Shared canonical 16×16 triangle grid, falling ball and supporting ray |
| `advanced-queries` | Two coincident boxes; ray plus sphere/capsule/box casts and overlaps |
| `filter-updates` | Alternating mutual collision-group visibility |
| `moving-ccd-off/on` | 0.5-unit ball at −3 moving +360/s against a dynamic 0.1-unit wall at −2/s; enabled requires all trials to stop before x=1 |
| `joint-motors` | Limited hinge door and driven slider with collisions disabled; motion within bounds |
| `spring-distance` | Two-unit distance spring with collisions disabled; bounded extension |
| `articulated-chain` | Eight ball-socket links with collisions disabled; final joint drift |

The Jolt adapter intentionally does not translate contact/sensor begin/end
events, kinematic capsule motion, production asset import, arbitrary Part shapes,
shape/density replacement, dynamic sleep-policy edits, connected-body collision
exclusion, or all authored denial
paths. `concurrent-admission` is an explicit unsupported row because the neutral
Engine physics boundary is serialized on Main; native concurrent body insertion
would measure a different contract. Jolt query shapes are cached by dimensions;
the cache lasts for one experimental world. The adapter does not implement the
production mesh residency budgets or revision accounting. The mesh case tests
support behavior, not the imported 300-decoy regression or shared mesh lifetime.

`Status=measured` means the fixture ran. `Quality.Status=pass` means its stated
gate passed, not that the entire physical behavior or solver settings are
equivalent. A failed gate invalidates timing claims for that row. Jolt returns
empty neutral contact-event vectors in this slice, so the benchmark uses pose,
mass, penetration, ray and joint checks instead of claiming event parity. The
root campaign must also retain Engine WorldRoot baselines, exact source and
build provenance, raw repeat distributions, CPU/RSS data, and separate soft and
two-way coupling evidence before making an architecture decision.
