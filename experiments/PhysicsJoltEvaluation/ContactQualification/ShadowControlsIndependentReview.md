---
status: experimental-finding
authority: non-normative
date: 2026-11-09
---

# Double-shadow small-control qualification

This reduction applied the **precommitted** bounds in `FreeFlightGateProtocol.md`
to `ShadowControls-20261109T120317` without changing a threshold. The source is
Engine `7f2fd23cd3b3846e6486e851270af9b1069f734e`, native
`1691460cdcd9bd401c877cc219f38a89071a76ca`, one Windows worker.
`ReviewShadowControls.py` independently verified source/manifest pins,
receipt and stdout/stderr hashes, compressed trace and fixture hashes, all
720 float32 position/velocity frame payload hashes, all 60 geometry gates,
source accounting identities and precision-mode flags, and authored case
equality within each A/AH/AS/ES/GS comparison. All 18 overlapping A/AH
payload hash histories exactly match the earlier frozen
`IdealControls-20261109T112942` corpus.

## Prespecified analytic free uniform launch

The free 9² cloth has all vertices launched at `(2,0,0)` m/s, with zero
gravity, damping, pins, and contacts. The double-shadow AS/ES/GS modes pass
all four gates at N16, N32 and N64. They produce the same published
full-frame hashes within each substep count.

| Mode | N16 / N32 / N64 max vertex velocity error | Max position error | Relative total P drift | Relative total K drift | Complete analytic gate |
| --- | --- | ---: | ---: | ---: | --- |
| A | 4.49e-5 / 8.85e-4 / 1.42e-3 m/s | up to 9.79e-5 m | up to 2.46e-4 | up to 4.91e-4 | Fail all three |
| AH, ideal velocity mode 1 | 0.005409 / 0.006516 / 0.006224 m/s | up to 3.13e-6 m | ≤ 9.79e-8 | ≤ 1.04e-6 | Fail all three |
| AS/ES/GS, double shadow mode 2 | **0 / 0 / 0 m/s** | **2.346933e-7 m** | **0** | **0** | **Pass all nine** |

The thresholds remain per-vertex speed ≤1e-5 m/s, position ≤2e-6 m,
relative total momentum and kinetic drift ≤1e-5. The exact zero velocity,
momentum and kinetic error is for the published float32 samples of this
bounded analytic fixture, not a general numerical-exactness claim.

## Direct raw cloth momentum under contact and pins

Mode 2 passes the separately prespecified maximum direct raw momentum
residual ≤10 ppm of the named-impulse scale in all 18 moving/static sphere
contact and nine pinned-launch cases. The worst mode-2 contact result is
`1.955715e-6` (AS moving, N64); the worst pinned result is
`7.815900e-8` (AS/ES/GS pinned, N64). The pinned fixtures genuinely
exercise structural reaction: the largest AS pinned reaction impulse per
frame is 17.327, 10.697 and 9.491 Ns at N16, N32 and N64. The A baseline
fails the direct raw gate in all corresponding cases; AH passes. Source
`PinConstraintNumericalImpulseNs` is nonzero on A/AH pinned controls,
while mode 2's double solve has no float position-store pin gap.

All original geometry gates pass these **small** controls. The mode-2
unclosed full-frame physical energy residual remains nonzero and near A/AH:
AS moving at N16/N32/N64 gives 18.457/28.345/19.191 J; AS static gives
22.139/6.048/4.449 J. Kinetic stage-gap identities are bookkeeping and
do not establish energy conservation, complete external work, or reciprocal
rigid momentum. Broad contact configurations and long settling remain
separate qualification work. The machine reduction is
`C:/Users/aiden/.codex/artifacts/jolt-cloth-contact-correction-20261109/ShadowControlsIndependentReview.json`.
