#requires -Version 7.0
# Offline, bounded evidence of the actual farm Luau PostSimulation callback.
# Sixty-callback beats are timestamped by PlayerHost in the same process.
# This does not claim individual callback gaps or recipient due-service latency.
param(
	[Parameter(Mandatory = $true)][string]$RunId,
	[Parameter(Mandatory = $true)][string]$ClientEvidenceRoot,
	[Parameter(Mandatory = $true)][string]$ReportPath
)

$ErrorActionPreference = 'Stop'
if ($RunId -cnotmatch '^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$') { throw 'invalid farm run identity' }
$Root = [IO.Path]::GetFullPath($ClientEvidenceRoot)
$Directory = Get-Item -LiteralPath $Root -ErrorAction Stop
if (-not $Directory.PSIsContainer -or $Directory.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) {
	throw 'client evidence root is missing or redirected'
}
$Phases = @('baseline', 'load', 'resident', 'evict', 'reload')
$Clients = [Collections.Generic.List[object]]::new()
for ($Slot = 0; $Slot -lt 32; $Slot++) {
	$Path = Join-Path $Root ('client-{0:D2}.stdout.log' -f $Slot)
	$File = Get-Item -LiteralPath $Path -ErrorAction Stop
	if ($File.PSIsContainer -or $File.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint) -or
		$File.Length -gt 4194304) { throw "client $Slot callback log is redirected or oversized" }
	$Lines = [IO.File]::ReadAllLines($Path)
	$BeatLines = @($Lines | Where-Object { $_.StartsWith('[Qualification:Callback] event=beat ', [StringComparison]::Ordinal) })
	$PhaseLines = @($Lines | Where-Object { $_.StartsWith('[Qualification:Callback] event=phase_result ', [StringComparison]::Ordinal) })
	$ReadyLines = @($Lines | Where-Object { $_ -match '^\[Qualification:Client\] event=ready ' })
	if ($ReadyLines.Count -ne 1 -or $ReadyLines[0] -notmatch "run_id=$RunId slot=$Slot nonce=([1-9][0-9]*) ") {
		throw "client $Slot callback log lacks its run-scoped ready identity"
	}
	$Nonce = $Matches[1]
	if ($PhaseLines.Count -ne 5 -or $BeatLines.Count -lt 5 -or $BeatLines.Count -gt 600) {
		throw "client $Slot callback phase or beat count is incomplete"
	}
	$PhaseResults = [Collections.Generic.List[object]]::new()
	$PriorTotal = 0L
	for ($Index = 0; $Index -lt 5; $Index++) {
		if ($PhaseLines[$Index] -cnotmatch '^\[Qualification:Callback\] event=phase_result phase=([a-z]+) callbacks=([0-9]+) total=([0-9]+)$') {
			throw "client $Slot has malformed callback phase result"
		}
		$Name = $Matches[1]; $Count = [long]$Matches[2]; $Total = [long]$Matches[3]
		if ($Name -cne $Phases[$Index] -or $Count -le 0 -or $Total -ne $PriorTotal + $Count -or $Total -gt 36000) {
			throw "client $Slot callback phase sequence or conservation failed"
		}
		$PhaseResults.Add([ordered]@{ Phase = $Name; Callbacks = $Count; Total = $Total })
		$PriorTotal = $Total
	}
	$PriorBeat = 0L; $PriorTick = 0L; $PriorTime = 0L; $PriorPhase = 0
	$MaximumBatchNanoseconds = 0L
	$PhaseBeatCounts = [long[]]::new(5)
	foreach ($Line in $BeatLines) {
		if ($Line -cnotmatch '^\[Qualification:Callback\] event=beat run_id=([0-9a-f-]+) slot=([0-9]+) nonce=([0-9]+) phase=([a-z]+) callbacks=([0-9]+) simulation_tick=([0-9]+) steady_ns=([0-9]+)$') {
			throw "client $Slot has malformed callback beat"
		}
		$BeatRun = $Matches[1]; $BeatSlot = [int]$Matches[2]; $BeatNonce = $Matches[3]
		$BeatPhase = $Matches[4]; $Beat = [long]$Matches[5]
		$Tick = [long]$Matches[6]; $Time = [long]$Matches[7]
		$PhaseIndex = [array]::IndexOf($Phases, $BeatPhase)
		$PhaseFirst = if ($PhaseIndex -gt 0) { [long]$PhaseResults[$PhaseIndex - 1].Total } else { 0L }
		$PhaseLast = if ($PhaseIndex -ge 0) { [long]$PhaseResults[$PhaseIndex].Total } else { 0L }
		if ($BeatRun -cne $RunId -or $BeatSlot -ne $Slot -or $BeatNonce -cne $Nonce -or
			$PhaseIndex -lt 0 -or $PhaseIndex -lt $PriorPhase -or $Beat -ne $PriorBeat + 60 -or
			$Beat -le $PhaseFirst -or $Beat -gt $PhaseLast -or
			$Beat -gt $PriorTotal -or $Tick -le $PriorTick -or $Time -le $PriorTime) {
			throw "client $Slot callback beat identity, order, or count failed"
		}
		if ($PriorTime -gt 0) { $MaximumBatchNanoseconds = [Math]::Max($MaximumBatchNanoseconds, $Time - $PriorTime) }
		$PhaseBeatCounts[$PhaseIndex]++
		$PriorBeat = $Beat; $PriorTick = $Tick; $PriorTime = $Time; $PriorPhase = $PhaseIndex
	}
	if ($PriorBeat -ne 60 * [Math]::Floor($PriorTotal / 60) -or @($PhaseBeatCounts | Where-Object { $_ -eq 0 }).Count -ne 0) {
		throw "client $Slot lacks callback beats in every phase or a terminal beat"
	}
	$Clients.Add([ordered]@{
		Slot = $Slot; Nonce = $Nonce; CallbackCount = $PriorTotal
		BeatCount = $BeatLines.Count; BeatsPerPhase = $PhaseBeatCounts
		MaximumObservedSixtyCallbackBatchNanoseconds = $MaximumBatchNanoseconds
		Phases = @($PhaseResults)
	})
}
$Output = [ordered]@{
	Format = 'GargantuanFarmCallbackCadence'; Version = 1; RunId = $RunId
	Status = 'OBSERVED'; Classification = 'ACTUAL_POSTSIMULATION_CALLBACK_BEATS'
	IndividualCallbackIntervals = 'NOT_MEASURED'; RecipientDueToObserved = 'NOT_MEASURED'
	Clients = @($Clients)
}
$ReportPath = [IO.Path]::GetFullPath($ReportPath)
if ($ReportPath.StartsWith($Root.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar,
	[StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $ReportPath)) {
	throw 'callback report must be new and outside immutable client evidence'
}
$Bytes = [Text.UTF8Encoding]::new($false).GetBytes(($Output | ConvertTo-Json -Depth 8))
$Stream = [IO.File]::Open($ReportPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $Stream.Write($Bytes) } finally { $Stream.Dispose() }
Write-Output "[Qualification:Callback] OBSERVED run=$RunId clients=32 output=$ReportPath"
