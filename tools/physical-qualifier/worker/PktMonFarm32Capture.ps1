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
$CaptureProfile = 'Farm32Capture16GiB-v2'
$TraceMaximumMiB = 16384
$NoWrapThresholdMiB = 15360
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
    # Start reserves 16 GiB ETL + 16 GiB export + 2 GiB headroom. Finalize
    # reserves 16 GiB export + 1 GiB headroom in addition to the retained ETL.
    $Drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot([IO.Path]::GetFullPath($PathName)))
    if (-not $Drive.IsReady -or $Drive.AvailableFreeSpace -lt $RequiredBytes) {
        throw 'Farm32 evidence volume lacks the fixed capture/export reserve.'
    }
}

function WriteBlock([IO.BinaryWriter]$Writer, [uint32]$Type, [byte[]]$Body) {
    $Padding = (4 - ($Body.Length % 4)) % 4
    $Length = [uint32](12 + $Body.Length + $Padding)
    if ($Writer.BaseStream.Position + $Length -ge $NoWrapThresholdMiB * 1MB) {
        throw 'Farm32 pcap export reached its completeness size bound.'
    }
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
    # Stream one event and packet at a time. This versioned exporter rejects
    # a packet before it would reach the 15 GiB completeness threshold.
    # Conversion runs offline, after the separately bounded privileged Stop.
    if (-not ('GargantuanQualification.Farm32V2NdisExport' -as [type])) {
        $CompressedDefinition = 'H4sIAAAAAAACCrVWW1PbOBR+z684zcOOXYLXTqC7OwFmgEAns8DSBtoHhgdhK0SDI3kkOZfd8t/3SLKJDTa021mTIbF0rt/R+Y5yxfg9TNZK0/mwk1feghEj91wozWIVnCwo17gXfKYkofKZ5PivYafDyZyqjMQUPhJ5T7jOCf+Uk5RNWUw0Exz+6WT5XcpiUBoXYohTohScEjkf9L/0LxKmTlaZkBoFAZ9MsgXRFGLBlYZUoL9zsmLzfH601lTBPkS7Z/AeorC/U/8aNugrLU3El1IsGGaA2t1zFkuhxFRvf2U8EUu1fTEaT7YvSfxA9THJdC5pt26riFwiCoKnaxfVNWerk0zEsysWP2BcVsM8nC5hhGpXbE696I/fwh5E9hOWn3L3TwwguNaxH1gbCKf1WoPL+nIIeUU2E5HLmPbK5EYUq8Ut2D3IGdcw5prKKRZlzBO68gtkzWOtuVQNlOHwaWdBJHzKqVzjssnA1v5M3Ns1r3R5SfTsap3R4JSl1Lz0oPu+62/MWHE8LwsqFR0xSWN7CPZhSlJFN3LuJHnGqztcz9y6Rc+a8/0mtYnGeswLNROOW/BqcJj1c5HQ4Bj3NL2gy96TLfOY/cM4pkoFXyXT1GlMZkTS4EJwfI9gbw/6YXMMVqcM/Qi9yrVb8lwwfhV789jdiQPFc6IV8J4kngrYLGNB+kxjIRNwX/X95QyTAM8rRPYLiG0bW10PA3uHUedp+jzCSoZOvUnAPGxaSgTjxFiLwjCCb9+KiIKy5y6QIcx2+e43WjMPtixWLqfDRgmDN9rIqNTM8sDGT7E2bI1zIxMcixw7ZA92MNTWSI4FxwOsgytxjaUY9CsGbsLb4AtJc2oRrHfafzUZtZv8GbROpYF+v4LazaDwBETBHfLpzW07aIW6OyamsHYhOKP8Xs8QwehVCGvCmNZr+ffL/Nuz1TMplrbNxnyB4yVBDiUnq5hmtpW6hsMhs8QG1BxxYAq0zDlOIZqAkDAn6VTIOU2Crt+OWUHm5eEyLO2oI3Ehmvg5M+xGUkvwBXfD9rOB0OzCzTM7gdABT4yrDTIIzIcdz0sETgDqO7u/Yl8FYUvElh0cnRdM0XPA92pOWrSLObC19XL7sdP8tvll26ocJDhJ6qV7o1wv/Ln6aWkuEuZgE8YVcIG/51lKcQRPTVYKv8QcjVNYCvmAzDtld/h/zjgzA7JWWUlxkvMySbf+2Gma6wvBkjozV6m8IPkqDboVNzG8cBUehiP8O8wr3msi/d9bt8JVdNg/GhzvjFpFvFzNMDk/eksgbBPYjs7eDu0tbDYz6UfQidpBCf/HjD/s7g52v8P1W0kXvdWQca/gz7LfXnR2FRJzKbskSWLG6j54O8gWXo0gf4EBjmT8X7khGaViG3XMq+8N+rBVp9at0nAlWdObRcJHRBUXo+BSKGYvY1ul2YP92t36h1q4627wkMUkA+pu8OgmniHbMuSEsnM53q1Asb8p3OHkrZNvvWSt5XLRtnbQK8fIYublpjZ+jXUPDmDQ919XbGHQBslqQX5e0so8q2Z5eN4Zpq2LmwrZo1jI3H4XjI8d8/cv4QFStwgOAAA='
        $DefinitionBytes = [Convert]::FromBase64String($CompressedDefinition)
        $InputStream = [IO.MemoryStream]::new($DefinitionBytes)
        $ZipStream = [IO.Compression.GZipStream]::new($InputStream, [IO.Compression.CompressionMode]::Decompress)
        $Reader = [IO.StreamReader]::new($ZipStream, [Text.Encoding]::UTF8)
        try { $Definition = $Reader.ReadToEnd() }
        finally { $Reader.Dispose(); $ZipStream.Dispose(); $InputStream.Dispose() }
        # EventLogReader lives in System.Diagnostics.EventLog on PowerShell 7;
        # its actual assembly also preserves the Windows PowerShell 5 path.
        Add-Type -TypeDefinition $Definition -ReferencedAssemblies @(
            'System.Core.dll', [System.Diagnostics.Eventing.Reader.EventLogReader].Assembly.Location) -ErrorAction Stop
    }
    return [GargantuanQualification.Farm32V2NdisExport]::Export($Source, $Destination, $IfIndex)
}

if ($Action -eq 'Start') {
    AssertFarm32DiskReserve $EvidenceDir 34GB
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
    $Owned = @{Profile=$CaptureProfile; Etl=$Etl; Pcap=$Pcap; InterfaceGuid=$Guid; MiniportIfIndex=$Adapter.ifIndex;
               CapturePort=$CapturePort; CaptureLayers=@('NDIS physical miniport');
               TraceMaximumMiB=$TraceMaximumMiB; NoWrapThresholdMiB=$NoWrapThresholdMiB;
               PerformanceMetadataMerge=$false}
    $Owned | ConvertTo-Json | Set-Content -LiteralPath $Marker -Encoding UTF8
    $StartArguments = @('trace','start','capture=yes','capturetype=physical',
        "CaptureInterface=$Guid",'Ethernet.Type=IPv4','Protocol=17',
        'IPv4.Address=10.253.3.1','CaptureMultiLayer=no','PacketTruncateBytes=1518',
        # Preserve the packet logger's ETL instead of rewriting it to merge
        # optional performance metadata at Stop. V4's merged file contained
        # metadata buffers in place of six packet-buffer sequence numbers.
        'report=disabled','perfMerge=no','persistent=no','fileMode=single',"maxSize=$TraceMaximumMiB","traceFile=$Etl")
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
    if ($Owned.Profile -ne $CaptureProfile -or
        $Owned.Etl -ne $Etl -or $Owned.Pcap -ne $Pcap -or $Owned.MiniportIfIndex -ne 19 -or
        $Owned.TraceMaximumMiB -ne $TraceMaximumMiB -or
        $Owned.NoWrapThresholdMiB -ne $NoWrapThresholdMiB -or
        $Owned.PerformanceMetadataMerge -isnot [bool] -or $Owned.PerformanceMetadataMerge) {
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
            AssertFarm32DiskReserve $EvidenceDir 17GB
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
                if ((Get-Item -LiteralPath $Pending).Length -ge $NoWrapThresholdMiB * 1MB) {
                    throw 'Farm32 pcap export exceeded its completeness size bound.'
                }
                Move-Item -LiteralPath $Pending -Destination $Owned.Pcap -ErrorAction Stop
                "Exported $Count complete fiber miniport frames." | Write-Output
            } finally {
                if (Test-Path -LiteralPath $Pending) { Remove-Item -LiteralPath $Pending -Force }
            }
        }
    } else {
        throw 'Task-owned Windows trace ETL is missing.'
    }
}
