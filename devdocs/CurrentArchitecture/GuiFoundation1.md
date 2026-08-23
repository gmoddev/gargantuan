---
status: accepted
date: 2026-08-23
owner: runtime-gui
renderer_dependency: Renderer Foundation 2C
related_code:
  - include/gargantuan/gui/GuiRuntime.hpp
  - include/gargantuan/gui/GuiTypes.hpp
  - include/gargantuan/gui/GuiResources.hpp
  - include/gargantuan/gui/GuiLimits.hpp
  - src/gui/GuiRuntime.cpp
  - src/gui/GuiResources.cpp
  - tests/GuiFoundationTests.cpp
  - tests/GuiFoundationBenchmark.cpp
---

# GUI Foundation 1

> Foundation 1 is the accepted base architecture and historical performance
> baseline. [GUI Foundation 2](GuiFoundation2.md) now defines current
> invalidation, stacking, scrolling, editable-text, and UI-publication behavior.

## Accepted architecture

The first complete runtime GUI foundation is an engine-owned retained-mode
projection. The authored `Instance` hierarchy remains semantic authority; GPU
state never does.

```text
ScreenGui / GuiObject semantic tree
    -> GuiRuntime observation and basic presentation resolution
    -> deterministic measure / arrange
    -> immutable GuiLayoutSnapshot
       -> GuiPresentationSnapshot -> RenderUiFrame / RenderUiBatch
       -> hit testing and routed input
       -> GuiAccessibilitySnapshot
    -> RenderPublisher -> RenderPublication -> RenderProjection -> SDL GuiPass
```

There is one `GuiRuntime` per active `DataModel`. It discovers at most eight
`ScreenGui` roots, observes reflected property/hierarchy changes, and keeps
separate layout, text, presentation, input, resource, and accessibility dirty
domains. A static root reuses committed layout, shaped text, atlas allocations,
and display data. Foundation 2 retains the complete-frame renderer contract but
shares its immutable frame across runtime, publisher, and projection; an absent
UI update preserves the current projection while an explicit empty update clears
it.

`GuiLayoutSnapshot` stores `ObjectId` identities and values, not mutable
`GuiObject` pointers. Display generation and hit testing consume this same
snapshot. Cached roots and renderer projections are disposable and are rebuilt
after viewport change, reload, Play clone construction, or renderer full resync.

## Public object model

The schema-backed Foundation 1 surface is `ScreenGui`, `Frame`, `TextLabel`,
`TextButton`, `ImageLabel`, and one `UIListLayout`. `GuiObject` retains `UDim2`
position/size, anchor, automatic size, layout/Z order, clipping, rotation,
visibility, interaction, and local presentation properties. `AbsolutePosition`,
`AbsoluteSize`, `AbsoluteRotation`, `Active`, and `GuiState` are runtime-derived
and non-writable.

`TextButton` is deliberately small: it supplies button accessibility semantics,
focusability, hover/press state, pointer activation, and Space/Return activation.
Interaction state is resolved before display generation. There is no widget
library or CSS-like style engine. `GuiResolvedPresentation` is the narrow seam
where future themes, classes, variants, contrast, and reduced-motion policy can
replace the current local-property resolver without teaching `GuiPass` widget
class names.

`ImageLabel.Image` and `TextLabel.FontFace` are logical identities. Foundation 1
has a bounded native image registration seam and a controlled default font
provider; neither property exposes paths, SDL objects, or
`RenderTextureIdentity` to game Luau. AssetService can replace those providers
later without changing the public objects.

## Coordinates, layout, clipping, and alpha

The viewport records physical render dimensions, a positive DPI scale, and
logical safe-area insets. Logical dimensions are `physical / dpi`. A root that
clips to the safe area uses this logical rectangle:

```text
x = safe.left
y = safe.top
width  = physicalWidth / dpi - safe.left - safe.right
height = physicalHeight / dpi - safe.top  - safe.bottom
```

For each axis outside list-layout placement:

```text
size = parentSize * Size.Scale + Size.Offset
position = parentOrigin
         + parentSize * Position.Scale + Position.Offset
         - AnchorPoint * size
```

Sizes are clamped non-negative. `UIListLayout` orders direct `GuiObject`
children stably by `(LayoutOrder, original tree order)` and applies `UDim`
padding along its horizontal or vertical main axis. A `Frame` measures visible
child extents; text measures shaped lines; an available image measures its
intrinsic physical dimensions divided by DPI. `AutomaticSize` applies on X, Y,
or both. Measure/arrange is capped and either commits one complete coherent root
or retains the prior root. A non-convergent automatic dependency receives a
bounded diagnostic and authored-size fallback only when another bounded pass is
available.

Rotation is inherited as an affine transform about each object's center. Visual
vertices and descendant geometry use the committed transform. Hit testing first
checks the committed axis-aligned effective clip, then applies the inverse
transform and tests the authored arranged rectangle. Foundation 1 clipping is an
explicit bounded compromise: nested clips are intersections of axis-aligned
transformed bounds and publish through the existing SDL scissor rectangle. It
does not promise arbitrary rotated mask/stencil clipping.

Foundation 1 originally operated as global Z ordering. Foundation 2 implements
the current contract: roots use `(DisplayOrder, ObjectId)`, `Global` sorts all
descendants by effective Z/tree order, and `Sibling` recursively sorts direct
siblings while keeping each descendant subtree inside its ancestor's stacking
position. Drawing and hit testing consume the same committed order.

Opacity uses one convention:

```text
effectiveOpacity = ancestorEffectiveOpacity * clamp(local Opacity, 0, 1)
localAlpha = 1 - clamp(local Transparency, 0, 1)
```

The local alpha is stored in vertex color and effective opacity is stored once on
the batch. Background, image, and text follow the same rule. Fully transparent
primitives are omitted.

## Text and logical textures

The repository-pinned SDL_ttf submodule at
`a42434b8c96daaf7650dbd0befe480c090d1c2eb` is built with its vendored FreeType
and HarfBuzz integrations (`SDLTTF_VENDORED=ON`, `SDLTTF_HARFBUZZ=ON`). SDL_ttf
3.3 development code is zlib licensed, vendored FreeType is used under the FTL,
and vendored HarfBuzz is Old MIT; these are compatible with the MPL-2.0 project
and require no runtime download. The controlled `GargantuanSans.ttf` fixture is
copied at build time from HarfBuzz's pinned `perf/fonts/Roboto-Regular.ttf`
fixture. That Roboto generation is Apache-2.0 licensed. No ambient system font
search occurs in deterministic tests.

SDL_ttf performs UTF-8 decoding, line layout, wrapping, font metrics, and the
HarfBuzz shaping path. A custom CPU text-engine consumer receives shaped glyph
indices and positions, and FreeType-backed `TTF_GetGlyphImageForIndex` supplies
the raster. Invalid UTF-8 is replaced deterministically with U+FFFD. Missing
glyph index zero renders the font's `.notdef` glyph and is recorded in shaped
state instead of being silently dropped. This is real shaping and rasterization,
but Foundation 1 does not claim a complete bidi algorithm, grapheme editing, or
a global font fallback/discovery chain. Foundation 2 adds bounded code-point-safe
editing and SDL preedit/commit integration without claiming grapheme or complete
bidi cursor behavior.

Glyphs live in at most four 1024x1024 RGBA atlas pages with deterministic shelf
allocation. Dirty glyph rectangles on one page are coalesced to one subregion
operation because Renderer 2C permits one texture operation per identity in a
publication. Logical images and atlas pages translate exclusively to
`RenderTextureCreate`, `RenderTextureUpdate`, and `RenderTextureRemove`; renderer
restart reconstructs residency from the publisher's CPU-side committed pixels.

## Input, focus, and callback safety

`UserInputService` always updates physical device state first. `GuiRuntime` then
routes the normalized host event against the last coherent committed snapshot.
Only unconsumed events continue to ActionMap/camera behavior. Consumption never
rewrites physical input state.

Pointer identity is an integer plus Mouse/Touch/Pen type; capture is keyed per
pointer, not by a singleton cursor. Foundation 2 translates SDL touch contacts
through this same route and tests touch scrolling/capture. Physical Android/iOS
behavior remains to be validated rather than inferred from desktop semantic
tests.

Hit testing scans committed nodes from highest paint order and rejects invisible,
clipped, non-interactable, or geometrically missed nodes. The boundary permits a
future spatial accelerator without API changes. A pointer-down route is captured
as generation-safe identities before callbacks:

```text
root capture -> ancestors capture -> target -> ancestors bubble -> root bubble
```

`GuiInputEvent:Consume()` stops later phases. Before each callback, the identity
is looked up again; destroyed/reparented objects are skipped and no borrowed tree
iterator survives arbitrary Luau. Tree changes are reconciled at the normal safe
point, not recursively from a callback. Capture, hover, pressed state, and focus
are pruned when identities disappear. Each root owns at most one focus target;
Tab traverses eligible nodes in deterministic root/tree order, and Space/Return
activate a focused `TextButton`.

Frame phasing is:

```text
host event -> UserInputService physical state
           -> GUI route against previous committed layout
           -> Luau callbacks may mutate semantic GUI
simulation -> PreRender callbacks
           -> GUI dirty reconciliation/layout/presentation
           -> RenderPublication -> renderer
```

## Accessibility, persistence, and Play

The committed renderer-independent accessibility snapshot contains stable
`ObjectId`, parent identity, role, name/value, enabled, focused, pressed,
selected, and logical traversal order. Buttons default to Button; visible text
contributes Text; images and frames remain decorative unless given an explicit
role/name. Hidden content is excluded. A future OS adapter consumes this
snapshot and never infers semantics from batches, glyphs, textures, or pixels.

Authored properties are ordinary schema state and therefore use normal
persistence and cloning. Layout geometry, snapshots, hover/pressed/focus,
capture, shaped runs, atlas residency, dirty flags, and renderer identities are
runtime-only. A PlaySession deserializes an independent semantic tree and owns an
independent `GuiRuntime`; Stop clears runtime focus/capture/signals/cache ownership
without mutating the authoring tree.

## Hard bounds and failure policy

| Resource | Foundation 1 bound |
| --- | ---: |
| roots / DataModel | 8 |
| nodes / root | 16,384 |
| hierarchy depth | 128 |
| layout passes / frame | 8 |
| automatic-size passes / root | 8 |
| display primitives | 131,072 |
| UI vertices / indices | 524,288 / 786,432 |
| clip depth / route depth | 32 / 128 |
| simultaneous captured pointers | 16 |
| UTF-8 bytes / object | 16 KiB |
| glyphs / text object | 4,096 |
| shaped glyphs / frame | 100,000 |
| atlas pages / dimensions | 4 / 1024x1024 RGBA8 |
| shaped-text cache entries / font instances | 16,384 / 64 |
| logical images / max dimension / total bytes | 64 / 1024 / 32 MiB |
| texture upload bytes / frame | 8 MiB |
| retained diagnostics / bytes each | 64 / 1,024 |

Traversal is iterative with explicit cycle/depth checks. An invalid root build or
display build is rejected as a unit and the prior coherent projection is kept.
Diagnostics are code/object coalesced and bounded. Text/image/cache limits fail
deterministically rather than searching the host or allocating without limit.

## Verification and performance scope

`gargantuan_gui_foundation_tests` is headless and covers the public Luau vertical
slice, desktop/high-DPI/portrait/landscape/safe-area layout, nested UDim2,
automatic/list layout, rotation/clipping/hit agreement, z/alpha presentation,
real shaped UTF-8 text and atlas residency, images, event phase order and
consumption, destruction during callbacks, pointer capture/touch identity,
focus/keyboard activation, accessibility stability, persistence, Play isolation,
signal retirement, renderer publication, and full resync.

`gargantuan_gui_foundation_benchmark` exercises semantic trees rather than
synthetic batches. Its Release scenarios cover 1K/10K static and 1%-dirty frames,
deep clipping, repeated/unique text and atlas reuse, 5K/10K buttons, ordered hit
testing, automatic size/list layout, viewport/DPI/safe-area change, and mixed
world/UI publication. It reports mean/P50/P95/P99 for total and available phase
timings plus batches, primitives, texture operations, and upload bytes. A
headless run reports GPU preparation as unavailable; the Renderer 2C GPU
benchmark remains the backend-only measurement and is not compared as equivalent
semantic work.

A fresh local Windows x64 Release run (120 sustained frames per scenario) produced
the following historical Foundation 1 end-to-end timings in microseconds.
Foundation 2 measurements supersede these results for current performance;
phase-level Foundation 1 timings and resource counts remain available in the
benchmark CSV output.

| Scenario | Mean | P50 | P95 | P99 |
| --- | ---: | ---: | ---: | ---: |
| 1K static Frames | 38.84 | 37.30 | 44.50 | 49.60 |
| 10K static Frames | 909.16 | 891.50 | 1,125.80 | 1,352.50 |
| 10K Frames, 1% presentation dirty | 14,752.35 | 14,132.80 | 18,993.30 | 23,119.90 |
| 10K Frames, 1% layout dirty | 24,044.58 | 23,990.90 | 27,544.00 | 32,256.00 |
| Deep clipping, 10K nodes | 895.04 | 884.10 | 1,113.50 | 1,227.90 |
| Repeated text, 1K labels | 2,033.97 | 1,936.80 | 2,797.70 | 3,375.90 |
| Unique text, 1K labels | 6,343.41 | 6,328.80 | 7,911.30 | 9,171.40 |
| 5K buttons | 28,343.42 | 28,886.20 | 33,453.30 | 36,737.90 |
| 10K buttons | 63,066.84 | 65,508.90 | 74,788.80 | 81,557.80 |
| Hit test, 1K interactive nodes | 9.27 | 9.40 | 15.40 | 16.20 |
| Hit test, 10K interactive nodes | 134.03 | 132.00 | 143.50 | 181.70 |
| AutomaticSize/list layout, 5K nodes | 15,637.44 | 15,246.70 | 19,562.10 | 23,430.00 |
| Viewport/DPI/safe-area change, 1K nodes | 1,023.45 | 942.70 | 1,274.90 | 2,851.50 |
| Mixed world/UI publication, 1K nodes | 41.78 | 41.00 | 45.50 | 52.20 |

Phone-sized deterministic layouts and the multi-pointer model establish mobile
intent, not physical mobile validation. SDL touch adaptation now exists, but
remaining device work includes actual Android/iOS validation, font quality/atlas
pressure, DPI changes, thermal and memory pressure, and real GPU draw/upload
performance.
