# Standalone lifecycle qualification

The integration base is `fb3cc0065`. Engine owns the Player, Server, package formats and session boundaries. Studio owns the Game menu and the outer Play session. Node owns issuance, redemption and managed deployment. The spatial campaign owns the common StarterGui projection and the final spatial recipient.

## Installed Windows evidence

These receipts cover incremental production builds. Each gate is tied to its recorded source and distribution. The final qualification below combines the unchanged implementation scopes with the final fixture corrections.

| Workload | Observed result | Worker receipt |
| --- | --- | --- |
| Minimal application and direct export | The application started before a gameplay session. The same Player launched existing titles directly. A menu-less application entry also entered the direct path. | `Profile-lifecycle_offline_package-Test-Test.log` |
| Repeated replacement | The installed Player retired and replaced 100 offline gameplay sessions in one process. The test took 20.67 seconds. | `StudioFixture-replacements-Test-Test.log` |
| Construction Yard | The package reopened independently named worlds with their separate durable voxel histories. | `YardExact-Test-Test.log` |
| Window input | Three actual menu button activations crossed start, pause and retirement. The renderer sampled displayed colors on the acquired swapchain image. | `PairedPackage-Test-Test.log` |
| Private and remote authority | Two separately installed Players joined a native Server. A private owner acquired a hold immediately after admission. A competing writer was denied and a fresh Player reacquired the selected slot. | `FastHold-Test-Test.log` |
| Shared audio output | WASAPI reported six mixed blocks, six menu-only blocks and 190560 held frames. | `Workstation-device.log` |
| Studio EditorHost | Three application Play runs exercised the normal PlaySessionId, catalog selection and restoring the authoring revision on Stop. The earlier fixture did not check the slot reported by the started world. The revised test requires both world and save identity on all six starts. | `StudioFixture-studio-Test-Test.log` |
| Protected distribution | Six direct package profiles, three application roles and sixteen denials. Installed signed multiplayer executed the owned GUI and its sibling module through join and reconnect. | `SignedPackage04/Qualification.json`, SHA-256 `6cd1d42379b09af5f09c8065474127ca3d36d272fb754d0933d5d3b27b7c651e` |

The managed Node Engine pin is `6f5fdf8c609b8ea5c06482684d74ac11fc87a6ac` (tree `05696731ba6a7a1ae2406ad2c3167a0110cefb71`). Its worker distribution `Engine-6f5fdf8c6-Distribution` has the same 116 file digests as the preceding `Engine-85d1c47a2-Distribution`; that source change only prints the official Node fixture timeline. The separate Node candidate is `b7e146c0d07c900b47b0efa69a693edf1a94bd78`. The later independent review correction changes startup cancellation and private-server retirement; its targeted Windows receipts are recorded below.

## Qualification procedures

Build the standard Player, Server distributions and packager from the candidate and run CTest with `-R gargantuan_lifecycle_` on the native worker. The Construction Yard test creates the project with the sample builder, packages and relocates it, interrupts an owned Player after reopening committed data, then starts a fresh Player. The two-player test also interrupts an owned private Server on supported hosts.

Run `tests/RuntimeDistributionQualification.py --loopback` with the matching packager, Player and Server distributions and a fresh private reports directory. It provisions distinct synthetic Local and Host runtime enrollments for the protected private pair. Reprotection must change the signed Server digest and keep the same world playable. The native launcher accepts the Server's operator-supplied runtime enrollment path through `--local-server-runtime-enrollment`. The enrollment belongs outside the distributed package.

Run `tests/StandaloneLifecycleTiming.py` with the same `--project`, `--packager` and `--runtime` and an unused `--output` JSON file. It measures direct and application frame intervals, graphical and headless operation, and settled process resources after 10, 50 and 100 replacements.

The authored Construction Yard menu has its own Windows test, `gargantuan_lifecycle_yard_menu`. It operates the installed window, places a wall, pauses, resumes, returns to the title and checks that a second Player finds the same wall in the selected world. The spatial session test uses the packaged terrain fixture and a dedicated Server. The Linux storage-full test takes a separate empty, bounded tmpfs as its profile mount and requires the original world to reopen after a real allocation failure.

The integrated paused-view correction comes from spatial commit `59d2a1af57571ffd34be4e6a437abd19dea1d9ca`. On the spatial build, resize and reattachment switched from 3,072 Workspace-red pixels to 3,072 named-region blue pixels. The lifecycle recipient uses the selected region and its terrain whenever a suspended child needs a new full publication. Recipient verification requires the lifecycle binary.

### Independent hosted sessions

An enrolled paired Server, a concrete GNS listen address and a private native invitation channel enable `SessionService:Host({SaveId = Slot.SaveId, MaxPlayers = 4, Discovery = "LAN"})` in a distributed title. The owner awaits the operation before hiding its menu. Guests select an opaque item from `FindServers("LAN")` and pass its ID to `Join({Kind = "Listing", Id = Listing.Id})`. Both owner and guests can open their menus while the Server advances shared time.

The trusted launcher supplies `--local-host-bind` and `--local-host-invitations`. The latter is an inherited Windows duplex message pipe server handle or connected POSIX local datagram descriptor. Frames contain at most 4,096 bytes of UTF-8 JSON. A version 1 Publish frame identifies its LAN session, project, pinned Server manifest, endpoint, expiry, Server public key and one-use guest proofs. The owner emits it after the Server confirms owner readiness. A version 1 Withdraw frame contains only the action, source and same session ID. The launcher removes a listing on withdrawal, owner-process exit or expiry. If a leave fails after draining starts, the owner retains the world for recovery with its listing withdrawn. The ordinary single-player launch needs no invitation channel.

A launcher can also start a distributed Player with `--session-directory-source LAN|Node` and `--session-directory-channel` on a separate private channel. Its version 1 Offer binds the project, exact Player manifest, session ID, endpoint and Server key to a display name and expiration. The Player returns up to 16 names and IDs per page and retains up to 256 active listings. Selecting a listing sends a Request with a new correlation ID. Its Grant must carry that ID and the same package and destination. Reconnect requests a fresh grant for the previous binding. Withdrawal, expiry and channel closure retire availability. An established gameplay connection has its own lifetime.

The browser-to-Node-to-Player gate requires the managed Server distribution, the actual Node worker, a locally verified HTTPS fixture, and a real native admission. The official fixture expects a streamed collection of 15 objects. The qualification below uses that official native path.

## Integration status

Released guest seats receive fresh proofs. Unused seats rotate before their deadlines. The launcher can deliver a Refresh frame to a previously enrolled Player through `--join-refresh-channel`. Its session ID and source must match the original invitation. The Player retains the original endpoint and Server key. The launcher forwards a seat grant only to its intended recipient and expires its local copy. Each independent guest admission receives a new ephemeral identity. A directory or account provider that needs persistent guest identity must use its established authentication provider.

The installed Windows workloads on the November 28 candidate covered the following boundaries.

| Workload | Observed result | Receipt |
| --- | --- | --- |
| LAN across two Windows PCs | The guest joined the staged Server, observed the advancing authority and owned GUI, left and reconnected in the same Player with a fresh grant. The Server denied the original proof on replay. The owner withdrew the original listing and retired its world. The host finished in 9.14 seconds. | `Physical-9770e765ea87/3670e296d2d34e29b0fbdeef1a6a3eb7/Evidence.json` |
| Installed directory client | The launcher supplied 17 entries, including a second page. The test rejected a mismatched project, canceled a pending request, withdrew a pending grant and exercised actual admission and reconnection. | `Full819834-Test-Test.log`, two-player case, 22.88 seconds |
| Protected native shared host | The guest executed the compiled owned GUI, the host withdrew its listing and a reprotected package reopened the same world. | `SignedFinal02/Qualification.json`, SHA-256 `d9fbde03545d3a76e9ea080e534ec5bc3612bdb3faef42413655e9b073c49253` |
| Packaged construction | The authored Yard menu received desktop input on its relocated window and found the committed wall on restart. The separately packaged spatial Yard completed its development-local loopback session in 10.54 seconds. | `Desktop-Yard.log`, `SpatialAuthoredRetry-Test-Test.log` |
| Studio EditorHost | Six started worlds reported the expected save and world IDs across three application Play runs. | `Full819834-Test-Test.log`, Studio case |
| Audio device | WASAPI mixed six combined and six menu-only blocks. The gameplay voice cursor held across 190560 sample frames. | `FinalRuntime-device.log` |

The two-PC test used the installed binaries from tree `9770e765ea877f4b5259f67efd2c9863b778bccd` with a fixed-port option in its Python driver. The scoped inbound rule covered the staged Server executable and one controller address. Both active and persistent rule stores were checked after removal. The Windows test used a short profile root. The earlier long-root attempt failed during host admission. Deployment under long Windows paths remains unqualified.

The spatial workload used versions 17/18 with development-local identity. The authenticated host and guest used the independent authentication contract. Authenticated non-loopback spatial play still needs its own protocol and product qualification. The independent review of the launcher directory found no outstanding defect after corrections to cancellation, identity binding and reconnection.

The Linux Release candidate at tree `b36c0ed5eaf1a63d89526644cfcd269c24e082b8` passed all eight selected native product tests, including two installed Players and the packaged spatial Yard. Its separate 4 MiB tmpfs test exhausted the real profile volume, retained the original world, and reopened it after a fresh-process retry. SQLite returned `UnknownOutcome` for a commit it could not confirm while the volume was full. The test required a recovery identifier and checked that the original world remained durable. It also accepts an explicit `StorageFull` result when rollback is confirmed. The receipts are `Linux-b36c0ed5eaf1/Result.json` and `Linux-b36c0ed5eaf1/StorageFull.log`.

The final desktop window-input case passed on the corrected per-monitor-DPI driver in 3.28 seconds (`Desktop-Window.log`). Studio's shell selected and saved the authored entry, completed two application Play sessions, and restored the authoring snapshot byte-for-byte (`StudioQualification-1eac98876dd4/6533f5c4018b492ca2d7376bbc0aab56/Lifecycle.json`). The independent Studio source is `0446ec1`.

The paired desktop timing receipt is `Timing-fc53f8dd1e5146668b454f7bff2da7a2.json`. Its application runs had windowed title and held-frame P95 intervals of 18.35–18.54 ms, and the maximum application transition frame interval was 19.00 ms. A direct package started without an application entry in both headless and windowed modes. The application package was 2,994 bytes larger. Handles stayed at 542 for the windowed application at cycles 10, 50, and 100. Windowed private bytes rose from 373–377 MB at cycle 10 to 387–392 MB at cycle 100; headless private bytes rose from 23–24 MB to 29–30 MB. These readings establish bounded observations through 100 replacements, without establishing a memory plateau.

The installed Windows lifecycle suite passed 12 of 12 selected tests across its implementation scopes. The first run passed 11; the remaining actual window-input case passed in 3.28 seconds after the test driver selected per-monitor DPI awareness (`Desktop-Window.log`). The authored Yard desktop case passed separately (`Desktop-Yard.log`). Studio selected and saved the authored entry, completed two application Play sessions, and restored its authoring snapshot byte-for-byte (`StudioQualification-1eac98876dd4/6533f5c4018b492ca2d7376bbc0aab56/Lifecycle.json`).

An independent read-only review found two teardown defects. Startup cancellation could discard a pending or uncertain world write; an already exited private Server could bypass its final save status. The correction keeps a cancelled start pending while its local and authoritative writes drain, reports a failed or uncertain result, and checks the child's exit status before retirement. The installed Windows Player, Server and EditorHost were rebuilt and staged. SHA-256 comparison confirmed the staged Player and Server executables match their rebuilt binaries. On those corrected files, `ReviewExit3-Test-Test.log` passed the private two-player case, including an interrupted child that reported `UnknownOutcome` and still allowed the selected world to reopen. `ReviewFinal-Test-Test.log` passed offline cancellation and Studio lifecycle, two of two. The independent patch re-review found no remaining material defect in those two corrections.

The corrected 116-file Windows distribution is `Engine-607f5d9ec-Distribution`, source tree `1be2ca46af08fe3cddc604cdd7369ffdda384173`. Its `SHA256.json` has SHA-256 `0b9ec93b55fc2fdab07c50694de835e88a3a560216df6f125732f8f748cc7f83`; every copied file was checked against its rebuilt source. The targeted Linux build used the same source tree in the retained native toolchain and passed offline, Studio and two-player lifecycle tests, three of three (`LinuxReviewFix-1be2ca46af08/Result.json`, `Exit=0`). The persistence implementation and `StandaloneWorldStorageFull.py` have no source difference from the earlier real ENOSPC candidate, so that full-disk receipt remains valid without rerunning the unaffected scenario.

The managed Node direct-entry official run used the actual Node worker, managed Server distribution, locally verified HTTPS services, 15 streamed content objects and native Player admission. Its final controller receipt `NodeImage/Retry7-2dedb6298f1b42c58ef848750ba3da37/final-controller-private.json` records `RunSucceeded`, `CapacityPassed`, `NativeSessionVerified`, `PlayerJobEmpty` and `ContainerAbsent` as true. The native Server accepted six peers, created and removed six Players, exited with code zero and left no active job processes. Four clients occupied the permitted seats, a fifth was denied, a released seat admitted a fresh client, and the capacity scenario drained.

The application-menu Node package independently completed the official native Player path. Its `NodeImage/Retry6-2dedb6298f1b42c58ef848750ba3da37/player-job-empty-private.json` records Player exit zero, an empty job and the official timeline found. That run's full controller result was false because the capacity launcher exercises the legacy direct-entry `--farm-run-id` path. The combined managed capacity and native-session receipt is therefore the direct-entry package, with application-menu native admission covered by the separate receipt. No single run claims both application-menu entry and managed capacity.

The full-disk recovery gate retains the persistence contract. A confirmed rollback may report `StorageFull`; a commit whose outcome cannot be confirmed reports `UnknownOutcome` with a recovery identifier. The Linux receipt exercised the latter on a full real tmpfs, preserved the original committed world, and reopened it in a fresh process.

Promotion candidates remain isolated in Engine `production/standalone-lifecycle-20261127`, Studio `production/standalone-lifecycle-20261127` at `0446ec1528cc86b52fdf9af9caa5335041eecd0f`, and Node `production/standalone-lifecycle-node` at `b7e146c0d07c900b47b0efa69a693edf1a94bd78`. The long Windows profile-root path and authenticated non-loopback spatial protocol remain outside the qualified deployment scope. No branch is merged or published.

The final review sweep closed three further boundary findings. Studio's application
Play now synchronizes menu text input before deciding whether an event reaches the
gameplay child, and blocks child input during leave. A packaged Player receiving a
native join proof requires the pinned Server key even for a loopback endpoint.
The private Server control thread has cancellable Windows pipe I/O and stop-aware
nonblocking POSIX writes, so a full parent pipe cannot hold its destructor.
The installed Windows `ReviewSweep-Test-Test.log` passed all five affected tests:
offline package, Studio, two Players, viewport input and window input. The Studio
test now explicitly stops an active application world and compares the exact
authoritative in-memory authoring snapshot before and after Play. The two-player
test also denies a guest proof without its Server key. Independent static review
found no material issue in these corrections. The persistence source and real
ENOSPC test remain unchanged from the qualified Linux full-disk candidate.

The managed Node qualification remains scoped to its existing verified receipts.
The full two-session and capacity controller result used a direct-entry package;
the application-menu package independently passed the official native Player
admission path. The latter did not complete the capacity controller, whose farm
launcher uses the direct-entry interface. These receipts establish the separate
paths without claiming one combined application-menu capacity run.
