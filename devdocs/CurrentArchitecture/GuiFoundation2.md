---
status: current
owner: runtime-gui
last_verified: 2026-08-23
renderer_dependency: Renderer Foundation 2C
related_code:
  - assets/classes/ScrollingFrame.luau
  - assets/classes/TextBox.luau
  - include/gargantuan/gui/GuiRuntime.hpp
  - include/gargantuan/gui/GuiTypes.hpp
  - include/gargantuan/gui/GuiLimits.hpp
  - src/gui/GuiRuntime.cpp
  - src/gui/GuiResources.cpp
  - tests/GuiFoundationTests.cpp
  - tests/GuiFoundationBenchmark.cpp
related_adrs: []
---

# GUI Foundation 2

## Scope and preserved architecture

Foundation 2 extends, rather than replaces, the retained Foundation 1 pipeline:

```text
ScreenGui / GuiObject semantic tree
    -> GuiRuntime dirty epochs and presentation resolution
    -> deterministic measure / arrange
    -> immutable GuiLayoutSnapshot
       -> persistent GuiPresentationProjection -> immutable RenderUiFrame
       -> hit testing / routed input
       -> GuiAccessibilitySnapshot
    -> RenderPublication -> RenderProjection -> SDL GuiPass
```

Layout, display generation, hit testing, scrolling, editable text, focus, and
accessibility remain headless engine concerns. `GuiPass` still consumes only
renderer-neutral vertices, indices, logical texture identities, rectangular
clips, layers, and opacity. It does not know about `TextButton`, `TextBox`,
`ScrollingFrame`, or stacking behavior.

## Incremental invalidation

Foundation 1 had distinct dirty concepts but converted any live root change into
large flat-set merges and complete display/accessibility reconstruction. The
benchmarks showed that the cost was not hit testing. Most time came from merging
large root snapshots, reconstructing display primitives/batches, copying complete
`RenderUiFrame` values through publication, and rebuilding accessibility after
interaction-state changes. Static button scenarios also drove pointer motion
through full presentation/publication work even when only one hovered identity
changed.

Foundation 2 assigns a monotonically increasing dirty epoch to a stable
`ObjectId` and combines only the semantic domains required by observed property
changes:

- `Layout`, `Presentation`, `Text`, `Resource`, `InputGeometry`,
  `Accessibility`, `Hierarchy`, and `Scroll` are bounded bit domains, not
  property-name queues.
- repeated writes coalesce in one object/epoch entry;
- unrelated roots keep their committed snapshots and never enter reconciliation;
- a color-only change refreshes that object's resolved presentation and retained
  solid-quad span without measure, arrange, shaping, or accessibility work;
- a position change rebuilds the independently placeable affected subtree when
  its parent geometry is unchanged, and otherwise rebuilds only the dependent
  root;
- focus/hover/press state dirties only the identities whose semantic state
  changed;
- failed root/display construction retains the previous coherent commit.

The persistent renderer-neutral presentation projection records the batch and
vertex span for eligible solid nodes. A local update clones the last immutable
frame, patches only the affected quads, and retains ordering/batching. Changes
that can alter primitive count, texture, clip, or paint order deliberately fall
back to a coherent display rebuild.

`RenderPublication` remains a complete-frame UI contract. It now distinguishes
an absent UI update from an explicit empty frame and can share one
`shared_ptr<const RenderUiFrame>` from `GuiRuntime` through `RenderPublisher` and
`RenderProjection`. This removes repeated cross-boundary geometry copies without
introducing a speculative backend delta. The final clone needed to create a new
immutable flat frame is still O(total frame vertices), and a dependent layout
root still creates an immutable flat root snapshot. Those two coherence
invariants explain the remaining non-proportional tail; semantic resolution and
primitive replacement themselves are proportional to affected objects.

## Measured effect

The table compares the accepted Foundation 1 Windows x64 Release baseline with a
fresh Foundation 2 Release run. Times are end-to-end microseconds; dynamic rows
use 120 sustained samples.

| Scenario | F1 mean | F2 mean | F2 P50 | F2 P95 | F2 P99 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 10K static Frames | 909.16 | 1.31 | 1.20 | 1.50 | 1.90 |
| 10K Frames, 1% presentation dirty | 14,752.35 | 866.93 | 942.90 | 1,081.00 | 1,139.20 |
| 10K Frames, 1% layout dirty | 24,044.58 | 5,500.35 | 5,475.80 | 6,563.00 | 6,774.00 |
| 5K static buttons | 28,343.42 | 2.87 | 1.20 | 1.30 | 1.80 |
| 10K static buttons | 63,066.84 | 6.79 | 2.00 | 2.30 | 3.00 |
| one moving hover among 10K buttons | n/a | 159.41 | 137.00 | 305.60 | 487.70 |
| one hover/press among 10K buttons | n/a | 1,097.96 | 1,049.20 | 1,375.10 | 1,759.00 |
| nested scrolling, 1K children | n/a | 400.63 | 378.30 | 519.40 | 575.40 |
| 10K children in one ScrollingFrame | n/a | 11,752.01 | 11,464.60 | 15,081.10 | 15,310.80 |
| text insert/delete | n/a | 20.29 | 9.00 | 16.20 | 40.10 |
| sibling-stacking stress, 10.1K nodes | n/a | 1.44 | 1.30 | 2.00 | 2.40 |
| mixed world/UI publication, 1K GUI nodes | 41.78 | 1.23 | 1.20 | 1.30 | 1.60 |

The benchmark reports observation, semantic resolution, layout, text,
presentation, accessibility, snapshot commit, display/frame construction,
frame copy, publication, and projection independently. For the 1%-presentation
case the mean solid-display/frame work is about 251 microseconds and publication
about 1 microsecond; it no longer performs layout or text shaping. For the
1%-layout case snapshot commit remains dominant at about 3.93 milliseconds.
Pressed-button accessibility still copies the flat accessibility snapshot and is
the dominant cost in that scenario. GPU preparation is intentionally unavailable
in the headless benchmark and remains a separate Renderer 2C measurement.

## Paint order and sibling stacking contexts

Roots always sort by `(DisplayOrder, ObjectId)`. Within a root:

- `Global` sorts all effective descendants by `(ZIndex, stable tree order)`;
- `Sibling` recursively stable-sorts each parent's direct GUI children by
  `(ZIndex, stable child order)` and emits each child's subtree atomically before
  the next sibling. Descendants therefore cannot escape an ancestor's sibling
  stacking position.

Nested sibling contexts use the same recursive rule. Layout transforms and
rectangular ancestor clips are committed before paint ordering, so transformed or
clipped descendants retain their context without changing the clip contract. The
layout snapshot stores this exact paint order; drawing scans it forward and hit
testing scans it backward. Tests distinguish Global from Sibling with overlapping
descendants and cover nested contexts, clipping, reparenting, destruction, and
`hit order == paint order`. KI-006 is resolved.

## ScrollingFrame

`ScrollingFrame` extends `Frame` with authored `CanvasPosition`, `CanvasSize`,
`AutomaticCanvasSize`, `ScrollingDirection`, and `ScrollBarThickness`, plus the
runtime-derived `ContentExtent`. Its viewport always clips descendants. Child
authored `Position` values remain unchanged; layout commits a canvas translation
and both display and hit testing consume the translated geometry.

Wheel input scrolls the nearest eligible ancestor and chains to an outer scroller
when an inner one is already at its bound. Touch and pen use the same per-pointer
gesture state as mouse: movement past the drag threshold transfers that pointer's
capture to the scroller and cancels button activation. Up/cancel/destruction
retires the gesture deterministically. Arrow, Page Up/Down, Home, and End scroll
the focused scroller or focused descendant. The renderer-neutral presentation
adds bounded solid scrollbar indicators; no per-widget GPU resource exists.

There is no inertia or virtualization in Foundation 2. The stable canvas/content
semantics and input boundary allow virtualization to be added later without
changing child authored positions or the public scroll model.

## TextBox, Unicode, and IME

`TextBox` extends `TextLabel`. Authored properties are `Text`,
`PlaceholderText`, `PlaceholderColor3`, `ReadOnly`, `MultiLine`,
`SecureTextEntry`, and bounded `MaxLength`. Runtime-only properties expose
zero-based Unicode code-point `CaretPosition`, `SelectionStart`,
`SelectionLength`, and transient `CompositionText`; `Submitted(Text)` fires for
Return in a single-line field.

Insertion, Backspace/Delete, logical arrows, Home/End, Shift selection, pointer
placement/drag selection, focus, single-line horizontal caret scrolling, and
multiline Return are implemented. The storage and public editing model never
place a boundary inside a UTF-8 sequence. Invalid authored input is normalized to
U+FFFD; committed host text rejects invalid/oversized UTF-8. This milestone is
code-point safe, not grapheme-aware. It intentionally uses no custom Unicode
segmentation algorithm. HarfBuzz/SDL_ttf still shapes displayed text and supplies
substring offsets for pointer/caret placement, but without a complete bidi
mapping visual caret navigation for bidirectional text is not claimed.

Secure fields preserve plaintext only as authored semantic state. Display,
shaping, renderer frames, and accessibility values receive a same-length masked
string. Plaintext can still be read by game code with normal property authority;
this is presentation privacy, not secret storage. The platform-neutral text-input
command also carries secure, multiline, and autocorrect policy. SDL maps secure
fields to hidden-password input with capitalization and autocorrect disabled where
the platform supports those hints; this does not claim secrecy beyond SDL and the
host operating system.

SDL text input is explicitly started/stopped from focused `TextBox` state and is
given the physical caret rectangle. `TextEditingEvent` carries bounded preedit
text and selection, committed text remains `TextInputEvent`, and focus loss or
commit clears composition. The semantic composition state machine is exercised
headlessly and the SDL desktop adapter is integrated. Candidate-list UI,
grapheme-aware composition selection, platform-specific IME UI policy, and full
cross-platform physical validation remain deferred. Clipboard commands are also
deferred: SDL has a native API, but the current one-way `HostCommand` boundary has
no safe bounded asynchronous return channel and game code must not receive an
ambient platform handle.

## Focus, presentation state, and capture

The existing one-focus-per-root model now covers buttons, scroll views, and text
fields with deterministic Tab traversal. Return inserts/submits while a
`TextBox` is focused; Space/Return activates a focused button otherwise. The
resolved presentation record carries hover, pressed, focused, disabled, and
selected state independently of widget class. Selection and caret are ordinary
solid display primitives, so SDL has no widget policy.

`InputSink` and `Interactable` are separate. A non-interactable node may remain a
hit target solely to consume input according to its sink, but it never becomes
pressed, captures for activation, edits text, or emits `Activated`. Every pointer
and keyboard release rechecks live interactability, including callback-time and
down/up transitions. Focus is retained when a widget is disabled so re-enabling
restores the stable navigation position, but disabled focused widgets cannot
activate and a disabled `TextBox` stops native text input.

Pointer capture remains keyed by normalized pointer identity. Button presses,
text selection, and scrolling all negotiate through the one router. A scroll drag
may transfer capture only after its threshold; destruction, reparenting, focus
loss, root teardown, and Play stop prune generation-checked state.

## Accessibility, persistence, and bounds

Accessibility adds renderer-independent TextBox/TextField and ScrollView roles.
Text fields publish masked or normal editable value as appropriate, read-only,
focus, caret, and selection. Scrollers publish current X/Y position and maximum
range. Stable accessibility identity remains the engine `ObjectId` and does not
change for layout-only work or renderer restart.

Ordinary schema persistence stores authored scroll policy/canvas position and
text value/placeholder/read-only/multiline/secure/max-length semantics. It does
not store caret, selection, composition, gesture capture, hover, focus, or atlas
state. Play clones get independent runtime editing/scroll state and Stop cannot
contaminate the authoring tree.

Foundation 1 bounds remain in force. Foundation 2 additionally enforces:

| Resource | Bound |
| --- | ---: |
| nested scroll containers | 16 |
| logical scroll extent per axis | 1,000,000 |
| editable text | 16 KiB / 4,096 code points |
| selection operations / frame | 1,024 |
| composition text | 4,095 bytes |
| future clipboard payload seam | 64 KiB |
| text edits / frame | 256 |

Limit failure rejects or clamps the individual semantic operation, emits a
bounded/coalesced diagnostic where applicable, and never publishes corrupt
partial geometry.

## Verification and remaining work

The headless GUI suite covers dirty-domain selectivity and coalescing, unrelated
roots, coherent failure retention, sibling/global overlap and callback-time tree
changes, scroll wheel/touch/nesting/clipping/hit/destruction/resize/focus,
code-point editing and invalid UTF-8 normalization, caret/selection/navigation,
secure native-input policy and masking, disabled/sink activation denial, IME
state, persistence, Play isolation, accessibility, and shared
UI publication. Desktop semantic touch translation is present, but no physical
Android/iOS performance or IME claim is made.

Foundation 3 priorities are persistent/paged layout and accessibility snapshots
to remove the remaining root-size copy tail, a measured renderer-neutral UI delta
only if flat-frame cloning is still dominant, grapheme and bidi cursor mapping via
a mature dependency, a bounded clipboard response protocol and richer IME
lifecycle/candidate integration, scroll virtualization/inertia, OS accessibility
adapters, and physical mobile validation. World/surface GUI and broad widget/style
libraries remain separate work.
