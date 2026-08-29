# First complete game validation

Status: Animation Foundation 2B semantic anchors revalidated locally on 2026-08-29. This document records a vertical-slice
falsification exercise; it does not claim that Gargantuan is generally usable.

## Game design and scope

`samples/FirstCompleteGame` is a small third-person collection/obstacle game.
The default player spawns over an authored floor, three cyan Parts act as
collectibles, an anchored obstacle moves across the course, and an imported
textured MeshPart marks the far end. Three authored `ProximityPrompt` Instances
use `InteractionService` for proximity, hold timing, semantic input, and final
validation. Ordinary client Luau listens to `Triggered` and owns collection,
round state, obstacle motion, completion, and reset. Each collectible has a
positional `Sound`, while a non-spatial Sound under `GameScripts` marks round
completion. A `ScreenGui`
shows an imported badge, imported-font progress text, a completion panel, and a
real `TextButton` restart action.
The canonical `Lighting` service authors afternoon sun, ambient color, exposure,
bounded fog, and one direct six-face `Sky` that reuses the imported square Image.

The deterministic `K` action completes the round for automation. It is a test
hook implemented through the public `ActionMap`, not a native gameplay bypass.
Networking, combat, VFX, and soft bodies were intentionally not added. The
authored `AnimatedBeacon` is a two-joint skinned MeshPart driven by the looping
`BeaconPulse` clip through the public Animator/AnimationTrack API. Its
`BeaconTipAnchor` binds `BeaconRoot/BeaconTip`; a quiet positional Sound and a
small ProximityPrompt beneath it follow the same renderer-independent semantic
world transform.

## Authoring workflow used

1. Build the current Studio and Engine Release binaries.
2. Create the project with Studio's `CreateProject` workflow (the automated
   equivalent of File -> New Project), then reopen the created project.
3. Import `assets/collection-beacon.gltf`, `assets/Roboto-Regular.ttf`, and
   `assets/collection-tone.wav` through the same EditorHost asset operations
   used by Studio's Assets panel. The glTF import creates the Mesh, Material,
   and Image records as one dependency group.
4. Through Studio's authenticated project-scoped bridge, use the normal
   `StudioCommandRunner`/EditorHost mutation lane to create the hierarchy, edit
   properties, and create the `RoundManager` Script. Studio reported no source
   diagnostics before Save.
5. Save, destroy the authoritative session, reopen, and compare hierarchy,
   scripts, asset catalog, authored transforms, and GUI layout.
6. Play through EditorHost/Studio semantics and repeat Start/Stop ten times.
   The headless gate separately drives the same launch snapshot through
   `PlaySession`, sends semantic host input, resizes the runtime publication,
   completes and restarts the round, and inspects the immutable world/UI
   publication without requiring a physical GPU backend.
7. Launch the unchanged project outside Studio with the supported standalone
   command below.

This could not be authored efficiently through the visible Studio GUI alone.
Studio lacks compound-value editors and transform gizmos for the required
`CFrame`, `Vector`, and `UDim2` work. The bridge and direct public EditorHost
asset operation were used as automation adapters to the same authoritative
commands; they are not alternate game/runtime APIs. A later hidden Studio
relaunch also stayed alive without publishing a project bridge or spawning its
EditorHost child, so the post-fix layout save/reopen was completed directly
through EditorHost. Both are Studio usability/automation defects and prevent an
honest claim of an entirely GUI-authored workflow.

## Systems exercised

- project creation, v4 instance persistence, Save, and reopen;
- Studio hierarchy, property, source, history/revision, and diagnostic lanes;
- Asset Foundation 2A compound glTF import and Mesh -> Material -> Image
  dependencies;
- imported 512 x 512 PNG and imported Roboto font resolution;
- canonical PCM16 WAV import, positional pickup playback, and non-positional
  completion playback through schema-backed Sound Instances;
- `ActionMap`, `DefaultPlayerController`, `DefaultCamera`, and
  `DefaultPlayerRuntime`;
- `ProximityPrompt`, `InteractionService`, keyboard/gamepad semantic bindings,
  touch presentation, zero-duration activation, hold activation, and semantic
  rigid-physics line of sight;
- kinematic player motion, physics stepping, camera, and renderer publication;
- saved Lighting/Sky authoring, coherent Image residency, environment-only
  publication, sun/shadow/fog/exposure state, and package shader inclusion;
- canonical skinned Mesh/Animation assets, renderer-neutral palette
  publication, GPU opaque/shadow skinning, CPU fallback, and renderer restart;
- canonical Attachment joint binding, transient `WorldCFrame`, bind-pose
  fallback, animated positional Sound, animated prompt range/LOS/hold
  validation, and zero-journal semantic movement;
- ordinary Luau scripts, services, signals, cleanup, and structured output;
- `ScreenGui`, `Frame`, `ImageLabel`, `TextLabel`, `TextButton`, layout, input,
  activation, visibility, and viewport resize;
- isolated Play/Stop, GUI/runtime mutation discard, and standalone execution.

The persisted project has 38 Instances, 9 authored GUI Instances, and 7 imported asset
records. A representative headless runtime composition step is approximately
0.1 ms on the validation machine; this is not a frame-time benchmark. A local
real-GPU cold offscreen capture was approximately 250 ms, including renderer
and GPU initialization. Real SDL GPU capture remains in the separate viewport
smoke and is intentionally not part of headless CTest. The game gate observed
no Error diagnostics.

## Native engine changes required

The original vertical slice exposed and fixed two composition defects.

1. `GuiObject.Position` and `GuiObject.Size` were declared serializable but the
   project codec omitted `UDim2`. Studio Save silently dropped every authored
   GUI rectangle. The v4 codec now writes and validates exact four-component
   `UDim2` values, and GUI Foundation has explicit layout round-trip coverage.
2. EditorHost's Play viewport stepped the isolated runtime but then re-extracted
   its world with an editor-owned `RenderPublisher`. That publisher had none of
   the runtime AssetService texture/mesh residency, so a valid textured
   MeshPart failed with `Render publication material references a texture
   without residency`. PlaySession now hands the exact runtime
   `RenderPublication` stream to the viewport CPU/GPU projections. Play/Stop
   replaces those projections so publication identities cannot cross sessions.

No native gameplay code, sanitizer suppression, broad test serialization, or
new foundation was added.

Environment / Lighting Foundation 1 later extended the same fixture without
adding gameplay code. Save/reopen and package gates now preserve the canonical
Lighting/Sky hierarchy, and the packaged 512 x 512 Image is reused for all six
faces so the environment exercises AssetService and renderer publication without
expanding the source asset set.

Interaction Foundation 1 subsequently replaced the game's remaining semantic
workaround. Engine now owns a bounded spatial prompt index, per-player
candidate/hold state, final range validation, and read-only presentation state.
The default visual composition remains engine-shipped Luau on ordinary GUI,
while `RoundManager` contains only three `Prompt.Triggered` connections and
game-specific collection behavior. The prior per-frame Luau distance loop was
removed rather than left as a fallback.

Physics Query Foundation 1 now supplies the general `Workspace:Raycast`
boundary used by interaction LOS. `Collectible2` persists
`RequiresLineOfSight=true`. The deterministic headless gate first proves the
prompt is available with a clear path, inserts a rigid wall between the player
and exact prompt anchor and proves it becomes unavailable, then destroys the
wall and proves availability returns. This uses the same query/filter/identity
path available to ordinary gameplay; there is no sample-specific LOS helper.

Animation Foundation 2A keeps Animator sampling, blending, hierarchy solving,
and the current semantic pose in the runtime. Graphical FirstCompleteGame GPU
validation consumes that publication with one stable skinned source resource
and a per-rig palette, observes advancing pose revisions and palette uploads,
and observes zero CPU-skinned vertex uploads. A fresh renderer full resync
recreates the source/palette resources from the current pose without restarting
the track. The headless and packaged gates assert palette-only publication with
no posed mesh or dynamic vertex update. Opaque and shadow passes bind the same
palette resource/revision.

Animation Foundation 2B adds no second pose evaluation. Engine now resolves the
authored `BeaconTipAnchor` from the CPU joint-model transform before Sound and
Interaction update. The deterministic headless gate records pose A, advances
the clip, observes a changed `Attachment.WorldCFrame`, proves the prompt is
available at pose B and unavailable at the stale pose-A point, and verifies the
quiet looping Sound hierarchy. Save/reopen preserves only CFrame/JointPath;
ten Play/Stop cycles preserve the complete authoring snapshot. The relocated
Release/Vulkan package proof compares the same socket with the GPU palette
oracle, observes 12 palette uploads and zero CPU vertex uploads, then replaces
the renderer while semantic revision continues.

## Studio and API findings

Findings use the requested A-F categories.

| Category | Evidence |
| --- | --- |
| A - missing feature | The original proximity primitive, semantic rigid raycast, animated joint anchor, working interaction LOS, and packaged launcher gaps are now closed. Shape/overlap queries and soft-body-to-anchor participation remain future work. |
| B - API ergonomics | Basic prompt interaction now requires an authored prompt plus `Triggered` handler. `Attachment.JointPath` is a bounded generic string; an asset-backed canonical-joint picker, device-aware/rebindable hints, and custom prompt presentation remain future ergonomics. |
| C - Studio UX | No transform gizmos or compound property editors; limited scalar Properties support; no bridge tools for Assets or Play; one hidden relaunch failed to establish a project session; no Run Standalone command. |
| D - documentation/discoverability | A developer currently needs CMake/build-tree knowledge, adjacent runtime modules/DLL knowledge, and internal awareness of asset references to run the project outside Studio. |
| E - architecture defect | `UDim2` persistence loss and the split Play-viewport resource publication were real cross-system defects. The gate covers `UDim2` and coherent runtime world/UI publication; the separate real-GPU viewport smoke covers device consumption. |
| F - game-specific need | The course geometry, crystal art, obstacle tuning, counter copy, and win-panel styling do not justify engine work. |

Animation Foundation 1 and 2A now animate the authored beacon without changing
the game loop. Audio Foundation 1 closes the earlier presentation gap with
optional cues that remain fail-open and irrelevant to gameplay correctness.

## Asset evidence

The seven project records reopen as `Ready`: two Mesh assets, Material, Image,
Font, Audio, and Animation. The static Mesh depends on Material, Material
depends on Image, and Animation depends on its compatible skinned Mesh. The authored
`MeshPart`, `ImageLabel`, `TextLabel`, all six Sky faces, and all five Sounds resolve only
`asset://` references.
The PNG was reduced from 1024 to 512 pixels after standalone validation showed
that the larger image plus font atlas exceeded the bounded per-frame GUI upload
budget. Reimporting the compound group through AssetService retained stable
references and removed that game warning.

Asset provenance and licenses are in `assets/NOTICE.md`.

## Play isolation and save/reopen evidence

`gargantuan_first_complete_game` performs ten real EditorHost
`StartPlaySession`/`StopPlaySession` cycles. It compares the complete authoring
snapshot and project state after the cycles. Completion makes all three runtime
collectibles transparent and shows the win UI; clicking the runtime button
emits the round-reset diagnostic. None of those mutations, player movement,
obstacle motion, GUI text/visibility, runtime Players, or default runtime
modules appear in the authoring snapshot.

The same gate saves a unique per-process copy, reopens it from disk, verifies
all required hierarchy/classes and Script source, checks all seven Ready assets and
their dependencies, and asserts every authored GUI `Position` and `Size`
remains a four-component `UDim2`. It also verifies all six authored properties
of the three collectible prompts, including the 0.4-second hold prompt, plus
the animated prompt, its Sound, and the Attachment CFrame/JointPath. The real sample was also closed/recreated and
reopened during authoring. Runtime-only state does not persist.

Run the gate with:

```powershell
ctest --test-dir build-production-checkpoint -C Release -R "^gargantuan_first_complete_game$" --output-on-failure
```

Each test process copies the sample to a unique temporary workspace and removes
only that owned directory, so the gate remains safe under parallel CTest.

## Standalone run (superseded by Packaging Foundation 1)

From the repository root:

```powershell
build-production-checkpoint\gargantuan.exe --project samples\FirstCompleteGame
```

The unchanged project reached `[Game:FirstCompleteGame] Ready 3` outside
Studio, initialized the default player modules, and emitted no game or engine
Error. The Windows SDL GPU backend emitted two warnings that optional validation
layers were unavailable and continued normally. The validation harness stopped
its exact process after observing Ready because the sample deliberately has no
quit menu.

The command above records the original vertical-slice evidence. Standalone
Packaging Foundation 1 supersedes it with a visible Studio package command and
the canonical CLI documented in the sample README. The resulting directory owns
its player, SDL runtime, default modules/font, shaders, canonical assets, and
notices and launches after relocation from an unrelated current working
directory. The ordinary standalone flow no longer requires CMake or build-tree
knowledge after the package has been produced.

## Next three priorities from this evidence

1. **Studio manipulation and property editing.** Add transform gizmos and
   schema-driven compound editors first. Their absence affected nearly every
   authored object and is the largest barrier to a GUI-only workflow.
2. **Packaging and standalone project launch (completed by Standalone Packaging
   Foundation 1).** Studio and CLI share PackageBuilder; Build and Run launches
   the produced package, whose runtime modules, shaders, DLL, and assets are
   executable-relative.
3. **Interaction/proximity semantics and diagnostics (completed by Interaction
   Foundation 1).** `ProximityPrompt` and `InteractionService` replace the
   per-game polling, threshold, hold, semantic input, and cleanup policy.

Audio Foundation 2, Animation Foundation, lighting/material improvements,
debugging/profiling, and GUI Foundation 3 remain candidates, but this slice did
not produce stronger blocking evidence for them than the three items above.
