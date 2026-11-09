---
status: experimental-qualification-design
authority: non-normative
date: 2026-11-09
---

# Contact qualification case matrix

Each case uses the V1 fixture and full poststep trace schema. Capture the
frozen Jolt, corrected Jolt, frozen XPBD and corrected XPBD implementations
with identical authored geometry wherever their contracts permit. Preserve
the original 128×128, 40-frame planar failure and its 10% axial gate as a
negative control. The earlier 21-case diagnosis is source evidence; this
matrix adds full velocities, face/edge geometry and impulse/work visibility.

| Case IDs | Authored change from the original x[-4,4], y[-1,7], Z=0 top-pinned cloth and radius-1 sphere | Primary discriminator |
| --- | --- | --- |
| `plane-0`, `plane-z+10um`, `plane-z-10um`, `plane-z+1cm`, `plane-z-1cm` | Sphere center Z=0, ±0.00001, ±0.01 m; same moving surface and +X launch. | Exact symmetry, tiny perturbation, mirror parity. |
| `plane-no-launch`, `plane-no-contact` | Remove cloth launch; remove collider while retaining other authored inputs. | Launch contribution and solver-only control. |
| `oblique+`, `oblique-` | Sphere center moves diagonally in XY with mirrored lateral component, retaining total path length and speed. | Normal selection and mirror behavior away from an axis. |
| `face-center`, `edge-midpoint`, `corner-vertex` | Align sphere's initial penetration path with triangle interior, shared edge midpoint, or grid vertex using the same radius and speed. | Missing feature contact and closest-point correctness. |
| `static-pose`, `kinematic-pose`, `kinematic-move`, `dynamic-light`, `dynamic-heavy` | Same authored center path where controllable; compare teleport-like static pose, pose-assigned kinematic, `MoveKinematic`, and dynamic masses 1 and 100 kg. | Moving-surface velocity, reaction impulse and finite mass. Dynamic cases record actual, not prescribed, paths. |
| `rot-xz`, `rot-yz`, `rot-oblique` | Rotate cloth, pins, gravity, sphere path and impulse as one rigid transform; one oblique transform mixes all axes. | Coordinate-frame invariance without geometry perturbation. |
| `pins-left`, `pins-corners`, `pins-free` | Move fixed boundary to left edge, pin only four corners, or remove pins with a centroid/rest-frame observation. | Boundary condition dependence and global drift. |
| `radius-half`, `radius-1p5`, `speed-4x` | Radius 0.5 or 1.5 m, or four times the drive speed with unchanged start clearance. | Curvature and impact speed. |
| `repeat-3`, `double-opposed`, `settle-600` | Three approach/retract contacts, two simultaneous spheres approaching from opposite sides, or 600 poststep frames after one contact on a bounded 17×17 grid. | Oscillation, competing normals and long settling. |

The 9×9 and 17×17 meshes exercise source-level discriminators cheaply; 32×32
checks resolution sensitivity. Retain one representative 128×128 original and
signed-offset pair so a correction does not merely fit the small grid. Run
worker caps 1 and 8 and Windows/Linux for accepted variants; compare exact
within-platform replay and explicitly bounded cross-platform differences.
Record feature contact counts, contact normal/impulse signs, actual sphere
motion, solver stage residuals, axial compression and tension, all-edge strain,
pin drift, triangle closest-point penetration, area/orientation, full velocities,
kinetic and spring-surrogate energy, external work and any missing accounting.

Before each capture, commit a protocol file with per-case numerical gates,
expected contact/no-contact status, observation window, worker caps and source
pins. Do not adapt the original failure's 0.10 axial limit, and do not claim a
complete quality pass when velocity, impulse, work or self-intersection checks
remain `INCOMPLETE`. Trace and timing captures use separate executions.
