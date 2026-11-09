---
status: measured-investigation
authority: non-normative
date: 2026-11-09
---

# Why the driven 16K cloth fails

The dominant trigger is our perfectly coplanar fixture. Jolt's contact projection
then creates a large local edge residual. Our moving-static-body semantics make
the result worse, but are not the sole cause. This is not evidence that the final
performance optimizations introduced the failure: the instrumented baseline is
byte-identical to the preserved final-C driven trace, and frozen B failed too.

## Discriminating evidence

21 diagnostic configurations cover 960 physical frames. Every float32 position
stream was independently rehashed against its sequential native FNV log and
receipt digests. An additional clean profiling baseline repeats the original
40 frames exactly. The original failed fixture, thresholds and qualified public
checkpoints remain unchanged; altered fixtures are not qualification replacements.

| Configuration | Peak tensile strain | Peak compression | Out-of-plane motion | Original 10% axial limit |
| --- | ---: | ---: | --- | --- |
| Original Jolt | 44.44% | 68.03% | Exactly zero | Fail |
| Remove launch impulse | 42.50% | 68.48% | Exactly zero | Fail |
| Eight pose updates, same 80 integrations/frame | 38.27% | 52.79% | Exactly zero | Fail |
| True velocity-driven kinematic sphere | 39.75% | 52.18% | Exactly zero | Fail |
| Eight constraint sweeps instead of two | 47.69% | 69.71% | Exactly zero | Fail |
| 100× lower stretch compliance | 126.35% | 96.40% | Exactly zero | Fail |
| Sphere center shifted 10 micrometres in Z | 6.35% | 3.14% | Up to 0.410 m | Pass axial metric only |
| Sphere center shifted 1 cm in Z | 6.00% | 3.00% | Up to 0.433 m | Pass axial metric only |
| Kinematic motion plus 1 cm Z offset | 6.56% | 2.95% | Up to 0.335 m | Pass axial metric only |
| Experimental XPBD High32, original plane | 42.66% | 36.79% | Exactly zero | Fail |
| Experimental XPBD High32, 1 cm Z offset | 5.86% | 3.03% | Up to 0.355 m | Pass axial metric only |

Zero friction, ten times smaller thickness, four times slower motion, and eight
pose updates without launch also fail the original planar case. See the complete
21-case table in the artifact package. Merely changing a body from static to
kinematic while continuing to set its pose produces a bitwise-identical trace.
The true kinematic probe uses `BodyInterface::MoveKinematic`, not pose assignment.

The positive and negative 1 cm offsets have **bitwise-identical XY positions** at
all vertices in all forty frames. All 472,679 nonzero Z values differ only by the
sign bit. This mirror control and the 10 micrometre control discriminate symmetry
breaking from a substantially gentler load or a different material.

## Fixture: the primary responsibility is ours

The 8 m cloth's rest vertices and pins have Z=0. Gravity is in Y, the initial
impulse is in X, and the sphere center has Z=0. Every edge correction and radial
contact normal therefore stays in the XY plane. The trace confirms that no
vertex ever leaves it in the original fixture, in either Jolt or High32 XPBD.
There is no transverse perturbation from which the cloth can choose a buckling
direction. A deterministic solver does not spontaneously manufacture one.

We then move a radius-1 m sphere about one metre into the cloth's original right
boundary. That is roughly sixteen grid spacings. In the exact invariant plane,
contact forces a compressed in-plane mesh rather than an out-of-plane wrapping
route. A minute transverse offset opens that route and changes the response
dramatically without retuning the material or solver.

The original test also launches the movable cloth vertices at approximately
+2 m/s toward a sphere moving at approximately −1.82 m/s. Removing the launch
does not cure the planar failure, so the launch is an aggravating load rather
than a necessary cause. The 0.05 m particle radius is large relative to 0.063 m
grid spacing, but reducing it to 0.005 m does not cure the failure either.

## Adapter and motion semantics: a real contributing distinction

The fixture creates an anchored static sphere and calls `UpdateBody` once per
frame. The adapter uses `SetPositionAndRotation`; it changes pose/broadphase but
does not supply the prescribed moving-surface velocity. Jolt explicitly
distinguishes static bodies from velocity-driven kinematic bodies in its
[pinned architecture documentation](https://github.com/jrouwe/JoltPhysics/blob/e77f175595e64cb44218cc9d9d56fc365ad0e36a/Docs/Architecture.md).

The original jump is about 0.0303 m per frame, nearly half a grid spacing.
Eight smaller authored jumps reduce compression but still fail. That probe
redistributes world-update/collision boundaries to retain 80 temporal integrations
and 160 Q2 sweeps per external frame; its summary's N10 is per API call, with
eight calls per frame. It changes update/contact sampling as well as motion
sampling, so it is not a pure interpolation-only experiment.

The separately implemented true kinematic motion retains the original U2/C8/I5/Q2
cadence and reduces compression to 52.18%, still in the plane and still failing.
The pinned Jolt soft-contact non-dynamic branch does not use a kinematic body's
point velocity in the same manner as its dynamic-body branch. Thus geometric
sweeping and moving-surface friction are distinct questions. The current adapter
does not establish a fully velocity-aware animated-collider contract.

## Jolt solver: where the error is introduced

A locally instrumented native fork scans 32,512 axial edges immediately before
and after `ApplyCollisionConstraintsAndUpdateVelocities`, after the last edge
sweep of each temporal integration. There are 3,200 measured integrations.

Contact projection increases the worst compression in 2,919 of them. At
integration2720 (frame34), the **same edge**, vertices6526→6527 at grid
(126,50)→(127,50), changes from −48.02% to −97.53% strain in that one phase.
This is an immediate local residual, not an inference from distant frame states.
The largest global-extremum increase is 59.37 percentage points at frame32;
its winning edge changes, so that particular number is not a same-edge trace.

Jolt solves the edge constraints before its final contact projection. There is
no subsequent edge solve in that integration. Later integrations relax part of
the contact-created distortion, but the final frame still retains 63.19% peak
absolute strain. Before-contact strain can already be large as well, so this is
an accumulating contact/elastic-constraint conflict rather than a claim that
all deformation is created in the last instant.

More pre-contact sweeps do not remove the final projection residual, and neither
they nor true kinematic motion can break an exactly invariant Z=0 plane. A fully
coupled contact/elastic solve would be a separate numerical change requiring the
original energy, material and contact regressions. These observations identify
a solver limitation in this configuration; they do not establish an upstream
implementation bug or prove an energy-conservation failure.

## Detailed work profile

The clean original-fixture profile disables residual scans. Selected native
scope counts/work over forty frames are:

| Scope | Calls | Summed scope elapsed time per external frame |
| --- | ---: | ---: |
| Edge constraints | 108,800 | 215.64 ms |
| Contact projection / friction / velocity update | 3,200 | 5.81 ms |
| Position integration | 3,200 | 6.92 ms |
| Soft collision jobs | 5,120 | 0.34 ms |
| FinishIteration, inclusive | 6,400 | 23.63 ms |
| Barrier work | 27,350 | 58.63 ms |

These are elapsed scope sums across workers, with nested/overlapping parents,
not exclusive CPU time or additive wall-clock percentages. The broad `Other`
bucket remains unseparated. Contact projection and friction share one scope.
The expensive residual-scanning baseline is excluded from timing comparisons.
All runs have native profiling enabled, so none establishes a performance ranking.
The deformation is a correctness/geometry issue even though elastic constraint
work dominates these leaf measurements.

## Engineering conclusion

Responsibility is divided: **our fixture supplies the primary symmetry trap;
our adapter supplies coarse static-pose motion; Jolt's sequential contact
projection determines much of the residual severity.** The same planar failure
in XPBD and the exact mirror/tiny-offset controls rule out blaming the optimized
Jolt patchset as the primary cause. Direct material mapping and successful
broadphase/contact observations provide no evidence of a unit conversion or
missing-collider admission error.

Keep the original planar case as a documented adversarial stress test. Add
explicitly specified asymmetric 3D contact fixtures beside it, with paired
reflection controls. Use a real kinematic-motion path for animated obstacles
when that is the intended physical input. Do not silently add random jitter,
replace the old test, or relax its gate to obtain a pass.

Passing axial strain in an offset fixture is not a complete contact solution:
the cloth can fold or self-intersect, Jolt's admitted path lacks soft self-contact,
and these probes do not measure external work/energy. Neither production physics
nor the qualified fork/PR checkpoint was changed. Worker source/configuration
was restored, its three qualification targets passed, and lease46 was released.

## Reproduction and evidence

Private Engine probe branch: `experiment/jolt-contact-diagnosis-20261109`,
runtime probe source `6934444bfbf77e7219957f0ae893c8ed3b538d45`.
Native instrumentation: `a837310e865e3592928c35d2c2dd70f6a568fc11`, based on7e.
Artifact root: `C:/Users/aiden/.codex/artifacts/jolt-contact-diagnosis-20261109`.

`ReduceContactProbes.py` reproduces geometry/FNV/receipt reductions;
`ContactReduction/ReduceContact.py` reproduces the 3,200 integration residual
and scope tables. Raw traces, command/environment configurations, executable,
source bundles, phase CSV/JSON, source audits and retirement receipts are retained.
The clean captured executable has SHA-256
`365d19a7a917ebf2786012c956797606907802a114d9e9801ad885c7dbc232e3`.
Its full-file FNV fingerprint matches all kinematic/symmetry log binary IDs.
The symmetry batch omitted its immediate Source.json; source association is
explicitly reconstructed from the exclusive leased script sequence, identical
binary IDs and captured executable/git heads before restoration.
