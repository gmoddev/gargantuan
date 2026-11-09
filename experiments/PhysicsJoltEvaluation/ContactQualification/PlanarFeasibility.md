---
status: constructive-geometric-certificate
authority: non-normative
date: 2026-11-09
---

# The planar contact fixture is geometrically feasible

The original 128×128 cloth occupies `[-4,4]×[-1,7]` at Z=0, with every
top-row vertex pinned at Y=7. The sphere has radius 1 m and the cloth's
particle shell is 0.05 m, so this certificate uses effective radius
`R=1.05 m`. The center moves linearly from `(5.2,3,0)` to `(4,3,0)` over
0.66 s. This is the prescribed geometric trajectory, not a solver result.

For progress `s∈[0,1]`, define `k(s)=s·R/sqrt(16−R²)` and map every cloth
point by

```text
F_s(x,y,0) = (x − k(s)(7−y), y, 0).
```

At Y=7 this leaves all pins exactly fixed. Its planar Jacobian is
`[[1,k],[0,1]]`, so its determinant is 1: every oriented triangle keeps
positive signed area, and no element folds or inverts in the plane. Horizontal
axial edges retain length; vertical axial edges acquire length factor
`sqrt(1+k²)`. At the final and worst shear, `k=0.2720398972` and axial
strain is `0.0363424654`, below the original 0.10 limit.

For every transformed point from an original `x≤4`,
`x'−k y'−(4−7k)=x−4≤0`. Convexity puts every transformed triangle in this
same closed half-plane. The sphere center is on the other side of the boundary
line, at distance

```text
d(s) = [1.2(1−s)+4 k(s)] / sqrt(1+k(s)²).
```

Let `K=R/sqrt(16−R²)`. Since `1.2−4K>0`, the derivative of
`d(s)=[1.2−(1.2−4K)s]/sqrt(1+K²s²)` is negative over `[0,1]`.
At `s=1`, `d(1)=R`; therefore `d(s)≥R` throughout the motion. The sphere's
effective disk and every cloth face, edge, and vertex have no overlap. This
proof covers the continuous sheared surface, not only sampled vertices.

`ProvePlanarFeasibility.py` independently enumerates the exact 128×128 grid
over 40 physical frames: 1,290,320 oriented triangle instances, all top pins,
all axial edges via their common affine length factors, and the half-plane
clearance bound. It reports maximum numerical half-plane violation
`4.44×10⁻¹⁶ m`, minimum signed area ratio `0.9999999999999895`, zero pin
drift, and zero minimum clearance slack at the final tangent. The exact
certificate is retained at
`C:/Users/aiden/.codex/artifacts/jolt-contact-diagnosis-20261109/PlanarFeasibilityCertificate.json`.

This disproves geometric incompatibility under **the original axial-edge
10% strain gate** combined with pin, nonpenetration, and planar noninversion.
The same shear stretches one family of triangle diagonals by about 14%, so it
does not certify a 10% limit on every structural edge. It also does not solve
the authored initial-value problem: the original cloth has a +X launch
velocity, gravity, damping, contact friction and finite-mass dynamics. The
constructed path prescribes different velocities and requires external work.
The measured planar failure therefore remains a solver-trajectory and contact
ordering question, rather than a proven geometric impossibility.
