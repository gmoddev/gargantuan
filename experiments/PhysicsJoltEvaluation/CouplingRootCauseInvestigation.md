---
status: experimental
authority: non-normative
date: 2026-11-07
---

# Coupling root-cause investigation

> Historical 3e checkpoint. Read the [completed S1 resolution and unified
> evaluation](SolverResolutionAndUnifiedDecision.md) for the subsequent
> source-level closure, qualified correction and architecture decision.

This follow-up starts at PR19 checkpoint
`926219fb2b387ef262db3aab154b7a8185d5176c`, with unmodified Jolt v5.6.0
`e77f175595e64cb44218cc9d9d56fc365ad0e36a`. It changes only isolated experiment
files. Production Box3D/XPBD, public semantics, persistence and main are preserved.
Do not merge the experiment or infer migration authorization.

## Findings

Native APIs reproduce the original one/two-sphere S1/S4 failures without an
Engine physics adapter. Phase accounting locates excess kinetic energy at hard
edge-position corrections converted into particle velocity. All recorded dynamic
contact impulse pairs dissipate kinetic energy. A 0.05 J one-vertex seed reproduces
the hard-cloth S4 instability without contact (peak 1227.84 J). The deeper reason
for the unstable hard-network mode, and general upstream defect status, remain
unresolved; this is a measured configuration-dependent numerical failure.

Jolt 24 iterations are24 integrations/reconstructions per collision step; the
old XPBD 8 iterations are eight projection passes after one integration. The
original stiffness and schedules were not equivalent. Compliance 1e-5 and four
collision steps repair the observed S4 bounds; material-only S1 remains incomplete
or failing, especially the two-sphere case. Original failures remain unchanged.

A matched-interval XPBD diagnostic first held the future rigid end pose across
24 short soft steps, creating an artificial velocity jump. The explicit
`--collider-path linear` correction interpolates translation across that outer
step. The default `sampled` mode preserves the failure. No clamp, threshold or
one-sided damping change is used to obtain the corrected result. Both final
bounded paths pass the sampled S4 gates, but their response velocities differ;
neither complete conservation nor general physics-equivalent speed is qualified.

## Targets and boundaries

Enable the existing opt-in `GARGANTUAN_BUILD_PHYSICS_JOLT_EVALUATION` and supply
the pinned clean `PHYSICS_EVALUATION_JOLT_ROOT`. Build with bounded concurrency:

```text
cmake --build <build> --parallel 2 --target PhysicsJoltNativeCouplingDiagnostic PhysicsJoltXpbdCouplingDiagnostic
```

The native target independently constructs the scene and links Jolt, allocation
hooks and JSON, without the Engine core/physics adapter or shared fixture builder.
The XPBD target uses a private diagnostic copy, never the production solver.
Separate targets ending in `Allocations` collect requested-byte counters; their
times are not performance evidence. Use fresh processes and preserve stderr.

Original native reproduction, one or two spheres, S1/S4:

```text
PhysicsJoltNativeCouplingDiagnostic --steps 4 --iterations 24 --count 1 --compliance 0 --frames 150
PhysicsJoltNativeCouplingDiagnostic --steps 4 --contact 0 --speed 0 --seed 1 --compliance 0 --particles 0 --frames 150
```

The second command has authored initial seed energy0.05 J, not its first
post-step energy. Changing only `--compliance 1e-6` is its compliant control.

Final bounded S4 material/schedule track:

```text
PhysicsJoltNativeCouplingDiagnostic --steps 4 --iterations 24 --compliance 1e-5 --friction 0 --particles 0 --frames 150 --workers 1
PhysicsJoltXpbdCouplingDiagnostic --substeps 4 --interleaved 1 --soft-substeps 24 --iterations 1 --stretch-compliance 1e-5 --friction 0 --damping 0.0004166666667 --clamp 0 --diagnostics 0 --collider-path linear --frames 150 --workers 1
```

Repeat at budgets 1/2/8 with three processes each. The XPBD collider pose is a
virtual interpolated sample; rigid publication remains at the outer endpoint
and feedback modifies later advancement. This is not simultaneous effective-mass
contact. In the symmetric test, residual angular mapping is tiny, but torque
about the precomputed rigid end COM and general rotating contact remain limitations.

To preserve the diagnostic sampling failure, switch only `linear` to `sampled`.
To inspect private XPBD phases use `--diagnostics 1`; verify observation toggles
leave physical results unchanged before using timing from diagnostics-off runs.

## Observational upstream patch

`MakeJoltInstrumentation.py <clean-pinned-Jolt> <output-patch>` generates
`JoltPhaseInstrumentation.patch`. Apply it only to a separate worktree at the
same pin. `DiagnosticJolt/CMakeLists.txt` builds an instrumented native target,
with `DIAGNOSTIC_JOLT_ROOT` and `EVALUATION_SOURCE_ROOT` specified. The patch adds
stage/reconstruction/contact callbacks without changing solver arithmetic.
Do not use that library as a production fix or rank its instrumented times.

Use `--trace <file> --trace-start 16 --trace-end 24` for the contact window.
Trace stage order is BeforeIntegration, AfterIntegration, BeforeCollision,
per-vertex reconstruction/contact events, AfterCollision. Candidate reconstructed
KE is meaningful only after the current iteration sets previous positions.
Jolt's native iteration counter resets per outer collision step; flattened
ordinals and frame/outer indices must be labelled explicitly.

The augmented spring-energy quantity is a positive-compliance distance-edge
estimate, not stored solver energy. Hard edges are excluded/identified, not
declared to have zero elastic energy. Record angular energy, external-work and
observation limits. A passing endpoint does not erase a failed internal step.

## Evidence and decision

Measured code checkpoints are049a4f321 (initial), dec153fd6 (held-pose schedule)
and 990da2643 (linear correction). The latter's full SHA is
`990da2643aae87e4fb73ac3cd3702934d693ca0d`. Final documentation may be later;
the exact measured/continuation distinction is in the completion receipt.

Full raw data, source/input/binary hashes, independent fresh reproductions,
phase ledgers and quality/cost CSVs are retained at
`C:/Users/aiden/.codex/artifacts/jolt-coupling-rootcause-20261107/`.
Read `JoltCouplingRootCauseReport.md` and the completion receipt there.

Keep production while qualifying a shared unified Jolt Engine provider as the
next consolidation candidate. Withdraw the blanket inference that these earlier
coupling failures establish intrinsic XPBD superiority or bad Jolt reciprocal
impulses. Hybrid world duplication, remaining low-step failures and full Engine
semantic/asset/lifecycle/replay integration remain real adoption costs.
