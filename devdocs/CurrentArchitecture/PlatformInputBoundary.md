---
status: current
owner: runtime
last_verified: 2026-08-23
related_code:
  - include/gargantuan/platform/HostEvent.hpp
  - src/platform/sdl/
  - src/Engine.cpp
  - src/services/ActionMap.cpp
  - src/services/UserInputService.cpp
  - src/classes/Camera.cpp
related_adrs: []
---

# Platform and input boundary

## Implemented boundary

The executable's SDL host adapter owns `SDL_Event` polling and translates one
event at a time into the closed Gargantuan `HostEvent` variant. `Engine`,
`UserInputService`, `ActionMap`, `InputObject`, and `Camera` consume only these engine-owned
values:

```text
SDLHost / SDL_Event
    -> TranslateSDLEvent
    -> HostEvent value
    -> Engine::ProcessEvent
       |- UserInputService physical state
       |- GuiRuntime routed input
       |- ActionMap
       `- native Camera controller when unconsumed
```

The variant alternatives are `KeyEvent`, `PointerMoveEvent`,
`PointerButtonEvent`, `TouchPointerEvent`, `WheelEvent`, `TextInputEvent`,
`TextEditingEvent`, `GamepadButtonEvent`, `GamepadAxisEvent`,
`WindowResizeEvent`, `FocusEvent`, and `WindowCloseEvent`. Unknown SDL events
and invalid key, pointer-button,
gamepad-button, gamepad-axis, window, or device values are ignored without
creating a partially initialized semantic event.

There is no engine event queue in this slice. `SDLHost::PollEvent` drains SDL's
platform queue and returns translated values synchronously, so the boundary
does not add an unbounded second queue or a heap allocation for ordinary
events. A future queued host must define and test an explicit capacity and
overflow policy.

## Key and text semantics

`KeyEvent` deliberately carries both:

- `PhysicalKey`, the keyboard position used by the current free-camera
  controller; and
- `LogicalKey`, the interpreted layout key that preserves the existing public
  `KeyCode` values used by `InputObject` and `UserInputService`.

It also carries press/release state, repeat state, modifiers, and a copied
keyboard device identity. Unsupported physical positions become `Unknown`;
an unsupported logical key is not published because existing developer-visible
input has no representation for it.

Committed text is a distinct `TextInputEvent`; key presses never synthesize
text. `BoundedUtf8` validates UTF-8 and stores at most 63 bytes inline. Invalid
or oversized payloads are rejected. SDL preedit becomes a bounded
`TextEditingEvent` with copied text plus selection start/length; it is not
published as an `InputObject` or confused with committed device state.
`GuiRuntime` consumes both forms for a focused `TextBox` after
`UserInputService` records physical state. A `SetTextInputState` host command
explicitly starts/stops SDL text input and supplies the physical caret rectangle.
Candidate-list UI and a complete cross-platform IME lifecycle remain deferred.

## Pointer, gamepad, and window behavior

Pointer events contain engine-owned position/delta values and the closed
`PointerButton` enum. SDL wheel direction is normalized before publication.
SDL finger down/motion/up values become `TouchPointerEvent` with a copied,
nonzero contact identity and normalized action; `GuiRuntime` converts them into
the same per-pointer logical route used by mouse and future pen input. Unknown
pointer identity is represented by value zero and never carries an SDL pointer.
The existing mouse-button, mouse-position, mouse-delta, and input signals remain
developer-facing behavior.

Gamepad input is not yet consumed by `ActionMap` or `UserInputService`. The SDL
adapter establishes the smallest future-safe semantic boundary: stable
button/axis enums, a copied nonzero device identity, and axis values normalized
to `[-1, 1]`. `UserInputService` does not yet expose gamepad events. Controller
mapping and remapping policy remain deferred.

The current runtime supports one presentation window, so events do not expose
a speculative engine window identity. Resize carries validated nonzero pixel
dimensions. Relative-pointer mode and text-input state are engine-owned
`HostCommand` values applied by `SDLHost`. The default Luau camera sets
`UserInputService.MouseBehavior`; after
synchronous semantic callbacks finish, `UserInputService` converts a changed
behavior to that host command. The legacy native Camera path can still return
the same command when defaults are disabled. Neither service receives or
retains an SDL window pointer. SDLHost stores only the latest numeric SDL window
ID and resolves it at command application, preventing a window pointer from
escaping the adapter.

## Routing and focus groundwork

`Engine::ProcessEvent` handles window lifecycle, records physical state in
`UserInputService`, routes against the last coherent `GuiRuntime` snapshot, then
continues to `ActionMap` and the current native Camera controller only when the
GUI did not consume the event. A consuming ActionMap binding prevents only
lower-level host/camera routing; it does not hide the event from
`UserInputService`. Focus loss clears physical and semantic active state, fires
the normal end/focus signals, retires GUI capture/composition, and releases
relative-pointer and native text-input modes.

`ActionMap` is the gameplay-semantic layer for keyboard, mouse-button, and
relative pointer-delta bindings. Bindings are bounded, support multiple sources
per action, carry priority/consumption metadata, and expose digital, scalar, and
frame-transient vector state. The shipped default binding module declares
movement, jump, orbit, and Look actions. Binding policy is not part of the host
adapter and the low-level UserInputService route remains public. See
[Player runtime](PlayerRuntime.md) for exact API and lifecycle semantics.

The Studio Play viewport uses this same semantic boundary. Avalonia supplies
pointer-button transitions and uncaptured absolute motion. Once the camera's
relative-pointer `HostCommand` returns through EditorHost, the supported Windows
Studio host reads physical relative mouse motion from `WM_INPUT`; it forwards
those raw X/Y deltas as `PointerMoveEvent.Delta` without reconstructing them from
cursor positions, clamping them to the viewport, or warping the cursor. The
position carried beside a captured move remains the fixed capture anchor and
does not determine motion. Pre-activation raw packets are discarded.

Studio owns the focused Play viewport capture and its forwarding lifecycle.
Capture and pending motion are cleared on RMB up, viewport or window focus loss,
Stop, unexpected runtime exit, viewport failure, session disposal, or host
disconnect. Re-entry establishes a new capture generation with no retained
delta. EditorHost and `PlaySession` receive only semantic host events; engine
services and gameplay code do not know about Avalonia coordinates, Win32 raw
input, cursor boundaries, or capture implementation.

Pen host adaptation, candidate-list/advanced IME policy, gamepad
publication/default bindings, persisted remapping, non-Windows Studio
relative-pointer backends, and multi-window architecture are not part of this
boundary. SDL touch and preedit adaptation are implemented, but physical
phone/tablet behavior has not been validated.

## Coupling audit

Before this boundary, SDL input semantics leaked through:

| Location | Previous coupling |
| --- | --- |
| `Engine.hpp` / `Engine.cpp` | Accepted and polled `SDL_Event`; interpreted resize and quit. |
| `UserInputService.hpp/.cpp` | Accepted SDL events and interpreted focus. |
| `InputObject.hpp/.cpp` | Exposed SDL key maps and constructed objects from `SDL_Event`. |
| `Camera.hpp/.cpp` | Accepted SDL events, retained SDL relative-mouse policy, and polled SDL scancodes/modifiers. |

After the change, SDL event and input types occur only in
`src/platform/sdl/SDLHost.*`, backend-specific translation tests, other
unrelated SDL implementation facilities such as logging/filesystem helpers,
and explicit renderer implementations. A non-SDL host can construct and feed
`HostEvent` values without SDL headers or `SDL_Event`.
