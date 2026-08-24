# First complete game validation

Status: validated locally on 2026-08-24. This document records a vertical-slice
falsification exercise; it does not claim that Gargantuan is generally usable.

## Game design and scope

`samples/FirstCompleteGame` is a small third-person collection/obstacle game.
The default player spawns over an authored floor, three cyan Parts act as
collectibles, an anchored obstacle moves across the course, and an imported
textured MeshPart marks the far end. Ordinary client Luau owns proximity
collection, round state, obstacle motion, completion, and reset. A `ScreenGui`
shows an imported badge, imported-font progress text, a completion panel, and a
real `TextButton` restart action.

The deterministic `K` action completes the round for automation. It is a test
hook implemented through the public `ActionMap`, not a native gameplay bypass.
Networking, combat, audio, animation, VFX, and soft bodies were intentionally
not added.

## Authoring workflow used

1. Build the current Studio and Engine Release binaries.
2. Create the project with Studio's `CreateProject` workflow (the automated
   equivalent of File -> New Project), then reopen the created project.
3. Import `assets/collection-beacon.gltf` and
   `assets/Roboto-Regular.ttf` through the same EditorHost asset operations
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
- `ActionMap`, `DefaultPlayerController`, `DefaultCamera`, and
  `DefaultPlayerRuntime`;
- kinematic player motion, physics stepping, camera, and renderer publication;
- ordinary Luau scripts, services, signals, cleanup, and structured output;
- `ScreenGui`, `Frame`, `ImageLabel`, `TextLabel`, `TextButton`, layout, input,
  activation, visibility, and viewport resize;
- isolated Play/Stop, GUI/runtime mutation discard, and standalone execution.

The persisted project has 23 Instances, 9 GUI Instances, and 4 imported asset
records. A representative headless runtime composition step is approximately
0.1 ms on the validation machine; this is not a frame-time benchmark. A local
real-GPU cold offscreen capture was approximately 250 ms, including renderer
and GPU initialization. Real SDL GPU capture remains in the separate viewport
smoke and is intentionally not part of headless CTest. The game gate observed
no Error diagnostics.

## Native engine changes required

Two composition defects were exposed and fixed.

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

## Studio and API findings

Findings use the requested A-F categories.

| Category | Evidence |
| --- | --- |
| A - missing feature | No semantic proximity/interact primitive is available, so Luau polls character/item distance during `PreSimulation`. There is also no packaged project launcher. |
| B - API ergonomics | Basic interaction requires manual distance thresholds, per-item state, and lifecycle cleanup. Asset references are durable but difficult to assign without copying opaque `asset://` values. |
| C - Studio UX | No transform gizmos or compound property editors; limited scalar Properties support; no bridge tools for Assets or Play; one hidden relaunch failed to establish a project session; no Run Standalone command. |
| D - documentation/discoverability | A developer currently needs CMake/build-tree knowledge, adjacent runtime modules/DLL knowledge, and internal awareness of asset references to run the project outside Studio. |
| E - architecture defect | `UDim2` persistence loss and the split Play-viewport resource publication were real cross-system defects. The gate covers `UDim2` and coherent runtime world/UI publication; the separate real-GPU viewport smoke covers device consumption. |
| F - game-specific need | The course geometry, crystal art, obstacle tuning, counter copy, and win-panel styling do not justify engine work. |

Audio and animation would improve presentation, but neither blocked this game's
complete loop. They therefore rank below the workflow and interaction evidence
found here.

## Asset evidence

The four project records reopen as `Ready`: Mesh, Material, Image, and Font.
The Mesh depends on Material, Material depends on Image, and the authored
`MeshPart`, `ImageLabel`, and `TextLabel` resolve only `asset://` references.
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
all required hierarchy/classes and Script source, checks four Ready assets and
their dependencies, and asserts every authored GUI `Position` and `Size`
remains a four-component `UDim2`. The real sample was also closed/recreated and
reopened during authoring. Runtime-only state does not persist.

Run the gate with:

```powershell
ctest --test-dir build-production-checkpoint -C Release -R "^gargantuan_first_complete_game$" --output-on-failure
```

Each test process copies the sample to a unique temporary workspace and removes
only that owned directory, so the gate remains safe under parallel CTest.

## Standalone run

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

This is a development invocation, not distribution. It requires a compatible
Release executable plus its adjacent `runtime` modules, SDL runtime, and shader
products. Requiring CMake/build-tree knowledge is the packaging gap; a full
packager is outside this task.

## Next three priorities from this evidence

1. **Studio manipulation and property editing.** Add transform gizmos and
   schema-driven compound editors first. Their absence affected nearly every
   authored object and is the largest barrier to a GUI-only workflow.
2. **Packaging and standalone project launch.** Provide an installed/dev
   launcher that finds runtime modules, shaders, and DLLs without exposing the
   CMake tree, plus a Studio Run Standalone action.
3. **Interaction/proximity semantics and diagnostics.** Add the smallest
   public proximity/interact primitive with clear Studio/Luau diagnostics so
   ordinary games do not each rebuild polling, threshold, and cleanup policy.

Audio Foundation, Animation Foundation, lighting/material improvements,
debugging/profiling, and GUI Foundation 3 remain candidates, but this slice did
not produce stronger blocking evidence for them than the three items above.
