$ErrorActionPreference = 'Stop'
$Listener = @(Get-NetTCPConnection -LocalAddress '127.0.0.1' -LocalPort 49961 -State Listen -ErrorAction SilentlyContinue)
if ($Listener.Count -eq 0) {
    @{ Present = $false } | ConvertTo-Json -Compress
    exit 0
}
if ($Listener.Count -ne 1) { throw '[Qualification:Lifecycle] Ambiguous worker reverse listener.' }
$Owner = Get-Process -Id $Listener[0].OwningProcess -ErrorAction Stop
@{
    Present = $true
    Address = '127.0.0.1'
    Port = 49961
    OwnerPid = [int]$Listener[0].OwningProcess
    ProcessName = $Owner.ProcessName
} | ConvertTo-Json -Compress
