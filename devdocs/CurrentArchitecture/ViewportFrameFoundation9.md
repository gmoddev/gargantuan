# ViewportFrame in Foundation 9

`ViewportFrame` is a reflected `GuiObject` for a three-dimensional preview
inside a normal GUI rectangle. It uses inherited layout, rotation, clipping,
background, opacity, visibility and paint order. A transparent background
leaves empty pixels open to the underlying UI and scene.

```luau
local Preview = Instance.new("ViewportFrame")
Preview.Size = UDim2.fromOffset(240, 144)
Preview.BackgroundTransparency = 1
Preview.MaxUpdateRate = 30
Preview.Parent = InventoryGui

local Camera = Instance.new("Camera")
Camera.CFrame = CFrame.new(0, 0, 6)
Camera.FieldOfView = 45
Camera.Parent = Preview
Preview.CurrentCamera = Camera

local Item = game:GetService("ReplicatedStorage").InventoryModels.Example:Clone()
Item.Parent = Preview
```

Place the Camera directly below the ViewportFrame or in an owned descendant
Folder. The nullable `CurrentCamera` reference accepts a live Camera whose
nearest ViewportFrame ancestor is this viewport. Destroying the camera or
moving it out of scope clears the reference. Background still renders when
the camera is empty. The pass derives the vertical projection using the
view's target dimensions and a fixed 0.1-to-1000 depth interval. It does not
change the Camera's suggested runtime viewport size or feed desktop input
to the Camera.

Use Folder to organize Parts and MeshParts. Parts use each object's own
`CFrame`, `Size`, color, shape and transparency. Folders are organizational
and do not add a transform. MeshParts resolve shared project mesh and
material assets, including supported texture slots and skin palettes. A
direct MeshPart Animator uses ordinary local AnimationTrack playback
through the existing runtime. Visible viewport rigs use presentation
visibility feedback independently of gameplay camera distance. Each view
samples authored or animated changes at the configured rate. Root motion
does not enter gameplay from a viewport. Other 3D object types, attachments,
lights, deformables and effects are outside this scene contract.

Default lighting uses a neutral directional key and ambient fill.
`RenderScale` requests a scale between 0.25 and 1 for physical GUI pixels
and defaults to 1. `MaxUpdateRate` ranges from 1 to 60 and defaults to 30.
The renderer may reduce either quality or cadence to fit the active budget.
The backend shrinks oversized targets proportionally and rounds the
dimensions to physical pixels.

## Ownership and publication

The viewport owns semantic children in the current DataModel. It creates no
secondary DataModel, WorldRoot, physics provider, network session or
gameplay renderer publication. If authored below a legacy Workspace
ScreenGui, the viewport still forms an extraction and physics boundary.
StarterGui clones the entire template hierarchy using the normal local
reference remapping. PlayerGui viewport children use the owner's scoped
GUI delivery. Viewport transforms, camera properties and asset references
use standard instance persistence in JSON and GAS. Rendered pixels,
surfaces, revision samples and skin resources have no persistent state.

On Main, GuiRuntime admits visible and unclipped ViewportFrames and samples
a bounded set of supported children. It compares each completed scene to the
previous one. Static scenes retain the same immutable description and
reuse the color target. Hidden or fully clipped views have no extraction
walk. RenderPublication carries a replacement list of active immutable
scenes alongside the existing GUI batches. Each viewport batch holds a
generation-safe viewport ObjectId distinct from the ordinary asset texture
namespace. EditorHost placement may transform and clip the GUI quad
independently while keeping the associated camera viewport projection.

The SDL GPU runtime and EditorViewportRenderer each own one secondary
renderer. Both share the device-local world mesh, texture and sampler
resources, while using a dedicated scratch projection and one lazily
created Opaque pass for all ViewportFrames. A redraw clears an RGBA8 target
to transparent and records the reusable 3D material pass before GuiPass.
GuiPass composites the target at the ordinary 2D draw position with its
clip and opacity. The pass uses the fixed neutral lighting without main
world fog, sky, shadows, probes or exposure. Studio authoring capture reuses
the same native viewport draw. It can render static authored poses without
stepping gameplay.

| Bound | Foundation 9 limit |
| --- | --- |
| Active views per surface | 8 eligible visible ViewportFrames in GUI paint order |
| Rendered objects per view | 64 |
| Inspected nodes per view | 256 at up to 32 nested Folder levels |
| Mesh material ranges plus primitives per view | 256 |
| Imported mesh indices per view | 1,048,576 across all material ranges |
| Active skinned rigs per view | 8 |
| Color/depth target dimensions | 512 maximum edge |
| Active target payload per renderer | Up to 16 MiB RGBA8 plus D32, before driver padding and retired allocations |
| Redraw submissions per frame | 2, chosen in round-robin order |

Limits produce a transparent foreground or retain the last submitted
coherent image while a scheduled replacement is pending. The GPU backend
publishes a cached target only when draws complete recording and commits its
revision after submission. Hiding a viewport, losing its GUI batch, removing
the camera or changing renderer generation releases its targets and palette
buffers through SDL GPU lifetime management. Resize only reallocates a
viewport target when its sampled dimensions change. No per-view GPU
readback, swapchain, synchronization wait or device is required. Headless
renderers own no viewport targets. The Filament renderer does not implement
this composition in Foundation 9.

Studio Play's CPU-only publication renderer supplies visual palettes to the
EditorHost SDL consumer. Its animation policy uses conservative evaluation
when the deferred renderer has no camera visibility feedback. A dedicated
headless renderer retains the semantic-only animation policy.

## Qualification

- `gargantuan_viewport_frame_tests` exercises camera scope and hierarchy,
  cloned identity remapping, JSON clone and GAS model/place persistence,
  aspect, hidden and clipped invalidation, render scale and world isolation.
  The same test compares a visible preview alone with a visible preview
  alongside 128 authored hidden previews. It reports steady CPU time and
  permanent `ViewportExtract` timing and scene-sample counts.
- `gargantuan_animation_foundation_tests` runs an authored skinned MeshPart
  and clip under a ViewportFrame using the normal pose evaluator, then checks
  that independent immutable palette revisions use the same source mesh.
- `gargantuan_editor_viewport_smoke` checks native Vulkan readback for
  multiple viewport images, clipping, transparent foreground, static reuse
  and cleanup. Its `--viewport-workloads` test mode holds ordinary gameplay
  draw fixed while varying view count, visibility, changed transforms,
  resolution and identity turnover. Its mostly-hidden case leaves seven of
  eight renderer descriptors unreferenced by GUI batches. It prints GPU wait separately from
  estimated CPU submission cost and counts GPU target allocations and bytes.
- Runtime work traces record `ViewportExtract` and `ViewportDraw` separately.
  Neither GPU fence wait nor offscreen readback qualifies physical display
  presentation, and target counts exclude driver overhead.

See the [Mirror proposal](../../docs/adr/PlanarMirror.md) for future planar
reflection design and its shared offscreen backend requirements.
