param(
    [Parameter(Mandatory=$true)][string]$EvidenceDir,
    [ValidateSet('Start','Stop')][string]$Action = 'Start',
    [ValidateSet(39450,39452)][int]$CapturePort = 39450
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Marker = Join-Path $EvidenceDir 'netsh-owner.json'
$Etl = Join-Path $EvidenceDir 'worker-capture.etl'
$Pcap = Join-Path $EvidenceDir 'worker-capture.pcapng'

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
                                      ([datetime]'1970-01-01T00:00:00Z').Ticks) / 10)
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

if ($Action -eq 'Start') {
    $Status = GetTraceStatus
    if ($Status -notmatch 'There is no trace session currently in progress') {
        throw 'An existing Windows trace session is protected.'
    }
    if (Test-Path -LiteralPath $Marker) { throw 'A capture ownership marker already exists.' }
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
               CapturePort=$CapturePort; CaptureLayers=@('NDIS physical miniport')}
    $Owned | ConvertTo-Json | Set-Content -LiteralPath $Marker -Encoding UTF8
    $StartArguments = @('trace','start','capture=yes','capturetype=physical',
        "CaptureInterface=$Guid",'Ethernet.Type=IPv4','Protocol=17',
        'IPv4.Address=10.253.3.1','CaptureMultiLayer=no','PacketTruncateBytes=1518',
        'report=disabled','persistent=no','fileMode=circular','maxSize=64',"traceFile=$Etl")
    InvokeNetsh $StartArguments | Write-Output
    $ActiveStatus = GetTraceStatus
    if (!$ActiveStatus.Contains($Etl)) { throw 'Windows trace did not become task-owned.' }
    $ActiveStatus | Set-Content -LiteralPath (Join-Path $EvidenceDir 'capture-active.txt')
} else {
    if (!(Test-Path -LiteralPath $Marker)) { return }
    $Owned = Get-Content -Raw -LiteralPath $Marker | ConvertFrom-Json
    if ($Owned.Etl -ne $Etl -or $Owned.Pcap -ne $Pcap -or $Owned.MiniportIfIndex -ne 19) {
        throw 'Capture ownership marker does not match the evidence directory.'
    }
    $Status = GetTraceStatus
    if ($Status -notmatch 'There is no trace session currently in progress') {
        if (!$Status.Contains($Owned.Etl)) { throw 'Current Windows trace differs from the task-owned session.' }
        InvokeNetsh @('trace','stop') | Write-Output
    }
    if (Test-Path -LiteralPath $Owned.Etl) {
        if (!(Test-Path -LiteralPath $Owned.Pcap)) {
            $Pending = $Owned.Pcap + '.pending'
            if (Test-Path -LiteralPath $Pending) { Remove-Item -LiteralPath $Pending -Force }
            try {
                $Count = ExportNdisTrace $Owned.Etl $Pending ([uint32]$Owned.MiniportIfIndex)
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
