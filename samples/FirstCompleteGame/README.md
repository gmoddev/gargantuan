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

## Open and run

Open this directory as a project in Gargantuan Studio and use Play/Stop. The
current standalone development invocation is:

```powershell
build-production-checkpoint\gargantuan.exe --project samples\FirstCompleteGame
```

The executable's adjacent `runtime` directory and renderer DLLs are required.
This build-tree coupling is a known packaging gap, not part of the sample.

All gameplay content resolves through `AssetService`. The project imports one
glTF mesh, its material/image dependency graph, and one font; gameplay Luau
does not read source asset files.

See [FirstCompleteGame.md](../../devdocs/Validation/FirstCompleteGame.md) for
the authoring record, automated evidence, and gaps found by this slice.
