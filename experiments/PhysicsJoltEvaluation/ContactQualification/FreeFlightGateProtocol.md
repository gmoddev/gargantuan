---
status: prespecified-experimental-gate
authority: non-normative
date: 2026-11-09
---

# Analytic free-flight and raw cloth-momentum gates

These bounds were set before observing the double-shadow precision-mode-2
traces. `ValidateFreeFlight.py` applies them to the full V1 position and
velocity history of the 9² free cloth with all vertices initially at
`(2,0,0)` m/s, no pins, gravity, damping, or colliders. The exact solution is
rigid uniform translation. From the published full state and authored masses,
the independent inspector requires all four maxima across all vertices and
frames:

| Quantity | Limit |
| --- | ---: |
| Per-vertex velocity error norm from `(2,0,0)` | ≤ 1e-5 m/s |
| Per-vertex position error norm from rest + velocity × physical time | ≤ 2e-6 m |
| Relative total cloth momentum drift from initial | ≤ 1e-5 |
| Relative total cloth kinetic energy drift from initial | ≤ 1e-5 |

`InspectEnergyLedger.py` separately requires maximum direct **raw** cloth
momentum residual ≤ 1e-5 of the named-impulse scale for the contact and
pinned-reaction controls. It recomputes raw residual each frame as published
`ΔΣmV` minus source-named gravity, authored impulses, damping, pin reaction,
and contact impulses. It does not subtract the would-be free-edge float
position-store error for precision mode 1 or 2. The `1e-5` threshold is the
same 10 ppm bound, fixed before mode-2 results. A closure pass does not imply
global energy conservation or a reciprocal rigid reaction. All original
10% axial strain and 1 mm surface penetration geometry gates remain in force.

The preserved A and ideal-velocity mode-1 H control traces discriminate the
analytic free-flight gate:

| Substeps | A max velocity error / max position error | A relative P / K drift | H1 max velocity error / max position error | H1 relative P / K drift |
| --- | --- | --- | --- | --- |
| 16 | 4.494e-5 m/s / 6.154e-6 m | 1.567e-5 / 3.134e-5 | 0.005409 m/s / 1.947e-6 m | 9.787e-8 / 5.438e-7 |
| 32 | 0.0008850 m/s / 8.591e-5 m | 2.182e-4 / 4.365e-4 | 0.006516 m/s / 2.451e-6 m | 5.225e-8 / 1.042e-6 |
| 64 | 0.001422 m/s / 9.792e-5 m | 2.456e-4 / 4.912e-4 | 0.006224 m/s / 3.129e-6 m | 7.653e-8 / 8.638e-7 |

All six fail the complete analytic gate. H1 passes the total P/K bounds,
showing why per-vertex and position checks are necessary. The A/H1 traces
are preserved under `IdealControls-20261109T112942` in the new artifact root.
Their source and full frame payload hashes were independently checked in
`IdealControlsIndependentReview.json`. Precision mode 2 must be evaluated
with the same code and limits; no result has yet been observed here.
