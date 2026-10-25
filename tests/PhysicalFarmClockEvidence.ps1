# Offline receipt plumbing for the unchanged, probe-scoped native clock join.
function Read-FarmClockObservation {
	param([string]$ServerRoot, [string]$ClientRoot, [string]$RunManifestPath)
	$ScriptPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../tools/physical-qualifier/farm_clock_exchange.py'))
	$Python = @(Get-Command python -CommandType Application -ErrorAction Stop)[0]
	$Output = @(& $Python.Source $ScriptPath (Join-Path $ServerRoot 'evidence-sha256.json') `
		(Join-Path $ClientRoot 'evidence-sha256.json') $RunManifestPath 2>&1)
	if ($LASTEXITCODE -ne 0 -or $Output.Count -ne 1) {
		$Detail = ((@($Output) | ForEach-Object { [string]$_ }) -join ' ')
		if ($Detail.Length -gt 512) { $Detail = $Detail.Substring(0, 512) }
		throw "bounded clock join rejected indexed evidence: $Detail"
	}
	$Observation = [string]$Output[0] | ConvertFrom-Json -AsHashtable
	$Manifest = Get-Content -LiteralPath $RunManifestPath -Raw | ConvertFrom-Json -AsHashtable
	if ($Observation.Format -cne 'GargantuanFarm32NativeClockExchange' -or
		$Observation.Version -ne 1 -or $Observation.RunId -cne $Manifest.RunId -or
		$Observation.Status -cnotin @('BOUNDED_AT_PROBE', 'NOT_MEASURED') -or
		$Observation.Clients -ne 32 -or $Observation.Epochs -ne 5 -or
		$Observation.ProbesPerClientEpoch -ne 4 -or $Observation.Sources.Count -ne 33 -or
		$Observation.Samples.Count -ne $(if ($Observation.Status -ceq 'BOUNDED_AT_PROBE') { 640 } else { 0 }) -or
		$Observation.PhaseLongOffset -cne 'NOT_MEASURED' -or
		$Observation.OneWayLatency -cne 'NOT_MEASURED') {
		throw 'bounded clock observation identity, scope or dimensions invalid'
	}
	foreach ($Pin in @(
		@('ServerIndexSha256', (Join-Path $ServerRoot 'evidence-sha256.json')),
		@('ClientIndexSha256', (Join-Path $ClientRoot 'evidence-sha256.json')),
		@('RunManifestSha256', $RunManifestPath), @('AnalyzerSha256', $ScriptPath))) {
		if ($Observation[$Pin[0]] -cne (Get-FileHash -LiteralPath $Pin[1] -Algorithm SHA256).Hash.ToLowerInvariant()) {
			throw 'clock receipt source pin changed'
		}
	}
	return $Observation
}
