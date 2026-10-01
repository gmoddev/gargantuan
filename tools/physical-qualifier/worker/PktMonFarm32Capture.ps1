param(
    [Parameter(Mandatory=$true)][string]$EvidenceDir,
    [ValidateSet('Start','Stop','Finalize')][string]$Action = 'Start',
    [ValidateSet(39450,39452)][int]$CapturePort = 39450
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Marker = Join-Path $EvidenceDir 'farm32-netsh-owner.json'
$Etl = Join-Path $EvidenceDir 'farm32-worker-capture.etl'
$Pcap = Join-Path $EvidenceDir 'farm32-worker-capture.pcapng'
# One noncircular ETL file. Reaching the reserve below the hard storage cap
# invalidates evidence rather than permitting silent truncation or wraparound.
$TraceMaximumMiB = 1024
$NoWrapThresholdMiB = 960
$UnixEpochTicks = [datetime]::new(1970, 1, 1, 0, 0, 0, [DateTimeKind]::Utc).Ticks

function InvokeNetsh([string[]]$Arguments) {
    $Output = & netsh @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw ($Output -join "`n") }
    return ($Output -join "`n")
}

function GetTraceStatus {
    $Output = & netsh trace show status 2>&1
    $Text = $Output -join "`n"
    if ($LASTEXITCODE -ne 0 -and $Text -notmatch 'There is no trace session currently in progress') {
        throw $Text
    }
    return $Text
}

function AssertTraceHasNoLostEvents([string]$Source, [string]$Summary) {
    $Output = & tracerpt -l $Source -summary $Summary -o NUL -y 2>&1
    if ($LASTEXITCODE -ne 0) { throw ($Output -join "`n") }
    if (!(Test-Path -LiteralPath $Summary -PathType Leaf)) {
        throw 'Task-owned NDIS loss summary is missing.'
    }
    $Text = Get-Content -LiteralPath $Summary -Raw
    $Match = [regex]::Match($Text, '(?m)^Total Events\s+Lost\s+(\d+)\s*$')
    if (!$Match.Success) { throw 'Task-owned NDIS event-loss count is unavailable.' }
    if ([uint64]$Match.Groups[1].Value -ne 0) {
        throw 'Task-owned NDIS trace lost events; capture is incomplete.'
    }
}

function AssertTraceBelowBound([long]$EtlBytes) {
    if ($EtlBytes -ge $NoWrapThresholdMiB * 1MB) {
        throw 'Task-owned NDIS trace reached its completeness size bound; capture is incomplete.'
    }
}

function AssertFarm32DiskReserve([string]$PathName, [long]$RequiredBytes) {
    # Reserve room for the nonwrapping ETL, a complete pcapng, and 512 MiB of
    # uncommitted headroom. Worker C: is often insufficient; no root is changed.
    $Drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot([IO.Path]::GetFullPath($PathName)))
    if (-not $Drive.IsReady -or $Drive.AvailableFreeSpace -lt $RequiredBytes) {
        throw 'Farm32 evidence volume lacks the fixed capture/export reserve.'
    }
}

function WriteBlock([IO.BinaryWriter]$Writer, [uint32]$Type, [byte[]]$Body) {
    $Padding = (4 - ($Body.Length % 4)) % 4
    $Length = [uint32](12 + $Body.Length + $Padding)
    $Writer.Write($Type)
    $Writer.Write($Length)
    $Writer.Write($Body)
    if ($Padding) { $Writer.Write([byte[]]::new($Padding)) }
    $Writer.Write($Length)
}

function ExportNdisTrace([string]$Source, [string]$Destination, [uint32]$IfIndex) {
    # The NDIS provider's packet event carries an Ethernet frame in property 3.
    # Packet Monitor's ETL converter cannot read this provider. Emit a complete
    # Ethernet pcapng so the normal qualifier validates the exact UDP tuples.
    $Events = Get-WinEvent -Path $Source -Oldest -ErrorAction Stop
    $Stream = [IO.File]::Open($Destination, [IO.FileMode]::CreateNew,
                              [IO.FileAccess]::Write, [IO.FileShare]::None)
    $Writer = [IO.BinaryWriter]::new($Stream)
    $Packets = 0
    try {
        $Body = [IO.MemoryStream]::new()
        $Part = [IO.BinaryWriter]::new($Body)
        $Part.Write([uint32]0x1A2B3C4D)
        $Part.Write([uint16]1)
        $Part.Write([uint16]0)
        $Part.Write([int64]-1)
        WriteBlock $Writer 0x0A0D0D0A $Body.ToArray()
        $Body.SetLength(0)
        $Body.Position = 0
        $Part.Write([uint16]1)
        $Part.Write([uint16]0)
        $Part.Write([uint32]65535)
        WriteBlock $Writer 1 $Body.ToArray()
        foreach ($Event in $Events) {
            if ($Event.ProviderName -ne 'Microsoft-Windows-NDIS-PacketCapture' -or $Event.Id -ne 1001) {
                continue
            }
            $Properties = $Event.Properties
            if ($Properties.Count -lt 4 -or [uint32]$Properties[0].Value -ne $IfIndex -or
                [uint32]$Properties[1].Value -ne $IfIndex) { continue }
            $Frame = [byte[]]$Properties[3].Value
            if ($Frame.Length -lt 14 -or $Frame.Length -ne [uint32]$Properties[2].Value) {
                throw 'NDIS packet event is truncated or malformed.'
            }
            $Microseconds = [int64](($Event.TimeCreated.ToUniversalTime().Ticks -
                                      $UnixEpochTicks) / 10)
            $Body.SetLength(0)
            $Body.Position = 0
            $Part.Write([uint32]0)
            $Part.Write([uint32]($Microseconds -shr 32))
            $Part.Write([uint32]($Microseconds -band 0xFFFFFFFFL))
            $Part.Write([uint32]$Frame.Length)
            $Part.Write([uint32]$Frame.Length)
            $Part.Write($Frame)
            WriteBlock $Writer 6 $Body.ToArray()
            $Packets++
        }
        if ($Packets -eq 0) { throw 'NDIS trace contains no complete frames from the worker fiber miniport.' }
    } finally {
        $Writer.Dispose()
    }
    return $Packets
}

function ExportNdisTraceFast([string]$Source, [string]$Destination, [uint32]$IfIndex) {
    # PowerShell event-property materialization took 26.656 s for a retained
    # 32-MiB pcap. The fixed-operation service permits only 30 s for Stop.
    # Compile the same bounded NDIS event filter/writer in-process for large
    # Phase 1 traces; keep the simple path for small capture-hook simulations.
    if (-not ('GargantuanQualification.FastNdisExport' -as [type])) {
        $CompressedDefinition = 'H4sIAAAAAAACCrVWW2/bNhR+969g/TBIi6NJdtJtcFwgjZPCWJqldbI+BHlgpOOYiEwKJOUL2vz3HpJSLCVS3K0YbdgWea7f4fmOc8X4PZlulIbFsJNXnoIxo/dcKM1iFZwugWs8Cz4DTUA+k5z8Pex0OF2AymgM5AOV95TrnPJPOU3ZjMVUM8HJ106W36UsJkrjRkzilCpFzqjSFwlTp+tMSI1CBFcm2ZJqILHgSqO8NN4upVgy9E5GpPuRxVIoMdP7XxhPxErtX4wn0/1LGj+APqGZziV0hzVbhVeJGQiebkgq0OY1Z+vTTMTzKxY/KDKyGmZxWJExql2xBXjRn7+HPRLZd1i+y9O/MIDgWsd+YG0gFNZrLVXry2XoFdlMRS5j6JXJjQGR5haoHskZ12TCNcgZAjrhCaz9AhmzrDWXKoZMwuHTyZJK8ikHucFtk4Gt27m4t3te6fKS6vnVJoPgjKVgHnqk+2vX35qx4ljrJUgFYyYhtgUckRlNFWzl3C3wjFd3MZ65dZueNef7TWpTjfVYFGomHLfh1eAw+x9FAsEJnmm4gFXvyZZZ5vw4jkGp4ItkGpzGdE4lBBeC43NEjo5IP2yOweqUob9Hr3LjtjwXjF/F3ix7OnWgeE60At6TxFMBm2UsSJ8hFjIh7qt+vppjEsTzCpFRAbFtQavrYWBvMOo8TZ9HWMnQqTcJmMVmpUQwSYy1KAwj8u1bEVFQ9twFdrc5Lp/9RmtmYcti5XIYNkoYvNFGBlIzUDap0k+xN2yNcysTnIgcO+SIHGCorZGcCI4XWAdX4hpLMehXDNyEt8E/NM3BIljvtP9qMmo3+TNonUkD/aiC2s2g8ESoIncbDTe37aAV6u6amMLajeAc+L2eI4LRqxDWhDGt1/Lvl/m3Z6vnUqxsm034EkdDghxKT9cxZLaVuobDSWaJjYC54oQpomXOcYJAQoQkC5rOhFxAEnT9dswKMi8vl2FpRx2JC9HEz5lhN5pagi+4m+w/GwjNLiz/ugmEDnhiXG2RQWDeHnheInACgO/s/oZ9FYQtEVt2cHReMEXPAd+rOWnRLubA3t7L48dO89P2l22rcpDgJKmXbke5Xvhz9dPS/AkwF5syrggX+HuRpYAjeGayUvglFmgcyErIB2TeGbvDzwXjzAzIWmUl4CTnZZJu/7HTNNeXgiV1Zq5SeUHyVRp0O25ieOE6PA7H+DrOK95rIv0/Wo/CdXTcfz84ORi3ini5mmNyfrRLIGwT2I/Od4e2C5vtTPo36ETtoIT/Y8ZvDw8Hhz/gelfSRW81ZNwr+LPstxedXYXE/Cm7pElixuqIeAfIFl6NIH8hAxzJ+Fn5h2SUimPUMY++N+iTvTq17pWGW6FohcFZaL2Zr5THxuLlJme/xmbv3pFB339dsYWZGiSrif68pJWpHDoGc0V5YxisLm7Iy5a4kLn9IRgfO+b1HYHJ0JQcDQAA'
        $DefinitionBytes = [Convert]::FromBase64String($CompressedDefinition)
        $InputStream = [IO.MemoryStream]::new($DefinitionBytes)
        $ZipStream = [IO.Compression.GZipStream]::new($InputStream, [IO.Compression.CompressionMode]::Decompress)
        $Reader = [IO.StreamReader]::new($ZipStream, [Text.Encoding]::UTF8)
        try { $Definition = $Reader.ReadToEnd() }
        finally { $Reader.Dispose(); $ZipStream.Dispose(); $InputStream.Dispose() }
        Add-Type -TypeDefinition $Definition -ReferencedAssemblies 'System.Core.dll' -ErrorAction Stop
    }
    return [GargantuanQualification.FastNdisExport]::Export($Source, $Destination, $IfIndex)
}

if ($Action -eq 'Start') {
    AssertFarm32DiskReserve $EvidenceDir 2560MB
    $Status = GetTraceStatus
    if ($Status -notmatch 'There is no trace session currently in progress') {
        throw 'An existing Windows trace session is protected.'
    }
    foreach ($Artifact in @($Marker, $Etl, $Pcap, ($Pcap + '.pending'),
        (Join-Path $EvidenceDir 'farm32-capture-active.txt'),
        (Join-Path $EvidenceDir 'farm32-capture-summary.txt'))) {
        if (Test-Path -LiteralPath $Artifact) {
            throw "Farm32 capture artifact already exists: $Artifact"
        }
    }
    $Adapter = Get-NetAdapter -Name 'Ethernet 4'
    $IP = Get-NetIPAddress -InterfaceIndex $Adapter.ifIndex -AddressFamily IPv4 |
          Where-Object {$_.IPAddress -eq '10.253.3.2' -and $_.PrefixLength -eq 30}
    $If = Get-NetIPInterface -InterfaceIndex $Adapter.ifIndex -AddressFamily IPv4
    if ($Adapter.Status -ne 'Up' -or $Adapter.LinkSpeed -ne '10 Gbps' -or !$IP -or
        $If.NlMtu -ne 1500 -or $If.Dhcp -ne 'Disabled' -or
        $Adapter.MacAddress -ne '0C-42-A1-52-38-A8' -or $Adapter.ifIndex -ne 19) {
        throw 'Worker Mellanox configuration does not match the qualified direct link.'
    }
    $Guid = ([Guid]$Adapter.InterfaceGuid).ToString('B')
    $Owned = @{Etl=$Etl; Pcap=$Pcap; InterfaceGuid=$Guid; MiniportIfIndex=$Adapter.ifIndex;
               CapturePort=$CapturePort; CaptureLayers=@('NDIS physical miniport');
               TraceMaximumMiB=$TraceMaximumMiB; NoWrapThresholdMiB=$NoWrapThresholdMiB}
    $Owned | ConvertTo-Json | Set-Content -LiteralPath $Marker -Encoding UTF8
    $StartArguments = @('trace','start','capture=yes','capturetype=physical',
        "CaptureInterface=$Guid",'Ethernet.Type=IPv4','Protocol=17',
        'IPv4.Address=10.253.3.1','CaptureMultiLayer=no','PacketTruncateBytes=1518',
        'report=disabled','persistent=no','fileMode=single',"maxSize=$TraceMaximumMiB","traceFile=$Etl")
    InvokeNetsh $StartArguments | Write-Output
    $ActiveStatus = GetTraceStatus
    if (!$ActiveStatus.Contains($Etl)) { throw 'Windows trace did not become task-owned.' }
    $ActiveStatus | Set-Content -LiteralPath (Join-Path $EvidenceDir 'farm32-capture-active.txt')
} else {
    if (!(Test-Path -LiteralPath $Marker)) {
        if ($Action -eq 'Stop') { return }
        throw 'Farm32 capture ownership marker is missing.'
    }
    $Owned = Get-Content -Raw -LiteralPath $Marker | ConvertFrom-Json
    if ($Owned.Etl -ne $Etl -or $Owned.Pcap -ne $Pcap -or $Owned.MiniportIfIndex -ne 19 -or
        $Owned.TraceMaximumMiB -ne $TraceMaximumMiB -or
        $Owned.NoWrapThresholdMiB -ne $NoWrapThresholdMiB) {
        throw 'Capture ownership marker does not match the evidence directory.'
    }
    $Status = GetTraceStatus
    if ($Status -notmatch 'There is no trace session currently in progress') {
        if (!$Status.Contains($Owned.Etl)) { throw 'Current Windows trace differs from the task-owned session.' }
        if ($Action -eq 'Finalize') { throw 'Farm32 trace is still active; stop it before offline export.' }
        InvokeNetsh @('trace','stop') | Write-Output
    }
    if (Test-Path -LiteralPath $Owned.Etl) {
        # Single-file mode never wraps. A full file may have stopped recording
        # before the workload finished, so treat the reserve as incomplete.
        $EtlBytes = (Get-Item -LiteralPath $Owned.Etl).Length
        AssertTraceBelowBound $EtlBytes
        if ($Action -eq 'Finalize') {
            if (Test-Path -LiteralPath $Owned.Pcap) {
                throw 'Farm32 pcap already exists; refusing to replace it.'
            }
            if (Test-Path -LiteralPath (Join-Path $EvidenceDir 'farm32-capture-summary.txt')) {
                throw 'Farm32 loss summary already exists; refusing to replace it.'
            }
            AssertFarm32DiskReserve $EvidenceDir 1536MB
            AssertTraceHasNoLostEvents $Owned.Etl (Join-Path $EvidenceDir 'farm32-capture-summary.txt')
            $Pending = $Owned.Pcap + '.pending'
            if (Test-Path -LiteralPath $Pending) {
                throw 'Farm32 pcap export already has a pending artifact.'
            }
            try {
                $Count = if ($EtlBytes -ge 4MB) {
                    ExportNdisTraceFast $Owned.Etl $Pending ([uint32]$Owned.MiniportIfIndex)
                } else {
                    ExportNdisTrace $Owned.Etl $Pending ([uint32]$Owned.MiniportIfIndex)
                }
                Move-Item -LiteralPath $Pending -Destination $Owned.Pcap -ErrorAction Stop
                if ((Get-Item -LiteralPath $Owned.Pcap).Length -ge 1024MB) {
                    throw 'Farm32 pcap export exceeded its completeness size bound.'
                }
                "Exported $Count complete fiber miniport frames." | Write-Output
            } finally {
                if (Test-Path -LiteralPath $Pending) { Remove-Item -LiteralPath $Pending -Force }
            }
        }
    } else {
        throw 'Task-owned Windows trace ETL is missing.'
    }
}
