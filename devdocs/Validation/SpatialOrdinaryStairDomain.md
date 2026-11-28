# Stock locomotion on a low step in two domains

The released Windows EditorHost climbed a one-unit step in Workspace and stopped against the same obstacle in a named SpatialRegion. Both fresh Play sessions used the stock character, camera, locomotion module, registered Parts and keyboard input. One session assigned the avatar's root and camera to the named region. The floor and step were created in that region at the same coordinates as the Workspace control.

| Collision domain | Final X | Maximum Y | Climbed |
| --- | ---: | ---: | --- |
| Workspace | 218.525 | 2.995 | Yes |
| SpatialRegion | 201.005 | 1.995 | No |

The avatar spawned at (200, 6, 100) above a floor at Y = 0. The step occupied X = 202 through 206 with its top at Y = 1. The host pressed D after the avatar first grounded and sampled 120 simulation ticks. The endpoint depends on frame timing. The crossing check requires X > 206 and maximum avatar Y > 2.75. The receipt records both executable and installed locomotion module hashes. [Play receipt](OrdinaryPhysicsAblations/StairDomains.json)

The stock module at `5b6db615b7` cast its step-up, forward and down previews against Workspace. Native `Character:Move` selected the effective world of the character root. A direct capsule cast in the same released EditorHost traveled 8.000 units through Workspace and 2.005 units through a coincident named world containing a wall.

The corrected stock module selects `Character:GetMotionWorld()` on a blocked step. Windows build `238e18c0c` climbed in Workspace, with the root assigned to the named domain, with a different camera domain, with a spatially assigned ancestor below the actor, and with the character parented to the domain. A separate physical D keypress in a foreground Studio Play session on the reserved worker reached x=218.230 and a peak of y=2.995. [Stabilization evidence](SpatialFinalStabilization.md)

Run the observation against a relocated Windows release with its adjacent runtime files. The output path must be new. [Replay](../../tests/SpatialOrdinaryStairDomain.py)

```powershell
python -B tests/SpatialOrdinaryStairDomain.py --engine $EditorHost --output StairDomains.json
```
