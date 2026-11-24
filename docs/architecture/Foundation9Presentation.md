---
status: current
owner: engine
---

# Foundation 9 runtime presentation and creator integration

Foundation 9 builds on Engine `7c0aca159` and private Studio `9e058914`.
Runtime presentation follows the existing Instance lifecycle, peer-specific
structural projection, authoritative EditorHost commands, and immutable
renderer publication.

## Player GUI and viewport scenes

Direct `ScreenGui` children of the canonical authored `StarterGui` supply
templates for each owner's `PlayerGui`. A Player keeps that container across
controller changes. Each root chooses its reset policy through
`ResetOnSpawn`. Studio projects open with an available canonical
`StarterGui` and ordinary structure/property authoring continues through
EditorHost.

Roblox's Creator Hub provides a usability reference for the template and
player container vocabulary and for a camera-driven 3D inventory preview.
Gargantuan binds provisioning to its own Player lifecycle, keeps its
Character implementation replaceable, and applies reset to eligible roots
on a later Character binding. [StarterGui reference](https://create.roblox.com/docs/reference/engine/classes/StarterGui)
· [Viewport examples](https://create.roblox.com/docs/ui/viewport-frames)

`ViewportFrame` participates in screen layout, clipping and composition as
a GUI object. Its camera lives with its presentation content and remains a
typed local reference through cloning and GAS. Transforms, mesh resources and
animation enter the existing immutable renderer publication. Viewport
presentation excludes gameplay physics. Shared asset data and bounded targets
support inventory inspection and several independently updated views.

## Creator paths

The edit GUI preview runs the runtime layout and presentation projection
against authored templates. The Studio toggle leaves project revisions and
properties untouched. Studio validates the displayed frame identity before
acting on a selection. The native GAS workflows use EditorHost for scope
replacement and transaction semantics, with project origin used for asset and
source resolution. The EditorHost transfer limit is 512 KiB of decoded
data per import, export, open or save. The standalone GAS v1 document
limit is 8 MiB. [GUI viewport protocol](../../devdocs/CurrentArchitecture/EditorHostProtocol.md#authoring-gui-preview-capability-version-1)
· [GAS authoring contract](../protocols/GAS-Authoring-v1.md)

[Relay Rescue](../../samples/GameplayFoundation7/README.md) provides a playable
two-client exercise. The persistent HUD shows player-local settings and an
animated equipment preview. The briefing panel is reset on respawn. The
mission exercises stock Character, a custom possessed drone, authoritative
gameplay, and GASM/GASP round trips.

## Follow-up priority

1. Planar Mirror can share target ownership and secondary view scheduling
   with ViewportFrame. World visibility, reflected camera handedness, and
   bounded recursion form separate correctness requirements. Use reflected
   world extraction and surface composition evidence before broadening the
   feature. [Vulkan synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html)
2. General secondary render-to-texture becomes practical once the target
   allocations, update policy and ownership are repeatable across several
   view types. Exposing arbitrary targets to Luau would require bounded
   residency and device recovery contracts.
3. Surface and billboard GUI can reuse screen layout, text atlases and
   texture compositing. World transforms, capture space, depth and
   authorization for interaction require their own scope and qualification.
4. GUI styles and reusable layouts can build on generated defaults and GAS
   models. Establish state precedence and predictable editor overrides
   before adding animation or styling rules that invalidate the retained
   layout broadly.
5. Terrain, environment surfaces, particles, beams, and trails depend on
   distinct renderer extraction, lifetime, and streaming budgets. A bounded
   effect proof should identify which parts of existing publication carry
   over.
6. Dedicated-server and player package audience separation is separate
   packaging work. A future artifact split needs to retain the approved
   owner-GUI Client Script and ModuleScript sources and their provenance in
   the player artifact. Validate hydration and deny server-only code from
   that artifact before changing deployment policy.

Qualification receipts include the precise source trees, benchmark repeats,
and the scope of native transport and offscreen rendering evidence.
