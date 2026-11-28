# Joints across coincident spatial domains

The Windows EditorHost ran `tests/creator/spatial/JointDomains.server.luau` in ordinary Play through `python -B tests/SpatialCreatorTrial.py --engine <installed gargantuan.exe>`. The harness creates a temporary project, starts Play, consumes its Script output and stops the session. The joint fixture is committed in `4480afff6`.

| Installed source | Windows executable SHA-256 | Result |
| --- | --- | --- |
| `d533a18ad` | `99139ace12829e690b689bfe53e92a3bb0d6b3b76136d2d728192b176a96864d` | An enabled spring and both inherited endpoints shared a detached region. The native shape cast refused the retired chart, but the public `Active` property returned true. The checked-in assertion failed. |
| `640354c4c` | `e8eb355d04ebc7ddbf503110b02518714e32c357fb6e341a82673894ecb0eec7` | The fixture passed. Detaching the chart made the spring inactive, stopped endpoint movement and preserved its neighbor's independent query. Reattachment restored native spring motion. |

The corrected run printed:

```text
[Reader:Joints] PASS casts=7.505/4.505 pulls=2.595/2.595/2.595/2.595/3.695 retiredImplicitActive=false
```

The two cast distances identify different Parts at coincident chart coordinates. The five displacements measure the native spring after bringing both endpoints and the constraint into the same chart, returning a moved endpoint, replacing a destroyed attachment, reattaching the explicitly assigned chart and reattaching the inherited chart. The fixture also verifies that a split spring exerts no measurable pull and that the active native assembly rejects a one-sided reassignment until disabled. It covers the ordinary `SpringConstraint` and `SphereCast` paths.
