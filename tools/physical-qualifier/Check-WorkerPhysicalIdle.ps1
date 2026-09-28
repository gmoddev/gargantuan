$ErrorActionPreference = 'Stop'
$Service = Get-Service -Name 'GargantuanPhysicalQualifierCapture' -ErrorAction Stop
if ($Service.Status -ne 'Running') { throw '[Qualification:Idle] Capture service unavailable.' }
if (Test-Path -LiteralPath 'C:\ProgramData\Gargantuan\PhysicalQualifierCapture\active-run.json') {
    throw '[Qualification:Idle] Capture service owns an active run.'
}
$Capture = (& pktmon status 2>&1 | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $Capture -notmatch '^Packet Monitor is not running\.') {
    throw '[Qualification:Idle] Packet Monitor is active or status unknown.'
}
$Filters = (& pktmon filter list 2>&1 | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $Filters -notmatch '^Packet Filters:\s+None$') {
    throw '[Qualification:Idle] Packet Monitor filters are active or unknown.'
}
if (@(Get-NetUDPEndpoint -LocalPort 39450 -ErrorAction SilentlyContinue).Count) {
    throw '[Qualification:Idle] Physical UDP listener already exists.'
}
@{
    CaptureService = 'Running'
    CaptureActiveRun = $false
    PacketMonitor = 'Stopped'
    PacketFilters = 'None'
    Udp39450 = 'Unbound'
    TimestampUnixMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
} | ConvertTo-Json -Compress
