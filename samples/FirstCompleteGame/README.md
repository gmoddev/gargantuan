# First Complete Game

This is Gargantuan's first deliberately end-to-end game sample. It is a small
third-person collection course, not a showcase: collect three cyan shards,
avoid the moving obstacle, and use the on-screen button to restart after the
win panel appears.

## Controls

- `W`, `A`, `S`, `D`: move through the default `ActionMap` and
  `DefaultPlayerRuntime` stack.
- Mouse/right mouse: default third-person camera controls.
- Space: default jump action.
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

All gameplay content resolves through `AssetService`. The project imports one
glTF mesh, its material/image dependency graph, and one font; gameplay Luau
does not read source asset files.

See [FirstCompleteGame.md](../../devdocs/Validation/FirstCompleteGame.md) for
the authoring record, automated evidence, and gaps found by this slice.
