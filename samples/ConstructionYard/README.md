# Construction Yard

Build a retaining wall alongside sculpted ground, excavate underneath it, and step into a workshop at the same coordinates. The yard uses one Workspace terrain field, a native BlockGrid for each construction and ordinary Parts for survey markers and a loose stone. Its world is editable in Studio.

With a Windows or Linux Engine built from the spatial construction candidate, run the following command from the repository root. Choose an unused destination directory.

```sh
python samples/ConstructionYard/BuildProject.py --engine /path/to/Engine --project /path/to/ConstructionYard
```

The builder uses EditorHost to place the grids and apply terrain brushes. It saves, reopens the project and checks cells in both domains. Open that project in Studio and press Play, or build a trusted Offline Player package.

| Key | Action |
| --- | --- |
| B | Add one brick to the next course of the current construction. |
| E | Excavate the outdoor terrain just ahead of the character. |
| F | At the doorway beside the spawn, enter the workshop or return to the yard. The camera follows the character after movement completes. |

The amber and blue markers use the same local position. The wall and workshop floors live in different physics worlds. Place and remove cells in the Studio grid inspector, then save and reopen to keep authored changes. Play uses a separate world, so Stop restores the edit scene.

The Yard server Script chooses the destination and landing position. It calls `Player:RequestSpatialTransfer` for both directions and updates the player's view on `Applied`. In a managed spatial multiplayer session the client sends the action name, the server resolves the sender and the controlling replica installs the destination collision before moving. The outdoor stone is anchored in multiplayer so a returning player can reserve the yard's collision. In offline Play the stone falls under gravity.

For player-built courses and excavations to survive a fresh Play session, select Persistent Play storage on a build with local SQLite support. The Yard script stores up to 128 edits in a versioned document under `ConstructionYard/YardV1`. On startup it replays those edits over the authored scene and waits for excavation collision to commit. An already applied edit is part of the stored prefix. Concurrent writers use the document revision, and a conflict reloads the authoritative document. With disabled storage, changes last for the current session.

## Package a multiplayer Yard

The builder writes the common session identity and shared-asset policy under the project's `.gargantuan` directory. Use the Engine packager from the same build as the Player and Server runtimes. From the repository root, replace the project and runtime paths below and choose fresh output directories.

```sh
gargantuan-packager build --project /path/to/ConstructionYard --runtime /path/to/ServerRuntimeDistribution --output /path/to/YardServer --configuration Development --distribution samples/ConstructionYard/DedicatedServer.json
gargantuan-packager build --project /path/to/ConstructionYard --runtime /path/to/RuntimeDistribution --output /path/to/YardPlayer --configuration Development --distribution samples/ConstructionYard/DistributedPlayer.json
gargantuan-packager validate /path/to/YardServer
gargantuan-packager validate /path/to/YardPlayer
```

Run `GargantuanServer --bind 127.0.0.1:36790 --persistence-provider sqlite --persistence-local-db /absolute/path/yard.sqlite` from the Server output and `GargantuanPlayer --connect 127.0.0.1:36790` from the Player output for each local player. Use the `.exe` executables on Windows. Reuse the database and project identity when restarting. Two players can extend the same wall or dig at their own feet. The server queues up to 16 pending edits, casts against the outdoor terrain before recording an excavation and retries a temporarily busy read within a bounded startup window.
