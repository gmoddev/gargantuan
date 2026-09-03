# First Complete Game

This is Gargantuan's first deliberately end-to-end game sample. It is a small
third-person collection course, not a showcase: collect three cyan shards,
avoid the moving obstacle, and use the on-screen button to restart after the
win panel appears.

Character Networking Foundation 3F leaves this sample's gameplay and package
surface unchanged. `BeaconLunge` remains a reliable semantic action, owner
corrections remain full-rate, and adaptive cadence applies only to already
relevant remote Characters. Network tier policy is intentionally not exposed
to sample Luau.

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
glTF mesh with looping `BeaconPulse` and one-shot `BeaconLunge` Animations, one
font, and one PCM16 WAV.
Each collectible plays the clip positionally while round completion plays it
as a non-positional UI sound. Gameplay Luau does not read source asset files,
and hearing the cues is never required for game correctness.

`AnimatedBeacon` authors an `Animator` directly under its `MeshPart`. The
`BeaconAnimation` gameplay script loads the canonical Animation asset, enables
looping, and starts it in Play. `BeaconTipAnchor` binds the canonical
`BeaconRoot/BeaconTip` joint path with a local socket offset. Its quiet looping
positional `BeaconTipSound` and small `BeaconTipPrompt` use that same animated
world position; no script copies a bone position into either consumer. The
prompt deliberately has a tiny activation range so it demonstrates the moving
semantic endpoint without competing with the collectible interaction. Stopping
Play discards the `AnimationTrack` and transient `WorldCFrame` while leaving the
authored CFrame/JointPath, rig, Animator, script reference, and canonical
package artifacts unchanged.

On a graphical renderer with GPU-skinning capability, the canonical skinned
source mesh is uploaded once and `AnimatedBeacon` advances by bounded palette
uploads; it does not publish a CPU-deformed vertex array every frame. Headless
Play evaluates the same track and publishes its semantic palette without
initializing SDL video or deforming the full mesh. Unsupported graphical
backends use the retained CPU reference/fallback path without a gameplay API
difference. The packaged GPU gate compares the semantic socket against the
current joint-model transform and GPU palette oracle, and renderer replacement
does not reset the socket or track.

`RootMotionShowcase` is ordinary persisted game Luau authored through the
generic EditorHost Script path. At Play it composes two NPC
`KinematicCharacter` Instances from the same skinned Mesh and `BeaconLunge`
clip, opts each track into `RootMotionEnabled`, and assigns a descendant
`RootPart`. The open NPC completes its lunge, while a rigid authored-at-runtime
barrier clips the second NPC through the same Character/capsule authority.
Neither NPC has a Player or Humanoid. Headless validation observes roughly
0.50 m versus 0.155 m during its fixed observation interval; the packaged
Vulkan proof independently observes full-versus-clipped displacement while GPU
palette animation continues. The requested clip delta never writes either
Character CFrame directly.

The headless Character-network acceptance gate reads this sample's actual
`BeaconLunge` AssetId and immutable catalog content revision, registers that
known action on a simulated server and client, requests it by semantic token,
derives its root movement on the server, and reconciles the predicted Character.
The client never sends CFrame or root displacement. This network proof uses the
production Character codec/scheduler over deterministic simulated transport;
the optional localhost gate repeats the same bind/input/action/state path over
GameNetworkingSockets and verifies disconnect cleanup. The package gate overlays
role-specific proof Scripts onto a temporary copy, then exercises the same
default input, movement, action presentation, remote-NPC, camera (graphical),
and teardown path in separate packaged processes without changing this saved
fixture. No sample-only RemoteEvent or Humanoid is involved.

The saved fixture also authors the canonical `Lighting` service with afternoon
sun, ambient light, exposure, bounded fog, and a direct six-face `Sky`. All six
faces reuse the packaged square Collection Badge Image so save/reopen, local
Play, headless publication, renderer restart, and package inspection exercise
the complete environment resource path without adding another source asset.

See [FirstCompleteGame.md](../../devdocs/Validation/FirstCompleteGame.md) for
the authoring record, automated evidence, and gaps found by this slice.
