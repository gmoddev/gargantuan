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

With source accounting enabled by this target, `External.XpbdContactAccounting`
separates normal projection and friction impulses from exact changes to node
position and previous position at each contact phase. Its prescribed collider
reaction is the equal-opposite **constraint reaction**, not an observed rigid
momentum change. `PrescribedSurfaceWorkJ` sums each contact impulse dotted with
the known sphere-center displacement divided by that substep's duration; it
does not use a frame-averaged velocity. The contact kinetic delta and
`ContactEnergyResidualJ` remain signed observations, not an automatically
dissipative or energy-conserving verdict. Separate integration, damping,
structural, and pin stage kinetic changes expose their contributions.
The integration change is split at the actual velocity writes into
gravity/force, uniform impulse, and point impulse kinetic deltas. Frame 0
therefore reports the queued authored launch impulse instead of attributing
that energy to unmeasured contact or actuator work.
`LedgerResidualJ` checks whether those recorded stage changes sum to the
measured kinetic change; this bookkeeping identity is not external-work
closure or a substitute for a physical energy gate. Static pose movement and
unmeasured structural stored energy remain explicit limits.

For separate timing captures, set `CaptureTrace:false` in the case and
`GANTRIA_CONTACT_ACCOUNTING=0`. The producer then skips fixture/JSON/velocity
capture and all full-node kinetic/momentum ledger scans while preserving the
physics calls, state identity/revision/finite checks, and a cumulative float32
position hash computed outside the physics clock. It prints P50/P95/P99 frame
physics time, XPBD worker utilization/jobs, process CPU and resource deltas.
These timing runs do not constitute quality evidence; keep the corresponding
full trace with accounting enabled for the physical gate.

For cloth, the same source phases report momentum start/end, gravity/force,
uniform and point impulse, damping, pinned-edge constraint impulse on cloth,
normal contact and friction impulses. `MomentumResidualNs` is the signed
remainder after these independently captured terms. A fixed pin's target
does no work, although the internal edge solve transmits a reaction impulse;
moving pin targets are outside this fixture producer. This momentum closure
is stronger than the contact action/reaction identity, which is defined by
negating the cloth constraint impulse and does not observe a dynamic rigid.
`UnpinnedStructuralNumericalImpulseNs` separately records the actual
mass-weighted sum of both float position updates for each free-free distance
constraint. Its mathematical value is zero, but independent float rounding can
make it nonzero in the reconstructed velocity. This term remains reported as
numerical momentum leakage even if subtracting it closes the source ledger;
it is never relabeled as a physical external impulse.

`GANTRIA_XPBD_IDEAL_VELOCITY=1` enables a separate private cloth-only
velocity reconstruction experiment on top of whichever A/E/G contact mode is
selected. The original modes remain unchanged when the flag is absent or `0`.
After integration, it snapshots the integrated position, previous position,
and velocity. Each distance solve records the difference between its ideal
mass-weighted equal-and-opposite position correction and the two actual
rounded float position writes in a per-node double accumulator. Final velocity
starts from the integrated velocity and adds the net later position and
previous-position changes, plus this numerical bias, divided by the substep
duration. Thus existing vertex/face collision and friction edits still affect
velocity; only free-free structural float-store leakage is removed. The
snapshot buffer is retained per live cloth body, bounded by its node count,
and reused across substeps. Rubber volume and shape recovery are deliberately
outside this experiment.

The opt-in trace reports both the **would-be**
`UnpinnedStructuralNumericalImpulseNs` from rounded position writes and the
new raw `MomentumResidualNs` after reconstruction; it no longer subtracts
that would-be leak as an external impulse. Two kinetic terms make the stage
bookkeeping explicit: `IntegrationPositionRoundingKineticDeltaJ` is the gap
between integrated velocity and position-derived velocity immediately after
integration, and `IdealVelocityReconstructionKineticDeltaJ` is the final gap
introduced by reconstruction. Neither term is external work or physical
dissipation. The 9×9 no-contact uniform-launch control in `Controls/` checks
that this mode does not manufacture contact impulses; static and prescribed
moving controls at 16/32/64 substeps test momentum leakage and convergence.
