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
		'qualified_accepted','qualified_bytes','bootstrap_accepted','bootstrap_bytes','bootstrap_retired','pending_token',
		'last_token','last_bytes','last_activated_us','last_first_us','last_completed_us',
		'max_running_byte_us','max_finite_shortfall_byte_us','failed','valid')
	$Expected = @('event','contract','run','connection') + $Numeric
	$HybridNumeric = @('completion_failed','last_completion_failed','completion_evidence_invalid','execution_failed',
		'main_opportunity_failed','turnover_failed','queued_failed','max_main_opportunity_us','max_turnover_us',
		'execution_opportunities','completed_turnovers','last_turnover_token','last_turnover_activated_us','last_turnover_us',
		'ordinary_registered','ordinary_completed','ordinary_pending','ordinary_max_latency_us',
		'ordinary_ever_failed','ordinary_last_completed_failed','ordinary_active_deadline_failed','ordinary_evidence_invalid',
		'ordinary_failure_sequence','ordinary_failure_phase','ordinary_failure_message','ordinary_failure_accepted_us',
		'ordinary_failure_completed_us','ordinary_failure_expected_bytes','ordinary_failure_first_sent_bytes')
	$Contract = $null
	$Rows = [Collections.Generic.List[object]]::new()
	$Seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	foreach ($Line in $Source) {
		$Row = [ordered]@{}
		foreach ($Field in $Line.Substring('[Qualification:FarmF1]'.Length).Trim().Split(' ')) {
			if ($Field -notmatch '^([a-z_]+)=([^ =]+)$' -or $Row.Contains($Matches[1])) { throw 'malformed F1 receipt' }
			$Row[$Matches[1]] = $Matches[2]
		}
		if (-not $Row.Contains('contract') -or $Row.contract -cnotin @('finite_grant_v1','HYBRID_F1_V1')) {
			throw 'unknown or missing F1 service contract'
		}
		$Hybrid = $Row.contract -ceq 'HYBRID_F1_V1'
		$RowExpected = @($Expected; if ($Hybrid) { $HybridNumeric })
		$RowNumeric = @($Numeric; if ($Hybrid) { $HybridNumeric })
		if ($null -eq $Contract) { $Contract = [string]$Row.contract }
		if ($Row.Count -ne $RowExpected.Count -or @($RowExpected | Where-Object { -not $Row.Contains($_) }).Count -or
			$Row.event -cne 'peer' -or $Row.contract -cne $Contract -or $Row.run -cne $Manifest.RunId -or
			-not $Connections.Contains($Row.connection) -or -not $Seen.Add($Row.connection)) { throw 'F1 schema or generation mismatch' }
		foreach ($Name in $RowNumeric) {
			[UInt64]$Value = 0
			if ($Row[$Name] -notmatch '^(0|[1-9][0-9]*)$' -or -not [UInt64]::TryParse($Row[$Name], [ref]$Value)) { throw 'invalid F1 counter' }
			$Row[$Name] = $Value
		}
		$ServiceFailed = if ($Hybrid) {
			$Row.completion_failed -ne 0 -or $Row.last_completion_failed -ne 0 -or $Row.completion_evidence_invalid -ne 0 -or
			$Row.execution_failed -ne 0 -or
			$Row.main_opportunity_failed -ne 0 -or $Row.turnover_failed -ne 0 -or $Row.queued_failed -ne 0 -or
			$Row.max_main_opportunity_us -gt 100000 -or $Row.max_turnover_us -gt 150000 -or
			$Row.execution_opportunities -eq 0 -or $Row.completed_turnovers -ne $Row.qualified_accepted -or
			$Row.last_turnover_token -ne $Row.last_token -or $Row.last_turnover_activated_us -ne $Row.last_activated_us -or
			$Row.last_turnover_us -lt $Row.last_completed_us -or
			[bigint]$Row.last_turnover_us - [bigint]$Row.last_turnover_activated_us -gt 150000 -or
			[bigint]$Row.last_turnover_us - [bigint]$Row.last_turnover_activated_us -gt [bigint]$Row.max_turnover_us -or
			$Row.ordinary_registered -eq 0 -or $Row.ordinary_completed -ne $Row.ordinary_registered -or $Row.ordinary_pending -ne 0 -or
			$Row.ordinary_max_latency_us -gt 50000 -or $Row.ordinary_ever_failed -ne 0 -or
			$Row.ordinary_last_completed_failed -ne 0 -or $Row.ordinary_active_deadline_failed -ne 0 -or $Row.ordinary_evidence_invalid -ne 0 -or
			$Row.ordinary_failure_sequence -ne 0 -or $Row.ordinary_failure_phase -ne 0 -or $Row.ordinary_failure_message -ne 0 -or
			$Row.ordinary_failure_accepted_us -ne 0 -or $Row.ordinary_failure_completed_us -ne 0 -or
			$Row.ordinary_failure_expected_bytes -ne 0 -or $Row.ordinary_failure_first_sent_bytes -ne 0 -or
			([bigint]$Row.last_completed_us - [bigint]$Row.last_activated_us) * 16777216 -gt [bigint]626199296000
		} else {
			$Row.max_running_byte_us -gt 18025216000 -or $Row.max_finite_shortfall_byte_us -ne 0 -or
			([bigint]$Row.last_completed_us - [bigint]$Row.last_activated_us) * 16777216 -gt
				([bigint]101911296000 + [bigint]$Row.last_bytes * 1000000)
		}
		if ($Row.samples -eq 0 -or $Row.completed -eq 0 -or $Row.completed -ne $Row.qualified -or
			$Row.failed -ne 0 -or $Row.valid -ne 1 -or $ServiceFailed -or $Row.last_token -eq 0 -or
			$Row.last_bytes -eq 0 -or $Row.last_bytes -gt 524288 -or $Row.last_activated_us -eq 0 -or
			$Row.last_first_us -lt $Row.last_activated_us -or $Row.last_completed_us -lt $Row.last_first_us -or
			[bigint]$Row.completed_bytes -lt [bigint]$Row.completed -or
			[bigint]$Row.completed_bytes -gt [bigint]$Row.completed * 524288 -or
			$Row.pending_token -ne 0 -or $Row.completed -ne $Row.qualified_accepted -or
			$Row.completed_bytes -ne $Row.qualified_bytes -or $Row.bootstrap_bytes -ne $Row.bootstrap_retired -or
			[bigint]$Row.bootstrap_bytes -lt [bigint]$Row.bootstrap_accepted -or
			[bigint]$Row.bootstrap_bytes -gt [bigint]$Row.bootstrap_accepted * 524288 -or
			[bigint]$Row.completed_bytes + [bigint]$Row.bootstrap_bytes -ne [bigint]$Row.accepted -or $Row.accepted -ne $Row.first_sent -or
			$Row.first_sent -ne $Row.acked -or $Row.acked -ne $Row.retired) { throw 'native finite-grant service or terminal conservation failed' }
		$Rows.Add($Row)
	}
	if ($Rows.Count -and ($Rows.Count -ne 32 -or $Connections.Count -ne 32)) { throw 'partial F1 evidence' }
	return [ordered]@{ Contract=$(if ($null -eq $Contract) { 'finite_grant_v1' } else { $Contract }); RunId=[string]$Manifest.RunId;
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

function Assert-FarmHybridService {
	param($Observation, $Publication, $Fairness)
	if ($Observation.Contract -cne 'HYBRID_F1_V1') { return } # Historical strict evidence retains its own gates.
	$FIFO = $Publication.OrdinaryFIFO
	if ($Observation.State -cne 'MEASURED_PASS' -or $Publication.OrdinaryDemand.State -cne 'MEASURED_PASS' -or
		$null -eq $FIFO -or $FIFO.State -cne 'MEASURED_PASS' -or $FIFO.Contract -cne 'HYBRID_F1_V1' -or
		$FIFO.RoleProcesses -ne 33 -or $FIFO.ServerConnections -ne 32 -or $FIFO.ClientCertificates -ne 32 -or
		$FIFO.Certificates -ne 64 -or $FIFO.Observations.Count -ne 64 -or $FIFO.MaximumLatencyMicroseconds -gt 50000 -or
		$FIFO.Boundary -cne 'SCHEDULER_ACCEPTED_TO_COMPLETE_POSITIVE_NATIVE_FIRST_SEND' -or
		$FIFO.Scope -cne 'ORIGINATING_FIXED_PHASES_1_TO_5' -or $Fairness.ServiceContract -cne $Observation.Contract) {
		throw 'hybrid native FIFO coverage, exact ordinary demand or versioned fairness conjunction is incomplete'
	}
}
