# Gargantuan capture-service integration

Generic service source has moved to the pinned
[Agent Coordinator helper](https://github.com/GantriaEngine/agent-coordinator/tree/24edb592ace678124731455b841e2623f64ba57a/privileged-helper).
This directory retains only the project installer wrapper and integration notes.
The fixed Packet Monitor hook remains at `../worker/PktMonCapture.ps1`.
The existing installed Gargantuan service stays the operational baseline.

This Windows-only helper gives the physical qualifier one pre-authorized capability: run the installed, hash-pinned `PktMonCapture.ps1` hook for one evidence directory under a configured root. It does not accept shell commands, executable names, Packet Monitor arguments, interface identifiers, or arbitrary output paths.

The service runs as LocalSystem. Its local named pipe grants access only to LocalSystem and the SID recorded during installation; Windows authenticates pipe clients against this ACL. Requests are newline-delimited JSON, limited to 16 KiB, and accept only `start`/`stop`/`status`, a canonical run UUID, an endpoint-helper PID, and an evidence path below the configured local root. `start` additionally requires that PID to be running the configured Python runtime. The service monitors that process identity and stops its capture when the endpoint helper exits, when the service stops, or at the 90-second hard deadline. The existing hook retains its exact-interface and Packet Monitor ownership checks and 64 MiB circular file cap. Every operation is appended to an admin-only JSONL audit file. An active run record is persisted before capture starts; after a service crash, startup attempts cleanup only for that recorded run. If ownership cannot be verified, new captures are refused until an administrator reconciles it.

## Build and install

Bootstrap the pinned upstream source first. Publish
`.agent-coordinator/privileged-helper/AgentCoordinator.CaptureService.csproj` for
`win-x64` with .NET 8 into that upstream helper's `publish` subfolder. The local
`Install-CaptureService.ps1` wrapper supplies the project-owned hook and locally
approved evidence root/runtime to the hash-verified upstream installer. It creates
a separate `GantriaAgentCoordinatorCapture` service with protected binaries/config,
installing-user/LocalSystem pipe ACL and fixed capture operations. Choose a fresh
evidence root, qualify the new service separately, and keep the old service as
rollback. No installation or physical capture is part of this extraction.

For historical installed-service recovery only, from an Administrator session:

```powershell
Stop-Service GargantuanPhysicalQualifierCapture
sc.exe delete GargantuanPhysicalQualifierCapture
```

Then remove `C:\Program Files\Gargantuan\PhysicalQualifierCapture` and `C:\ProgramData\Gargantuan\PhysicalQualifierCapture` only after preserving `audit.jsonl` if needed. Evidence is not removed.

## Qualifier use

Pass `--server-capture-client "C:\Program Files\Gargantuan\PhysicalQualifierCapture\PhysicalQualifier.CaptureService.exe"` to the existing `stage` command. The server endpoint config invokes only:

```text
PhysicalQualifier.CaptureService.exe start <evidence-directory> <run-uuid> <endpoint-pid>
PhysicalQualifier.CaptureService.exe stop <evidence-directory> <run-uuid> <endpoint-pid>
```

These calls use the local named pipe. The endpoint helper PID is leased to capture until stop, helper exit, service shutdown, or the hard deadline. The control protocol and probe stay unchanged. The existing direct PowerShell hook configuration remains available for machines that do not install this service.

## Verification

Run the project tests and service/client IPC tests before deployment. Verify installation with service state, installed hook SHA-256, config ACL, pipe denial for a different SID, fixed-operation allowlist, path traversal/symlink denial, double-start denial, wrong-run stop denial, timeout cleanup, service-shutdown cleanup, and stale marker recovery. Do not verify it by starting a real capture except as part of an explicitly authorized qualification attempt.
