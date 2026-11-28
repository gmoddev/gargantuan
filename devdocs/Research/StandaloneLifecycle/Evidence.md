# Experimental evidence and independent review

## What ran

Tests used private temporary outputs, headless Engines, dummy audio and loopback. The checks below were executed for this investigation. Historical qualification of GUI, networking, BlockGrid and Studio is indexed separately in [the inventory](Inventory.md). Production API names appear only in the proposal. No native binding was installed by these tests.

| Question | Executed witness | Result and boundary |
| --- | --- | --- |
| Can Gargantuan run an interactive menu before a Player exists? | Construct one native DataModel, ScriptEngine and GuiRuntime, execute a client-context Script that creates a ScreenGui/TextButton, reconcile layout and drive pointer down/up through native routing. No gameplay Engine or Server exists yet. | First click changed Luau button text to 1; graph contained zero Players. The graph uses a legacy local GUI root for this experiment. Restricted app authority and capture projection still require production implementation. |
| Can the same menu interact during a parked world? | Build a full offline Engine with its normal LocalPlayer/PlayerGui, hide the title, attempt a click, then show it during pause. Service the app VM and route real GuiRuntime pointer events while the gameplay Engine receives zero steps. | The hidden click had no effect. The visible click and application timer ran. A falling physics body retained its exact parked height, and simulation ordinal held constant. |
| Can resume exclude a long pause from dt and waits? | Inject independent clocks at Engine/ThreadEngine time observation. Schedule a gameplay 125 ms delay and wait. Advance simulation 32 ms, application 60,000 ms, then simulation 16 ms and 80 ms. | Both cycles sampled 16 ms on the first resumed PreSimulation. The timer had not fired at that point. At 128 ms eligible elapsed time it fired exactly once, the wait reported 128 ms and the part had moved downward again. These are logical times sampled by real native callbacks. |
| What survives replacement and failure? | Retire two real offline Engines/worlds and their VMs in succession beneath the same native menu. Restore menu visibility after leave; compare Player lifetime and Script coroutine cleanup. Reject a NetworkClient Engine built with no trusted LocalPlayer. | Two fresh Players and owner GUIs. Old Player destroyed and managed thread released at each retirement. After the failed client construction, the original app button accepted its fourth click. Session handoff is coordinated by the fixture; GPU device reuse and startup-failure UI were not exercised. |
| Does direct startup still exist? | Launch an existing relocated installed offline package through GargantuanPlayer --headless --startup-smoke. | Exit 0. This is a baseline witness for a future compatibility test. |
| Does multiplayer exercise the real host boundaries? | Attempt server bind on packaged Player, then bring up a separate packaged Server on loopback. Join twice with separate ordinary client processes. Stop Server and attempt a third join. | Player rejects bind with exit 2. Each valid client installed verified terrain before readiness and returned 0. Server admitted both and exited 0. Connection to the stopped server returned 7. Fresh processes demonstrate host admission/reentry; the ordinary product still exits after a failed join. |
| Can remote authority and cancellation be expressed independently? | Execute a strict Luau policy model with typed caller classes, separate simulation/service clocks, two owners, cancelled attempt and stale objects/generation. | Remote pause and remote release denied. One holder could not release another. Service advanced 60 while simulation stayed fixed. Cancelled ready callback and old-generation pause both rejected. This models a proposed command; no multiplayer pause wire protocol exists at the source pin. |

### Native probe provenance and reproduction

[NativeProbe.cpp](NativeProbe.cpp) uses Engine's own Luau, GUI, pointer hit testing, physics, task scheduler and offline Player. [BuildProbe.py](BuildProbe.py) links against a retained Windows MSVC/Ninja build. It explicitly links schema registration objects. For older builds it compiles scratch copies of Engine.cpp and ThreadEngine.cpp with **two clock observation points** redirected to the test clock. The production source pin already exposes `Engine::SetDevelopmentClock` and `ThreadEngine::SetDevelopmentClock`; the probe selects those on compatible newer builds. Only the retained build path below was executed.

The local retained build's source checkout is based on `5268935242ae968ce673108ed38f9fdcb3ee3919` with existing terrain changes. Its Engine.cpp source SHA-256 was `f73c7d65c65c2498f0651f4765392f1a863ae3c638663249904a947af29b3fdc`; ThreadEngine.cpp was `fb081df99072e4c0085fbd69dd6fe026634cc5544bd56df524b363a0f7ea0118`. The ordered aggregate digest of the 334 reused core object file digests was `32d77c067921fb03642d4e4ed86cfc6036e01f8ad9e76bec751c321c5b7e21c5`. The successful executable's SHA-256 was `c7f30ef8e41041726ca62fac128e3bad6e1c8415dbb8a8e3b4a02f33eff2b0cc`. The linker emitted LNK4098 about default CRT libraries. The probe exited 0. This receipt is bounded component evidence and does not qualify the current spatial branch as a distributed product.

From an x64 MSVC developer shell, in this research directory, use a new empty output directory:

```powershell
py -3 BuildProbe.py --build C:/Sandbox/Codex/Builds/TerrainFoundation1/Windows-MSVC-Release --scratch C:/Sandbox/Codex/Temp/StandaloneLifecycleRecheck
luau-analyze LifecycleTypes.luau AuthorityProbe.luau
luau AuthorityProbe.luau
```

The remote worker's sustained-build slot was held by spatial qualification. Verification used one local probe target and existing native objects. No clean Engine build, worker checkout edits or running server were required after the test.

```text
[Lifecycle:Probe] pregame_click=1 players=0
[Lifecycle:Probe] resume_sample_ms=16 tick=3 timer=0
[Lifecycle:Probe] cycle=1 parked_app_ms=60000 resumed_dt_ms=16 wait_elapsed_ms=128 ran=1
[Lifecycle:Probe] resume_sample_ms=16 tick=3 timer=0
[Lifecycle:Probe] cycle=2 parked_app_ms=60000 resumed_dt_ms=16 wait_elapsed_ms=128 ran=1
[Lifecycle:Probe] retired_worlds=2 unbound_client=rejected recovered_click=4
[Lifecycle:Authority] remote_denied=true control_alive=true nested_release=true stale_ready_and_pause_rejected=true
```

### Packaged products

[PackageProbe.py](PackageProbe.py) copies three retained recipient products into a temporary directory and checks normal executable entry points, role denial, readiness and terminal codes. The bind address is loopback-only and both successful clients connect to the same owned Server. It takes the following package directories on the research machine:

```powershell
py -3 PackageProbe.py `
  --offline "C:/Sandbox/Codex/Artifacts/EngineReleaseLicensing-20261127/QualifiedFinal/Work/Relocated recipients/Player" `
  --client "C:/Sandbox/Codex/Artifacts/TerrainPackageClient/20261127-010805/client" `
  --server "C:/Sandbox/Codex/Artifacts/TerrainPackageClient/20261127-010805/server"
```

| Executable | SHA-256 |
| --- | --- |
| Offline Player | `9ad6049676e70be812a716d7b7c6520784d0147d856fe0ae558baea48f3f60d8` |
| Development terrain Player | `e8f196365c1dbb09365ccf2acc0700fd6a513ce07760c2c68acd93ffc86bc83f` |
| Development terrain Server | `1596aa5bec55334c9ac8577c0074389c2d2478a46a542ab72c0223716b5b0083` |

### What remains to demonstrate

The local Luau analyzer accepted the candidate type module and authority probe. The five Luau blocks in the four example workflows compiled. The examples reference proposed bindings and have no product execution receipt. Every relative document/source link in the research packet resolved at the pinned checkout.

A packaged graphical app loop, persistent app/device composition through session replacement, authenticated private-server pause, remote observation of global pause, audio cursor behavior, interleaved raw/GUI input, slot crash recovery, real LAN or Node directory, menu authoring in installed Studio and graphical performance still require implementation and qualification. The native probe's clock is driven by the fixture. The Luau [authority probe](AuthorityProbe.luau) is a deterministic counterexample test for specified policy, independent of the native GameSession tests. Neither claims wire or access-control enforcement for a new method.

## Independent review

A separate reviewer first traced the selected Engine and divergent Node/hosting sources without drafting this design. After the design, typed surface and examples were written, the reviewer inspected those artifacts and the probes. The following findings changed the proposed contracts.

| Finding | Disposition |
| --- | --- |
| PlayerHost rejects incompatible offline/distributed combinations and spatial non-loopback combinations. A naïve browser could expose server-private material. | Explicit installed audience/profile checks before loading app dependencies or starting a host. Keep secure hosted admission on its separately qualified implementation line. |
| StarterGui duplicates eligible roots per owner. Networked startup waits for LocalPlayer. | Designate a separate client-public presentation closure, reuse native GUI, and verify zero fake Players at entry. |
| Engine::Destroy also destroys a borrowed renderer. A surviving menu cannot reuse that device by accident. | Assign the window/device to Player, give each session a detachable publication adapter, test reset/texture identities and input receipts. |
| Signal Wait/Connect and task.spawn can bypass an isolated timer gate. Raw input arrives at UserInputService before GUI consumption. | Gate every native entry by lane/owner; suppress gameplay before raw action and camera dispatch. Account for held keys, polling and independent input owners. |
| Absolute interaction timers, native network handshake deadlines and buffered audio do not share one safe pause delta. | Separate service ordinal, sim clock, app clock, voice cursors and deadlines. Add exact resume tests and independent control servicing. |
| Menu camera must belong to its ViewportFrame subtree. Grid transfer/collision receipts can change between arrival and installation. | Validate reference closure at capture, recheck native domain membership and readiness generation at use. Reuse existing grid persistence and installed collision contracts. |
| Failed graceful Leave, uncertain Create, and a headless display selector had inconsistent API outcomes. | Restore Active/Ready and the previous input/simulation claims after a confirmed graceful failure. Resolve uncertain catalog operations by durable RecoveryId. GetModes returns a tagged result. Forced shutdown records last confirmed storage. |
| SaveId privacy and invoking app methods from a replicated Script were underspecified. | Remote snapshots have nil SaveId; WorldId is opt-in public data. Gate menu/session/preferences/quit against the local app execution. Headless and ordinary world scripts receive only their defined authority. |
| Research proof could be mistaken for a packaged feature. | Record native component results, actual packaged outcomes and modeled pause authority separately. Make graphical integration, performance and real global-pause protocol explicit gates. |
