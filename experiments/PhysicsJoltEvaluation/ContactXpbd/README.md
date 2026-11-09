---
status: isolated-experiment
authority: non-normative
date: 2026-11-09
---

# XPBD cloth contact probes

`PhysicsJoltXpbdContactProbe` uses a private copy of the **production** XPBD
backend. It does not use the separate staggered `CouplingXpbdBackend.cpp` and
does not claim reciprocal dynamic rigid contact. Its factory names are local to
the target; production source and defaults are unchanged.

`GANTRIA_XPBD_CONTACT_ORDER` selects:

| Mode | Per-iteration sequence | Interpretation |
| --- | --- | --- |
| `A` or unset | structure, vertex contact | Frozen production order. |
| `B` | vertex contact, structure | Order-only diagnostic; final structure may reopen contact. |
| `C` | vertex contact, structure, vertex contact | Alternating projection diagnostic. |
| `D` | unilateral vertex normal multiplier, structure, normal multiplier | Experimental coupled normal support. Default normal compliance is 0.1 times stretch compliance; `GANTRIA_XPBD_CONTACT_NORMAL_COMPLIANCE` may set a finite value in `[0,1]`. |
| `E` | structure, vertex contact, triangle closest-point contact | Geometry discriminator. Exact center-on-face contact chooses oriented triangle winding as its side policy. Face contact has no separate friction or reciprocal rigid response. |
| `G` | structure, vertex contact | Private relative-velocity response for prescribed translating spheres. The final contact uses cloth displacement minus the known collider displacement over the same substep; one-way only. |

Friction remains in the original vertex contact response on the final collision
pass. D's retained multiplier can keep that response active when the final
normal correction is zero. E is **not** a complete alternative contact solver;
its symmetry-breaking winding policy and missing face friction must be assessed
from its actual traces.

`PhysicsJoltContactQualification Case.json OutputDirectory` accepts bounded
9, 17, 32, or 128 square cloth meshes; 1–600 physical frames; worker cap 1–8;
top, left, four-corner, free or explicit pins; up to eight spheres; static pose,
kinematic pose, native Jolt `MoveKinematic`, and Jolt dynamic motion. A case can
specify `RotationWxyz` for cloth rest geometry. `RotateAuthoredInputs:true`
also rotates gravity, initial velocity, sphere centers about the cloth origin,
and path velocities; leave it false when those values are already rotated.
XPBD's public collider snapshots support one-way static/pose-kinematic cases;
the producer rejects reciprocal dynamic and native kinematic move requests.
An omitted `KinematicUpdate` means native `move` for Jolt and one-way `pose`
for XPBD. G receives an ordered per-substep sphere-center displacement from
the producer and rejects a missing/mismatched list; A–E ignore it. The XPBD
trace labels collider velocity as a prescribed frame pose difference, not a
native measured rigid velocity. Only pure sphere translation is modeled, with no
angular collider velocity or actuator-work accounting.

The producer writes `ContactFixtureV1.json` and uncompressed full-frame
`ContactTraceV1.jsonl`. The run wrapper is responsible for gzip packaging,
source pins, and receipt hashes. Until `ContactTraceV1.jsonl.gz` and `Source.json`
exist and pass the validator, provenance is incomplete. `Stages` includes the
signed momentum change **derived from exact XPBD contact projection and friction
edits to reconstructed velocity**; it is a solver diagnostic, not a measured
rigid reaction impulse or complete external-work balance. Static pose movement
can perform unmeasured projection work even when native static velocity is zero.
