# Offline adoption of the bounded, hash-indexed native Character publication join.
# Both callers keep provider qualification separate from this diagnostic subset.

function Read-FarmPublicationObservation {
	param([string]$ServerRoot, [string]$ClientRoot,
		[System.Collections.IDictionary]$ServerIndex,
		[System.Collections.IDictionary]$ClientIndex,
		[string]$RunManifestPath, [string]$ScratchParent)
	$ServerMembers = @($ServerIndex.Files | Where-Object Name -CEQ 'publication-service.bin')
	$ClientMembers = @($ClientIndex.Files | Where-Object Name -CMatch '^publication-service-[0-9]+\.bin$')
	if ($ServerMembers.Count -eq 0 -and $ClientMembers.Count -eq 0) {
		return [ordered]@{ State = 'NOT_MEASURED'; Reason = 'indexed native Character publication traces absent' }
	}
	if ($ServerMembers.Count -ne 1 -or $ClientMembers.Count -ne 32) {
		throw 'role-local publication traces are incomplete'
	}
	$ScriptPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../tools/physical-qualifier/farm_publication_join.py'))
	if (-not (Test-Path -LiteralPath $ScriptPath -PathType Leaf)) {
		throw 'bounded publication join analyzer is missing'
	}
	$Python = @(Get-Command python -CommandType Application -ErrorAction Stop)[0]
	$ScratchParent = [IO.Path]::GetFullPath($ScratchParent)
	if (-not (Test-Path -LiteralPath $ScratchParent -PathType Container) -or
		(Get-Item -LiteralPath $ScratchParent).Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) {
		throw 'publication analysis scratch parent is missing or redirected'
	}
	$Scratch = Join-Path $ScratchParent ('farm-publication-analysis-' + [Guid]::NewGuid().ToString('N'))
	[void][IO.Directory]::CreateDirectory($Scratch)
	try {
		$Output = @(& $Python.Source $ScriptPath (Join-Path $ServerRoot 'evidence-sha256.json') `
			(Join-Path $ClientRoot 'evidence-sha256.json') $RunManifestPath $Scratch `
			--expected-clients 32 2>&1)
		$ExitCode = $LASTEXITCODE
	} finally {
		[IO.Directory]::Delete($Scratch, $false)
	}
	if ($ExitCode -ne 0 -or $Output.Count -ne 1) {
		$Detail = ((@($Output) | ForEach-Object { [string]$_ }) -join ' ')
		if ($Detail.Length -gt 512) { $Detail = $Detail.Substring(0, 512) }
		throw "bounded publication join rejected indexed role evidence: $Detail"
	}
	$Observation = [string]$Output[0] | ConvertFrom-Json -AsHashtable
	if ($Observation.Format -cne 'GargantuanFarmPublicationJoin' -or
		$Observation.Version -ne 1 -or $Observation.Status -cne 'ACCEPTED_STATE_CHAIN_OBSERVED' -or
		$Observation.Clients -ne 32 -or $Observation.DueCompleteness -cne 'OBSERVED' -or
		$Observation.CrossHostDueToHandled -cne 'NOT_MEASURED') {
		throw 'bounded publication join output is invalid'
	}
	$Observation['AnalyzerSha256'] = (Get-FileHash -LiteralPath $ScriptPath -Algorithm SHA256).Hash.ToLowerInvariant()
	$Observation['TraceParserSha256'] = (Get-FileHash -LiteralPath `
		(Join-Path ([IO.Path]::GetDirectoryName($ScriptPath)) 'farm_publication_trace.py') `
		-Algorithm SHA256).Hash.ToLowerInvariant()
	return $Observation
}
