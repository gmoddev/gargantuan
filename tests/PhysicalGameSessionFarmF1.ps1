#requires -Version 7.0
# Fixed native summaries retain every grant's verdict before ACK retirement and
# possible requalification. Replay identity/conservation; never infer F1 from ACK.
function Read-FarmF1Observation {
	param([Parameter(Mandatory)][string]$ServerRoot, [Parameter(Mandatory)][string]$RunManifestPath)
	$Manifest = Get-Content -LiteralPath $RunManifestPath -Raw | ConvertFrom-Json
	$Path = Join-Path $ServerRoot 'server.stdout.log'
	$File = Get-Item -LiteralPath $Path -ErrorAction Stop
	if ($File.Length -gt 16777216 -or @($Manifest.Nonces).Count -ne 32) { throw 'F1 bounded source or manifest invalid' }
	$Lines = @([IO.File]::ReadLines($Path))
	$Source = @($Lines | Where-Object { $_.StartsWith('[Qualification:FarmF1]') })
	$Ready = @($Lines | Where-Object { $_.StartsWith('[Qualification:Server] event=ready ') })
	$Connections = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	$Nonces = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	foreach ($Line in $Ready) {
		if ($Line -notmatch ' run=([^ ]+) nonce=([1-9][0-9]*) connection_slot=([1-9][0-9]*) connection_generation=([1-9][0-9]*)( |$)' -or
			$Matches[1] -cne $Manifest.RunId -or [string]$Matches[2] -cnotin @($Manifest.Nonces | ForEach-Object { [string]$_ }) -or
			-not $Nonces.Add($Matches[2]) -or -not $Connections.Add($Matches[3] + ':' + $Matches[4])) { throw 'F1 ready identity mismatch' }
	}
	$Numeric = @('samples','completed','qualified','completed_bytes','accepted','first_sent','acked','retired',
		'last_token','last_bytes','last_activated_us','last_first_us','last_completed_us',
		'max_running_byte_us','max_finite_shortfall_byte_us','failed','valid')
	$Expected = @('event','contract','run','connection') + $Numeric
	$Rows = [Collections.Generic.List[object]]::new()
	$Seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	foreach ($Line in $Source) {
		$Row = [ordered]@{}
		foreach ($Field in $Line.Substring('[Qualification:FarmF1]'.Length).Trim().Split(' ')) {
			if ($Field -notmatch '^([a-z_]+)=([^ =]+)$' -or $Row.Contains($Matches[1])) { throw 'malformed F1 receipt' }
			$Row[$Matches[1]] = $Matches[2]
		}
		if ($Row.Count -ne $Expected.Count -or @($Expected | Where-Object { -not $Row.Contains($_) }).Count -or
			$Row.event -cne 'peer' -or $Row.contract -cne 'finite_grant_v1' -or $Row.run -cne $Manifest.RunId -or
			-not $Connections.Contains($Row.connection) -or -not $Seen.Add($Row.connection)) { throw 'F1 schema or generation mismatch' }
		foreach ($Name in $Numeric) {
			[UInt64]$Value = 0
			if ($Row[$Name] -notmatch '^(0|[1-9][0-9]*)$' -or -not [UInt64]::TryParse($Row[$Name], [ref]$Value)) { throw 'invalid F1 counter' }
			$Row[$Name] = $Value
		}
		if ($Row.samples -eq 0 -or $Row.completed -eq 0 -or $Row.completed -ne $Row.qualified -or
			$Row.failed -ne 0 -or $Row.valid -ne 1 -or $Row.max_running_byte_us -gt 18025216000 -or
			$Row.max_finite_shortfall_byte_us -ne 0 -or $Row.last_token -eq 0 -or
			$Row.last_bytes -eq 0 -or $Row.last_bytes -gt 524288 -or $Row.last_activated_us -eq 0 -or
			$Row.last_first_us -lt $Row.last_activated_us -or $Row.last_completed_us -lt $Row.last_first_us -or
			([bigint]$Row.last_completed_us - [bigint]$Row.last_activated_us) * 16777216 -gt
				([bigint]101911296000 + [bigint]$Row.last_bytes * 1000000) -or
			[bigint]$Row.completed_bytes -lt [bigint]$Row.completed -or
			[bigint]$Row.completed_bytes -gt [bigint]$Row.completed * 524288 -or
			$Row.completed_bytes -ne $Row.accepted -or $Row.accepted -ne $Row.first_sent -or
			$Row.first_sent -ne $Row.acked -or $Row.acked -ne $Row.retired) { throw 'native finite-grant service or terminal conservation failed' }
		$Rows.Add($Row)
	}
	if ($Rows.Count -and ($Rows.Count -ne 32 -or $Connections.Count -ne 32)) { throw 'partial F1 evidence' }
	return [ordered]@{ Contract='finite_grant_v1'; RunId=[string]$Manifest.RunId;
		State=$(if ($Rows.Count -eq 32) { 'MEASURED_PASS' } else { 'NOT_MEASURED' });
		Observations=@($Rows.ToArray() | Sort-Object connection);
		SourceSha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant();
		AnalyzerSha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant() }
}

function Assert-FarmF1Admission {
	param($Observation, $Admission)
	if ($Observation.State -ceq 'NOT_MEASURED') { return }
	[bigint]$Accepted = 0; [bigint]$Retired = 0
	foreach ($Row in $Observation.Observations) { $Accepted += $Row.accepted; $Retired += $Row.retired }
	if ($Accepted -ne [bigint]$Admission.accepted -or $Retired -ne [bigint]$Admission.retired -or
		[bigint]$Admission.terminal_release -ne 0) { throw 'F1 aggregate differs from exact terminal admission' }
}
