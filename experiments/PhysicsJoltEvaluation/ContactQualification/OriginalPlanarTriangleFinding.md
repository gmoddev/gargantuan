---
status: experimental-finding
authority: non-normative
date: 2026-11-09
---

# Original coplanar trace: strain precedes self-overlap

`InspectPlanarTriangles.py` independently reads the sealed original 40-frame
`CleanProfile-20261109T085529/BaselineClean.trace` PJTRACE1 position stream.
Its SHA-256 is
`b9cc9db786d0843550c1e6fab1b2bcea457784d3f58b780caadb76b861ce759f`.
The script reconstructs all 32,258 oriented grid faces using the exact
`BuildSoft` winding in `RigidJoltBackend.cpp`:
`(A,A+N,A+N+1)` and `(A,A+N+1,A+1)`. It checks every stored float32 xyz
position, all axial edges, signed XY triangle areas, and all nonadjacent
outer-boundary edge pairs in float64 arithmetic. All 40 frames have exactly
zero Z.

| Observation | Preserved source result |
| --- | --- |
| First frame above the original 10% axial strain gate | Frame 6, 11.6420%; minimum signed face area ratio still 0.8471. |
| Last planar frame with every face positive and outer boundary simple | Frame 31, minimum signed area ratio 0.5610; maximum axial strain already 42.9517%. |
| First inverted face and adjacent positive-area overlap | Frame 32, 30 inverted faces; maximum axial strain 68.0329%. |
| Explicit overlap witness at frame 32 | Faces 13460 and 13459 share edge `[6782,6910]`. Their opposite vertices 6911 and 6781 have signed side values `-0.00340316` and `-0.00339050`, on the same side of that edge. |
| Worst signed area | Face 17778 (`[8958,9086,9087]`) at frame 37, ratio `-1.25969`. |
| End frame | Frame 39 has 124 inverted faces. |

The first 32 frames have strictly positive triangle areas and a simple outer
boundary. For a continuous piecewise-linear mapping of the triangulated disk,
every nondegenerate orientation-preserving face contributes +1 to the
topological degree of an interior regular point. The simple Jordan boundary
has degree 1 inside and 0 outside, so two distinct face interiors cannot
overlap in these frames. The frame-32 adjacent witness demonstrates actual
positive-area planar self-overlap. Boundary segments themselves do not cross
in any of the 40 frames.

Thus the axial gate fails 26 frames before the first planar inversion or
interior overlap in this trace. The later inversion is a separate physical
failure. Neither defect is explained away by geometric infeasibility: the
separate planar shear certificate constructs a pinned, collision-clear,
noninverted endpoint under 10% axial strain. That construction is a geometric
existence proof, not a dynamic solution for the authored launch and moving
sphere. This trace analysis does not measure contact impulses, external work,
or the exact moment of inversion within frame 32.

The full per-frame machine-readable result is retained at
`C:/Users/aiden/.codex/artifacts/jolt-cloth-contact-correction-20261109/OriginalPlanarTriangles.json`
(SHA-256
`2dda2765e26c3af9785ee4dabdf76326c09f34b80d394008de8ffe7ef22177bf`).
The sealed diagnosis root was read only; all new evidence is in the new
correction artifact root.
