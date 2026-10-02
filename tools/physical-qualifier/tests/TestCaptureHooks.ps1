# Control-only simulation: functions shadow Windows cmdlets; no NIC/capture changes.
param([string]$HookPath, [switch]$Farm32)
$ErrorActionPreference = 'Stop'
$Hook = if ($HookPath) { $HookPath } else { Join-Path $PSScriptRoot '..\worker\PktMonCapture.ps1' }
$HookText = [IO.File]::ReadAllText($Hook)
if ($Farm32) {
    # Only replace the OS disk query. Exercise the actual guard without needing
    # 34 GiB free on hosted CI or allocating a near-cap capture.
    $DiskQuery = '$Drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot([IO.Path]::GetFullPath($PathName)))'
    if (-not $HookText.Contains($DiskQuery)) { throw 'Farm32 disk query seam changed' }
    $HookText = $HookText.Replace($DiskQuery,
        '$Drive = [pscustomobject]@{IsReady=$true; AvailableFreeSpace=$global:TestDiskFree}')
}
$HookBlock = [ScriptBlock]::Create($HookText)
$Prefix = if ($Farm32) { 'farm32-' } else { '' }
$TraceMaximumMiB = if ($Farm32) { 16384 } else { 256 }
$NoWrapThresholdMiB = if ($Farm32) { 15360 } else { 240 }
$global:TestDiskFree = 64GB
$TestDir = Join-Path ([IO.Path]::GetTempPath()) ('qualifier-hook-test-' + [Guid]::NewGuid())
New-Item -ItemType Directory -Path $TestDir | Out-Null
$global:TestActive = $false
$global:TestForeign = $false
$global:TestEtl = ''
$global:TestEventsEmpty = $false
$global:TestLostEvents = 0
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
function global:tracerpt {
    $Arguments = @($args)
    $Index = [array]::IndexOf($Arguments, '-summary')
    if ($Index -lt 0 -or $Index + 1 -ge $Arguments.Count) { throw 'Unexpected tracerpt request' }
    @('Total Buffers Processed 1', 'Total Events  Processed 1',
      "Total Events  Lost      $global:TestLostEvents") |
        Set-Content -LiteralPath $Arguments[$Index + 1]
    $global:LASTEXITCODE = 0
}
function Assert([bool]$Value,[string]$Detail) { if (!$Value) { throw $Detail } }
try {
    $First = Join-Path $TestDir 'first'
    New-Item -ItemType Directory -Path $First | Out-Null
    if ($Farm32) {
        $global:TestDiskFree = 34GB - 1
        $Rejected = $false
        try { & $HookBlock $First Start 39450 } catch { $Rejected = $_.Exception.Message -match 'fixed capture/export reserve' }
        Assert $Rejected 'Farm32 start accepted insufficient room for retained ETL plus export'
        Assert ($global:TestCalls.Count -eq 0) 'Low disk denial invoked capture'
        $global:TestDiskFree = 34GB
    }
    & $HookBlock $First Start 39450
    Assert $global:TestActive 'Capture did not start'
    $Expected = 'trace start capture=yes capturetype=physical CaptureInterface={a33455f8-3b6f-46f1-b981-7c861e6b3cd3} Ethernet.Type=IPv4 Protocol=17 IPv4.Address=10.253.3.1 CaptureMultiLayer=no PacketTruncateBytes=1518 report=disabled persistent=no fileMode=single maxSize=' + $TraceMaximumMiB + ' traceFile=' + $global:TestEtl
    Assert ([bool]($global:TestCalls | Where-Object {$_ -eq $Expected})) 'Physical-interface trace filters or storage bound changed'
    $Owner = Get-Content (Join-Path $First ($Prefix + 'netsh-owner.json')) -Raw | ConvertFrom-Json
    Assert ($Owner.MiniportIfIndex -eq 19 -and $Owner.CapturePort -eq 39450 -and
            $Owner.TraceMaximumMiB -eq $TraceMaximumMiB -and $Owner.NoWrapThresholdMiB -eq $NoWrapThresholdMiB -and
            ($Owner.CaptureLayers -join ',') -eq 'NDIS physical miniport') 'Capture ownership marker is incomplete'
    if ($Farm32) { Assert ($Owner.Profile -ceq 'Farm32Capture16GiB-v2') 'Farm32 versioned profile is absent' }
    & $HookBlock $First Stop
    Assert (!$global:TestActive) 'Capture did not stop'
    if ($Farm32) {
        $global:TestDiskFree = 17GB - 1
        $Rejected = $false
        try { & $HookBlock $First Finalize } catch { $Rejected = $_.Exception.Message -match 'fixed capture/export reserve' }
        Assert $Rejected 'Farm32 finalize accepted insufficient export room'
        Assert (-not (Test-Path (Join-Path $First 'farm32-capture-summary.txt'))) 'Low disk finalize consumed evidence'
        $global:TestDiskFree = 17GB
        & $HookBlock $First Finalize
        $global:TestDiskFree = 64GB
    }
    $Pcap = Join-Path $First ($Prefix + 'worker-capture.pcapng')
    Assert (Test-Path $Pcap) 'Export missing'
    $Bytes = [IO.File]::ReadAllBytes($Pcap)
    Assert ($Bytes.Length -gt 80 -and $Bytes[0] -eq 10 -and $Bytes[1] -eq 13 -and
            $Bytes[2] -eq 13 -and $Bytes[3] -eq 10) 'Export is not pcapng'
    $Timestamp = ([uint64][BitConverter]::ToUInt32($Bytes, 60) -shl 32) -bor
                 [BitConverter]::ToUInt32($Bytes, 64)
    $ExpectedTimestamp = [DateTimeOffset]::Parse('2026-09-28T08:00:00Z').ToUnixTimeMilliseconds() * 1000
    Assert ($Timestamp -eq $ExpectedTimestamp) 'Ethernet pcapng timestamp is not UTC'
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
    if ($Farm32) { & $HookBlock $Third Finalize }

    $Fourth = Join-Path $TestDir 'fourth'
    New-Item -ItemType Directory -Path $Fourth | Out-Null
    & $HookBlock $Fourth Start
    $global:TestEventsEmpty = $true
    $Rejected = $false
    if ($Farm32) { & $HookBlock $Fourth Stop }
    try { & $HookBlock $Fourth $(if ($Farm32) { 'Finalize' } else { 'Stop' }) } catch { $Rejected=$true }
    Assert $Rejected 'Zero-frame trace was accepted'
    Assert (!(Test-Path (Join-Path $Fourth ($Prefix + 'worker-capture.pcapng')))) 'Failed export was published'
    Assert (!(Test-Path (Join-Path $Fourth ($Prefix + 'worker-capture.pcapng.pending')))) 'Failed export left a pending file'

    $Fifth = Join-Path $TestDir 'fifth'
    New-Item -ItemType Directory -Path $Fifth | Out-Null
    & $HookBlock $Fifth Start
    $Trace = Join-Path $Fifth ($Prefix + 'worker-capture.etl')
    if ($Farm32) {
        # Exercise the production integer guard above 4 GiB without allocation.
        $ScriptAst = [System.Management.Automation.Language.Parser]::ParseInput([IO.File]::ReadAllText($Hook),[ref]$null,[ref]$null)
        $SizeFunction = $ScriptAst.Find({param($Node) $Node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -eq 'AssertTraceBelowBound'}, $true)
        Assert ($null -ne $SizeFunction) 'Farm32 size guard is missing'
        Invoke-Expression $SizeFunction.Extent.Text
        $DiskFunction = $ScriptAst.Find({param($Node) $Node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -eq 'AssertFarm32DiskReserve'}, $true)
        Assert ($null -ne $DiskFunction) 'Farm32 disk reserve guard is missing'
        Invoke-Expression $DiskFunction.Extent.Text
        $Rejected = $false
        try { AssertFarm32DiskReserve $Fifth ([long]::MaxValue) } catch { $Rejected = $_.Exception.Message -match 'fixed capture/export reserve' }
        Assert $Rejected 'Farm32 insufficient-disk condition was accepted'
        $NoWrapThresholdMiB = 15360
        AssertTraceBelowBound (15GB - 1)
        $Rejected = $false
        try { AssertTraceBelowBound (15GB) } catch { $Rejected = $_.Exception.Message -match 'completeness size bound' }
        Assert $Rejected 'Farm32 trace at its non-wrap completeness threshold was accepted'
        # Compile the actual embedded streaming exporter, including its runtime
        # EventLog dependency. A counting stream exercises 64-bit offsets with
        # no large allocation, file, capture, or ETW session.
        $Packed = [regex]::Match($HookText, '\$CompressedDefinition = ''([^'']+)''').Groups[1].Value
        $PackedStream = [IO.MemoryStream]::new([Convert]::FromBase64String($Packed))
        $Zip = [IO.Compression.GZipStream]::new($PackedStream, [IO.Compression.CompressionMode]::Decompress)
        $Reader = [IO.StreamReader]::new($Zip)
        try { $Definition = $Reader.ReadToEnd() } finally { $Reader.Dispose() }
        Add-Type -TypeDefinition $Definition -ReferencedAssemblies @(
            'System.Core.dll', [System.Diagnostics.Eventing.Reader.EventLogReader].Assembly.Location)
        Add-Type -TypeDefinition @'
using System;
using System.IO;
public sealed class Farm32CountingStream : Stream {
    private long Offset;
    public long Written;
    public override bool CanRead { get { return false; } }
    public override bool CanSeek { get { return true; } }
    public override bool CanWrite { get { return true; } }
    public override long Length { get { return Offset; } }
    public override long Position { get { return Offset; } set { Offset = value; } }
    public override void Flush() { }
    public override int Read(byte[] Buffer, int Start, int Count) { throw new NotSupportedException(); }
    public override long Seek(long Value, SeekOrigin Origin) { throw new NotSupportedException(); }
    public override void SetLength(long Value) { throw new NotSupportedException(); }
    public override void Write(byte[] Buffer, int Start, int Count) { Offset += Count; Written += Count; }
    public override void WriteByte(byte Value) { Offset++; Written++; }
}
'@
        $PacketWriter = [GargantuanQualification.Farm32V2NdisExport].GetMethod('WritePacket',
            [Reflection.BindingFlags]'NonPublic,Static')
        $Counter = [Farm32CountingStream]::new()
        $Writer = [IO.BinaryWriter]::new($Counter)
        try {
            $Counter.Position = 4GB
            [void]$PacketWriter.Invoke($null, @($Writer, [byte[]]::new(44), [long]1))
            Assert ($Counter.Position -eq 4GB + 76) 'Fast export truncated a 64-bit file offset'
            $Counter.Position = 15GB - 77
            [void]$PacketWriter.Invoke($null, @($Writer, [byte[]]::new(44), [long]2))
            Assert ($Counter.Position -eq 15GB - 1) 'Fast export rejected a complete packet below threshold'
            $Counter.Position = 15GB - 76
            $BeforeWritten = $Counter.Written
            $Rejected = $false
            try { [void]$PacketWriter.Invoke($null, @($Writer, [byte[]]::new(44), [long]3)) }
            catch { $Rejected = $_.Exception.ToString() -match 'completeness size bound' }
            Assert ($Rejected -and $Counter.Written -eq $BeforeWritten) 'Fast export wrote a packet reaching the limit'
        } finally { $Writer.Dispose() }
        $global:TestEventsEmpty = $false
        & $HookBlock $Fifth Stop
        & $HookBlock $Fifth Finalize
    } else {
        $Stream = [IO.File]::Open($Trace, [IO.FileMode]::Open, [IO.FileAccess]::Write)
        try { $Stream.SetLength(240MB) } finally { $Stream.Dispose() }
        $Rejected = $false
        try { & $HookBlock $Fifth Stop } catch { $Rejected = $_.Exception.Message -match 'completeness size bound' }
        Assert $Rejected 'A single-file trace at its completeness threshold was accepted'
        Assert (!(Test-Path (Join-Path $Fifth 'worker-capture.pcapng'))) 'Near-cap trace published a partial pcap'
    }

    $Sixth = Join-Path $TestDir 'sixth'
    New-Item -ItemType Directory -Path $Sixth | Out-Null
    & $HookBlock $Sixth Start
    $global:TestEventsEmpty = $false
    $global:TestLostEvents = 1
    $Rejected = $false
    if ($Farm32) { & $HookBlock $Sixth Stop }
    try { & $HookBlock $Sixth $(if ($Farm32) { 'Finalize' } else { 'Stop' }) } catch { $Rejected = $_.Exception.Message -match 'lost events' }
    Assert $Rejected 'A trace with lost ETW events was accepted'
    Assert (!(Test-Path (Join-Path $Sixth ($Prefix + 'worker-capture.pcapng')))) 'Lossy trace published a partial pcap'
    if ($Farm32) {
        $Seventh = Join-Path $TestDir 'seventh'
        New-Item -ItemType Directory -Path $Seventh | Out-Null
        [IO.File]::WriteAllText((Join-Path $Seventh 'farm32-worker-capture.etl'), 'preserved')
        $Before = @($global:TestCalls | Where-Object { $_ -like 'trace start *' }).Count
        $Rejected = $false
        try { & $HookBlock $Seventh Start } catch { $Rejected = $true }
        Assert $Rejected 'Farm32 capture overwrote an existing ETL'
        Assert (@($global:TestCalls | Where-Object { $_ -like 'trace start *' }).Count -eq $Before) 'Farm32 preexisting-artifact denial started a trace'
        Assert ((Get-Content -LiteralPath (Join-Path $Seventh 'farm32-worker-capture.etl') -Raw) -eq 'preserved') 'Farm32 existing ETL changed'
    }
    Write-Output 'Capture hook simulation passed: exact scope, export, ownership and cleanup.'
} finally {
    Remove-Item Function:\netsh,Function:\Get-NetAdapter,Function:\Get-NetIPAddress,Function:\Get-NetIPInterface,Function:\Get-WinEvent,Function:\tracerpt
    $Resolved = [IO.Path]::GetFullPath($TestDir)
    if ((Split-Path $Resolved -Parent) -ne ([IO.Path]::GetTempPath().TrimEnd('\')) -or
        (Split-Path $Resolved -Leaf) -notlike 'qualifier-hook-test-*') {
        throw 'Refusing cleanup outside the generated test directory.'
    }
    Remove-Item -LiteralPath $TestDir -Recurse -Force
}
