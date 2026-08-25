# Editor Viewport Presentation Foundation 2

Status: implemented and validated on Windows on 2026-08-25.

## Decision and measured cause

The old production Studio path was capture-driven. An Avalonia
`DispatcherTimer` fired every 50 ms, optionally submitted the editor camera, and
then requested `CaptureViewport`. EditorHost rendered only because that request
arrived, synchronously read the GPU target back, converted BGRA to a newly
allocated RGB frame, copied it into the version-1 ring, and returned a command
response. Studio copied that frame into a new managed array, allocated a new
`WriteableBitmap`, converted RGB back to BGRA, and assigned the bitmap. The
50-ms timer was therefore an exact approximately 20-Hz presentation ceiling and
also coupled camera submission, Play stepping, rendering, readback, IPC, and UI
presentation.

Foundation 2 selects **Decision A: optimized CPU transport**, with a bounded
shared-memory latest-frame mailbox. This is the supported Windows primary path
and the cross-platform design direction; explicit capture remains the fallback.
The choice is evidence-based:

- pinned SDL 3.4.12 selects Vulkan on the tested AMD and NVIDIA systems and its
  public GPU API exposes opaque `SDL_GPUTexture` values. It has no supported
  shareable-texture creation flag or API for exporting a D3D/Vulkan native
  resource handle. Unsupported backend casts would violate renderer ownership;
- Avalonia 11.0.10 has public custom-rendering/GPU-interop seams, including D3D11
  shared-image import, but an importer does not solve SDL's missing supported
  exporter; and
- measured raw BGRA transport sustains 60 Hz at 1080p and scales materially above
  120 Hz at the normal editor size on the primary machine.

GPU sharing remains a future option behind the presentation boundary if SDL
adds an official export contract. It is not a correctness blocker in the
supported architecture.

## Producer and consumer ownership

The runtime renderer path remains:

```text
DataModel/runtime -> RenderPublisher -> RenderPublication
                  -> EditorViewportRenderer -> SDL GPU backend
```

`EditorHost` owns the renderer, readback buffer, `LatestFrameMailbox`, render
cadence, and generation. Studio owns only a read-only mapping view and its
presentation resources. No SDL, Avalonia, pointer, texture, or project-authority
type crosses the boundary.

`OpenViewportTransport` negotiates `LatestFrameMailbox`, version 2, `BGRA8`.
The fixed mapping has two slots, each with a 128-byte header and room for a
3840x2160 BGRA frame. The approximately 63.3-MiB mapping is bounded. A random
`Local\\GargantuanViewportMailbox-*` mapping and random
`Local\\GargantuanViewportFrame-*` auto-reset event are created under the
current process token. Studio validates the exact mapping size, names, protocol,
format, dimensions, checked payload size, sequence, generation, mode, and Play
identity before allocation or presentation.

Each slot records:

- monotonic transport sequence and completion timestamp;
- viewport generation and dimensions;
- Edit/Play mode and exact `PlaySessionId`;
- editor camera revision and informational render/publication revision; and
- render submission, GPU readback wait, and CPU extraction durations.

The producer marks one modulo-selected slot `Writing`, fills metadata/pixels,
publishes it as `Complete`, atomically advances the latest sequence, and signals
the event. It never waits for Studio. Studio revalidates state and sequence after
copying. There is no visual-frame queue: one slot may be consumed while the
other is replaced, and an unconsumed older completed frame is disposable.

## Cadence, backpressure, and Play

`StartViewportPresentation` advertises `Adaptive`, `30`, `60`, `120`, `144`,
and `Uncapped`. Adaptive currently uses a 144-Hz production interval; it is not a
display-vsync claim. The EditorHost request reader uses a bounded 64-request
queue while the owner loop independently pumps 120-Hz Play simulation and live
viewport production. A slow/minimized Studio stops presentation through the
session command; simulation does not wait for mapping slots or UI consumption.

Edit and Play use the same mailbox and offscreen renderer. Play presented an
additional issue exposed by the new independent cadences: `HeadlessRenderer`
formerly retained only the newest semantic publication. At a faster simulation
cadence, presentation could skip a required incremental base publication.
`PlaySession` now retains a bounded queue of 32 semantic deltas. EditorHost
applies every ordered delta but reads back only the newest visual result. Queue
overflow clears the queue and requests a full resync. Thus semantic renderer
ordering is preserved without turning visual frames into a backlog.

Camera commands carry a monotonic camera revision. Studio coalesces pointer
updates and EditorHost rejects older revisions. Camera transport is independent
of visual-frame notification. Simulation timing remains unchanged by
presentation frequency.

## Generation and lifecycle

Generation changes retire frames across resize, project replacement, Play
start, Play stop, renderer recovery, transport replacement, and shutdown. A
frame sequence is transport identity, not a project-journal revision. Studio
rejects an older sequence, generation, size, Edit/Play mode, or Play session.
This prevents an Edit frame flashing after Play and a Play frame flashing after
Stop.

Resize requests are Studio-coalesced for 75 ms. An unchanged size is idempotent;
a changed size recreates the target once, increments generation, and makes old
dimensions stale. Presentation-stage faults clear renderer/projection state,
request a full publication, retain one bounded diagnostic, and recover on the
next frame. Transport close, EOF, or process death releases OS handles. Renderer
restart similarly requires a new full publication; no Studio restart is part of
the contract.

Explicit `CaptureViewport` remains separate for screenshots, compatibility,
tests, and unsupported transports. It may synchronously wait for one complete
CPU-readable frame. It no longer defines normal Studio cadence. Picking remains
publication-based and returns stable ObjectId identity; presentation sequencing
does not become project authority.

## Copy and allocation result

Old steady-state CPU path:

```text
GPU transfer -> mapped BGRA -> allocated RGB -> v1 ring
             -> allocated Studio byte[] -> allocated bitmap -> RGB/BGRA copy
```

Foundation 2 path:

```text
GPU transfer -> reused Engine BGRA -> latest mailbox
             -> reused Studio BGRA buffer -> reused locked WriteableBitmap
```

The bitmap is allocated once per size, not per frame. The Engine BGRA extraction
buffer and Studio acquisition buffer are also resized only when dimensions
change. Commands contain metadata only; live pixels no longer traverse JSON,
Base64, or the serialized request/response stream. At 844x565 the raw shared
memory payload is 1.82 MiB/frame (about 236 MiB/s at the measured 130 source FPS,
or 109 MiB/s when a 60-Hz compositor consumes it). The primary remaining costs
are GPU readback plus the bounded mailbox, managed-buffer, and bitmap copies.

## Baseline and result

Primary hardware: Ryzen 9 7950X3D, Radeon RX 7900 XT, Windows, Release, SDL
Vulkan. Old figures below are direct capture-path achieved FPS and p95 capture
milliseconds; they exclude Avalonia allocation/composition, whose timer still
capped visible Studio to approximately 20 FPS.

| Size | 20 | 30 | 60 | 120 | 144 | Unbounded |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 844x565 | 20.0 / 1.80 | 29.7 / 2.21 | 59.8 / 2.65 | 118.5 / 1.99 | 142.3 / 1.66 | 636.8 / 2.37 |
| 1280x720 | 19.9 / 2.44 | 29.9 / 12.29 | 59.6 / 4.17 | 118.7 / 2.72 | 142.6 / 5.21 | 397.4 / 3.53 |
| 1920x1080 | 19.7 / 4.97 | 29.5 / 6.10 | 59.9 / 5.35 | 118.9 / 4.99 | 143.7 / 5.19 | 233.6 / 5.24 |

The new end-to-end mailbox benchmark records source/consumer FPS and consumer
frame-age p50/p95/p99 milliseconds:

| Scenario | Size | 30 | 60 | 120 | 144 | Adaptive |
| --- | --- | --- | --- | --- | --- | --- |
| Edit | 844x565 | 29.6 / .55/.93/1.10 | 57.6 / .52/.77/.84 | 111.6 / .56/.93/1.12 | 130.3 / .80/1.27/1.67 | 130.1 / .89/1.55/1.73 |
| Edit | 1280x720 | 29.4 / 1.84/3.17/3.56 | 57.8 / 1.61/2.01/2.31 | 111.1 / 1.55/2.00/2.22 | 130.2 / 1.55/2.06/2.43 | 131.6 / 1.59/2.01/2.50 |
| Edit | 1920x1080 | 29.3 / 2.33/3.89/4.22 | 57.2 / 2.89/5.15/6.38 | 111.1 / 3.30/5.40/6.04 | 125.1 / 4.44/6.41/8.54 | 132.7 / 3.89/6.29/6.80 |
| Play | 844x565 | 29.4 / .85/1.16/1.26 | 57.2 / .82/1.12/1.35 | 110.3 / .83/1.39/1.51 | 129.7 / .83/1.32/1.50 | 129.6 / .55/.91/.98 |
| Play | 1280x720 | 29.5 / 1.42/1.90/1.96 | 57.6 / 1.52/1.96/2.38 | 111.0 / 1.47/1.99/2.60 | 130.4 / 1.60/2.09/3.04 | 130.5 / 1.63/2.32/2.61 |
| Play | 1920x1080 | 29.6 / 3.81/4.16/5.24 | 57.3 / 3.53/4.63/5.34 | 110.2 / 3.24/5.22/6.69 | 130.7 / 3.59/5.47/6.82 | 131.0 / 4.00/5.98/7.03 |

The first actual visible Studio acceptance at 844x565 showed Edit 127 FPS at
6.5 ms age, Play 117 FPS at 5.8 ms, and Edit after Stop 114 FPS at 5.3 ms. A
final display-paced rerun settled at 64/60/61 FPS with 12.9/10.0/9.3 ms age;
the variation is Avalonia/compositor scheduling rather than a restored Engine
capture cap. Camera
injection to consumed frame measured 18.10 ms total (6.88 ms command, 6.56 ms
acceptance-to-frame, 4.65 ms frame-to-consumer). Edit-to-first-Play-frame was
465.96 ms, dominated by Play snapshot/startup; Play-to-first-Edit-frame was
355.17 ms, dominated by cold Edit full-publication reconstruction. No stale
frame was presented.

## Diagnostics and validation

Bounded state exposes produced frames, latest sequence, failures, resize count,
mode/session/camera/source identity, and the most recent render/readback/extract
timings. Studio separately rolls at most 512 samples over five seconds for
present FPS, age, dropped frames, coalesced notifications, bitmap allocations,
and copy time. These metrics remain Studio-local and are not telemetry.

Deterministic tests cover protocol negotiation, malformed bounds, 1280x720,
idempotent configure/start/stop, resize generation, stale camera rejection,
monotonic production without capture requests, injected publish failure and
recovery, batched Play publications, and Play/Edit retirement. Explicit capture,
headless rendering, GUI, soft body, package/player, sequential and parallel
Release CTest remain separate gates.

The first hosted Windows run exposed a headless ownership regression. Consuming
Play render publications constructed an SDL GPU renderer whenever a viewport size
had been configured, even when live presentation was inactive, so a runner with
no usable GPU could not start Play. Publication consumption is now renderer-free;
renderer creation belongs only to explicit capture or active presentation. The
mailbox/state-machine test uses a deterministic injected CPU frame source, while
the separate GPU viewport tests continue to own real renderer/readback coverage.

Remaining work is not a return to capture polling: remove synchronous GPU fence
readback with supported asynchronous transfer primitives, obtain display-paced
callbacks from Avalonia rather than treating Adaptive as 144 Hz, add renderer
device-loss fault injection at the platform boundary, and reevaluate official
GPU sharing only when both SDL export and Avalonia import are supported.
