# Control-only simulation: functions shadow Windows cmdlets; no NIC/capture changes.
param([string]$HookPath)
$ErrorActionPreference = 'Stop'
$Hook = if ($HookPath) { $HookPath } else { Join-Path $PSScriptRoot '..\worker\PktMonCapture.ps1' }
$HookBlock = [ScriptBlock]::Create([IO.File]::ReadAllText($Hook))
$TestDir = Join-Path ([IO.Path]::GetTempPath()) ('qualifier-hook-test-' + [Guid]::NewGuid())
New-Item -ItemType Directory -Path $TestDir | Out-Null
$global:TestActive = $false
$global:TestForeign = $false
$global:TestEtl = ''
$global:TestEventsEmpty = $false
$global:TestCalls = [System.Collections.Generic.List[string]]::new()
function global:netsh {
    param([Parameter(ValueFromRemainingArguments=$true)][string[]]$Arguments)
    $global:TestCalls.Add(($Arguments -join ' '))
    $global:LASTEXITCODE = 0
    if (($Arguments[0..2] -join ' ') -eq 'trace show status') {
        if (!$global:TestActive) {
            $global:LASTEXITCODE = 1
            return 'There is no trace session currently in progress.'
        }
        $Path = if ($global:TestForeign) { 'C:\unrelated.etl' } else { $global:TestEtl }
        return "Status: Running`nTrace File: $Path"
    }
    if (($Arguments[0..1] -join ' ') -eq 'trace start') {
        $global:TestEtl = ($Arguments | Where-Object {$_ -like 'traceFile=*'}).Substring(10)
        [IO.File]::WriteAllBytes($global:TestEtl,[byte[]]@(1,2,3))
        $global:TestActive = $true
        return 'Trace started.'
    }
    if (($Arguments[0..1] -join ' ') -eq 'trace stop') {
        $global:TestActive = $false
        return 'Trace stopped.'
    }
    throw 'Unexpected netsh command'
}
function global:Get-NetAdapter {
    [pscustomobject]@{ifIndex=19;Status='Up';LinkSpeed='10 Gbps';MacAddress='0C-42-A1-52-38-A8';
                     InterfaceGuid='A33455F8-3B6F-46F1-B981-7C861E6B3CD3'}
}
function global:Get-NetIPAddress { [pscustomobject]@{IPAddress='10.253.3.2';PrefixLength=30} }
function global:Get-NetIPInterface { [pscustomobject]@{NlMtu=1500;Dhcp='Disabled'} }
function global:Get-WinEvent {
    if ($global:TestEventsEmpty) { return }
    $Frame = [byte[]]@(0x0C,0x42,0xA1,0x52,0x38,0xA8,0x0C,0x42,0xA1,0x49,0xD5,0xE0,
        0x08,0x00,0x45,0x00,0x00,0x1C,0x00,0x01,0x40,0x00,0x40,0x11,0x00,0x00,
        10,253,3,1,10,253,3,2,0xF2,0x30,0x9A,0x1A,0x00,0x08,0x00,0x00)
    [pscustomobject]@{ProviderName='Microsoft-Windows-NDIS-PacketCapture';Id=1001;
        TimeCreated=[datetime]'2026-09-28T08:00:00Z';Properties=@(
            [pscustomobject]@{Value=[uint32]19},[pscustomobject]@{Value=[uint32]19},
            [pscustomobject]@{Value=[uint32]$Frame.Length},[pscustomobject]@{Value=$Frame})}
}
function Assert([bool]$Value,[string]$Detail) { if (!$Value) { throw $Detail } }
try {
    $First = Join-Path $TestDir 'first'
    New-Item -ItemType Directory -Path $First | Out-Null
    & $HookBlock $First Start 39450
    Assert $global:TestActive 'Capture did not start'
    $Expected = 'trace start capture=yes capturetype=physical CaptureInterface={a33455f8-3b6f-46f1-b981-7c861e6b3cd3} Ethernet.Type=IPv4 Protocol=17 IPv4.Address=10.253.3.1 CaptureMultiLayer=no PacketTruncateBytes=1518 report=disabled persistent=no fileMode=circular maxSize=64 traceFile=' + $global:TestEtl
    Assert ([bool]($global:TestCalls | Where-Object {$_ -eq $Expected})) 'Physical-interface trace filters or 64 MiB bound changed'
    $Owner = Get-Content (Join-Path $First 'netsh-owner.json') -Raw | ConvertFrom-Json
    Assert ($Owner.MiniportIfIndex -eq 19 -and $Owner.CapturePort -eq 39450 -and
            ($Owner.CaptureLayers -join ',') -eq 'NDIS physical miniport') 'Capture ownership marker is incomplete'
    & $HookBlock $First Stop
    Assert (!$global:TestActive) 'Capture did not stop'
    $Pcap = Join-Path $First 'worker-capture.pcapng'
    Assert (Test-Path $Pcap) 'Export missing'
    $Bytes = [IO.File]::ReadAllBytes($Pcap)
    Assert ($Bytes.Length -gt 80 -and $Bytes[0] -eq 10 -and $Bytes[1] -eq 13 -and
            $Bytes[2] -eq 13 -and $Bytes[3] -eq 10) 'Export is not pcapng'
    & $HookBlock $First Stop
    Assert ((Get-Item $Pcap).Length -eq $Bytes.Length) 'Idempotent stop changed the export'

    $Second = Join-Path $TestDir 'second'
    New-Item -ItemType Directory -Path $Second | Out-Null
    $Before = $global:TestCalls.Count
    $Rejected = $false
    try { & $HookBlock $Second Start 39453 } catch { $Rejected=$true }
    Assert $Rejected 'An unapproved capture port was accepted'
    Assert ($global:TestCalls.Count -eq $Before) 'Invalid port invoked netsh'
    $global:TestActive = $true
    $global:TestForeign = $true
    $Rejected = $false
    try { & $HookBlock $Second Start } catch { $Rejected=$true }
    Assert $Rejected 'Existing trace session was not protected'
    Assert ($global:TestActive) 'Existing trace session was mutated'
    $global:TestActive = $false
    $global:TestForeign = $false

    $Third = Join-Path $TestDir 'third'
    New-Item -ItemType Directory -Path $Third | Out-Null
    & $HookBlock $Third Start 39452
    $global:TestForeign = $true
    $Rejected = $false
    try { & $HookBlock $Third Stop } catch { $Rejected=$true }
    Assert $Rejected 'Foreign trace session was not protected during stop'
    Assert $global:TestActive 'Foreign trace session was stopped'
    $global:TestForeign = $false
    & $HookBlock $Third Stop

    $Fourth = Join-Path $TestDir 'fourth'
    New-Item -ItemType Directory -Path $Fourth | Out-Null
    & $HookBlock $Fourth Start
    $global:TestEventsEmpty = $true
    $Rejected = $false
    try { & $HookBlock $Fourth Stop } catch { $Rejected=$true }
    Assert $Rejected 'Zero-frame trace was accepted'
    Assert (!(Test-Path (Join-Path $Fourth 'worker-capture.pcapng'))) 'Failed export was published'
    Assert (!(Test-Path (Join-Path $Fourth 'worker-capture.pcapng.pending'))) 'Failed export left a pending file'
    Write-Output 'Capture hook simulation passed: exact scope, export, ownership and cleanup.'
} finally {
    Remove-Item Function:\netsh,Function:\Get-NetAdapter,Function:\Get-NetIPAddress,Function:\Get-NetIPInterface,Function:\Get-WinEvent
    $Resolved = [IO.Path]::GetFullPath($TestDir)
    if ((Split-Path $Resolved -Parent) -ne ([IO.Path]::GetTempPath().TrimEnd('\')) -or
        (Split-Path $Resolved -Leaf) -notlike 'qualifier-hook-test-*') {
        throw 'Refusing cleanup outside the generated test directory.'
    }
    Remove-Item -LiteralPath $TestDir -Recurse -Force
}
