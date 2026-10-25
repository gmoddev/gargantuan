param(
	[Parameter(Mandatory = $true)][string[]]$Path
)

$ErrorActionPreference = 'Stop'
$Culture = [System.Globalization.CultureInfo]::InvariantCulture

function Get-Distribution {
	param([double[]]$Values)
	if ($Values.Count -eq 0) { return $null }
	$Sorted = @($Values | Sort-Object)
	return [ordered]@{
		Count = $Sorted.Count
		Mean = ($Sorted | Measure-Object -Average).Average
		P50 = $Sorted[[int][math]::Floor(($Sorted.Count - 1) * 0.50)]
		P95 = $Sorted[[int][math]::Floor(($Sorted.Count - 1) * 0.95)]
		P99 = $Sorted[[int][math]::Floor(($Sorted.Count - 1) * 0.99)]
		Max = $Sorted[-1]
	}
}

$Results = foreach ($InputPath in $Path) {
	$Lines = @(Get-Content -LiteralPath $InputPath | Where-Object { $_.StartsWith('[Runtime:PlayerFrame] ') } |
		ForEach-Object { $_.Substring('[Runtime:PlayerFrame] '.Length) })
	if ($Lines.Count -lt 2 -or -not $Lines[0].StartsWith('unix_us,')) {
		throw "Missing Player frame trace: $InputPath"
	}
	# A forcibly terminated process can leave one partial final stdout row.
	# Do not turn it into a zero or include interleaved buffered SDL diagnostics.
	$FieldCount = $Lines[0].Split(',').Count
	$CompletePattern = '^\d+(,\d+){' + ($FieldCount - 1) + '}$'
	$CompleteLines = @($Lines | Select-Object -Skip 1 | Where-Object { $_ -match $CompletePattern })
	$Rows = @(@($Lines[0]) + $CompleteLines | ConvertFrom-Csv)
	$Measurements = [ordered]@{}
	foreach ($Field in @('interval_ns', 'event_gap_ns', 'poll_ns', 'engine_ns', 'session_ns', 'copy_ns',
		'semantic_ns', 'load_ns', 'load_validate_ns', 'construct_ns', 'parent_ns', 'properties_ns',
		'live_apply_ns', 'apply_total_ns', 'decode_ns', 'render_extract_ns', 'render_publish_ns')) {
		$Values = @($Rows | ForEach-Object { [double]::Parse($_.$Field, $Culture) / 1000000 })
		$Measurements[$Field.Replace('_ns', '_ms')] = Get-Distribution $Values
	}
	$Peak = $Rows | Sort-Object { [double]::Parse($_.poll_ns, $Culture) } -Descending | Select-Object -First 1
	[ordered]@{
		Path = (Resolve-Path -LiteralPath $InputPath).Path
		Frames = $Rows.Count
		IncompleteRows = $Lines.Count - 1 - $CompleteLines.Count
		TraceLimitReached = $Rows.Count -ge 4096
		FirstUnixMicroseconds = $Rows[0].unix_us
		LastUnixMicroseconds = $Rows[-1].unix_us
		StructuralBytes = ($Rows | Measure-Object -Property bytes -Sum).Sum
		StructuralFrames = ($Rows | Measure-Object -Property frames -Sum).Sum
		StructuralOperations = ($Rows | Measure-Object -Property operations -Sum).Sum
		CharacterMessages = if ($FieldCount -ge 27) { ($Rows | Measure-Object -Property character_messages -Sum).Sum } else { $null }
		RemoteMessages = if ($FieldCount -ge 27) { ($Rows | Measure-Object -Property remote_messages -Sum).Sum } else { $null }
		CharacterMaximumServiceGapMs = if ($FieldCount -ge 27) { [double]$Rows[-1].character_max_gap_ns / 1000000 } else { $null }
		RemoteMaximumServiceGapMs = if ($FieldCount -ge 27) { [double]$Rows[-1].remote_max_gap_ns / 1000000 } else { $null }
		Distributions = $Measurements
		WorstPollFrame = $Peak
		Complete = [bool](Select-String -LiteralPath $InputPath -SimpleMatch '[Content:OfficialHost] ClientTimeline' -Quiet)
	}
}
$Results | ConvertTo-Json -Depth 8
