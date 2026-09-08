param(
	[Parameter(Mandatory = $true)][string]$InputDirectory,
	[Parameter(Mandatory = $true)][string]$OutputFile
)
$ErrorActionPreference = 'Stop'

function Get-Distribution {
	param([double[]]$Values)
	if ($Values.Count -eq 0) { throw 'Memory analysis has no samples for a required phase' }
	$Ordered = @($Values | Sort-Object)
	return [ordered]@{
		Count = $Ordered.Count
		Minimum = $Ordered[0]
		Mean = ($Ordered | Measure-Object -Average).Average
		P50 = $Ordered[[math]::Floor(($Ordered.Count - 1) * 0.50)]
		P95 = $Ordered[[math]::Floor(($Ordered.Count - 1) * 0.95)]
		P99 = $Ordered[[math]::Floor(($Ordered.Count - 1) * 0.99)]
		Maximum = $Ordered[-1]
	}
}

function Get-Slope {
	param([object[]]$Samples, [string]$Property)
	$MeanX = ($Samples | ForEach-Object { [double]$_.ElapsedMs / 1000 } | Measure-Object -Average).Average
	$MeanY = ($Samples | ForEach-Object { [double]$_.$Property } | Measure-Object -Average).Average
	$Numerator = 0.0
	$Denominator = 0.0
	foreach ($Sample in $Samples) {
		$Delta = [double]$Sample.ElapsedMs / 1000 - $MeanX
		$Numerator += $Delta * ([double]$Sample.$Property - $MeanY)
		$Denominator += $Delta * $Delta
	}
	if ($Denominator -eq 0) { throw 'Memory analysis requires a nonzero observation duration' }
	return $Numerator / $Denominator
}

$Results = @()
foreach ($File in Get-ChildItem -LiteralPath $InputDirectory -Filter '*-memory.csv' -File | Sort-Object Name) {
	$Prefix = $File.FullName.Substring(0, $File.FullName.Length - '-memory.csv'.Length)
	$ServerLog = [System.IO.File]::ReadAllText("$Prefix-server.log")
	$PlayerComplete = (Test-Path -LiteralPath "$Prefix-player.log") -and
		([System.IO.File]::ReadAllText("$Prefix-player.log") -match '\[Content:OfficialHost\] ClientTimeline')
	$Cycles = @([regex]::Matches($ServerLog, '\[Content:Churn\] cycle=(\d+) elapsed_ms=(\d+)'))
	$DrainMatch = [regex]::Match($ServerLog, '\[Content:Churn\] FINAL_DRAIN elapsed_ms=(\d+)')
	if ($Cycles.Count -eq 0 -or -not $DrainMatch.Success -or $ServerLog -notmatch 'CONTENT_CHURN_OK') {
		$Results += [ordered]@{ Profile = $File.BaseName; Complete = $false }
		continue
	}
	$Samples = @(Import-Csv -LiteralPath $File.FullName | Where-Object { [double]$_.WorkingSetBytes -gt 0 })
	$FirstLoad = [regex]::Match($ServerLog, 'CONTENT_LIFECYCLE_FIRST_RESIDENT[^\r\n]*ElapsedMilliseconds=(\d+)')
	$Bootstrap = [regex]::Match($ServerLog, 'StartupWallMilliseconds=(\d+)')
	$Anchor = [regex]::Match($ServerLog, 'ClockAnchorUnixMilliseconds=(\d+)')
	$Aligned = $Anchor.Success -and $Samples[0].PSObject.Properties.Name -contains 'UnixMilliseconds'
	$Offset = 0.0
	if ($Aligned) {
		$Offset = [double]$Anchor.Groups[1].Value - ([double]$Samples[0].UnixMilliseconds - [double]$Samples[0].ElapsedMs)
	}
	$WarmEnd = [double]$Cycles[[math]::Floor($Cycles.Count / 2)].Groups[2].Value + $Offset
	$DrainStart = [double]$DrainMatch.Groups[1].Value + $Offset
	$Steady = @($Samples | Where-Object { [double]$_.ElapsedMs -ge $WarmEnd -and [double]$_.ElapsedMs -lt $DrainStart })
	$Drained = @($Samples | Where-Object { [double]$_.ElapsedMs -ge $DrainStart })
	$Quarter = [math]::Max(1, [math]::Floor($Steady.Count / 4))
	$Result = [ordered]@{
		Profile = $File.BaseName
		Complete = $true
		Coverage = $(if ($PlayerComplete) { 'Server churn and Player vertical' } else { 'Server churn only; no Player pass implied' })
		ServerChurnComplete = $true
		PlayerComplete = $PlayerComplete
		Cycles = $Cycles.Count
		Units = 'bytes; slopes are bytes/second'
		SteadyWindow = 'second half of completed churn cycles, before final drain'
		ClockAlignment = $(if ($Aligned) { 'Host and sampler aligned by Unix-millisecond anchor; sampling interval approximately 100 ms' } else { 'Historical unaligned host/sampler clocks: steady window approximate; startup milestone RSS unavailable' })
		HostToSamplerOffsetMs = $(if ($Aligned) { $Offset } else { $null })
		SteadyStartMs = $WarmEnd
		DrainStartMs = $DrainStart
		BaselineRss = [double]$Samples[0].WorkingSetBytes
		Rss = Get-Distribution @($Samples | ForEach-Object { [double]$_.WorkingSetBytes })
		WarmupRss = Get-Distribution @($Samples | Where-Object { [double]$_.ElapsedMs -lt $WarmEnd } | ForEach-Object { [double]$_.WorkingSetBytes })
		SteadyRss = Get-Distribution @($Steady | ForEach-Object { [double]$_.WorkingSetBytes })
		SteadyPrivate = Get-Distribution @($Steady | ForEach-Object { [double]$_.PrivateBytes })
		DrainedRss = Get-Distribution @($Drained | ForEach-Object { [double]$_.WorkingSetBytes })
		DrainedPrivate = Get-Distribution @($Drained | ForEach-Object { [double]$_.PrivateBytes })
		SteadyRssSlope = Get-Slope $Steady 'WorkingSetBytes'
		SteadyPrivateSlope = Get-Slope $Steady 'PrivateBytes'
		SteadyFirstQuarterRss = Get-Distribution @($Steady | Select-Object -First $Quarter | ForEach-Object { [double]$_.WorkingSetBytes })
		SteadyLastQuarterRss = Get-Distribution @($Steady | Select-Object -Last $Quarter | ForEach-Object { [double]$_.WorkingSetBytes })
		Threads = Get-Distribution @($Samples | ForEach-Object { [double]$_.Threads })
		Handles = Get-Distribution @($Samples | ForEach-Object { [double]$_.Handles })
	}
	foreach ($Milestone in @(@('PostBootstrapRss', $Bootstrap), @('PostFirstLoadRss', $FirstLoad))) {
		if ($Aligned -and $Milestone[1].Success) {
			$At = [double]$Milestone[1].Groups[1].Value + $Offset
			$Sample = $Samples | Where-Object { [double]$_.ElapsedMs -ge $At } | Select-Object -First 1
			$Result[$Milestone[0]] = [double]$Sample.WorkingSetBytes
		}
	}
	$Results += $Result
}
$Results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $OutputFile -Encoding utf8
$Results | ForEach-Object { "[Content:Memory] profile=$($_.Profile) complete=$($_.Complete) cycles=$($_.Cycles)" }
