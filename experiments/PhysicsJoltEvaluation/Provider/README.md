# Experimental shared-world Jolt provider

`CreateUnifiedJoltProvider` creates one Jolt `PhysicsSystem` and two existing
neutral adapters. The rigid adapter owns the native world; the soft adapter
retains a reference to it and must retire first. This factory is confined to
the experiment. Production `WorldRoot`, factories, serialization, and default
physics remain unchanged.

`PhysicsJoltProviderQualification` exercises the adapters with observable
physical gates: contact begin/end, shape replacement mass, a capsule stopped by
a wall, canonical mesh ray and release, cloth deflection with rigid feedback,
rubber pushing a rigid target, and stale rigid/soft IDs after slot reuse. It
uses the patched Jolt velocity reconstruction and two half-frame world updates,
each with eight collision steps and 12 soft integrations per step, matching the
prior qualified coupling schedule.
The fixture also uses `WorldRootTestAccess`, the existing friend seam in Engine
tests, to inject the provider into a fresh `DataModel` before admitting Parts or
Cloth. It verifies authored Part admission, motion publication, stream release
and readmission, Cloth vertex publication, and teardown through `Workspace`.

The soft adapter builds neutral cloth and regular rubber lattices directly
from `SoftBodyDefinition`. It retains neutral IDs, fixed topology indices, and
owned extracted positions. Moving attachment edits preserve the live native
body and deformation. Full definition edits replace the native soft body and
increment topology and vertex revisions. Each rigid step advances both body
kinds in the same Jolt world; the following soft step publishes those already
advanced states. No parallel Instance authority or backend handle crosses the
Engine boundary.
The unified factory fixes the experimental world at two Jolt half-frame updates
with eight collision steps each, even when `WorldRoot` supplies its production
four substeps. The plain rigid factory still honors its requested substep count.
`WorldRoot` applies pending forces, gravity, and attachment edits before rigid
`Step`; those inputs reach the native combined solve. The later soft `Step`
extracts that same accepted state. Its collider snapshot is currently redundant
work because native Jolt already owns the colliders.

Current limitations before production consideration:

- Soft collision mode `RigidPrimitives` currently admits native soft/soft
  collision too. Its quality and cost are not yet qualified against the
  neutral XPBD contract, which has no soft/soft contact.
- Soft body force is applied as a fixed 1/60-second velocity change at call
  time; the neutral XPBD backend accumulates force until its next step.
- The provider does not yet implement full canonical mesh residency accounting,
  byte budgets, or shared asset resource retirement metrics. It retains mesh
  buffers for each live body and releases them on body destruction.
- The capsule mover uses bounded shape casts and tangential clipping. Initial
  penetration correction, multiple simultaneous planes, and Box3D-equivalent
  floor/step behavior remain open.
- Rigid joint weld and connected-body collision exclusion remain unsupported.
  The existing experimental joint types still need authored limit/motor parity
  fixtures on the shared provider.
- Cloth, rubber, and reciprocal contact are represented by physical fixtures;
  they are not a quality-matched parity or performance claim. Self collision,
  swept soft contact, and long-duration material calibration remain unqualified.
