param(
    [Parameter(Mandatory=$true)][string]$Label,
    [Parameter(Mandatory=$true)][string]$RunId
)
$ErrorActionPreference = 'Stop'
if ($Label -cnotmatch '^[0-9a-f]{16}$') { throw '[Qualification:Evidence] Invalid worker run label.' }
if ($RunId -cne ([guid]::Parse($RunId)).ToString()) { throw '[Qualification:Evidence] Invalid physical run ID.' }
$ConfigFile = "C:\Sandbox\Codex\Evidence\physical-qualifier\lifecycle-$Label\server.json"
$Config = Get-Content -LiteralPath $ConfigFile -Raw | ConvertFrom-Json
$Evidence = "C:\GargantuanQualification\physical-qualifier-service-evidence\lifecycle-$Label"
if ($Config.RunId -cne $RunId -or $Config.EvidenceDir -cne $Evidence) {
    throw '[Qualification:Evidence] Worker config does not match fresh evidence root.'
}
if (Test-Path -LiteralPath $Evidence) { throw '[Qualification:Evidence] Worker evidence already exists.' }
$Root = "C:\Sandbox\Codex\Artifacts\gargantuan-3l-physical\$Label"
if (-not (Test-Path -LiteralPath $Root -PathType Container)) { throw '[Qualification:Evidence] Worker artifact missing.' }
if (Test-Path -LiteralPath (Join-Path $Root 'worker-evidence-preflight.json')) {
    throw '[Qualification:Evidence] Worker preflight already consumed.'
}
$TaskName = "Gargantuan3L-Evidence-$Label"
if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
    throw '[Qualification:Evidence] Worker preflight task already exists.'
}
$Python = 'C:\Users\host\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\pythonw.exe'
$Launcher = 'C:\Sandbox\Codex\Artifacts\gargantuan-3l-physical\worker_evidence_preflight_launcher.py'
$Action = New-ScheduledTaskAction -Execute $Python -Argument "$Launcher $Label $RunId" -WorkingDirectory 'C:\Sandbox\Codex\Workspaces\agent-coordinator-foundation-2-server'
$Principal = New-ScheduledTaskPrincipal -UserId 'HOSTPC\host' -LogonType Interactive -RunLevel Limited
$Settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName $TaskName -Action $Action -Principal $Principal -Settings $Settings | Out-Null
Start-ScheduledTask -TaskName $TaskName
Write-Output '[Qualification:Evidence] Worker restricted preflight started.'
