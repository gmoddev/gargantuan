# Editor viewport v1

## Implemented now

EditorHost owns an offscreen renderer whose current private implementation uses
SDL GPU. `EditorViewport.hpp` exposes no SDL or GPU type, and Studio never
receives a GPU handle or links against renderer internals. The viewport interface exposes six
bounded protocol methods:

| Method | Contract |
| --- | --- |
| `ConfigureViewport` | Negotiates an RGB8 surface up to 1024 x 1024 and one million pixels. |
| `SetViewportCamera` | Sets a finite absolute position, target, and optional 1-120 degree vertical field of view. |
| `OpenViewportTransport` | Explicitly opens the advertised version-1 RGB8 shared-memory ring. |
| `CloseViewportTransport` | Releases the host's mapping handle. |
| `CaptureViewport` | Renders the current Workspace and publishes to the open ring, or returns bounded Base64 fallback data. |
| `PickViewport` | Casts a camera ray through a pixel and returns the nearest live BasePart `ObjectId`. |

The renderer reuses the normal shadow and opaque passes against engine-owned
color/depth textures, downloads only after a GPU fence, and contains GPU/setup
failures as structured `ViewportUnavailable` errors. Capture remains on the
serialized EditorHost domain. It does not execute scripts, step physics, or
permit Studio to mutate the source outside existing validated commands.
All six viewport methods require the explicit `ViewportControl` capability at
EditorHost dispatch. The private Studio service may expose camera, size, copied
frame metadata, and picked ObjectIds, but never the shared mapping name, native
handle, renderer pointer, SDL/Vulkan object, or IPC stream to Luau.

Camera pose, field of view, and viewport size belong to an EditorHost-owned
session camera. They are not persisted in the project or published to the
document change journal.

## Shared-memory frame ring v1

The handshake advertises `SharedMemoryRing` only when the platform
implementation is available. Studio must select version 1 and `RGB8` through
`OpenViewportTransport`; it may not infer support. Windows is the implemented
platform in this pass. Other platforms retain the Base64 version-1 fallback
until an equivalent named mapping implementation exists.

EditorHost creates and owns a random, session-scoped named mapping. Its fixed
9,437,440-byte allocation contains a 64-byte ring header followed by three
fixed-capacity slots. Each slot has a 64-byte header and space for at most
1024 x 1024 x 3 RGB8 bytes. Headers describe the ring/frame version, sequence,
timestamp, dimensions, format, payload byte count, and slot state. Neither
mapping size nor slot offsets are supplied by Studio.

Publication changes a selected slot from `Writing` to `Complete` only after its
metadata and payload are copied. Sequence numbers increase monotonically within
an open transport. The producer selects slots modulo three and never waits for
the consumer, so allocation cannot grow and a slow Studio causes old frames to
be overwritten. Studio scans for the newest complete sequence, copies it, then
rechecks state and sequence; an incomplete or concurrently overwritten copy is
discarded. Intermediate frames are explicitly ephemeral.

On normal shutdown Studio closes its view and requests
`CloseViewportTransport`; EditorHost then closes its mapping handle. EOF or a
process crash also releases that process's operating-system handles. Windows
removes the named mapping when the last handle closes, so normal sessions leave
no persistent mapping resource. Failed validation closes any partially opened
client view and asks the host to close its mapping.

The mapping contains CPU RGB pixels, never GPU memory, pointers, or renderer
objects. Its random name is disclosed only through the token-bound EditorHost
session and uses the process token's default mapping access control. Consumers
still validate every fixed header field, slot state, dimension, format,
sequence, and payload size before use.

Picking currently intersects BasePart-oriented bounding boxes. This is
deterministic and sufficient to prove selection identity, but it is not yet
mesh-accurate and does not include GUI or editor gizmos.

## Luau Studio UI boundary

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
must not receive arbitrary native or operating-system access. The private
Studio adapts these methods through a restricted `ViewportService` and resolves
picked `ObjectId` values through the Studio document model. Frame bytes remain
in the C# presentation layer; Luau receives only bounded dimensions and
sequence metadata.

## Remaining work

- add a continuous capture/presentation loop and measure 30/60 FPS latency;
- implement the same named-ring contract on non-Windows platforms;
- add camera orbit/pan helpers above the absolute-pose command;
- define mesh-accurate picking and editor-gizmo precedence;
- make renderer lifetime/device-loss tests part of automated GPU CI; and
- add camera orbit/pan policy in the Luau editor layer without widening the native service.
