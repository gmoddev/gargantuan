# Relay Rescue

Restore the relay with two players. One player holds the green ground switch
while another launches a survey drone from the cyan pad and reaches the
elevated beacon. Fly away and return between charges. Restore three charges
to complete the mission, then switch roles to try both controllers.

- **On foot** — WASD moves the stock Character, Space jumps, and right mouse
  orbits the camera. The orange hazard drains health and sends the player
  back to the start when health reaches zero.
- **Drone** — WASD flies the drone, and Space lands and returns the player
  to the stock Character. The drone holds altitude and stops against the
  tall divider.
- **Sound** — the crystal button toggles game audio for your client. The
  beacon plays a positional cue for each charge. The third charge releases
  the orange cargo marker onto the bouncy platform beside the course.

Open this directory as a project in Gargantuan Studio. Local Play previews the
course, on-foot movement, hazard, respawn, and beacon animation. Drone flight
and the cooperative objective run in the dedicated multiplayer session.

## Run with two clients

Use a GNS-enabled Release build of Engine Foundation 8. Run these from the
repository root using the matching build output for the packager and both
runtime distributions.

```powershell
$Build = "build-release"
& "$Build/gargantuan-packager.exe" build --project samples/GameplayFoundation7 --output packages/RelayPlayer --configuration Release
& "$Build/gargantuan-packager.exe" build --project samples/GameplayFoundation7 --output packages/RelayServer --configuration Release --runtime "$Build/ServerRuntimeDistribution"
```

Start the server in one terminal, then run the player command in two terminals.

```powershell
& "packages/RelayServer/GargantuanServer.exe" --bind 127.0.0.1:46000
& "packages/RelayPlayer/GargantuanPlayer.exe" --connect 127.0.0.1:46000
```

The server script owns health, respawn, team, possession, movement, and mission
progress. Each charge awards a relay cell in the contributing Player's inventory
folder. It uses authoritative positions for the launch pad, ground switch,
and relay. The local script submits bounded movement and a land action, follows
the projected drone, and plays a beacon clip whose speed follows the server's
switch state. The drone is an anchored Part moved by a sphere cast and the
public `CharacterControlService:MovePossessedPart` API. The Engine publishes its
pose, and replicated attributes carry mission state.

## Edit the game

Edit [RelayServer.luau](RelayServer.luau) for gameplay and
[RelayClient.luau](RelayClient.luau) for input and presentation. Run
`python samples/GameplayFoundation7/BuildProject.py` to embed their current
sources in the saved project. Pass `--check` to verify the saved snapshot.
The beacon uses Gargantuan's existing FirstCompleteGame two-joint rig and
canonical animation artifacts copied into this sample's own catalog, so the
project opens independently. The client also uses the original sample badge
and tone. See [asset provenance](../FirstCompleteGame/assets/NOTICE.md).
