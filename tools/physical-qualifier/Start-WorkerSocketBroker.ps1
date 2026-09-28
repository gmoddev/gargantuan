param(
    [Parameter(Mandatory=$true)][string]$Label,
    [Parameter(Mandatory=$true)][string]$ManifestSHA256,
    [Parameter(Mandatory=$true)][string]$BrokerSHA256
)
$ErrorActionPreference = 'Stop'
if ($Label -cnotmatch '^[0-9a-f]{16}$' -or $ManifestSHA256 -cnotmatch '^[0-9A-F]{64}$' -or
    $BrokerSHA256 -cnotmatch '^[0-9A-F]{64}$') {
    throw '[Qualification:Socket] Invalid broker identity or pin.'
}
$Root = "C:\Sandbox\Codex\Artifacts\gargantuan-3l-physical\$Label"
$Manifest = Join-Path $Root 'broker-manifest.json'
if ((Get-FileHash -LiteralPath $Manifest -Algorithm SHA256).Hash -cne $ManifestSHA256) {
    throw '[Qualification:Socket] Broker manifest pin changed.'
}
$TaskName = "Gargantuan3L-SocketBroker-$Label"
if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
    throw '[Qualification:Socket] Broker task already exists.'
}
$Python = 'C:\Users\host\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\pythonw.exe'
$Broker = Join-Path $Root 'worker_socket_broker.py'
if ((Get-FileHash -LiteralPath $Broker -Algorithm SHA256).Hash -cne $BrokerSHA256) {
    throw '[Qualification:Socket] Broker source pin changed.'
}
$Config = Join-Path $Root 'server-broker.json'
$Package = Join-Path $Root 'physical-catalog.zip'
$ManifestItem = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
if ((Get-FileHash -LiteralPath $Package -Algorithm SHA256).Hash -cne $ManifestItem.PackageSHA256) {
    throw '[Qualification:Socket] Broker package pin changed.'
}
$ToolRoot = Join-Path $Root 'broker-tool'
if (Test-Path -LiteralPath $ToolRoot) { throw '[Qualification:Socket] Protected broker tool already exists.' }
Expand-Archive -LiteralPath $Package -DestinationPath $ToolRoot
$Tool = Join-Path $ToolRoot 'physical-qualifier\qualifier.py'
if ((Get-FileHash -LiteralPath $Tool -Algorithm SHA256).Hash -cne $ManifestItem.ToolSHA256) {
    throw '[Qualification:Socket] Broker tool pin changed.'
}
foreach ($File in @($Manifest, $Broker, $Config, $Package)) {
    & icacls $File /inheritance:r /grant:r 'HOSTPC\host:F' '*S-1-5-18:F' '*S-1-5-32-544:F' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw '[Qualification:Socket] Unable to protect broker input.' }
}
& icacls $ToolRoot /inheritance:r /grant:r 'HOSTPC\host:F' '*S-1-5-18:F' '*S-1-5-32-544:F' /T | Out-Null
if ($LASTEXITCODE -ne 0) { throw '[Qualification:Socket] Unable to protect broker tool.' }
$Repository = 'C:\Sandbox\Codex\Workspaces\agent-coordinator-foundation-2-server'
$Action = New-ScheduledTaskAction -Execute $Python -Argument "-B $Broker $Manifest $ManifestSHA256" -WorkingDirectory $Repository
$Principal = New-ScheduledTaskPrincipal -UserId 'HOSTPC\host' -LogonType Interactive -RunLevel Limited
$Settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 10)
Register-ScheduledTask -TaskName $TaskName -Action $Action -Principal $Principal -Settings $Settings | Out-Null
Start-ScheduledTask -TaskName $TaskName
Write-Output '[Qualification:Socket] Fixed one-run worker broker started.'
