---
status: measured-constructive-mode
authority: non-normative
date: 2026-11-09
---

# A compressed planar snapshot has a transverse descent direction

The sealed original `CleanProfile/BaselineClean.trace` has SHA-256
`b9cc9db786d0843550c1e6fab1b2bcea457784d3f58b780caadb76b861ce759f`.
At poststep frame 34 its 128×128 cloth remains exactly in Z=0. The Engine
adapter creates 97,026 native edge constraints: horizontal, vertical and both
diagonals with stretch compliance `10⁻⁶`, and two-hop horizontal/vertical
constraints with bend compliance `10⁻⁴`. `ProveTransverseMode.py` reconstructs
that exact graph from `RigidJoltBackend.cpp` and scans all unpinned vertices.

For the **spring-potential surrogate**

```text
U = Σ_edges (L−L0)² / (2 C),
```

the second derivative for moving only one vertex transversely from a planar
snapshot is `Σ_incident (1−L0/L)/C`. Free vertex 6398, grid (126,49), has
11 incident edges and derivative `−1,981,366.0475 J/m²`. Moving it either
`+10 µm` or `−10 µm` in Z lowers the surrogate by `0.0000990683 J`; at
`±0.1 mm` the decrease is `0.00990677 J`. These finite differences agree
with half the analytic Hessian times displacement squared.

This motion leaves all top pins and XY projections fixed. It therefore
preserves projected triangle orientation, and each point of an incident
triangle or edge gains nonnegative squared distance to the sphere center in
Z=0. Sphere penetration cannot increase. The chosen vertex is itself about
0.02615 m outside the effective radius-1.05 m sphere at this snapshot.

This is an explicit negative Rayleigh direction for the stated edge-energy
surrogate. It confirms that the planar configuration need not be a local
minimum of that surrogate, while deterministic planar contact and edge updates
can remain trapped in the invariant subspace. The result does **not** claim
that Jolt's retained-lambda XPBD iterations minimize this potential, or that
total mechanical energy decreases once contact, damping, moving-surface work,
pin reactions and actual velocities are included. The full receipt is at
`C:/Users/aiden/.codex/artifacts/jolt-cloth-contact-correction-20261109/TransverseModeCertificate.json`.
