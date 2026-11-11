---
status: experimental
owner: rendering
---

# Renderer Foundation 5 CPU attribution

The Foundation 4 qualification records a repeatable 50,000-object culled
submission regression: the HDR mean CPU submission rose from 6.74 to 8.77 ms,
and matched SDR controls measured 6.80–7.52 versus 8.01–8.24 ms. All controls
reported 25,000 camera-culled objects per frame. The completed-frame P95
ranges overlapped. These are CPU submission and completed-frame measurements,
not isolated GPU execution times. Preserve the original receipts in
`RendererFoundation4Qualification.md`.

## Causal controls

`gargantuan_renderer_benchmark --incremental --backend=sdl
--scenario=culled --count=50000 --culled-percent=0|50|100
--sdl-profile=on --samples=<fresh-path>` retains the original 50% fixture
ordering and lets the same binary render all, half, or none of the objects
in front of the camera. It preserves publication count, source mesh, material,
object identity, and static delta shape. The cull-count gate checks the exact
expected count. Frame, projection, backend apply, submission, completion,
per-pass, and detailed object-path samples remain separate CSV columns.
Use 0/50/100 and reverse the order in a second run. Repeat with
`--sdl-profile=off` to quantify detailed clock overhead. Pair HDR and SDR
controls only within the same presentation mode, and keep GPU fences and
queue mode consistent. This fixture changes the number of visible draws on
purpose, so the slope is diagnostic rather than a claim about an unchanged
visual workload.

## Source path and timing boundaries

The opaque pass visits every projected object. For each visible record it
looks up an animation pose and local mesh bounds, then tests a conservative
frustum box. A rejected object skips native mesh lookup, material resolution,
opaque grouping, and draw recording. Its source-level costs are thus traversal,
lookups, and culling. The one-range culled fixture is ineligible for Foundation
4's two-to-eight-range opaque grouping, so its grouping proof is absent.
Visible objects still pay expanded material uniform preparation. The shadow
pass has an independent traversal when it records a depth map, so shadow
classification must be measured separately.

Detailed counters report objects visited, hidden by publication visibility,
frustum tests, native mesh lookups, and world primitive draws. The detailed
timers measure camera traversal, pose/bounds/frustum work, native mesh lookup,
grouping attempt, primitive command recording, and transparent sort/draw.
Camera traversal includes the nested categories; grouping attempt may include
a flush that records primitives. Treat these fields as attribution clues,
not additive components. Detailed clocks are enabled only by
`--sdl-profile=on`; unmeasured fields are `NA` in benchmark output.
Whole-pass and projection validation/mutation clocks are coarse boundaries.
The first twelve phase columns (shadow classification through command acquire)
are populated with profiling on and off. Only the six nested object-path
columns and object counters require `--sdl-profile=on`.
The projection remains renderer-owned derived state, and failed publication
preflight still leaves it unchanged.

## Optimization gate

Do not add a second authoritative scene graph or weaken conservative culling.
An incremental visibility index or native instancing path should be adopted
only after the physical 0/50/100 controls identify a dominant cost. Re-run the
opaque-ordering pixel controls when changing draw order. F4's strict depth
`LESS` winner is traversal-order dependent for coplanar surfaces; unrelated
geometry may be reordered only under its conservative pairwise independence
proof, or the original order must be retained.

## Bounded source-kernel probe, 2026-11-11

`tests/RendererCpuMicroprobe.cpp` copies the exact Foundation 3 and 4
conservative camera-box test, and the two `PartUniforms` layouts and value
packing for the untextured 50K fixture. It runs 0/50/100% culled variants in
alternating kernel order, with no SDL calls. A local MSVC `/O2` build completed
in 5.5 seconds; the timed run completed in 0.8 seconds. The first two rounds
were treated as warmup. Each figure below is the median of 14 retained
observations in milliseconds. The full raw samples, executable, and SHA-256
hashes are in
`C:/Users/aiden/.codex/artifacts/renderer-foundation5-20261111/cpu-microprobe`.

| Culled | Cull only | F3 cull + 176-byte uniform | F4 cull + 224-byte uniform |
| ---: | ---: | ---: | ---: |
| 0% | 1.55 | 2.10 | 2.40 |
| 50% | 1.06 | 1.32 | 1.54 |
| 100% | 0.44 | 0.44 | 0.45 |

The source file SHA-256 was
`142C07FA5C953BF8D705E0DE22F3B0ABE7381DAD6B95A481C2463111FF33D4AC`;
the executable SHA-256 was
`A9DB6F209DF88F8D1B2932D7ED02EC383EA93FCF9ACB69E223EBB5D36C5D2AC4`.
The extra uniform preparation/copy scales with visible objects and contributes
roughly 0.22 ms at the 50% median in this local probe. It cannot explain the
entire physical 1–2 ms F4 submission increase by itself. The real draw path
also records GPU commands and binds state; those costs require the instrumented
physical same-binary controls. This probe does not measure GPU execution,
SDL uniform pushing, texture residency, shadow recording, or allocation.

## Compact traversal experiment

The renderer-owned projection can lazily mirror its `unordered_map` iteration
order as a vector of object, pose, and mesh-bounds pointers. Existing object,
pose, and mesh-bounds updates mutate map values in place, so the vector keeps
its addresses. Structural create/remove, mesh create/remove, pose create/remove,
full resync, snapshot application, and clear invalidate it. The next enabled
opaque traversal rebuilds it in the map's current order. An invalid publication
does not change either projection or traversal. No Engine Instance pointer or
new authority enters the renderer.

`--sdl-compact-traversal=on|off` is a same-binary experiment. Off uses the
original map walk, pose lookup, and mesh-bounds lookup. On visits the cached
records in identical order and retains the same culling, grouping, native
resource lookup, material, draw, and transparent behavior. The cache is built
only when enabled; static frames reuse it, while structural churn pays a
rebuild. Compare alternating on/off rows for 0/50/100% culling, static,
mostly-static, dynamic, and mixed scenes. A timing improvement requires
unchanged cull and draw counters and exact opaque-order pixel controls.
Neither a quicker isolated CPU walk nor reduced submission alone establishes
a completed-frame improvement.
