param(
    [Parameter(Mandatory=$true)][string]$Label,
    [Parameter(Mandatory=$true)][string]$RunId
)
$ErrorActionPreference = 'Stop'
if ($Label -cnotmatch '^[0-9a-f]{16}$' -or $RunId -cne ([guid]::Parse($RunId)).ToString()) {
    throw '[Qualification:Interactive] Invalid preflight identity.'
}
$Root = "C:\Sandbox\Codex\Artifacts\gargantuan-3l-physical\$Label"
if (-not (Test-Path -LiteralPath $Root -PathType Container) -or
    (Test-Path -LiteralPath (Join-Path $Root 'worker-interactive-preflight.json'))) {
    throw '[Qualification:Interactive] Missing or consumed worker artifact.'
}
$TaskName = "Gargantuan3L-InteractivePreflight-$Label"
if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
    throw '[Qualification:Interactive] Preflight task already exists.'
}
$Python = 'C:\Users\host\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\pythonw.exe'
$Launcher = Join-Path $Root 'worker_interactive_preflight_launcher.py'
$Repository = 'C:\Sandbox\Codex\Workspaces\agent-coordinator-foundation-2-server'
$Action = New-ScheduledTaskAction -Execute $Python -Argument "-B $Launcher $Label $RunId" -WorkingDirectory $Repository
$Principal = New-ScheduledTaskPrincipal -UserId 'HOSTPC\host' -LogonType Interactive -RunLevel Limited
$Settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName $TaskName -Action $Action -Principal $Principal -Settings $Settings | Out-Null
Start-ScheduledTask -TaskName $TaskName
Write-Output '[Qualification:Interactive] Worker evidence and LAN preflight started.'
