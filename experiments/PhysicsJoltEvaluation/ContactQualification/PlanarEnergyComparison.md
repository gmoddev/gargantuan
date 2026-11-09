---
status: experimental-finding
authority: non-normative
date: 2026-11-09
---

# The feasible shear is not a low-energy solution

`ComparePlanarEnergy.py` rehashes the sealed original `BaselineClean.trace`
(`b9cc9db786d0843550c1e6fab1b2bcea457784d3f58b780caadb76b861ce759f`),
reads its frame-39 128² positions, and reconstructs the authored source graph:
32,512 axial, 32,258 diagonal shear, and 32,256 two-hop bend edges. It
computes the XPBD/Jolt distance-compliance spring surrogate
`U = Σ (|xᵢ−xⱼ|−L₀)² / (2 compliance)` with stretch compliance `1e-6` and bend
compliance `1e-4`. It evaluates the original snapshot and the analytic
collision-clear affine shear at the same final sphere pose `(4,3,0)` and
effective radius 1.05 m.

| Component | Original frame 39 | Affine planar shear |
| --- | ---: | ---: |
| Axial edge U | 121,244 J | 42,598 J |
| Diagonal shear edge U | 110,894 J | 2,335,640 J |
| Two-hop bend edge U | 5,999 J | 1,690 J |
| **Total distance-compliance U** | **238,137 J** | **2,379,928 J** |
| Maximum axial strain | 63.19% | 3.63% |
| Maximum diagonal strain | 28.64% | 14.41% |
| Sphere-shell penetration at final frame | 0.800 mm over 52 faces | 0 |

The shear has **9.994 times** the original frame's distance-spring energy,
almost entirely from diagonal shear spread over the whole 128² cloth. Its
low axial strain and collision clearance are a geometric feasibility
certificate; they are **not** evidence that the solver missed a lower-energy
planar state. The original frame has 0.800 mm surface penetration and 124
inverted faces, so it is not a feasible comparator under strict
nonpenetration and noninversion either. This two-endpoint comparison neither
finds the constrained minimum nor accounts for kinetic energy, gravity,
contact potential, damping, pin work, prescribed collider work, or retained
XPBD/Jolt multipliers.

Separate local evidence still matters: the same source spring surrogate has a
negative free-node transverse Hessian at original frame 6 (the first axial
gate failure), `-417,397 J/m²` at node 8318, while all faces remain positive
and the outer boundary simple. A ±10 µm Z lift lowers that local surrogate
without moving pins or worsening sphere distance. This is a local
energy-direction statement, not proof of dynamic buckling or a complete
physical correction. Exact planar initial data remain an invariant subspace
for in-plane structural forces and the source vertex-level radial coplanar
contact normals, so the
solver can remain on a high-strain planar trajectory even when a transverse
descent direction exists. Missing face contact and contact/structure ordering
are separately evidenced by the controlled solver screens.

The machine certificate is
`C:/Users/aiden/.codex/artifacts/jolt-cloth-contact-correction-20261109/PlanarEnergyComparison.json`
(SHA-256
`7ffc98217566f41f40e9037b3fc0e7d43725248a0af08bd277115b19af92a6f7`).
The independent frame-6 transverse certificate is
`C:/Users/aiden/.codex/artifacts/jolt-cloth-contact-correction-20261109/TransverseModeFrame6.json`
(SHA-256
`435fe0182051feff3502ca5e23183d09c2e54af7850cec2de5fbca2596aaae2ca`).
The sealed prior artifact root was read only.
