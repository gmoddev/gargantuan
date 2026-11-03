---
status: current
owner: editor-runtime
last_verified: 2026-11-03
related_code:
  - src/editor/EditorHost.cpp
  - include/gargantuan/editor/PlaySession.hpp
  - src/editor/PlaySession.cpp
  - tests/InputActionTests.cpp
related_adrs: []
---

# Play input inspection v1

This additive EditorHost read operation consumes the persistent Input Action
foundation. It creates no input pipeline, semantic evaluator, authoring mutation,
subscription, event history or runtime-to-server authority.

`Handshake.Capabilities` advertises `PlayInputInspection`. A client must check
that capability before calling `GetPlayInputSnapshot` with exactly
`{ "PlaySessionId": "<active engine-issued canonical decimal>" }`. The token
and native `ReadDataModel` capability are mandatory. Missing/noncanonical IDs or
extra fields fail `MalformedRequest`; absent/replaced IDs fail `StalePlaySession`;
a retained non-running session fails `PlaySessionNotRunning`. A missing runtime
input service fails `InputInspectionUnavailable` without constructing one.
Inspection resolves the exact evaluator owned by the runtime Engine. A replacement
InputService in the mutable hierarchy is never adopted by class/name. Destroying
the active evaluator makes inspection unavailable. Ordinary detach retains its
DataModel association and exact evaluator identity; it does not adopt another
service. The getter also defensively rejects a mismatched runtime scope (normal
cross-DataModel reparenting is already forbidden by the Instance boundary).

The result is `{ PlaySessionId, Snapshot }`. Snapshot contains:

| Field | Contract |
| --- | --- |
| `Version` | `1`, independent of envelope/schema versions |
| `Scope` | Runtime DataModel `{ Slot, Generation }`; never the authoring scope |
| `Revision` | Compiled definition revision, not project revision or event sequence |
| `MemoryBytes` | Native graph capacity estimate, not process working set |
| `Modifiers` | Bits 0..7: left/right Control, left/right Shift, left/right Alt, left/right Meta |
| `PressedKeys` | At most 4096 numeric physical-authority logical KeyCodes |
| `Contexts` | At most 64 rows: Id, Name, Enabled, Priority, Consume |
| `Actions` | At most 256 rows: Id, Context, Name, Enabled, Type, Down, Value, Vector{X,Y}, LastTransition, LastReason |
| `Bindings` | At most 2048 rows: Id, Action, Name, Enabled, Type, Code, Modifiers, ExactModifiers, Priority, Consume, Matched, Active, Suppressed, Eligible, WinningContext |
| `ValidationErrors` | At most 64 native validation strings; no source text |

All IDs use the existing wire ObjectId encoding; absent WinningContext is null.
Names are presentation labels, never identity or a mapping to authoring objects.
Names exceeding 256 UTF-8 bytes use a fixed omission label rather than slicing
UTF-8. The graph's enum names are closed strings (`Bool/Scalar/Vector2`,
`Key/MouseButton/PointerDelta`, and documented cancellation reasons). Vector is
the raw accumulator, matching local InputService inspection.

The owner-thread request captures the current disposable compiled graph. A dirty
definition may trigger the ordinary bounded compile/publication path; inspection
does not advance simulation. Values can change immediately afterward. No retained
live pointer, native handle, source buffer, committed text or preedit crosses the
boundary. The ordinary 8 MiB response limit still applies. Polling is explicit and
optional; clients should allow one outstanding request and use a modest cadence,
drop responses whose PlaySessionId/scope no longer match, and stop polling on
Stop, disconnect, window disposal or feature disable.

The owning copy cannot mutate runtime state. Reads do not change authoring
revision, persisted revision, dirty flag, history or journal. Runtime ObjectIds
must not be submitted to authoring operations, persisted as durable document
bindings, or guessed to correspond by label/path. Restart invalidates the old
PlaySessionId and creates a fresh scope. The existing local InputService evaluator
remains sole semantic authority; a client action never grants trusted server
gameplay authority.

Qualification: InputActionTests exercises capability/field/identity rejection,
runtime identity isolation, persisted definitions, Ctrl+Shift+S, copy ownership,
focus cancellation, bounds, Stop/restart and unchanged authoring state. Native
physical devices and a Studio UI are separate qualification gates.
See [source-bound qualification](PlayInputInspectionValidation.md) for the
Windows 54-test result, binary hash, limits and continuation.

The chord integration test also identified an existing EditorHost admission bug:
the default enum reflection range omitted modifiers/navigation above 127. The
host now validates against the declared key enums with explicit reflection
ranges, rejecting holes and values above uint16 before casting. No second input
authority or enlarged arbitrary numeric vocabulary is introduced.
