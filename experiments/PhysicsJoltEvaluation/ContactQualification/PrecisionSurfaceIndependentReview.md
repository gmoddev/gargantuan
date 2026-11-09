---
status: experimental-finding
authority: non-normative
date: 2026-11-09
---

# Precision and native face-response controls

`ReviewPrecisionSurface.py` independently checked the staged fixture and
compressed-trace SHA-256 values, exact source pins, all 18 XPBD source-accounting
identities, 12 matched native fixture definitions, and every no-contact frame's
position and velocity hashes. The machine reduction is
`C:/Users/aiden/.codex/artifacts/jolt-cloth-contact-correction-20261109/PrecisionSurfaceIndependentReview.json`.

## XPBD float-position momentum leakage

The corrected 18-case precision corpus uses Engine
`252340fa4566164eade9f0631cd333921c3f46f5` (including the actual
free-free structural float-position delta measurement) and native
`069b75b6222f19a8db2434a91fe2f72b3ae998de`. All geometry gates and
source-ledger identities pass. Each triplet below is N16/N32/N64; normalized
momentum values use the same named-impulse denominator in all three columns.

| Variant | Raw momentum residual | Measured free-edge rounding | Adjusted residual | Maximum unclosed physical energy residual, J |
| --- | --- | --- | --- | --- |
| A moving | .005510/.011920/.048015 | .005512/.011879/.048013 | .000057/.000217/.001245 | 18.455/28.349/19.141 |
| A static | .002930/.020231/.055879 | .002907/.020252/.055916 | .000040/.000217/.000984 | 22.142/6.055/4.439 |
| E moving | .004572/.014413/.034698 | .004634/.014478/.034535 | .000067/.000217/.001390 | 18.467/28.303/19.134 |
| E static | .004503/.016126/.070503 | .004530/.016342/.070503 | .000040/.000286/.000984 | 22.163/6.059/4.441 |
| G moving | .004382/.010940/.033153 | .004246/.010952/.033153 | .000136/.000369/.001537 | 18.716/28.405/19.153 |
| G static | .002930/.020231/.055879 | .002907/.020252/.055916 | .000040/.000217/.000984 | 22.142/6.055/4.439 |

The maxima are taken separately across frames, so raw and measured values in
the table need not identify the same frame. The source-ledger per-frame vector
identity does close after subtracting the measured free-free term. Across
the 18 maxima, raw/adjusted ratios are 21.57–112.41. The measured term
accounts for most of the raw momentum leakage and is **numerical error**, not
an external force or observed rigid reaction. Both raw and adjusted maxima
increase with finer substeps in every variant. Shrinking the time step
amplifies rounded world-position deltas when reconstructing velocity; these
data cannot support momentum-convergence or physical-energy-conservation
claims. The moving energy diagnostic is nonmonotonic; static held improves
from N16 to N64, with nonzero residual. The source-ledger identity is an
instrumentation check and the physical energy diagnostic remains ungated.

The earlier `Precision-20261109T111541` corpus had an older transferred
Engine binary and no free-free term. It is preserved separately and is not
combined with these corrected-source rows.

## Native sphere-face response

`BroadNative-20261109T104906` A and `Surface-20261109T110536` H/IG use
different Engine/native source pins, but all 12 compared authored `Case.json`
definitions agree after removing `CaseId`. The no-contact negative control
has identical position **and** velocity hashes at all 60 frames for A/H/IG.

| Fixture | A peak penetration | H peak penetration | IG peak penetration | IG peak axial strain |
| --- | ---: | ---: | ---: | ---: |
| 9² face center | 250 mm | 0.176 µm | 0 | 0.051% |
| 9² edge midpoint | 250 mm | 1.202 µm | 0 | 0.059% |
| 9² corner vertex | 2.783 mm | 0.044 µm | 0 | 0.086% |
| 9² plane ±10 µm | 61.32 mm | 0.501 mm | 5.597 µm | 1.865% |
| 9² plane ±1 cm | 61.29 mm | 0.391 mm | 0.960 µm | 0.738% |
| Original 128² exact plane | 0.800 mm | 2.797 µm | 2.630 µm | 68.065% |

H/IG pass the tiny face, edge, corner, and nonzero-offset geometry gates;
exact-plane 9² and original 128² still fail axial strain (IG 50.984% and
68.065%, respectively). The original IG exact-plane trace reports 582
projected orientation flip instances over its full trace. This is a large
surface-gap correction and not a correction of the exact-planar strain and
inversion path. Native H/IG physical qualification remains incomplete:
elastic energy, complete external work, reciprocal dynamic response, and
three-dimensional self-intersection are not established by these traces.
