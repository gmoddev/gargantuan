param(
    [string]$EvidenceRoot = 'C:\GargantuanQualification\physical-qualifier-service-evidence',
    [string]$LeaseImagePath = 'C:\Sandbox\Codex\Tools\physical-qualifier\runtime\python.exe'
)
$ErrorActionPreference = 'Stop'
$ToolRoot = Split-Path -Parent $PSScriptRoot
$UpstreamInstaller = Join-Path $ToolRoot '.agent-coordinator\privileged-helper\Install-CaptureService.ps1'
if (!(Test-Path -LiteralPath $UpstreamInstaller)) { throw 'Bootstrap the pinned Agent Coordinator first.' }
$Lock = Get-Content -LiteralPath (Join-Path $ToolRoot 'upstream.lock.json') -Raw | ConvertFrom-Json
$Expected = $Lock.Files.'privileged-helper/Install-CaptureService.ps1'
if ((Get-FileHash -LiteralPath $UpstreamInstaller -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Expected) {
    throw 'Pinned upstream installer hash differs; refusing installation.'
}
$HookSource = [IO.Path]::GetFullPath((Join-Path $ToolRoot 'worker\PktMonCapture.ps1'))
& $UpstreamInstaller -EvidenceRoot $EvidenceRoot -LeaseImagePath $LeaseImagePath -HookSource $HookSource
