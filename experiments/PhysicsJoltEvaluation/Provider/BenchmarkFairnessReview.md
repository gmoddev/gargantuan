# 16K cloth reference benchmark fairness review

## Evidence and integrity

The copied Windows artifacts are `Quality-Trace-20261108T123351` and
`Quality-Timing-Repeats-20261108T124606` under
`C:\Users\aiden\.codex\artifacts\jolt-optimized-foundation-20261108`.
The trace run identifies Engine `96b353467e4bafc29e50d3f8d7bcd7ffa57a1b08`,
Jolt `d8c46588b23700d24ebdaeada0ff460d34b1f830`, and binary SHA-256
`5047e4084169e92336f381929c8f9b1d1a7377d2db964666f94d4a96e6302dfe`.
The timing run identifies Engine `1b7e2147df69515b6510148fcca323429b503d79`,
the same Jolt source, and binary SHA-256
`c21ff35ab0d02fb787d3facc946594240c106a73299a73fad114e4520105bb5a`.
The two Engine commits have identical `ProviderBenchmark.cpp` and
`RunPerformanceQualification.py`; the intervening benchmark change is the
comparator's requested-worker-cap gate. Distinct binaries and runs are kept
separate.

I recomputed SHA-256 for every copied stdout, stderr, trace, comparator output,
and CMake cache against the receipts and metadata: no mismatch across 2 trace
processes and 12 timing processes. The original worker binary is not in the
copied folders, so its reported binary SHA-256 cannot be independently rehashed
there. All 14 native exits are zero, all seven pairs are complete with no
harness error, and every process has 10 warmup, 30 measured, and 40 state
frames. Both native rows report the same scene, frame interval, requested cap,
and run pair. The Jolt rows report the compiled W2/C8/I5/Q2 schedule, 80 soft
integrations per frame; the XPBD rows report 32 experimental reference substeps.

## Physical time and quality

The scene is 16,384-vertex cloth with no rigid collider. The uniform impulse is
applied once before frame zero. Jolt advances its unified world by 1/60 second
and then publishes the soft state; XPBD advances 32 steps of (1/60)/32 second.
The trace writes after each completed frame, so frame index `k` samples the
same nominal physical time `(k + 1)/60` on both sides, including warmup. The
last sample is at 40/60 = 0.667 seconds. The quality trace is instrumented and
is excluded from timing ranking.

I decoded both raw little-endian traces independently: headers agree on
version 1, one body, 16,384 vertices, 40 frames, 10 warmup frames, and a
float32 interval of 0.0166666675 seconds. All coordinates are finite. The
pointwise displacement peaks at frame 32: RMS 0.00391063 m and maximum
0.01091058 m. At frame 39, RMS is 0.00339889 m and maximum is 0.00488768 m.
These are observed differences, with no predeclared acceptance threshold or
claim of identical material behavior. Every XPBD reference run's separate
zero-gravity free-velocity control passed: observed frame retention
0.95964333 versus expected 0.96000000, within its 0.002 gate.

## Uninstrumented cost and scope

Three fresh, counterbalanced warm pairs at each requested worker cap had no
quality trace, no timing instrumentation, and no supplied quality envelope.
At cap 1, median per-run mean total was 182.876 ms for Jolt and 264.236 ms
for the XPBD reference (XPBD/Jolt 1.445). At cap 8, the medians were
67.2247 ms and 264.051 ms (3.928). XPBD reported actual worker count 1 at
cap 1 and 8 at cap 8, with mean utilization about 1.00 and 0.12 respectively;
one cloth body supplies limited parallel work. Equal requested caps and actual
use within cap pass the comparator's budget gate. All six comparison receipts
retain `timing_winner=null` because the XPBD 32-substep row is an experimental
quality reference, not the production default, and no reviewed physical
envelope was supplied.

The evidence supports a bounded 0.667-second, no-collider trajectory and
separately measured provider-call costs for these exact builds. It does not
establish production-default quality parity, interacting rigid feedback,
long-run stability, an Engine frame speedup, or a globally optimal schedule.
