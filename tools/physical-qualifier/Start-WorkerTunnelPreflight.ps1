param(
    [Parameter(Mandatory=$true)][string]$Label,
    [Parameter(Mandatory=$true)][string]$RunId,
    [switch]$Interactive
)
$ErrorActionPreference = 'Stop'
if ($Label -cnotmatch '^[0-9a-f]{16}$' -or $RunId -cne ([guid]::Parse($RunId)).ToString()) {
    throw '[Qualification:Tunnel] Invalid preflight identity.'
}
$Root = "C:\Sandbox\Codex\Artifacts\gargantuan-3l-physical\$Label"
$ProofName = if ($Interactive) { 'worker-tunnel-interactive-proof.json' } else { 'worker-tunnel-restricted-proof.json' }
if (-not (Test-Path -LiteralPath $Root -PathType Container) -or
    (Test-Path -LiteralPath (Join-Path $Root $ProofName))) {
    throw '[Qualification:Tunnel] Missing or consumed worker artifact.'
}
$TaskName = "Gargantuan3L-TunnelPreflight-$Label"
if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
    throw '[Qualification:Tunnel] Preflight task already exists.'
}
$Python = 'C:\Users\host\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\pythonw.exe'
$Launcher = Join-Path $Root 'worker_tunnel_preflight_launcher.py'
$Repository = 'C:\Sandbox\Codex\Workspaces\agent-coordinator-foundation-2-server'
$Mode = if ($Interactive) { ' INTERACTIVE' } else { '' }
$Action = New-ScheduledTaskAction -Execute $Python -Argument "-B $Launcher $Label $RunId$Mode" -WorkingDirectory $Repository
$Principal = New-ScheduledTaskPrincipal -UserId 'HOSTPC\host' -LogonType Interactive -RunLevel Limited
$Settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName $TaskName -Action $Action -Principal $Principal -Settings $Settings | Out-Null
Start-ScheduledTask -TaskName $TaskName
Write-Output '[Qualification:Tunnel] Worker reverse probe started.'
