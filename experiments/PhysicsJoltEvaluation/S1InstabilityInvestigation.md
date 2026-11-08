# S1 contact failures and nonlinear cloth instability

Experimental investigation, 2026-11-08 UTC. Derived from the preserved
`3e54402ecc46580dbc8f6f93d4354ee4207997bb` continuation of draft PR19.
Jolt remains pinned to `e77f175595e64cb44218cc9d9d56fc365ad0e36a`.
Production physics, mainline, and PR19's head were not changed or merged.
Unified Jolt qualification has **not** started.

The immediate numerical gain is now measured at individual edge operations,
and nonlinear local expansion is demonstrated at actual deformed cloth states.
The compliant S1 failures additionally depend strongly on the outer coupling
cadence and repeated contact-position loading. These are experimental causal
findings, not a qualified correction or a general upstream-defect verdict.

## Immutable executions

| Run | Executed source | Scope |
| --- | --- | --- |
| [42434395381](https://github.com/gmoddev/gargantuan/actions/runs/42434395381) | `7615e55024a924388717a3af2a187812f86dc6d8` | Pristine/native observation parity, S1/S4 controls, rest spectrum and native mode injection |
| [42435117794](https://github.com/gmoddev/gargantuan/actions/runs/42435117794) | `15f043aebbbfbb22b13bb31cc1d81be19e62157c` | Projection ablations, matched soft-work cadence, precision replay |
| [42435811640](https://github.com/gmoddev/gargantuan/actions/runs/42435811640) | `830f165afe0366af40b0f63be8c30ac5930847f4` | Per-edge work, nonlinear Jacobians and geometry controls; no Jolt build |

All three hosted runs completed successfully. This means their builds and
harnesses completed; physical failures remain failures. There were 64 native
Jolt processes and 30 standalone C++ projection replays, plus four hosted Python
numerical analyses and the retained independent reductions. No original
1,520-process matrix or production target was
rerun. GCC 13.3.0 on Ubuntu 24.04 compiled the probes with at most two concurrent
build jobs. NumPy 2.3.4 used one BLAS thread in disposable virtual environments.
The shared Windows worker was reserved by another campaign at admission, so
hosted execution was used. This campaign acquired no worker lease; the final
coordinator status was Free after the other campaign released its reservation.

Evidence is preserved separately under
`C:\Users\aiden\.codex\artifacts\jolt-s1-instability-20261108`, including raw
JSONL, all three hosted artifacts, native binaries, generated upstream patches,
build logs, source IDs, analytical scripts, independent reviews, and a SHA256
manifest. Previous campaign source and artifacts remain unchanged.

## S1: internal measurements change the endpoint-only assessment

The material-only cases retain structural compliance `1e-5`, bend compliance
`1e-4`, friction `0.2`, 24 soft integrations per outer step, fixed pins and zero
gravity. All kinetic figures below include rigid angular energy. The unchanged
necessary rejection ceiling is `1.05 * InitialK + 1e-5`.

| Case | Initial K / ceiling, J | Published maximum K, J | First internal kinetic crossing |
| --- | --- | --- | --- |
| C1 S1 | 3.339905664 / 3.506910948 | 3.464212182 | Frame 18, local iteration 20, vertex 84 reconstruction: 3.473338928 → **3.553744736** J; post-contact 3.521222534 J |
| C2 S1 | 6.679811329 / 7.013811895 | **17.162413030** | Frame 18, local iteration 2, vertex 100 reconstruction: 3.802580531 → **13.998738368** J; post-contact 14.976967098 J |

Frames and outer steps are zero-based; local iterations are one-based. S1 has
one outer step. C1's old kinetic pass applies only to published frame endpoints.
It does not survive observation inside the step.

The frame-18 kinetic ledgers close as follows:

| Case | Integration/damping ΔK | Edge-position/reconstruction ΔK | Dynamic impulse-pair ΔK | Net ΔK, J |
| --- | ---: | ---: | ---: | ---: |
| C1 S1 | -0.143116526 | +3.496856596 | -1.825194348 | +1.528545816 |
| C2 S1 | -0.670153565 | +20.327556726 | -7.770560350 | +11.886840999 |

Maximum frame closure residual is `1.81e-6 J`. Every measured dynamic contact
pair in the material C1/C2 S1/S4 windows loses kinetic energy; maximum linear
momentum closure residual is `7.42e-8 kg m/s`. Thus the rebound is not evidence
that the velocity-impulse operation itself adds energy.

Contact-position corrections load the edge network before later reconstruction.
For C2 at frame 18/local 1, vertex 100 is displaced by `0.078010738 m`; the
observer's positive-compliance edge estimate rises by `55.034196 J` in that
event. The first substep's contact corrections add `66.387035 J` to that
estimate. The next edge sweep reduces it while reconstruction adds kinetic
energy. This estimate, `Uhat = sum C^2/(2*compliance)`, is a spring analogy,
not a complete solver-internal mechanical-energy balance. C2's necessary K
failure and C1's internal K failure do not depend on accepting Uhat as potential.

## Contact and cadence isolations

Refreshing sphere tangent planes at the **same frozen rigid center** barely
changes C2 S1's published maximum K: `17.162413 → 17.038077 J`. Disabling friction
also leaves it at `17.079877 J`. Measured plane-depth error is at most
`0.000194 m`, compared with an actual projection depth of `0.0780 m`.
Stale tangent geometry and friction are not the dominant explanation here.
This refresh does not rerun the broadphase or reselect the other sphere in C2.

Suppressing only that first vertex-100 correction does **not** remove the
failure: the next iteration applies a `0.07559367 m` correction and the first
kinetic breach moves from local iteration 2 to 3. Published maximum K remains
`17.065766 J`. Half correction behaves similarly (`17.066409 J`). This is
repeated strain loading, not a single erroneous impulse or isolated event.

Disabling every contact-position correction keeps published K within initial K,
but C2 penetrates by `0.070394 m` and contact callbacks increase from 5 to 308.
It is an intentionally incomplete collision control, not a quality-matched fix.
Stopping contact after frame 17 preserves its preloaded state and avoids the
later published failure only by permitting passage through the cloth; stopping
after frame 18 retains the failure. Extra edge sweeps are not a cure: C2 S1's
published K rises to `36.482024 J` with two and `75.062465 J` with four sweeps.

The stronger temporal comparison holds **both** the inner timestep and total
soft integrations per frame fixed while varying outer rigid/contact cadence:

| C2 schedule | Soft integrations/frame | Inner dt | Published max K, J | Max internal post-contact K in traced frames 17–20, J |
| --- | ---: | --- | ---: | ---: |
| S1 / I24 | 24 | 1/1440 s | 17.162413 | **17.686608** |
| S2 / I12 | 24 | 1/1440 s | 6.679811 | 6.679811 |
| S1 / I48 | 48 | 1/2880 s | 28.521996 | 29.881008 |
| S2 / I24 | 48 | 1/2880 s | 6.679811 | 6.880913 |
| S2 / I48 | 96 | 1/5760 s | 6.679811 | **7.880514**, first breach frame 18/outer 0/local 4 |
| S4 / I24 | 96 | 1/5760 s | 6.679811 | 6.679811 |

Outer coupling cadence is a confirmed contributor independent of soft timestep
and total soft work. These comparisons vary rigid pose/plane/contact collection
and velocity writeback together; they do not isolate those components from each
other. Endpoint passes also hide internal failures: S2/I48 breaches the kinetic
ceiling, while S2/I12 and S2/I24 reach internal `K+Uhat = 14.019053 J` despite
their endpoint results. None is promoted to unified-provider qualification.

## Underlying hard-cloth instability

The original contact-free center-Z seed has initial K `0.05 J`. The native S4
hard cloth reaches `1227.840903 J` at frame 73 without any contact. First positive
net edge-candidate work occurs at frame 0/outer 2/local 10 (flattened ordinal 58).
The first internal 5% crossing is frame 3/outer 1/local 18 (ordinal 42).

### Rest modes and precision are insufficient explanations

The optimized native edge order was exported, rather than inferred from authored
order. The flat rest-state XY update spectral radius is below one (`0.996244`
for S1, `0.997046` for S4); the rest Z block is neutral. Native injection of the
predicted in-plane modes follows their early decay. Thus an unstable **flat rest
linear mode** is excluded for this fixture. Unseeded native cloth remains still.

A separate scalar C++ replay implements native integration, optimized edge order,
compliance scaling, damping and velocity reconstruction. Float/captured replay
matches every native particle P/V component in S4 frames 0–7 exactly; all
150-frame K observations agree within the observer-accumulation difference
(`1.41e-5 J` maximum). Adding the observational per-edge trace preserves **all
150 replay frame rows exactly**.

| S4 replay arithmetic | Rest lengths | Published-frame maximum K, J |
| --- | --- | ---: |
| float | captured native | 1227.840893 |
| double | captured native | 1334.341330 |
| double | recomputed from initial positions | 1249.254417 |
| extended precision | captured native | 1223.276514 |
| extended precision | recomputed from initial positions | 1251.924927 |

Float32 solver arithmetic and captured edge-rest-length mismatch are **not
necessary** for this instability. The widened runs retain the exported initial
positions and float-derived timestep; this is not proof that every discretization
choice is irrelevant. Peak timing differs, so high-precision paths are not
claimed to match native trajectories point by point.

### Nonlinear residual corrections do positive work

For candidate velocity `v=(x-x_previous)/h`, an edge position correction with
velocity change `dv` has the exact kinetic decomposition
`DeltaK = sum m*v.dot(dv) + 0.5*sum m*|dv|^2`.
At the first net-positive native sweep, calibrated replay measures
`-0.079509867013 + 0.079545106704 = +0.000035239691 J`.
Per-edge decomposition closes within `3.47e-18 J`.

For example, optimized edge 402 (vertices 71–84) is still stretched by
`8.693337e-5 m`, inherited from `9.605289e-5 m` at the prior positions. Its
relative normal candidate velocity is already closing the gap at
`-0.052448 m/s`. The position correction reinforces that closing velocity and
adds `0.007581277 J` of candidate K. Other edge corrections remove much of it;
the sweep leaves the measured positive remainder. Cardinal edges contribute
net `+0.003183014 J`, diagonals `-0.003150404 J`, and bend edges
`+0.000002629 J`. It would be incorrect to attribute all positive work to
compressed diagonals alone.

The deformed projection map also differs from its flat-rest linearization.
For an edge with current length L and target r, hard projection has tangential
relative derivative r/L; compressed edges therefore expand transverse
perturbations. Differentiating the actual ordered nonlinear sweep at preserved
native states includes this residual-dependent derivative. At frame 0/ordinal 58,
the projection spectral radius is `1.000067518` and the position/velocity update
radius is `1.008077308`. At the first 5% crossing it is `1.005884320`.
Two independent directional finite-difference checks per snapshot agree within
`2.3e-10` relative error.

The first substep already has a slightly expanding local derivative while its
actual kinetic energy decreases. Local spectral expansion alone is therefore
not sufficient evidence that the current trajectory gains energy.

These establish nonlinear local expansion and exact positive numerical work,
not a proof that a single eigenmode persists throughout the time-varying
150-frame trajectory. The accumulating gain arises in an incompletely satisfied,
ordered constraint network followed by velocity reconstruction. A complete
global stability theorem or unique nonlinear saturation mode remains unresolved.

### Geometry controls bound the claim

Double/recomputed S4 replay removes edge families while preserving the remaining
order. Published-frame maxima are `1249.254417 J` for original cloth; removing one diagonal family
still reaches `3750.852079 J`; removing both gives `0.039907261 J`. Removing only
positive-compliance bend edges still reaches `1593.477943 J`; removing both
diagonal and bend families gives `0.046060859 J`.

The diagonal constraint network is a confirmed contributor in these controls;
double bracing and the positive-compliance bend edges are not necessary.
Removing constraints changes shear/deformation behavior and is not a correction
or a quality-matched pass. One-pin, tangent-two-pin and orthogonal-two-pin
single-particle controls show no runaway in their sampled outputs, so instability is not attributed to
every hard edge. The smallest confirmed reproducer retained here is the exported
169-vertex cloth with the standalone ordered-edge replay, independent of Engine,
Jolt jobs, rigid bodies and contact.

The separate free-cloth compliance `1e-5` controls also show no large kinetic
runaway in 150 published frames: S1/S4 maximum K is `0.035439388/0.038604458 J`.
However, S1's maximum `K+Uhat` is `0.055703357 J`, above its `0.05 J` initial K;
softening the material is not promoted to a blanket energy qualification.

## Reproduction and boundaries

With the repository's pinned JSON headers initialized:

```sh
c++ -std=c++23 -O2 -ffp-contract=off -fno-fast-math -Ivendor/json/include \
  experiments/PhysicsJoltEvaluation/ProjectionPrecisionReplay.cpp -o replay
./replay --graph experiments/PhysicsJoltEvaluation/CausalFixtures/NativeOptimizedClothGraph.json \
  --precision double --rest recomputed --steps 4 --frames 150 --seed-z 1 --particles 0
```

The hosted workflow's explicit `s1_diagnostic_only`, `s1_followup_only` and
`projection_causal_only` routes reproduce the three bounded campaigns. Normal
experiments link pristine pinned Jolt. Changes to edge-pass count, sphere-plane
refresh and contact-projection scale require the separately generated `--s1`
diagnostic patch; pristine builds reject their non-default values. Native scene
controls remain separate fixture options. `S1PhaseLedger.py`, `AnalyzeNativeModeResponse.py` and
`AnalyzeDeformedProjection.py` retain the reductions and their model limits.

Source mechanisms are visible in pinned upstream
[integration](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/SoftBody/SoftBodyMotionProperties.cpp#L325),
[edge projection](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/SoftBody/SoftBodyMotionProperties.cpp#L573),
and [reconstruction/contact handling](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Jolt/Physics/SoftBody/SoftBodyMotionProperties.cpp#L700).

Independent source, raw-stream energy, momentum, native/replay parity, precision
and nonlinear-Jacobian reviews are preserved in the evidence directory, including
rejected hypotheses and unsuccessful smaller controls. There is no new physics
timing comparison, no claim of XPBD superiority, no production patch, no merge,
and no backend migration recommendation. Further qualification requires an
admissible material/contact configuration with internal-step quality evidence;
these diagnostic ablations do not supply one.
