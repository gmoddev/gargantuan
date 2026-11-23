# Relay Rescue GPU journey

The saved two-player Relay Rescue mission can run through the production
Player's SDL GPU renderer. The dedicated server and two distinct network
client Engines share one test process and use real GNS loopback. Each client
renders into a 320 × 240 offscreen target and waits on its GPU submission
fence. The fixture opens no desktop windows.

Use a Windows x64 Release Ninja build with `GARGANTUAN_WITH_GNS=ON`, normal
shader generation and a working Vulkan graphics driver. Use the build
directory that holds the executable, shaders and runtime libraries.

```powershell
$Build = 'build-release'
cmake --build $Build --parallel 6 --target gargantuan_gameplay_sample_tests gargantuan_shaders
Remove-Item Env:SDL_VIDEODRIVER -ErrorAction SilentlyContinue
& (Join-Path $Build 'gargantuan_gameplay_sample_tests.exe') --gns-gpu
```

The no-argument and `--gns` tests retain their headless behavior. The explicit
GPU case adds authoritative stock movement through a host key event, drone
movement and retirement in both clients' render publications, and the pilot's
published follow camera. The existing mission still exercises the saved
Luau controller, divider collision, two-player cooperation, inventory
ownership, landing, death, respawn, and disconnect. GPU gates require
completed submissions, drawing, HUD batches and GPU skin palette uploads.

Each client emits one bounded `[Gameplay:RelayGpu]` record. The interval,
submission and GPU fence-wait distributions include startup and mission
bursts. The interval is between starts of a single client's draw calls during
the shared test loop. Submission and fence values are CPU-clock measurements
from the SDL backend. The loop includes a deliberate sleep and serial peer
steps. Swapchain presentation, OS input latency, independently paced clients,
and physical multi-host traffic need separate qualification.

The packaged GNS smoke in
[`tests/PackagedGameSession.ps1`](../../tests/PackagedGameSession.ps1) also
accepts `-GraphicalClient ON` on a GPU-enabled interactive test desktop.
That uses the Player host's native window and proves a separate cross-process
First Complete Game journey.
