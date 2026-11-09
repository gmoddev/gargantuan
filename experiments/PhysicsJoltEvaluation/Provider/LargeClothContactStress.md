---
status: measured-negative-control
authority: non-normative
date: 2026-11-09
---

# Large cloth driven-contact stress result

The final performance candidate passed the no-collider quality comparison,
but the new 128×128 cloth driven-sphere stress scene failed the previously
declared absolute cloth-strain bound in **both** frozen B and C. The result is
retained as a physical-quality failure. The 16K driven-contact scene is outside
the accepted quality domain; B-to-C similarity alone cannot qualify it.

## Source and gate

The frozen-B long-run source is Engine
`c8da8d4db19272a3209fce3b83988b1a7a9a9144` with native Jolt
`d8b980a9f4c997ff9b715d6f17d48a15ecc6de37`. The C long-run source is
Engine `f4ffd911baab53ebee82a3d3d62e040568d17d7e` with native Jolt
`7e59df5fd7f3e23504f2e696e2b095bf706b26b4`; C uses the opt-in 1024
constraint batch for 16K cloth. Both were run on the same Windows worker with
eight total workers, 40 frames at 1/60 s, identical scene material, impulse,
and sphere drive. The two coupled receipts exited successfully. The exact
source manifests, raw binary traces, stdout, and receipts are in
`C-Long-20261109T070355`; the matching no-collider B reference is in
`B-Long-20261109T063118`.

`CoupledProtocol.json` was saved before either coupled candidate capture. It
keeps the original general cloth bound of maximum axial strain 0.10, maximum
pin error 0.0001 m, and measured primitive penetration 0.1 m, plus the fixed
all-frame B-to-C pointwise RMS 0.005 m and maximum displacement 0.015 m.
The 0.05 peak and 0.02 final strain limits in `QualityProtocol.json` belong
to the separate uncoupled 40-frame comparison; they were not substituted for
this coupled gate. No threshold was changed after observing these traces.

## Measured response

`AdmissionTraceCompare.py` streamed all 40 frames and rehashed every
recorded float32 position against each run's printed cumulative hash. All
positions were finite, the hash logs matched, and provider receipts checked
stable IDs, topology, advancing revisions, and no lost work. The measured
results are:

| Measure | Frozen B | C | Gate |
| --- | ---: | ---: | ---: |
| Peak absolute axial strain | 0.681171 | 0.680329 | <=0.10: both fail |
| Peak tensile axial strain, independent edge scan | 0.444204 | 0.444357 | <=0.10: both fail |
| Final absolute axial strain | 0.643880 | 0.631888 | Recorded |
| Maximum pin error | 0.000000237 m | 0.000000237 m | <=0.0001 m |
| Measured poststep penetration | 0 m | 0 m | <=0.1 m |
| Native contacting vertices | 9,594 | 9,586 | Positive response |

The peak B-to-C all-frame RMS was 0.000661900 m and peak vertex displacement
was 0.002744375 m, below the fixed 0.005/0.015 m comparison bounds. Both
driven traces diverged from their matching no-collider controls. The initial
geometric surface gap was 0.200413 m; both runs entered the contact band.
These passing *relative* and contact-response checks do not cancel the
absolute strain failure. The report is retained as
`AdmissionReduction/C-Coupled-driven-sphere.json` with `Pass:false` and
`PeakAxialStrain:false`.

The static-sphere control passed its comparison but had zero contact band and
zero captured contacting vertices in both B and C. Its peak absolute strain
was about 0.0412. It is collider-presence overhead evidence, not demonstrated
collision response. The no-collider 600-frame cloth16K C run passed the
fixed all-frame comparison to B (peak RMS 0.000646364 m, maximum vertex
displacement 0.001817667 m); cloth1K, cloth4K, and rubber4K traces were
bitwise identical to B throughout 600 or 300 frames. Their reports are
retained under `AdmissionReduction/C-Long-*.json`.

## Contact-local strain mechanism

`ProviderBenchmark.cpp` begins a radius-1 m anchored sphere at x=5.2 m,
0.2 m beyond the cloth's right edge at x=4 m. It moves the center 1.2 m
left over 0.66 s. The final sphere surface reaches x=3 m, about 1 m or
16 axial grid spacings into the cloth's original right boundary. Jolt moves
the anchored native body at each frame boundary. The cloth particle material
thickness is 0.05 m.

At frame 32, the peak compressed horizontal edge starts at grid coordinate
(126,52). Its rest length is about 0.06299 m. B/C lengths are
0.02008/0.02014 m, or 68.12%/68.03% compression. The sphere center is at
x=4.2 m and one endpoint in each trace lies within about 0.0000003 m of
the 0.05 m thickness shell around it. At frame 37, the peak tensile vertical
edge at (127,71) has B/C lengths 0.09097/0.09098 m, or 44.42%/44.44%
stretch; one endpoint is again on that shell. The same edges in frozen-B's
no-collider trace remain near rest length. The severe deformation is
localized around the moving contact boundary and appears in B and C at the
same frames and grid locations. Finite positions, native contact callbacks,
and the zero geometric penetration proxy do not establish acceptable cloth
shape or energy behavior.

## Qualification scope

The earlier 16K no-contact 40-frame quality claim in
`OptimizedFoundationAndBackendDecision.md` was explicitly for 0.667 s
without contact. Its separate driven-contact controls established actual
callbacks and divergence from no-collider trajectories, but did not
establish a 10% strain envelope for 16K driven cloth. `FeatureMatrix.md`
expressly limits selected coupled fixture evidence to 9×9 cloth and 5³
rubber, not 4K/16K contact quality. Those original small native and unified
contact/energy gates remain required and are not weakened by this new stress
failure.

The existing no-collider and static-sphere controls already discriminate the
moving-contact load from a general admission or free-solve instability. No
fixture forcing, material, or solver setting was changed to make this result
pass. Improving 16K driven-contact deformation is future physical-quality
work; any new candidate must rerun the same frozen stress gate before that
domain can be claimed. No energy conclusion is drawn from position traces
or geometric contact proximity.
