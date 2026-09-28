param(
    [Parameter(Mandatory=$true)][string]$Label,
    [Parameter(Mandatory=$true)][string]$RunId
)
$ErrorActionPreference = 'Stop'
if ($Label -cnotmatch '^[0-9a-f]{16}$' -or $RunId -cne ([guid]::Parse($RunId)).ToString()) {
    throw '[Qualification:Socket] Invalid preflight identity.'
}
$Root = "C:\Sandbox\Codex\Artifacts\gargantuan-3l-physical\$Label"
if (Test-Path -LiteralPath (Join-Path $Root 'worker-socket-restricted-proof.json')) {
    throw '[Qualification:Socket] Preflight already consumed.'
}
$TaskName = "Gargantuan3L-SocketPreflight-$Label"
if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
    throw '[Qualification:Socket] Preflight task already exists.'
}
$Python = 'C:\Users\host\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\pythonw.exe'
$Launcher = Join-Path $Root 'worker_socket_preflight_launcher.py'
$Repository = 'C:\Sandbox\Codex\Workspaces\agent-coordinator-foundation-2-server'
$Action = New-ScheduledTaskAction -Execute $Python -Argument "-B $Launcher $Label $RunId" -WorkingDirectory $Repository
$Principal = New-ScheduledTaskPrincipal -UserId 'HOSTPC\host' -LogonType Interactive -RunLevel Limited
$Settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName $TaskName -Action $Action -Principal $Principal -Settings $Settings | Out-Null
Start-ScheduledTask -TaskName $TaskName
Write-Output '[Qualification:Socket] Restricted worker socket preflight started.'
