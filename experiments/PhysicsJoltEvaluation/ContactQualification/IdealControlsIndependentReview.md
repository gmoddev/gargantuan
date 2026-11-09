---
status: experimental-finding
authority: non-normative
date: 2026-11-09
---

# XPBD ideal-velocity paired controls

`ReviewIdealControls.py` independently checked the exact Engine pin
`27cdfdf4ec518275b5e934318070aa3cb1f1a532`, native pin
`069b75b6222f19a8db2434a91fe2f72b3ae998de`, packaged fixture and
trace SHA-256 values, all 648 float32 position/velocity frame hashes, 27
paired authored case definitions, and 54 geometry and source-ledger verdicts.
The machine reduction is
`C:/Users/aiden/.codex/artifacts/jolt-cloth-contact-correction-20261109/IdealControlsIndependentReview.json`.
All 54 cases pass their geometry gates and source accounting identities.
H is an opt-in experiment; the unmodified A/E/G runs in the same source use
their original velocity reconstruction.

## Raw cloth momentum

The table shows the maximum, over 12 frames, of the norm of **published**
cloth momentum change minus named gravity, authored impulse, damping, pin,
and contact impulses, normalized by the same named-impulse scale. H's
would-be free-edge float error is independently reported and is **not**
subtracted from raw momentum. Triplets are N16/N32/N64 substeps.

| Pair | A/E/G raw residual | AH/EH/GH raw residual | Maximum unclosed physical energy residual, A → H, J |
| --- | --- | --- | --- |
| A moving | .005510/.011920/.048015 | 2.395e-7/7.931e-7/2.051e-6 | 18.455/28.349/19.141 → 18.455/28.343/19.195 |
| A static | .002930/.020231/.055879 | 2.399e-7/3.099e-7/8.821e-7 | 22.142/6.055/4.439 → 22.140/6.047/4.448 |
| E moving | .004572/.014413/.034698 | 5.007e-7/4.824e-7/2.275e-6 | 18.467/28.303/19.134 → 18.469/28.289/19.152 |
| E static | .004503/.016126/.070503 | 1.953e-7/3.225e-7/8.821e-7 | 22.163/6.059/4.441 → 22.164/6.053/4.449 |
| G moving | .004382/.010940/.033153 | 4.681e-7/6.939e-7/1.956e-6 | 18.716/28.405/19.153 → 18.721/28.394/19.195 |
| G static | .002930/.020231/.055879 | 2.399e-7/3.099e-7/8.821e-7 | 22.142/6.055/4.439 → 22.140/6.047/4.448 |

The maximum raw momentum residual improves by roughly 9,130–79,922 times
for these 18 contact pairs, while measured full-frame physical energy
residuals remain essentially unchanged and nonmonotonic with substeps.
Source kinetic reconstruction-gap terms make the stage ledger close; that
closure is bookkeeping and does not establish physical energy conservation.
The maxima in each column may arise on different frames.

## Free uniform-launch negative control

This fixture has a free 9² cloth, all vertices authored at `(2,0,0)` m/s,
no sphere, gravity, damping, or prescribed work. Ideal uniform translation
would keep every vertex velocity there. A/E/G are identical on this
negative control, as are AH/EH/GH.

| Substeps | Raw momentum A → H | Max vertex `||v−(2,0,0)||`, A → H | RMS vertex error over 12×81, A → H | Final kinetic energy A → H, initial 16.200000 J |
| --- | ---: | ---: | ---: | ---: |
| 16 | 1.502e-5 → 3.338e-7 | 0.00004494 → 0.005409 m/s | 0.00003103 → 0.001266 m/s | 16.199504 → 16.200002 J |
| 32 | 2.077e-4 → 5.603e-7 | 0.0008850 → 0.006516 m/s | 0.0004307 → 0.001743 m/s | 16.206899 → 16.200013 J |
| 64 | 9.550e-4 → 1.025e-6 | 0.001422 → 0.006224 m/s | 0.0004925 → 0.001769 m/s | 16.192043 → 16.200012 J |

H improves total cloth momentum 45/371/932 times and preserves global
kinetic energy much better, but increases local velocity error by about
3.6–40.8 times in RMS and 4.4–120 times in maximum error. The source bias
applies ideal per-edge structural corrections even when float world-position
writes round away part of those corrections. This is a plausible explanation
for the new local jitter, not yet a proven per-edge attribution. Net
momentum alone is therefore insufficient as an acceptance metric.

`PinConstraintNumericalImpulseNs` is zero in all 54 cases. The ideal pin
reaction branch is source-reviewed but not empirically stressed here; a
pinned-edge motion case should exercise it before broad adoption. No full
physical qualification is claimed: reciprocal rigid momentum, elastic
energy, complete work and dissipation, and nonlocal self-intersection remain
incomplete in these controls.
