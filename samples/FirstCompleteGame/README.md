# First Complete Game

This is Gargantuan's first deliberately end-to-end game sample. It is a small
third-person collection course, not a showcase: collect three cyan shards,
avoid the moving obstacle, and use the on-screen button to restart after the
win panel appears.

The second collectible requires an unobstructed rigid-physics line of sight.
Its prompt exercises the same general `Workspace:Raycast` boundary available
to gameplay rather than a sample-specific visibility shortcut.

## Controls

- `W`, `A`, `S`, `D`: move through the default `ActionMap` and
  `DefaultPlayerRuntime` stack.
- Mouse/right mouse: default third-person camera controls.
- Space: default jump action.
- `E` or gamepad South / `A`: activate the nearest visible collectible prompt.
- Touch/click the prompt panel: use the same validated interaction path.
- `K`: deterministic completion action used by the automated gate.
- **Restart round**: resets collectibles, GUI state, and the runtime character.

## Open, package, and run

Open this directory as a project in Gargantuan Studio and use Play/Stop. To test
the actual standalone boundary, use **File > Package Game...**, choose Release,
and select **Build and Run**.

The same canonical PackageBuilder is available to CI and terminal workflows:

```powershell
New-Item -ItemType Directory -Force packages | Out-Null
build-production-checkpoint\gargantuan-packager.exe build `
    --project samples\FirstCompleteGame `
    --output packages\FirstCompleteGame `
    --configuration Release
build-production-checkpoint\gargantuan-packager.exe validate packages\FirstCompleteGame
packages\FirstCompleteGame\GargantuanPlayer.exe
```

After creation, only the output directory is needed. It may be moved, and the
player may be launched from any current working directory. Runtime modules,
shaders, SDL, canonical assets, and notices are already inside the package; CMake,
the repository, Studio, and asset source files are not runtime dependencies.

All gameplay content resolves through `AssetService`. The project imports a
static glTF mesh with its material/image dependency graph, a two-bone skinned
glTF mesh with a looping `BeaconPulse` Animation, one font, and one PCM16 WAV.
Each collectible plays the clip positionally while round completion plays it
as a non-positional UI sound. Gameplay Luau does not read source asset files,
and hearing the cues is never required for game correctness.

`AnimatedBeacon` authors an `Animator` directly under its `MeshPart`. The
`BeaconAnimation` gameplay script loads the canonical Animation asset, enables
looping, and starts it in Play. Stopping Play discards the `AnimationTrack` and
pose while leaving the authored rig, Animator, script reference, and canonical
package artifacts unchanged.

On a graphical renderer with GPU-skinning capability, the canonical skinned
source mesh is uploaded once and `AnimatedBeacon` advances by bounded palette
uploads; it does not publish a CPU-deformed vertex array every frame. Headless
Play evaluates the same track and publishes its semantic palette without
initializing SDL video or deforming the full mesh. Unsupported graphical
backends use the retained CPU reference/fallback path without a gameplay API
difference.

The saved fixture also authors the canonical `Lighting` service with afternoon
sun, ambient light, exposure, bounded fog, and a direct six-face `Sky`. All six
faces reuse the packaged square Collection Badge Image so save/reopen, local
Play, headless publication, renderer restart, and package inspection exercise
the complete environment resource path without adding another source asset.

See [FirstCompleteGame.md](../../devdocs/Validation/FirstCompleteGame.md) for
the authoring record, automated evidence, and gaps found by this slice.
