# Editor viewport v1

## Implemented now

EditorHost owns an offscreen SDL GPU renderer. Studio never receives a GPU
handle or links against renderer internals. The current synchronous vertical
slice exposes four bounded protocol methods:

| Method | Contract |
| --- | --- |
| `ConfigureViewport` | Negotiates an RGB8 surface up to 1024 x 1024 and one million pixels. |
| `SetViewportCamera` | Sets a finite absolute position, target, and optional 1-120 degree vertical field of view. |
| `CaptureViewport` | Renders the current Workspace and returns a versioned, Base64-encoded RGB8 frame. |
| `PickViewport` | Casts a camera ray through a pixel and returns the nearest live BasePart `ObjectId`. |

The renderer reuses the normal shadow and opaque passes against engine-owned
color/depth textures, downloads only after a GPU fence, and contains GPU/setup
failures as structured `ViewportUnavailable` errors. Capture remains on the
serialized EditorHost domain. It does not execute scripts, step physics, or
permit Studio to mutate the source outside existing validated commands.

Camera pose, field of view, and viewport size belong to an EditorHost-owned
session camera. They are not persisted in the project or published to the
document change journal.

The Base64 frame is intentionally a proof transport. It is bounded and easy to
validate, but copies several times and is not the intended steady-state path.
A later revision may negotiate a local shared texture or shared-memory ring
while preserving the same camera, picking, identity, and error contracts.

Picking currently intersects BasePart-oriented bounding boxes. This is
deterministic and sufficient to prove selection identity, but it is not yet
mesh-accurate and does not include GUI or editor gizmos.

## Future Luau Studio UI

The protocol is UI-toolkit independent. The private Studio's current Python
shell is only an integration adapter. The intended split is:

```text
Native Studio bootstrap
|- process / IPC / window / viewport presentation
`- Luau editor runtime
   |- DocumentService
   |- SelectionService
   |- ViewportService
   `- CommandService
```

Explorer, Properties, toolbars, selection behavior, settings, and most editor
workflows should be Luau-authored consumers of those explicit services. Luau
must not receive arbitrary native or operating-system access. `ViewportService`
will adapt the public EditorHost methods and resolve picked `ObjectId` values
through the Studio document model.

## Remaining work

- replace synchronous Base64 capture with a negotiated local surface/ring;
- add frame backpressure and cancellation;
- add camera orbit/pan helpers above the absolute-pose command;
- define mesh-accurate picking and editor-gizmo precedence;
- make renderer lifetime/device-loss tests part of automated GPU CI; and
- bootstrap the restricted Luau editor-service runtime in the private app.
