param(
    [Parameter(Mandatory)][string]$CapacityLog,
    [Parameter(Mandatory)][string[]]$ByteLogs,
    [Parameter(Mandatory)][string]$OutputFile
)
$ErrorActionPreference = 'Stop'
function Read-Records([string]$Path, [string]$Marker) {
    foreach ($Line in [System.IO.File]::ReadLines((Resolve-Path -LiteralPath $Path))) {
        if (-not $Line.Contains($Marker)) { continue }
        $Record = [ordered]@{}
        foreach ($Match in [regex]::Matches($Line, '(\w+)=([^\s]+)')) {
            $Number = 0.0
            if ([double]::TryParse($Match.Groups[2].Value, [Globalization.NumberStyles]::Float,
                    [Globalization.CultureInfo]::InvariantCulture, [ref]$Number)) {
                $Record[$Match.Groups[1].Value] = $Number
            } else { $Record[$Match.Groups[1].Value] = $Match.Groups[2].Value }
        }
        [pscustomobject]$Record
    }
}
$Cases = @(Read-Records $CapacityLog '[Network:CapacityResult]')
if ($Cases.Count -ne 10) { throw 'Expected the complete ten-case rate/admission matrix' }
foreach ($Case in $Cases) {
    if ($Case.submitted -ne $Case.delivered -or $Case.messages -ne $Case.received -or
        $Case.structuralAdmitted -ne 1450000 -or $Case.requests -ne 60 -or $Case.responses -ne 60 -or
        $Case.eventResponses -ne 60 -or $Case.actionResponses -ne 60) { throw 'Incomplete reliable matrix case' }
}
if (@($Cases | Group-Object overrideRate,admissionPercent).Count -ne 10) { throw 'Duplicate matrix profile' }
foreach ($Rate in @(262144,524288,1048576,2097152,4194304)) {
    foreach ($Percent in @(50,75)) {
        if (@($Cases | Where-Object {$_.overrideRate -eq $Rate -and $_.admissionPercent -eq $Percent}).Count -ne 1) {
            throw 'Missing required rate/admission pair'
        }
    }
}
$Bytes = foreach ($Path in $ByteLogs) {
    $Metrics = @(Read-Records $Path '[Network:StructuralBytes]')
    $Totals = @(Read-Records $Path '[Network:StructuralByteTotals]')
    if ($Totals.Count -ne 4 -or $Metrics.Count -ne 32) { throw "Incomplete byte attribution: $Path" }
    foreach ($Phase in @('baseline','load','evict','reload')) {
        if (@($Totals | Where-Object phase -eq $Phase).Count -ne 1) { throw "Missing/duplicate phase: $Path" }
        foreach ($Metric in @('frame','operations','bytesPerOperation','flatEnterGroup','removalFrame',
                'allPeersReceiveStructuralPerTick','allPeersReceiveReliablePerTick','allPeersEncodedStructuralPerTick')) {
            if (@($Metrics | Where-Object {$_.phase -eq $Phase -and $_.metric -eq $Metric}).Count -ne 1) {
                throw "Missing/duplicate byte distribution: $Path"
            }
        }
    }
    [pscustomobject]@{File = Split-Path $Path -Leaf; Metrics = $Metrics; Totals = $Totals;
        Phases = @(Read-Records $Path '[Content:Scale] packageVersion=')}
}
$Report = [ordered]@{
    Method = 'Backend-only opaque 2000-byte groups; no official RemoteManager or production admission change. Byte counters observe canonical lossless simulated recipients; encoded counters are separately labelled. -1 denotes no samples.'
    Capacity = $Cases
    CanonicalBytes = @($Bytes)
    Artifacts = @(@($CapacityLog) + $ByteLogs | ForEach-Object {
        [pscustomobject]@{File = Split-Path $_ -Leaf; SHA256 = (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash}
    })
}
$Report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $OutputFile -Encoding utf8
Write-Output "[Network:EnvelopeAnalysis] cases=$($Cases.Count) fixtures=$($ByteLogs.Count) output=$OutputFile"
