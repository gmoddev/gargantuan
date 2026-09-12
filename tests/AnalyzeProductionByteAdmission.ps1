param([Parameter(Mandatory)][string]$CapacityLog, [Parameter(Mandatory)][string]$OutputFile)
$ErrorActionPreference = 'Stop'
function Read-Records([string]$Marker) {
    foreach ($Line in [System.IO.File]::ReadLines((Resolve-Path -LiteralPath $CapacityLog))) {
        if (-not $Line.StartsWith($Marker)) { continue }
        $Record = [ordered]@{}
        foreach ($Match in [regex]::Matches($Line, '(\w+)=([^\s]+)')) {
            $Number = 0.0
            if ([double]::TryParse($Match.Groups[2].Value, [Globalization.NumberStyles]::Float,
                [Globalization.CultureInfo]::InvariantCulture, [ref]$Number)) { $Record[$Match.Groups[1].Value] = $Number }
            else { $Record[$Match.Groups[1].Value] = $Match.Groups[2].Value }
        }
        [pscustomobject]$Record
    }
}
$Cases = @(Read-Records '[Network:CapacityResult]')
$Accounting = @(Read-Records '[Network:ProductionAdmission]')
if ($Cases.Count -ne 12 -or $Accounting.Count -ne 12) { throw 'Expected twelve complete production-accountant cases' }
$Results = foreach ($Rate in @(262144,524288,1048576,2097152,4194304,8388608)) {
    foreach ($Chunk in @(2000,524288)) {
        $Case = @($Cases | Where-Object {$_.profileRate -eq $Rate -and $_.chunk -eq $Chunk})
        $Admission = @($Accounting | Where-Object {$_.R -eq $Rate -and $_.chunk -eq $Chunk})
        if ($Case.Count -ne 1 -or $Admission.Count -ne 1) { throw 'Missing or duplicate profile/group case' }
        $Case = $Case[0]; $Admission = $Admission[0]
        if ($Case.productionAdmission -ne 1 -or $Case.overrideRate -ne 2*$Rate -or $Case.admissionPercent -ne 75 -or
            $Case.submitted -ne $Case.delivered -or $Case.messages -ne $Case.received -or
            $Case.structuralAdmitted -ne 1450000 -or $Admission.accepted -ne 1450000 -or
            $Case.requests -ne 60 -or $Case.responses -ne 60 -or $Case.eventResponses -ne 60 -or $Case.actionResponses -ne 60 -or
            $Admission.globalCreditHigh -gt 524288 -or $Admission.peerBacklogHigh -gt 1048640-262176) {
            throw 'Incomplete delivery, incorrect effective profile or breached accountant bound'
        }
        # Necessary capacity arithmetic only. This does not label a laboratory
        # probe distribution as certification of the official gameplay path.
        $Compatible = 1048640*1000 -le $Rate*150
        $SampleTargets = $Case.rpcP95Ms -le 150 -and $Case.rpcP99Ms -le 250 -and $Case.rpcMaxMs -le 500 -and
            $null -ne $Case.eventGapMaxMs -and $Case.eventGapMaxMs -le 250 -and $Case.actionMaxMs -le 250
        if ($Compatible -and -not $SampleTargets) { throw 'Capacity-compatible backend profile failed the selected probe targets' }
        [ordered]@{Rate=$Rate; BackendRate=2*$Rate; Chunk=$Chunk; CapacityCompatible=$Compatible;
            MeasuredProbeTargetsPass=$SampleTargets; Capacity=$Case; Admission=$Admission}
    }
}
[ordered]@{
    Method='Same production integer byte accountant; one GNS loopback peer; opaque complete messages, not GRPL or RemoteManager. 1,450,000 structural bytes; 60 requests of each reliable probe type. Backend=2R includes explicitly configured headroom; do not attribute all improvement against inherited 256 KiB/s to admission alone. No network capacity is inferred from loopback.'
    Results=@($Results); SourceLog=(Split-Path $CapacityLog -Leaf); SHA256=(Get-FileHash -LiteralPath $CapacityLog).Hash
} | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $OutputFile -Encoding utf8
Write-Output "[Network:ByteAdmissionAnalysis] cases=12 all-delivered=true output=$OutputFile"
