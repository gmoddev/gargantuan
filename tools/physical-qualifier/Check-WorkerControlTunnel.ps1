$ErrorActionPreference = 'Stop'
$Listener = Get-NetTCPConnection -LocalAddress '127.0.0.1' -LocalPort 49961 -State Listen -ErrorAction SilentlyContinue
if (-not $Listener -or @($Listener).Count -ne 1) {
    throw '[Qualification:Lifecycle] Worker reverse control tunnel is not listening on 127.0.0.1:49961.'
}
Write-Output '[Qualification:Lifecycle] Worker reverse control tunnel LISTENING.'
