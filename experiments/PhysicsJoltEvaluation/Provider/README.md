# Experimental shared-world Jolt provider

`CreateUnifiedJoltProvider` creates one Jolt `PhysicsSystem` and two existing
neutral adapters. The rigid adapter owns the native world; the soft adapter
retains a reference to it and must retire first. This factory is confined to
the experiment. Production `WorldRoot`, factories, serialization, and default
physics remain unchanged.

`PhysicsJoltProviderQualification` exercises the adapters with observable
physical gates: contact begin/end, shape replacement mass, a capsule stopped by
a wall, canonical mesh ray and release, a weld holding under impulse, cloth
deflection with rigid feedback, rubber pushing a rigid target, and stale
rigid/soft IDs after slot reuse. A sensor traversed between the two Jolt
half-updates must publish both begin and end in the same neutral step. It
uses the patched Jolt velocity reconstruction and two half-frame world updates,
each with eight collision steps and 12 soft integrations per step, matching the
prior qualified coupling schedule. The opt-in `PHYSICS_GANTRIA_CONVERGENCE`
candidate uses five soft integrations and two constraint sweeps per collision
step (80 integrations per frame); the default remains 12/1 (192 integrations).
The opt-in `PHYSICS_GANTRIA_SLEEP_FIX` enables the Jolt full-update sleep clock.
The Engine worker passed the candidate's provider fixtures at Engine
`b2df82ae5` and Jolt `d8c46588`. Hosted qualification of that exact source
passed three fresh physical-fixture repeats on Windows and Linux ASan/UBSan.
Separately, the macro-off plain rigid target compiled and its bounded rigid
matrix passed against stock Jolt `e77f1755` at Engine `057adfeb6`; this does
not qualify the unified soft provider on stock Jolt.
The fixture also uses `WorldRootTestAccess`, the existing friend seam in Engine
tests, to inject the provider into a fresh `DataModel` before admitting Parts or
Cloth. It verifies authored Part admission, motion publication, stream release
and readmission, Cloth vertex publication, and teardown through `Workspace`.
It serializes a detached semantic Part/Cloth subtree, reopens it in a new
`DataModel` with the test provider installed before parenting, and verifies
fresh backend admission without persisting native identity.
Additional `Workspace` scenes author moving cloth attachments, motor-driven
hinge and slider joints with limits, and a high-speed CCD Part against a thin
wall; their gates inspect published physical poses and neutral identities.
When built against the separate opt-in Jolt rigid/soft attachment branch with
`PHYSICS_GANTRIA_ATTACHMENTS`, this experiment also exposes a private bridge
from neutral soft/rigid IDs to one dynamic Jolt attachment. It does not add an
Engine property or save a native attachment. The DataModel fixture connects an
authored Cloth vertex to a dynamic Part after both are admitted by `WorldRoot`,
checks reciprocal motion, rejects a pinned vertex and a duplicate link, and
checks automatic detachment on Cloth destruction and stale-ID rejection after
native body churn. The helper allows only one live attachment per endpoint;
rigid motion/mesh transitions while linked reject before native mutation.

The soft adapter builds neutral cloth and regular rubber lattices directly
from `SoftBodyDefinition`. It retains neutral IDs, fixed topology indices, and
owned extracted positions. Moving attachment edits preserve the live native
body and deformation. Full definition edits replace the native soft body and
increment topology and vertex revisions. Each rigid step advances both body
kinds in the same Jolt world; the following soft step publishes those already
advanced states. No parallel Instance authority or backend handle crosses the
Engine boundary.
Exact-hard stretch constraints and exact-hard cloth bend or rubber shape/volume
constraints are rejected at admission because the corrected numerical profile
has not qualified those constraint graphs.
The unified factory fixes the experimental world at two Jolt half-frame updates
with eight collision steps each, even when `WorldRoot` supplies its production
four substeps. The plain rigid factory still honors its requested substep count.
`WorldRoot` applies pending forces, gravity, and attachment edits before rigid
`Step`; those inputs reach the native combined solve. The later soft `Step`
extracts that same accepted state. Its collider snapshot is currently redundant
work because native Jolt already owns the colliders.

The sleep-clock qualification covers C1/C4/C8 and unified U2/C8: a resting
ball publishes one contact begin, no end during five asleep frames, and one end
after departure. A same-plane box-to-static-mesh replacement preserves the
neutral floor ID, support height, and contact identity. Moving that floor away
wakes its former contact, publishes one end, and allows the ball to fall.
Adding overlapping static geometry wakes a compatible sleeping ball for a
separating response; a mask-incompatible ball remains asleep. The wake query
uses native broadphase and collision-layer filters and activates only dormant
bodies, so it does not reset active bodies' sleep timers. The authored Cloth/Part
attachment fixture also observes the linked rigid sleeping at rest and
reawakening from soft vertex motion.

Current limitations before production consideration:

- Jolt 5.6 does not solve soft/soft contacts: its soft collision collector
  accepts rigid bodies only. The provider filters soft/soft layer pairs before
  narrow phase. Soft/soft behavior remains unavailable.
- Soft forces accumulate until the next combined rigid step. They are staged
  against the actual step duration, validated, then applied to native vertex
  velocities before the Jolt world advances. Rigid forces and torques are
  reissued once per half-frame `Update`, because Jolt clears them at the end
  of each `Update`; a no-contact F/m fixture checks the resulting momentum.
  Direct soft impulses reject any result that would make a vertex velocity
  nonfinite before mutating any vertex. Point impulses use the neutral
  four-nearest movable-node inverse-distance distribution.
- Canonical mesh residency is charged by resource ID, revision, and immutable
  buffer identity. Shared consumers charge canonical bytes once and each native
  shape by Jolt's reported size. The adapter enforces configured resource,
  consumer, canonical-byte, native-byte, and native-shape limits and retires
  charges on final release. It does not yet cache one native shape across
  consumers of equal scale; each consumer owns a Jolt shape allocation.
  Static-mesh-to-primitive transitions replace the native Jolt body while
  preserving the neutral `PhysicsBodyId`. A transition with a live attached
  constraint rejects before mutation until constraint recreation can be made
  transactional. Waking newly overlapped sleepers uses the new static shape's
  AABB; a compatible body inside a sparse mesh's bounds can wake even when no
  triangle actually touches it, so large sparse mesh edits have an unmeasured
  activation cost.
- The capsule mover uses bounded shape casts and tangential clipping. Initial
  penetration correction, multiple simultaneous planes, and Box3D-equivalent
  floor/step behavior remain open.
- Connected-body collision exclusion uses the native contact listener for
  joint pairs with `CollideConnected=false`; the provider fixture checks
  suppression while a weld is live and contact restoration after removal.
  The authored hinge and slider fixture tests motion and limits. Separate
  fixtures exercise distance-spring settling and a four-link articulated
  chain under gravity and impulse. These short scenes do not establish
  long-duration gameplay quality.
- Cloth, rubber, and reciprocal contact are represented by physical fixtures;
  they are not a quality-matched parity or performance claim. Self collision,
  swept soft contact, and long-duration material calibration remain unqualified.
  Jolt's native regular-lattice rubber uses edge, cell-diagonal, and
  tetrahedral-volume constraints. `ShapeCompliance` controls cell-diagonal
  compliance here; that is an approximation of Engine XPBD shape matching,
  not a calibrated equivalent.
