#requires -Version 7.0
# Fixed local-client capture for one already staged Farm32 run. Invoke only from
# a locally approved adapter with a pinned dumpcap binary and dedicated root.
param(
    [Parameter(Mandatory=$true)][string]$EvidenceRoot,
    [Parameter(Mandatory=$true)][string]$RunId,
    [Parameter(Mandatory=$true)][string]$DumpcapPath,
    [Parameter(Mandatory=$true)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$DumpcapSha256
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$CaptureSeconds = 600
$CaptureProfile = 'Farm32Capture16GiB-v2'
$AutostopKilobytes = 16777216
$CompletenessBytes = 15GB
$ReservedBytes = 18GB
$RunGuid = [guid]::Empty
if (-not [guid]::TryParse($RunId, [ref]$RunGuid) -or $RunGuid.ToString('D') -cne $RunId) {
    throw 'Farm32 run ID must be a canonical UUID.'
}
$Root = [IO.Path]::GetFullPath($EvidenceRoot).TrimEnd([IO.Path]::DirectorySeparatorChar)
$VolumeRoot = [IO.Path]::GetPathRoot($Root)
if ($VolumeRoot.StartsWith('\\') -or -not [IO.Path]::IsPathFullyQualified($Root) -or
    $Root -eq $VolumeRoot.TrimEnd([IO.Path]::DirectorySeparatorChar)) {
    throw 'Farm32 client evidence root must be a local absolute path.'
}
$EvidenceDir = Join-Path $Root $RunId
if (-not (Test-Path -LiteralPath $EvidenceDir -PathType Container)) {
    throw 'Farm32 client run directory must already exist.'
}
foreach ($Candidate in @($Root, $EvidenceDir)) {
    $Current = [IO.Path]::GetPathRoot($Candidate)
    foreach ($Segment in $Candidate.Substring($Current.Length).Split([IO.Path]::DirectorySeparatorChar)) {
        if ($Segment.Length -eq 0) { continue }
        $Current = Join-Path $Current $Segment
        if ((Get-Item -LiteralPath $Current).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw 'Farm32 client evidence path contains a reparse point.'
        }
    }
}
$Pcap = Join-Path $EvidenceDir 'farm32-client-capture.pcapng'
$Marker = Join-Path $EvidenceDir 'farm32-client-capture.json'
$Output = Join-Path $EvidenceDir 'farm32-dumpcap-output.txt'
$ErrorFile = Join-Path $EvidenceDir 'farm32-dumpcap-error.txt'
foreach ($Artifact in @($Pcap, $Marker, $Output, $ErrorFile)) {
    if (Test-Path -LiteralPath $Artifact) { throw "Farm32 client capture artifact already exists: $Artifact" }
}
$Drive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($EvidenceDir))
if (-not $Drive.IsReady -or $Drive.AvailableFreeSpace -lt $ReservedBytes) {
    throw 'Farm32 client evidence volume lacks the fixed 18 GiB capture reserve.'
}
$Executable = [IO.Path]::GetFullPath($DumpcapPath)
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf) -or
    (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash -ine $DumpcapSha256) {
    throw 'Locally pinned dumpcap executable is missing or changed.'
}
$FiberAddress = @(Get-NetIPAddress -AddressFamily IPv4 | Where-Object {
    $_.IPAddress -eq '10.253.3.1' -and $_.PrefixLength -eq 30
})
if ($FiberAddress.Count -ne 1) { throw 'Expected local fiber IPv4 identity is unavailable.' }
$Adapter = Get-NetAdapter -InterfaceIndex $FiberAddress[0].InterfaceIndex
$Interface = Get-NetIPInterface -InterfaceIndex $Adapter.ifIndex -AddressFamily IPv4
if ($Adapter.Status -ne 'Up' -or $Adapter.LinkSpeed -ne '10 Gbps' -or
    $Interface.NlMtu -ne 1500 -or $Interface.Dhcp -ne 'Disabled') {
    throw 'Local fiber adapter does not match the qualified Farm32 capture identity.'
}
$Device = '\Device\NPF_' + ([guid]$Adapter.InterfaceGuid).ToString('B')
$MarkerValue = [ordered]@{
    Format = 'GargantuanFarm32Dumpcap'
    Version = 1
    Profile = $CaptureProfile
    RunId = $RunId
    Device = $Device
    Filter = 'udp port 39450 and host 10.253.3.2'
    DurationSeconds = $CaptureSeconds
    AutostopKilobytes = $AutostopKilobytes
    CompletenessBytes = $CompletenessBytes
    DumpcapSha256 = $DumpcapSha256.ToLowerInvariant()
    Pcap = $Pcap
}
$MarkerStream = [IO.File]::Open($Marker, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try {
    $MarkerBytes = [Text.Encoding]::UTF8.GetBytes(($MarkerValue | ConvertTo-Json -Depth 4))
    $MarkerStream.Write($MarkerBytes, 0, $MarkerBytes.Length)
} finally { $MarkerStream.Dispose() }
$Start = [Diagnostics.ProcessStartInfo]::new($Executable)
$Start.UseShellExecute = $false
$Start.CreateNoWindow = $true
$Start.RedirectStandardOutput = $true
$Start.RedirectStandardError = $true
foreach ($Argument in @('-i', $Device, '-f', $MarkerValue.Filter,
    '-a', "duration:$CaptureSeconds", '-a', "filesize:$AutostopKilobytes",
    '-w', $Pcap, '-q')) { [void]$Start.ArgumentList.Add($Argument) }
$Child = [Diagnostics.Process]::Start($Start)
if ($null -eq $Child) { throw 'Pinned dumpcap did not start.' }
try {
    $OutputTask = $Child.StandardOutput.ReadToEndAsync()
    $ErrorTask = $Child.StandardError.ReadToEndAsync()
    if (-not $Child.WaitForExit(($CaptureSeconds + 30) * 1000)) {
        $Child.Kill($true)
        throw 'Farm32 dumpcap exceeded its independent hard duration.'
    }
    [Threading.Tasks.Task]::WaitAll(@($OutputTask, $ErrorTask))
    if ($OutputTask.Result.Length -gt 16384 -or $ErrorTask.Result.Length -gt 16384) {
        throw 'Farm32 dumpcap diagnostics exceeded their bound.'
    }
    [IO.File]::WriteAllText($Output, $OutputTask.Result, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($ErrorFile, $ErrorTask.Result, [Text.UTF8Encoding]::new($false))
    if ($Child.ExitCode -ne 0) { throw "Farm32 dumpcap exited $($Child.ExitCode)." }
    if (-not (Test-Path -LiteralPath $Pcap -PathType Leaf)) { throw 'Farm32 client capture is missing.' }
    $Bytes = (Get-Item -LiteralPath $Pcap).Length
    if ($Bytes -le 0 -or $Bytes -ge $CompletenessBytes) {
        throw 'Farm32 client capture is empty or reached its completeness size bound.'
    }
    [pscustomobject]@{RunId=$RunId; Pcap=$Pcap; Bytes=$Bytes;
        Sha256=(Get-FileHash -LiteralPath $Pcap -Algorithm SHA256).Hash.ToLowerInvariant()}
} finally { $Child.Dispose() }
