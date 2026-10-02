#requires -Version 7.0
# Parser-only recovery evidence tests. No endpoint process or capture is started.
$ErrorActionPreference = 'Stop'
$Runner = Join-Path $PSScriptRoot 'PhysicalGameSessionFarm.ps1'
$Tokens = $null
$Errors = $null
$Ast = [Management.Automation.Language.Parser]::ParseFile($Runner, [ref]$Tokens, [ref]$Errors)
if ($Errors.Count) { throw "farm parser syntax failed: $($Errors[0].Message)" }
$Needed = @('Get-Fields', 'Read-SharedLogLines', 'Get-Records', 'Get-RecoveryDiagnostics',
	'Test-RecoveryQuiescent', 'Test-RecoveryServiceHealthy',
	'Get-RecoveryUnsigned', 'Get-RecoveryCeilDiv',
	'Assert-RecoveryQuote', 'Assert-RecoveryRecords')
foreach ($Function in $Ast.FindAll({ param($Node)
	$Node -is [Management.Automation.Language.FunctionDefinitionAst]
}, $true)) {
	if ($Function.Name -in $Needed) { . ([scriptblock]::Create($Function.Extent.Text)) }
}
foreach ($Name in $Needed) {
	if (-not (Get-Command $Name -CommandType Function -ErrorAction SilentlyContinue)) {
		throw "missing recovery parser $Name"
	}
}
$RunId = '7c93e53d-0e0c-4b8d-8a3b-9a761a406ebd'
$ExpectedNonces = @(0..31 | ForEach-Object { [string](1000 + $_) })
$Connections = @(0..31 | ForEach-Object { "$(1 + $_):1" })
$Cases = @('gameplay', 'structural', 'mixed')
$Root = Join-Path ([IO.Path]::GetTempPath()) ('farm-recovery-parser-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($Root)
try {
	$ServerOutput = [Collections.Generic.List[string]]::new()
	$ServerError = [Collections.Generic.List[string]]::new()
	for ($Index = 0; $Index -lt 32; $Index++) {
		$ServerOutput.Add("[Qualification:Recovery] event=object run=$RunId index=$Index object_slot=$(100 + $Index) object_generation=1")
	}
	foreach ($CaseIndex in 0..2) {
		$Case = $Cases[$CaseIndex]
		$Tick = 1000 + 1000 * $CaseIndex
		$Tail = 10000 + 10000 * $CaseIndex
		$ServerOutput.Add("[Qualification:Recovery] event=case_start run=$RunId case=$Case tick=$Tick monotonic_us=$(100000000 * ($CaseIndex + 1))")
		for ($Peer = 1; $Peer -le 32; $Peer++) {
			$ServerError.Add("[Runtime:Luau] [Qualification:Recovery] event=ready_ack case=$Case peer_slot=$Peer peer_generation=1 count=$Peer")
			$ServerError.Add("[Runtime:Luau] [Qualification:Recovery] event=offered_ack case=$Case peer_slot=$Peer peer_generation=1 count=$Peer")
		}
		if ($Case -ne 'gameplay') {
			for ($Opportunity = 1; $Opportunity -le 480; $Opportunity++) {
				$Time = 100000000 * ($CaseIndex + 1) + $Opportunity * 16667
				$ServerOutput.Add("[Qualification:Recovery] event=structural_offer run=$RunId case=$Case opportunity=$Opportunity mutations=16 name_bytes=24576 tick=$($Tick + $Opportunity) monotonic_us=$Time")
				$ServerOutput.Add("[Qualification:Recovery] event=retention run=$RunId case=$Case opportunity=$Opportunity retained=16 oldest=1 required=100 margin=99")
			}
		}
		$ServerOutput.Add("[Qualification:Recovery] event=all_opportunities run=$RunId case=$Case opportunities=480 elapsed_us=8000000 tick=$($Tick + 480)")
		$RawBytes = if ($Case -eq 'gameplay') { 0 } else { 188743680 }
		$ServerOutput.Add("[Qualification:Recovery] event=cessation run=$RunId case=$Case tick=$($Tick + 481) monotonic_us=$(100000000 * ($CaseIndex + 1) + 8000001) journal_tail=$Tail raw_name_bytes=$RawBytes retained_high=16 minimum_retention_margin=99")
		$ServerError.Add("[Qualification:Recovery] event=quote_capture run=$RunId case=$Case status=READY reason=none tick=$($Tick + 481)")
		$ServerError.Add("[Qualification:Recovery] event=cessation_barrier run=$RunId case=$Case journal_tail=$Tail")
		for ($Peer = 1; $Peer -le 32; $Peer++) {
			$ServerError.Add("[Runtime:Luau] [Qualification:Recovery] event=probe_ack case=$Case peer_slot=$Peer peer_generation=1")
			if ($Case -ne 'gameplay') {
				$ServerError.Add("[Runtime:Luau] [Qualification:Recovery] event=name_ack case=$Case peer_slot=$Peer peer_generation=1")
			}
		}
		for ($Peer = 1; $Peer -le 32; $Peer++) {
			$ServerError.Add("[Qualification:Recovery] event=quote_peer run=$RunId case=$Case connection_slot=$Peer connection_generation=1 accepted_unretired_complete_bytes=0 future_complete_bytes=77 w_complete_upper_bytes=77")
		}
		$ServerError.Add("[Qualification:Recovery] event=quote_result run=$RunId case=$Case status=PASS w_complete_upper_bytes=2464 quoted_frames=32 audited_frames=32 audited_accepted_bytes=2464 bound_us=20470537 reason=none tick=$($Tick + 482)")
		$SampleFields = "outstanding=0 active_grants=0 scheduler_queued=0 native_queued=0 native_observed=32 feedback_observed=32 accepted=100 first_sent=100 acked=100 retired=100 terminal_release=0 journal_backlog=0 materialization_backlog=0 current_tail=$Tail retained=16 oldest=1 required=100 margin=99 retained_high=16 minimum_retention_margin=99 journal_failures=0"
		$ServerError.Add("[Qualification:Recovery] event=sample run=$RunId case=$Case elapsed_us=19000000 $SampleFields")
		$ServerError.Add("[Qualification:Recovery] event=sample run=$RunId case=$Case elapsed_us=20000001 $SampleFields")
		$ServerError.Add("[Qualification:Recovery] event=reader run=$RunId case=$Case catalog=1 connection_slot=0 connection_generation=0 next_sequence=$Tail prepared=0 pending_relevance=0")
		for ($Peer = 1; $Peer -le 32; $Peer++) {
			$ServerError.Add("[Qualification:Recovery] event=reader run=$RunId case=$Case catalog=0 connection_slot=$Peer connection_generation=1 next_sequence=$Tail prepared=0 pending_relevance=0")
		}
		$ServerError.Add("[Qualification:Recovery] event=strict_snapshot run=$RunId case=$Case elapsed_us=20000001 cessation_tail=$Tail retained_work_bytes=NOT_MEASURED")
		$ServerError.Add("[Qualification:Recovery] event=terminal_reader run=$RunId case=$Case catalog=1 connection_slot=0 connection_generation=0 next_sequence=$Tail prepared=0 pending_relevance=0")
		for ($Peer = 1; $Peer -le 32; $Peer++) {
			$ServerError.Add("[Qualification:Recovery] event=terminal_reader run=$RunId case=$Case catalog=0 connection_slot=$Peer connection_generation=1 next_sequence=$Tail prepared=0 pending_relevance=0")
		}
		$ServerError.Add("[Qualification:Recovery] event=strict_deadline_barrier run=$RunId case=$Case elapsed_us=20471000 bound_us=20470537")
	}
	$Server = [pscustomobject]@{ OutputPath = (Join-Path $Root 'server.stdout.log');
		ErrorPath = (Join-Path $Root 'server.stderr.log') }
	[IO.File]::WriteAllLines($Server.OutputPath, $ServerOutput)
	[IO.File]::WriteAllLines($Server.ErrorPath, $ServerError)
	$Clients = @(
		for ($Slot = 0; $Slot -lt 32; $Slot++) {
			$Output = [Collections.Generic.List[string]]::new()
			$ErrorLines = [Collections.Generic.List[string]]::new()
			foreach ($Case in $Cases) {
				$ExpectedRpc = if ($Case -eq 'structural') { 1 } else { 16 }
				$ExpectedEvents = if ($Case -eq 'structural') { 60 } else { 480 }
				$ErrorLines.Add("[Runtime:Luau] [Qualification:Recovery] event=client_offered case=$Case player_id=$(1 + $Slot) opportunities=480 rpc_completed=$ExpectedRpc rpc_errors=0 event_attempts=$ExpectedEvents event_offers=$ExpectedEvents event_acks=$ExpectedEvents")
				$ErrorLines.Add("[Runtime:Luau] [Qualification:Recovery] event=client_probes case=$Case player_id=$(1 + $Slot) rpc_acks=10 rpc_errors=0 event_acks=10 rpc_p95_us=1000 rpc_p99_us=1000 rpc_max_us=1000 event_max_us=1000")
				if ($Case -eq 'gameplay') { continue }
				$CaseIndex = if ($Case -eq 'structural') { 1 } else { 2 }
				for ($Index = 0; $Index -lt 32; $Index++) {
					$Opportunity = if ($Index -lt 16) { 479 } else { 480 }
					$Letter = [char]([int][char]'a' + (($CaseIndex * 7 + $Opportunity + $Index) % 26))
					$Output.Add("[Qualification:Client] event=name_object run_id=$RunId slot=$Slot nonce=$($ExpectedNonces[$Slot]) case=$Case index=$Index object_slot=$(100 + $Index) object_generation=1 name_bytes=24576 letter=$Letter")
				}
				$Output.Add("[Qualification:Client] event=names_observed run_id=$RunId slot=$Slot nonce=$($ExpectedNonces[$Slot]) case=$Case objects=32")
				$ErrorLines.Add("[Runtime:Luau] [Qualification:Recovery] event=client_names case=$Case player_id=$(1 + $Slot) objects=32 bytes_per_name=24576")
			}
			$Label = 'client-{0:D2}' -f $Slot
			$Client = [pscustomobject]@{ OutputPath = (Join-Path $Root "$Label.stdout.log");
				ErrorPath = (Join-Path $Root "$Label.stderr.log") }
			[IO.File]::WriteAllLines($Client.OutputPath, $Output)
			[IO.File]::WriteAllLines($Client.ErrorPath, $ErrorLines)
			$Client
		}
	)
	$Positive = Assert-RecoveryRecords -Server $Server -Clients $Clients `
		-ExpectedNonces $ExpectedNonces -ExpectedConnections $Connections
	if ($Positive.State -ne 'MEASURED_PASS' -or $Positive.Cases.Count -ne 3 -or
		$Positive.ExactRetainedWorkBytes -ne 7392 -or
		@($Positive.Cases | Where-Object ExactRetainedWorkBytes -ne 2464).Count -ne 0 -or
		@($Positive.Cases | Where-Object { $_.RetainedWorkBoundUs -lt 20470500 }).Count -ne 0 -or
		@($Positive.Cases | Where-Object StrictConvergenceSufficientProof -ne 'MEASURED_PASS').Count -ne 0 -or
		@($Positive.Cases | Where-Object FixedServiceRecovery -ne 'MEASURED_PASS').Count -ne 0) {
		throw 'valid recovery trace failed strict sufficient proof'
	}
	$RejectedHardTimeout = $false
	try { [void](Assert-RecoveryRecords -Server $Server -Clients $Clients `
		-ExpectedNonces $ExpectedNonces -ExpectedConnections $Connections `
		-MaximumRunMicroseconds 20470536L) } catch { $RejectedHardTimeout = $true }
	if (-not $RejectedHardTimeout) { throw 'retained-work deadline beyond hard run timeout was accepted' }
	$OriginalError = [IO.File]::ReadAllLines($Server.ErrorPath)
	$TerminalStructuralSample = @($OriginalError | Where-Object {
		$_ -match 'event=sample run=.* case=structural elapsed_us=19000000 '
	})[0].Replace('elapsed_us=19000000', 'elapsed_us=20470530')
	$DelayedStructural = [Collections.Generic.List[string]]::new()
	foreach ($Line in $OriginalError) {
		if ($Line -match 'event=sample run=.* case=structural elapsed_us=(19000000|20000001) ') {
			$DelayedStructural.Add($Line.Replace('outstanding=0', 'outstanding=77').Replace(
				'active_grants=0', 'active_grants=1').Replace('native_queued=0', 'native_queued=77').Replace(
				'first_sent=100 acked=100 retired=100', 'first_sent=23 acked=23 retired=23'))
		} else { $DelayedStructural.Add($Line) }
		if ($Line -match 'event=strict_snapshot run=.* case=structural ') {
			$DelayedStructural.Add($TerminalStructuralSample)
		}
	}
	[IO.File]::WriteAllLines($Server.ErrorPath, $DelayedStructural)
	$Delayed = Assert-RecoveryRecords -Server $Server -Clients $Clients `
		-ExpectedNonces $ExpectedNonces -ExpectedConnections $Connections
	if ($Delayed.State -ne 'MEASURED_PASS' -or
		$Delayed.Cases[1].FixedServiceRecovery -ne 'MEASURED_PASS' -or
		$Delayed.Cases[1].StrictConvergenceSufficientProof -ne 'MEASURED_PASS' -or
		$Delayed.Cases[1].ObservedStructuralConvergenceUs -ne 20470530) {
		throw 'positive W_i structural convergence after the fixed service snapshot was rejected'
	}
	[IO.File]::WriteAllLines($Server.ErrorPath, $OriginalError)
	function Assert-RecoveryQuoteRejected {
		param([string[]]$Lines, [string]$Reason)
		[IO.File]::WriteAllLines($Server.ErrorPath, $Lines)
		$Rejected = $false
		try { [void](Assert-RecoveryRecords -Server $Server -Clients $Clients `
			-ExpectedNonces $ExpectedNonces -ExpectedConnections $Connections) }
		catch { $Rejected = $true }
		if (-not $Rejected) { throw "invalid recovery quote was accepted: $Reason" }
		[IO.File]::WriteAllLines($Server.ErrorPath, $OriginalError)
	}
	Assert-RecoveryQuoteRejected -Reason 'missing capture' -Lines @($OriginalError |
		Where-Object { $_ -notmatch 'event=quote_capture run=.* case=gameplay ' })
	Assert-RecoveryQuoteRejected -Reason 'missing peer generation' -Lines @($OriginalError |
		Where-Object { $_ -notmatch 'event=quote_peer run=.* case=structural connection_slot=1 ' })
	Assert-RecoveryQuoteRejected -Reason 'nonpassing quote' -Lines @($OriginalError | ForEach-Object {
		if ($_ -match 'event=quote_result run=.* case=gameplay ') { $_.Replace('status=PASS', 'status=NOT_MEASURED') }
		else { $_ }
	})
	Assert-RecoveryQuoteRejected -Reason 'peer byte mismatch' -Lines @($OriginalError | ForEach-Object {
		if ($_ -match 'event=quote_peer run=.* case=mixed connection_slot=1 ') {
			$_.Replace('w_complete_upper_bytes=77', 'w_complete_upper_bytes=78')
		} else { $_ }
	})
	Assert-RecoveryQuoteRejected -Reason 'aggregate byte mismatch' -Lines @($OriginalError | ForEach-Object {
		if ($_ -match 'event=quote_result run=.* case=structural ') {
			$_.Replace('w_complete_upper_bytes=2464', 'w_complete_upper_bytes=2465')
		} else { $_ }
	})
	Assert-RecoveryQuoteRejected -Reason 'unaudited frame' -Lines @($OriginalError | ForEach-Object {
		if ($_ -match 'event=quote_result run=.* case=gameplay ') {
			$_.Replace('audited_frames=32', 'audited_frames=31')
		} else { $_ }
	})
	Assert-RecoveryQuoteRejected -Reason 'late structural convergence' -Lines @($OriginalError | ForEach-Object {
		if ($_ -match 'event=sample run=.* case=gameplay ') {
			$_.Replace('elapsed_us=19000000', 'elapsed_us=20480000').Replace(
				'elapsed_us=20000001', 'elapsed_us=20480000')
		} else { $_ }
	})
	$Moved = @($OriginalError | Where-Object { $_ -match 'event=name_ack case=structural peer_slot=32 ' })
	$Modified = [Collections.Generic.List[string]]::new()
	foreach ($Line in $OriginalError) {
		if ($Line -match 'event=name_ack case=structural peer_slot=32 ') { continue }
		$Modified.Add($Line)
		if ($Line -match 'event=strict_snapshot run=.* case=structural ') { $Modified.Add($Moved[0]) }
	}
	[IO.File]::WriteAllLines($Server.ErrorPath, $Modified)
	$Late = Assert-RecoveryRecords -Server $Server -Clients $Clients `
		-ExpectedNonces $ExpectedNonces -ExpectedConnections $Connections
	if ($Late.State -ne 'INCONCLUSIVE_NOT_MEASURED' -or
		$Late.Cases[1].StrictConvergenceSufficientProof -ne 'INCONCLUSIVE_NOT_MEASURED') {
		throw 'late final Name was falsely proven within strict bound'
	}
	[IO.File]::WriteAllLines($Server.ErrorPath, $OriginalError)
	$PostCessation = @($OriginalError | ForEach-Object {
		if ($_ -match 'event=sample run=.* case=mixed elapsed_us=19000000 ') {
			$_.Replace('current_tail=30000', 'current_tail=30001')
		} else { $_ }
	})
	[IO.File]::WriteAllLines($Server.ErrorPath, $PostCessation)
	$RejectedMutation = $false
	try { [void](Assert-RecoveryRecords -Server $Server -Clients $Clients `
		-ExpectedNonces $ExpectedNonces -ExpectedConnections $Connections) } catch { $RejectedMutation = $true }
	if (-not $RejectedMutation) { throw 'post-cessation structural mutation was accepted' }
	[IO.File]::WriteAllLines($Server.ErrorPath, $OriginalError)
	$OriginalOutput = [IO.File]::ReadAllText($Server.OutputPath)
	$BadMargin = [regex]::Replace($OriginalOutput,
		'(event=retention[^\r\n]*?margin=)99', '${1}-1',
		[Text.RegularExpressions.RegexOptions]::None, [timespan]::FromSeconds(1))
	if ($BadMargin -ceq $OriginalOutput) { throw 'negative-margin fixture did not change' }
	[IO.File]::WriteAllText($Server.OutputPath, $BadMargin)
	$RejectedMargin = $false
	try { [void](Assert-RecoveryRecords -Server $Server -Clients $Clients `
		-ExpectedNonces $ExpectedNonces -ExpectedConnections $Connections) } catch { $RejectedMargin = $true }
	if (-not $RejectedMargin) { throw 'sampled negative journal retention margin was accepted' }
	[IO.File]::WriteAllText($Server.OutputPath, $OriginalOutput)
	function Assert-RetentionRejected {
		param([string]$OutputText, [string]$ErrorText, [string]$Reason)
		if ($OutputText -ceq $OriginalOutput -and $ErrorText -ceq ($OriginalError -join "`n")) {
			throw "retention fixture did not mutate: $Reason"
		}
		[IO.File]::WriteAllText($Server.OutputPath, $OutputText)
		[IO.File]::WriteAllText($Server.ErrorPath, $ErrorText)
		$Rejected = $false
		try { [void](Assert-RecoveryRecords -Server $Server -Clients $Clients `
			-ExpectedNonces $ExpectedNonces -ExpectedConnections $Connections) }
		catch { $Rejected = $true }
		if (-not $Rejected) { throw "invalid retention evidence was accepted: $Reason" }
		[IO.File]::WriteAllText($Server.OutputPath, $OriginalOutput)
		[IO.File]::WriteAllLines($Server.ErrorPath, $OriginalError)
	}
	Assert-RetentionRejected -Reason 'reader margin arithmetic' -ErrorText ($OriginalError -join "`n") `
		-OutputText ($OriginalOutput.Replace('opportunity=1 retained=16 oldest=1 required=100 margin=99',
			'opportunity=1 retained=16 oldest=1 required=101 margin=99'))
	Assert-RetentionRejected -Reason 'forged 480-sample minimum' -ErrorText ($OriginalError -join "`n") `
		-OutputText ($OriginalOutput.Replace('case=structural tick=2481 monotonic_us=208000001 journal_tail=20000 raw_name_bytes=188743680 retained_high=16 minimum_retention_margin=99',
			'case=structural tick=2481 monotonic_us=208000001 journal_tail=20000 raw_name_bytes=188743680 retained_high=16 minimum_retention_margin=100'))
	Assert-RetentionRejected -Reason 'recovery sampled high-water mismatch' -OutputText $OriginalOutput `
		-ErrorText ([regex]::Replace(($OriginalError -join "`n"),
			'(event=sample run=[^\n]+ case=mixed elapsed_us=19000000 [^\n]*?retained_high=)16', '${1}15'))
	$OriginalClient = [IO.File]::ReadAllText($Clients[0].OutputPath)
	[IO.File]::WriteAllText($Clients[0].OutputPath, $OriginalClient.Replace('object_slot=100', 'object_slot=999'))
	$Rejected = $false
	try { [void](Assert-RecoveryRecords -Server $Server -Clients $Clients `
		-ExpectedNonces $ExpectedNonces -ExpectedConnections $Connections) } catch { $Rejected = $true }
	if (-not $Rejected) { throw 'wrong client ObjectId was accepted' }
} finally {
	$Resolved = [IO.Path]::GetFullPath($Root)
	$Temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
	if ($Resolved.StartsWith($Temp, [StringComparison]::OrdinalIgnoreCase)) {
		Remove-Item -LiteralPath $Root -Recurse -Force
	}
}
Write-Output '[Qualification:FarmRecovery] MOCK_TEST_OK'
