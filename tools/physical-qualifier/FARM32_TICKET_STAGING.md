# Farm32 private ticket staging

`farm_ticket_staging.py` prepares fixed one-run Agent Coordinator inputs. It
does not connect to another host, launch a process, start capture, or promote a
physical acceptance gate. The host and role launchers still verify every pin.

1. On the controlling PC, create a new private directory outside source,
   packages, evidence, capture, and registries:

   ```powershell
   python tools/physical-qualifier/farm_ticket_staging.py new C:\Users\aiden\.codex\private-farm-runs\<fresh-name>
   ```

   Keep its Windows ACL restricted to the current user, Administrators, and
   SYSTEM. `identity.json` contains the new canonical `RunId`, distinct
   `CoordinatorRunId`, and two independent 256-bit tokens. Never publish this
   directory or copy it into a package, evidence root, issue, or PR.

2. Pass that `RunId` to `NewPhysicalGameSessionFarmManifest.ps1` for a fresh,
   byte-identical Server/Clients manifest. Run each endpoint's ordinary
   inventory preflight for the same manifest and collect the two fresh reports
   into the controlling PC's private staging area. This step must happen close
   to execution: preflight is valid for only 120 seconds.

3. Prepare a closed-schema JSON spec and seal the private root:

   ```powershell
   python tools/physical-qualifier/farm_ticket_staging.py seal C:\Users\aiden\.codex\private-farm-runs\<fresh-name> C:\Users\aiden\.codex\private-farm-runs\<fresh-name>-spec.json
   ```

   The spec has exactly `Format: GargantuanFarm32TicketSpec`, `Version: 1`,
   `ManifestSource`, `Host`, and `Roles`. `Host` has exactly `HostIp` (IPv4),
   `Port`, `JournalRoot`, and `ListeningPath`. `Roles` has exactly `SERVER` and
   `CLIENT`. Each role has these fields:

   | Field group | Required fields |
   | --- | --- |
   | Identity | `EndpointId`, `PeerIp` |
   | Source input | `ManifestSource`, `PreflightSource` |
   | Private endpoint stage | `StageRoot`, `ManifestPath`, `PreflightPath`, `FarmConfigPath`, `CaptureConfigPath`, `TicketPath`, `WorkflowPath`, `CaptureControllerPath` |
   | Runtime outputs and fixed roots | `JournalRoot`, `ResultPath`, `PackageRoot`, `EvidenceRoot`, `RunRegistryRoot`, `CaptureRoot` |
   | Installed pins | `PowerShell`, `Supervisor`, `CaptureEngine` |

   Each installed pin is `{ "Path": "C:\\...", "Sha256": "64 lowercase hex" }`.
   `PowerShell` must name `pwsh.exe`; `Supervisor` must name
   `PhysicalGameSessionFarmEndpoint.ps1`. The `SERVER` capture engine has
   `Service` (`AgentCoordinator.CaptureFarm32Service.exe`) and `Hook`
   (`CaptureFarm32.ps1`) pins. The `CLIENT` engine has `CaptureScript`
   (`DumpcapFarm32Capture.ps1`) and `Dumpcap` (`dumpcap.exe`) pins. A Node
   manifest additionally requires `NodeRootCertificatePath` on both roles;
   each endpoint validates its root-certificate hash against the manifest.
   Source manifests must have identical **bytes**. The private
   stager validates the fresh inventory report and identity before writing
   tickets; the endpoint then independently checks installed pins and paths.

4. `copy-plan.json` lists only fixed source file, endpoint, destination, and
   SHA-256 tuples. Stage those files into pre-created endpoint-local private
   `StageRoot` directories using an explicitly reviewed fixed SCP file-copy
   operation; do not turn the plan into a remote shell command. Preserve file
   bytes, verify each destination hash locally on that endpoint, and keep the
   directory ACL private. The plan's state is `FILES_ONLY_UNEXECUTED`; it is
   not a deployment or qualification receipt. The host config remains in the
   controlling PC's private root and points at its byte-pinned local copies of
   both role tickets. The `SERVER` role ticket copy goes to the worker, and the
   `CLIENT` role ticket copy stays on the controlling PC.

Any stale report, changed manifest, altered pin, reused private root, path
escape, extra schema field, or duplicate destination fails closed. No command
or executable path arrives from the coordinator wire. Fresh one-run secrets
must be retired after the campaign by the authorized outer lifecycle cleanup.
