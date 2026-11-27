# Terrain Quarry

Follow the quarry road from the western landing to the branching cave and
east survey station. Two players can excavate the old trench, fill the east
bank, and paint its survey mark. A loose stone rests on the same native
terrain collider as the Characters.

## Build the editable project

Use a GNS-enabled Engine build with the packager and both runtime
distributions. Run the commands from the repository root. The project
destination must be new and its parent directory must exist.

```powershell
$Build = "build-release"
$Project = "TerrainQuarryProject"
python samples/TerrainQuarry/BuildProject.py --engine "$Build/gargantuan.exe" --project $Project
& "$Build/gargantuan-packager.exe" build --project $Project --output QuarryServer --runtime "$Build/ServerRuntimeDistribution" --configuration Development
& "$Build/gargantuan-packager.exe" build --project $Project --output QuarryPlayer --runtime "$Build/RuntimeDistribution" --configuration Development
```

The builder creates a project through local EditorHost, applies bounded
brushes, installs the two ordinary Luau scripts, and saves the resulting
terrain. It reopens the project and tests the spawn collider. Open the
generated project in Studio to adjust the field with Terrain tools or
save a GASP v2 place. The source recipe lives in
[`BuildProject.py`](BuildProject.py).

Add `--mixed-lod` when building the project to exercise camera-driven
visual detail. Draws return to authored detail as the camera approaches.
Both visual modes use the same collider.

## Play with two clients

Start the dedicated server, then run the player command twice for a private
loopback Development session.

```powershell
& ".\QuarryServer\GargantuanServer.exe" --bind 127.0.0.1:46010
& ".\QuarryPlayer\GargantuanPlayer.exe" --connect 127.0.0.1:46010
```

Walk east with WASD and press E near each station, in order. The stations
are at X=25, X=58, and X=90. Read the gameplay output for station progress.
The server verifies the Character's position and accepts one work order
at a time. Each completed order changes the authoritative field, produces
a collision-ready receipt, and advances both players' quarry progress.
Reconnect to the same server to continue from its current phase.

The server can persist completed work orders using the Engine's local
SQLite provider. Use a build with `GARGANTUAN_WITH_LOCAL_SQLITE=ON` and
give the server a task-owned database path.

```powershell
& ".\QuarryServer\GargantuanServer.exe" --bind 127.0.0.1:46010 --persistence-provider sqlite --persistence-local-db "./quarry.sqlite"
```

The server stores the versioned quarry phase and replays its fixed sequence
of edits on the authored terrain at restart. The saved project stays the
baseline for that replay. The default provider gives each server process a
fresh quarry. The sample uses a single shared development world key.

## Repeat the headless qualification

```powershell
python samples/TerrainQuarry/Qualify.py --build $Build --output QuarryEvidence
```

The verifier creates a separate project with a scripted route, builds both
packages, and runs two concurrent players plus a reconnect over real GNS.
It records each server sample and compares the resulting collision on the
three client sessions. An SQLite-enabled build also accepts `--sqlite` to
check the same terrain changes after a clean server restart.

[Network profile](../../docs/adr/TerrainNetworkBootstrap.md) ·
[Campaign evidence](../../devdocs/Validation/TerrainCampaign.md)
