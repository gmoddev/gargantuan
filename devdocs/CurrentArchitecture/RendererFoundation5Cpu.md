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
