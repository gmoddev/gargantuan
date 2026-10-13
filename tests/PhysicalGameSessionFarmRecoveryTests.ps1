#requires -Version 7.0
# Parser-only recovery evidence tests. No endpoint process or capture is started.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'RecoveryCausalEvidence.ps1')
. (Join-Path $PSScriptRoot 'RecoveryCausalEvidenceFixture.ps1')
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
$QualificationSource = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '../src/host/server/PhysicalScaleQualification.hpp'))
$RecoveryStart = $QualificationSource.IndexOf('if (State == Stage::OverloadRecovery) {', [StringComparison]::Ordinal)
if ($RecoveryStart -lt 0) { throw 'missing recovery observation state' }
$RecoverySource = $QualificationSource.Substring($RecoveryStart)
$Position = 0
foreach ($Snippet in @(
	'StepCessationQuote();',
	'const auto Metrics = Session.GetMetrics();',
	'const auto Retention = ObserveRetention(Tick);',
	'const auto ObservedAt = std::chrono::steady_clock::now();',
	'ObservedAt - OverloadCeased).count();',
	'const bool SnapshotDue = !RecoverySnapshotWritten &&',
	'const bool DeadlineDue = (RecoverySnapshotWritten || SnapshotDue) && QuoteSealed &&',
	'if (SnapshotDue)',
	'if (DeadlineDue)')) {
	$Found = $RecoverySource.IndexOf($Snippet, $Position, [StringComparison]::Ordinal)
	if ($Found -lt 0) { throw "recovery observation can predate quote replay or metrics: $Snippet" }
	$Position = $Found + $Snippet.Length
}
$ReplayStart = $QualificationSource.IndexOf('void StepCessationQuote() {', [StringComparison]::Ordinal)
$ReplayEnd = $QualificationSource.IndexOf('[[nodiscard]] static std::string QuoteReasonToken', $ReplayStart, [StringComparison]::Ordinal)
if ($ReplayStart -lt 0 -or $ReplayEnd -lt $ReplayStart) { throw 'missing bounded cessation replay' }
$ReplaySource = $QualificationSource.Substring($ReplayStart, $ReplayEnd - $ReplayStart)
$AdvanceCount = [regex]::Matches($ReplaySource, 'AdvanceFrozenJournalQuote\(').Count
if ($AdvanceCount -ne 0 -or $ReplaySource -notmatch 'QuoteWorker->TryPop\(\)' -or
	$ReplaySource -notmatch 'Consumed < 64') {
	throw 'cessation reference must encode off Main and consume bounded metadata per tick'
}
$HostSource = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '../src/host/server/ServerHost.cpp'))
$SessionEnd = $HostSource.IndexOf('const auto SessionEnded = std::chrono::steady_clock::now();', [StringComparison]::Ordinal)
$QualificationStart = $HostSource.IndexOf('const auto QualificationStarted = std::chrono::steady_clock::now();', [StringComparison]::Ordinal)
$ScaleStep = $HostSource.IndexOf('ScaleQualification->Step(Runtime->GetSimulationTick(), {', [StringComparison]::Ordinal)
if ($SessionEnd -lt 0 -or $QualificationStart -le $SessionEnd -or $ScaleStep -le $QualificationStart -or
	$QualificationSource -notmatch 'quote_advance_attempted=' -or
	$QualificationSource -notmatch 'planning_us=' -or
	$QualificationSource -notmatch 'journal_records=' -or
	$QualificationSource -notmatch 'quote_complete=' -or
	$QualificationSource -notmatch 'capture_us=' -or
	$QualificationSource -notmatch 'quote_lag_records=') {
	throw 'farm-only per-tick source attribution lacks ordered Session/quote timing and progress'
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
				$ServerOutput.Add("[Qualification:Recovery] event=retention run=$RunId case=$Case opportunity=$Opportunity retained=16 oldest=1 required=100 margin=99 mutation_samples=$(16 * $Opportunity)")
			}
		}
		$ServerOutput.Add("[Qualification:Recovery] event=all_opportunities run=$RunId case=$Case opportunities=480 elapsed_us=8000000 tick=$($Tick + 480)")
		$RawBytes = if ($Case -eq 'gameplay') { 0 } else { 188743680 }
		$MutationSamples = if ($Case -eq 'gameplay') { 0 } else { 7680 }
		$CessationMicroseconds = 100000000 * ($CaseIndex + 1) + 8000001
		$NativeBaselineMicroseconds = $CessationMicroseconds + 500
		$ServerOutput.Add("[Qualification:Recovery] event=cessation run=$RunId case=$Case tick=$($Tick + 481) monotonic_us=$CessationMicroseconds journal_tail=$Tail raw_name_bytes=$RawBytes retained_high=16 minimum_retention_margin=99 retention_mutation_samples=$MutationSamples")
		$ServerError.Add("[Qualification:Recovery] event=quote_capture run=$RunId case=$Case status=READY reason=none native_baseline_us=$NativeBaselineMicroseconds tick=$($Tick + 481)")
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
		$ServerError.Add("[Qualification:Recovery] event=quote_result run=$RunId case=$Case contract=causal_fence_v1 status=PASS w_complete_upper_bytes=2464 quoted_frames=32 bound_us=20470537 reason=none tick=$($Tick + 482)")
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
		foreach ($Row in @(Save-RecoveryCausalFixture -Root $Root -RunId $RunId -Case $Case -Tail $Tail -CessationMicroseconds $CessationMicroseconds -NativeBaselineMicroseconds $NativeBaselineMicroseconds)) { $ServerError.Add($Row) }
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
	# Prospective recovery cohorts retain a slow successful first attempt and
	# establish service with a later complete ten-RPC/ten-Event cohort. The old
	# immediate-cohort evidence is tested unchanged below.
	$LegacyClients = @($Clients | ForEach-Object { ,([IO.File]::ReadAllLines($_.ErrorPath)) })
	$ReadyRows = @(0..31 | ForEach-Object { "[Qualification:Server] event=ready run=$RunId nonce=$($ExpectedNonces[$_]) connection_slot=$($_ + 1) connection_generation=1 player_id=$($_ + 1)" })
	$BeforeCohortOutput = [IO.File]::ReadAllLines($Server.OutputPath)
	[IO.File]::WriteAllLines($Server.OutputPath, @($ReadyRows) + @($BeforeCohortOutput))
	$CohortServerError = @($OriginalError | ForEach-Object {
		if ($_ -match 'event=probe_ack case=(gameplay|structural|mixed) ') {
			$CaseIndex = [array]::IndexOf($Cases, $Matches[1])
			$_ + " contract=bounded_recovery_cohort_v1 attempt=2 monotonic_us=$(100000000 * ($CaseIndex + 1) + 15000001)"
		} else { $_ }
	})
	[IO.File]::WriteAllLines($Server.ErrorPath, $CohortServerError)
	$CohortClients = @()
	for ($Slot = 0; $Slot -lt 32; $Slot++) {
		$Lines = [Collections.Generic.List[string]]::new()
		foreach ($CaseIndex in 0..2) {
			$Case = $Cases[$CaseIndex]; $Player = $Slot + 1
			$Origin = 1000000000000L + 100000000000L * $CaseIndex
			$Deadline = $Origin + 20000000000L
			$At = $Origin + 1000000L
			foreach ($Attempt in 1..2) {
				$Start = $At
				$Lines.Add("[Qualification:Recovery] event=cohort_start contract=bounded_recovery_cohort_v1 case=$Case player_id=$Player attempt=$Attempt recovery_started_clock_ns=$Origin deadline_clock_ns=$Deadline started_clock_ns=$Start client_tick=1")
				for ($Index = 1; $Index -le 10; $Index++) {
					$Latency = if ($Attempt -eq 1 -and $Index -eq 1) { 600000L } else { 1000L }
					$Finish = $At + $Latency * 1000
					$Sequence = ($Attempt - 1) * 10 + $Index
					$Lines.Add("[Qualification:Recovery] event=rpc_probe contract=bounded_recovery_cohort_v1 case=$Case player_id=$Player attempt=$Attempt index=$Index client_tick=1 started_clock_ns=$At ended_clock_ns=$Finish elapsed_us=$Latency ok=1")
					$Lines.Add("[Qualification:Recovery] event=event_offer contract=bounded_recovery_cohort_v1 case=$Case player_id=$Player attempt=$Attempt index=$Index sequence=$Sequence started_clock_ns=$Finish ok=1")
					$EventEnd = $Finish + 1000000
					$Lines.Add("[Qualification:Recovery] event=event_probe contract=bounded_recovery_cohort_v1 case=$Case player_id=$Player attempt=$Attempt index=$Index sequence=$Sequence started_clock_ns=$Finish ended_clock_ns=$EventEnd elapsed_us=1000 ok=1")
					$At = $Finish + 200000000
				}
				$Maximum = if ($Attempt -eq 1) { 600000 } else { 1000 }
				$Status = if ($Attempt -eq 1) { 'UNPROVEN' } else { 'QUALIFIED' }
				$Lines.Add("[Qualification:Recovery] event=cohort_end contract=bounded_recovery_cohort_v1 case=$Case player_id=$Player attempt=$Attempt recovery_started_clock_ns=$Origin deadline_clock_ns=$Deadline started_clock_ns=$Start ended_clock_ns=$At rpc_acks=10 rpc_errors=0 event_acks=10 event_errors=0 pending_events=0 rpc_p95_us=$Maximum rpc_p99_us=$Maximum rpc_max_us=$Maximum event_max_us=1000 status=$Status reason=none")
				$At += 1000000000L
			}
			$Lines.Add("[Qualification:Recovery] event=client_probes contract=bounded_recovery_cohort_v1 case=$Case player_id=$Player attempt=2 rpc_acks=10 rpc_errors=0 event_acks=10 rpc_p95_us=1000 rpc_p99_us=1000 rpc_max_us=1000 event_max_us=1000")
		}
		$Original = [IO.File]::ReadAllLines($Clients[$Slot].ErrorPath)
		$New = @($Original | Where-Object { $_ -notmatch 'event=client_probes ' }) + @($Lines)
		$CohortClients += ,$New
		[IO.File]::WriteAllLines($Clients[$Slot].ErrorPath, $New)
	}
	$CohortPass = Assert-RecoveryRecords -Server $Server -Clients $Clients -ExpectedNonces $ExpectedNonces -ExpectedConnections $Connections
	if ($CohortPass.State -ne 'MEASURED_PASS') { throw 'bounded recovery did not retain slow first cohort and qualify later service' }
	function Assert-CohortRejected([string[]]$Lines, [string]$Reason) {
		[IO.File]::WriteAllLines($Clients[0].ErrorPath, $Lines)
		$Rejected = $false
		try { [void](Assert-RecoveryRecords -Server $Server -Clients $Clients -ExpectedNonces $ExpectedNonces -ExpectedConnections $Connections) }
		catch { $Rejected = $true }
		if (-not $Rejected) { throw "invalid bounded cohort accepted: $Reason" }
		[IO.File]::WriteAllLines($Clients[0].ErrorPath, $CohortClients[0])
	}
	foreach ($Mutation in @('raw-gap', 'raw-duplicate', 'offer-gap', 'clock-retime', 'rpc-error', 'event-error', 'offer-error', 'summary-forgery',
		'pending', 'unknown-version', 'partial-version', 'origin-reset', 'attempt-gap', 'attempt-cap', 'cadence', 'overlap', 'early-summary', 'false-qualification', 'terminal-failure', 'duplicate-field')) {
		$Changed = [Collections.Generic.List[string]]::new()
		foreach ($Line in $CohortClients[0]) {
			$Row = $Line
			if ($Line -match 'case=gameplay ' -and $Line -match 'attempt=1 ') {
				if ($Mutation -eq 'raw-gap' -and $Line -match 'event=rpc_probe .* index=1 ') { continue }
				if ($Mutation -eq 'offer-gap' -and $Line -match 'event=event_offer .* index=1 ') { continue }
				if ($Mutation -eq 'clock-retime' -and $Line -match 'event=rpc_probe .* index=1 ') { $Row = $Row.Replace('elapsed_us=600000', 'elapsed_us=1') }
				if ($Mutation -eq 'rpc-error' -and $Line -match 'event=rpc_probe .* index=1 ') { $Row = $Row.Replace('ok=1', 'ok=0') }
				if ($Mutation -eq 'event-error' -and $Line -match 'event=event_probe .* index=1 ') { $Row = $Row.Replace('ok=1', 'ok=0') }
				if ($Mutation -eq 'offer-error' -and $Line -match 'event=event_offer .* index=1 ') { $Row = $Row.Replace('ok=1', 'ok=0') }
				if ($Mutation -eq 'pending' -and $Line -match 'event=cohort_end ') { $Row = $Row.Replace('pending_events=0', 'pending_events=1') }
				if ($Mutation -eq 'false-qualification' -and $Line -match 'event=cohort_end ') { $Row = $Row.Replace('status=UNPROVEN', 'status=QUALIFIED') }
				if ($Mutation -eq 'duplicate-field' -and $Line -match 'event=cohort_start ') { $Row += ' attempt=1' }
				if ($Mutation -eq 'raw-duplicate' -and $Line -match 'event=rpc_probe .* index=1 ') { $Changed.Add($Row) }
			}
			if ($Line -match 'case=gameplay ' -and $Line -match 'attempt=2 ') {
				if ($Mutation -eq 'attempt-gap') { $Row = $Row.Replace('attempt=2', 'attempt=3') }
				if ($Mutation -eq 'attempt-cap') { $Row = $Row.Replace('attempt=2', 'attempt=21') }
				if ($Mutation -eq 'origin-reset') { $Row = $Row.Replace('recovery_started_clock_ns=1000000000000', 'recovery_started_clock_ns=1000000000001') }
				if ($Mutation -cin @('cadence', 'overlap') -and $Line -match 'event=cohort_start ') { $Row = $Row -replace 'started_clock_ns=100[0-9]+ client_tick=', 'started_clock_ns=1000001000001 client_tick=' }
				if ($Mutation -eq 'summary-forgery' -and $Line -match 'event=client_probes ') { $Row = $Row.Replace('rpc_max_us=1000', 'rpc_max_us=1') }
			}
			if ($Mutation -eq 'unknown-version') { $Row = $Row.Replace('bounded_recovery_cohort_v1', 'bounded_recovery_cohort_v2') }
			if ($Mutation -eq 'partial-version' -and $Line -match 'event=rpc_probe .* case=gameplay .* attempt=1 index=1 ') { $Row = $Row.Replace('contract=bounded_recovery_cohort_v1 ', '') }
			if ($Mutation -eq 'early-summary' -and $Line -match 'event=cohort_start .* case=gameplay .* attempt=2 ') {
				$Changed.Add(@($CohortClients[0] | Where-Object { $_ -match 'event=client_probes .* case=gameplay ' })[0])
			}
			$Changed.Add($Row)
		}
		if ($Mutation -eq 'terminal-failure') { $Changed.Add('[Qualification:Recovery] event=recovery_failed contract=bounded_recovery_cohort_v1 case=gameplay player_id=1 attempt=2 reason=deadline') }
		if (($Changed -join "`n") -ceq ($CohortClients[0] -join "`n")) { throw "cohort negative did not change: $Mutation" }
		Assert-CohortRejected -Lines $Changed.ToArray() -Reason $Mutation
	}
	$ExactServerDeadline = @($CohortServerError | ForEach-Object {
		if ($_ -match 'event=probe_ack case=gameplay peer_slot=1 ') { $_.Replace('monotonic_us=115000001', 'monotonic_us=128000001') }
		elseif ($_ -match 'event=sample run=.* case=gameplay elapsed_us=19000000 ') { $_.Replace('elapsed_us=19000000', 'elapsed_us=20000000') }
		else { $_ }
	})
	[IO.File]::WriteAllLines($Server.ErrorPath, $ExactServerDeadline)
	if ((Assert-RecoveryRecords -Server $Server -Clients $Clients -ExpectedNonces $ExpectedNonces -ExpectedConnections $Connections).State -ne 'MEASURED_PASS') { throw 'exact immutable20s recovery ACK was rejected' }
	[IO.File]::WriteAllLines($Server.ErrorPath, @($ExactServerDeadline | ForEach-Object { if ($_ -match 'event=probe_ack case=gameplay peer_slot=1 ') { $_.Replace('monotonic_us=128000001', 'monotonic_us=128000002') } else { $_ } }))
	$DeadlineRejected = $false
	try { [void](Assert-RecoveryRecords -Server $Server -Clients $Clients -ExpectedNonces $ExpectedNonces -ExpectedConnections $Connections) } catch { $DeadlineRejected = $true }
	if (-not $DeadlineRejected) { throw 'immutable server20s deadline plus1 was accepted' }
	[IO.File]::WriteAllLines($Server.ErrorPath, $OriginalError)
	[IO.File]::WriteAllLines($Server.OutputPath, $BeforeCohortOutput)
	for ($Slot = 0; $Slot -lt 32; $Slot++) { [IO.File]::WriteAllLines($Clients[$Slot].ErrorPath, $LegacyClients[$Slot]) }
	$LegacyLate = @($LegacyClients[0] | ForEach-Object { if ($_ -match 'event=client_probes case=gameplay ') { $_.Replace('rpc_max_us=1000', 'rpc_max_us=600000') } else { $_ } })
	Assert-CohortRejected -Lines $LegacyLate -Reason 'legacy first late probe still fails unchanged'
	[IO.File]::WriteAllLines($Clients[0].ErrorPath, $LegacyClients[0])
	$TerminalStructuralSample = @($OriginalError | Where-Object {
		$_ -match 'event=sample run=.* case=structural elapsed_us=19000000 '
	})[0].Replace('elapsed_us=19000000', 'elapsed_us=20470530')
	$DelayedStructural = [Collections.Generic.List[string]]::new()
	foreach ($Line in $OriginalError) {
		if ($Line -match 'event=causal_result run=.* case=structural ') { $Line = $Line.Replace('prefix_converged_us=19000000', 'prefix_converged_us=20470530') }
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
	Assert-RecoveryQuoteRejected -Reason 'native baseline differs from capture' -Lines @($OriginalError | ForEach-Object {
		if ($_ -match 'event=quote_capture run=.* case=gameplay ') {
			$Changed = $_.Replace('native_baseline_us=108000501', 'native_baseline_us=108000502')
			if ($Changed -ceq $_) { throw 'native baseline mismatch fixture did not change' }
			$Changed
		} else { $_ }
	})
	$CausalPath = Join-Path $Root 'recovery-gameplay.tsv'
	$OriginalCausal = [IO.File]::ReadAllLines($CausalPath)
	$Downgraded = [string[]]$OriginalCausal.Clone()
	$Downgraded[0] = "format=GargantuanRecoveryCausalV1`trun=$RunId`tcase=gameplay"
	[IO.File]::WriteAllLines($CausalPath, $Downgraded)
	$Historical = Assert-RecoveryCausalEvidence -Path $CausalPath -RunId $RunId -Case gameplay `
		-ExpectedConnections $Connections -CessationMicroseconds 108000001 -JournalFence 10000
	if ($Historical.FormatVersion -ne 1) { throw 'historical V1 standalone replay was rejected' }
	$RejectedV1 = $false
	try { [void](Assert-RecoveryRecords -Server $Server -Clients $Clients `
		-ExpectedNonces $ExpectedNonces -ExpectedConnections $Connections) } catch { $RejectedV1 = $true }
	if (-not $RejectedV1) { throw 'new farm capture accepted V1 causal evidence downgrade' }
	[IO.File]::WriteAllLines($CausalPath, $OriginalCausal)
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
			$_.Replace('quoted_frames=32', 'quoted_frames=31')
		} else { $_ }
	})
	Assert-RecoveryQuoteRejected -Reason 'late structural convergence' -Lines @($OriginalError | ForEach-Object {
		if ($_ -match 'event=causal_result run=.* case=gameplay ') {
			$_.Replace('prefix_converged_us=19000000', 'prefix_converged_us=20480000')
		} else { $_ }
	})
	Assert-RecoveryQuoteRejected -Reason 'quote-delayed structural observation past exact bound' -Lines @(
		$OriginalError | ForEach-Object {
			if ($_ -match 'event=causal_result run=.* case=structural ') {
				$_.Replace('prefix_converged_us=19000000', 'prefix_converged_us=20470538')
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
		if ($_ -match 'event=sample run=.* case=mixed ') {
			$_.Replace('current_tail=30000', 'current_tail=30001')
		} else { $_ }
	})
	[IO.File]::WriteAllLines($Server.ErrorPath, $PostCessation)
	$LiveResult = Assert-RecoveryRecords -Server $Server -Clients $Clients `
		-ExpectedNonces $ExpectedNonces -ExpectedConnections $Connections
	if ($LiveResult.State -ne 'MEASURED_PASS') { throw 'later source work invalidated a completed finite baseline prefix' }
	[IO.File]::WriteAllLines($Server.ErrorPath, $OriginalError)
	$OriginalOutput = [IO.File]::ReadAllText($Server.OutputPath)
	# The same legal entry cadence can have shorter adjacent completion gaps
	# when mutation/retention work duration changes. No clock or host is run.
	$EntryRows = @([IO.File]::ReadAllLines($Server.OutputPath) | ForEach-Object {
		if ($_ -match 'event=structural_offer .* opportunity=([0-9]+) .* monotonic_us=([0-9]+)$') {
			$Opportunity = [int]$Matches[1]
			$Start = [long]$Matches[2] - 16667
			$Duration = if ($Opportunity % 2) { 1000L } else { 1L }
			$_ -replace 'monotonic_us=[0-9]+$', "cadence_boundary=opportunity_start_v1 opportunity_start_us=$Start monotonic_us=$($Start + $Duration)"
		} else { $_ }
	})
	[IO.File]::WriteAllLines($Server.OutputPath, $EntryRows)
	$EntryResult = Assert-RecoveryRecords -Server $Server -Clients $Clients `
		-ExpectedNonces $ExpectedNonces -ExpectedConnections $Connections
	if ($EntryResult.State -ne 'MEASURED_PASS') { throw 'legal entry cadence with variable completion duration was rejected' }
	function Assert-CadenceRejected {
		param([string[]]$Lines, [string]$Reason)
		[IO.File]::WriteAllLines($Server.OutputPath, $Lines)
		$Rejected = $false
		try { [void](Assert-RecoveryRecords -Server $Server -Clients $Clients `
			-ExpectedNonces $ExpectedNonces -ExpectedConnections $Connections) } catch { $Rejected = $true }
		if (-not $Rejected) { throw "invalid structural cadence accepted: $Reason" }
	}
	foreach ($Mutation in @('short-entry', 'missing-marker', 'missing-start', 'missing-both', 'unknown-marker', 'reversed-completion', 'overlap', 'regressed-start')) {
		$BadRows = @($EntryRows | ForEach-Object {
			if ($_ -match 'event=structural_offer .* case=structural opportunity=([12]) ') {
				$Opportunity = [int]$Matches[1]
				$Row = $_
				if ($Mutation -eq 'short-entry' -and $Opportunity -eq 2) { $Row = $Row.Replace('opportunity_start_us=200016667', 'opportunity_start_us=200016666') }
				if ($Mutation -eq 'missing-marker' -and $Opportunity -eq 2) { $Row = $Row.Replace('cadence_boundary=opportunity_start_v1 ', '') }
				if ($Mutation -eq 'missing-start' -and $Opportunity -eq 2) { $Row = $Row.Replace('opportunity_start_us=200016667 ', '') }
				if ($Mutation -eq 'missing-both' -and $Opportunity -eq 2) { $Row = $Row.Replace('cadence_boundary=opportunity_start_v1 opportunity_start_us=200016667 ', '') }
				if ($Mutation -eq 'unknown-marker' -and $Opportunity -eq 2) { $Row = $Row.Replace('opportunity_start_v1', 'opportunity_start_v2') }
				if ($Mutation -eq 'reversed-completion' -and $Opportunity -eq 1) { $Row = $Row.Replace('monotonic_us=200001000', 'monotonic_us=199999999') }
				if ($Mutation -eq 'overlap' -and $Opportunity -eq 1) { $Row = $Row.Replace('monotonic_us=200001000', 'monotonic_us=200016668') }
				if ($Mutation -eq 'regressed-start' -and $Opportunity -eq 2) { $Row = $Row.Replace('opportunity_start_us=200016667', 'opportunity_start_us=199999999') }
				$Row
			} else { $_ }
		})
		if (($BadRows -join "`n") -ceq ($EntryRows -join "`n")) { throw "cadence fixture did not change: $Mutation" }
		Assert-CadenceRejected -Lines $BadRows -Reason $Mutation
	}
	$LegacyShort = @($OriginalOutput -split '\r?\n' | ForEach-Object {
		if ($_ -match 'event=structural_offer .* case=structural opportunity=2 ') {
			$_.Replace('monotonic_us=200033334', 'monotonic_us=200033333')
		} else { $_ }
	})
	Assert-CadenceRejected -Lines $LegacyShort -Reason 'legacy completion gap 16666 remains rejected'
	[IO.File]::WriteAllText($Server.OutputPath, $OriginalOutput)
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
		[IO.File]::WriteAllText($Server.ErrorPath, $ErrorText + "`n")
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
	Assert-RetentionRejected -Reason 'missing mutation-point sample' -ErrorText ($OriginalError -join "`n") `
		-OutputText ($OriginalOutput.Replace('case=structural opportunity=1 retained=16 oldest=1 required=100 margin=99 mutation_samples=16',
			'case=structural opportunity=1 retained=16 oldest=1 required=100 margin=99 mutation_samples=15'))
	Assert-RetentionRejected -Reason 'cessation mutation count mismatch' -ErrorText ($OriginalError -join "`n") `
		-OutputText ($OriginalOutput.Replace('case=structural tick=2481 monotonic_us=208000001 journal_tail=20000 raw_name_bytes=188743680 retained_high=16 minimum_retention_margin=99 retention_mutation_samples=7680',
			'case=structural tick=2481 monotonic_us=208000001 journal_tail=20000 raw_name_bytes=188743680 retained_high=16 minimum_retention_margin=99 retention_mutation_samples=7679'))
	Assert-RetentionRejected -Reason 'forged 480-sample minimum' -ErrorText ($OriginalError -join "`n") `
		-OutputText ($OriginalOutput.Replace('case=structural tick=2481 monotonic_us=208000001 journal_tail=20000 raw_name_bytes=188743680 retained_high=16 minimum_retention_margin=99',
			'case=structural tick=2481 monotonic_us=208000001 journal_tail=20000 raw_name_bytes=188743680 retained_high=16 minimum_retention_margin=100'))
	Assert-RetentionRejected -Reason 'recovery sampled high-water mismatch' -OutputText $OriginalOutput `
		-ErrorText ([regex]::Replace(($OriginalError -join "`n"),
			'(event=sample run=[^\n]+ case=mixed elapsed_us=19000000 [^\n]*?retained_high=)16', '${1}15'))
	# A native check between printed recovery ticks can observe a lower margin
	# or higher retention count. Later printed samples retain those exact extrema.
	$SparseExtrema = [regex]::Replace(($OriginalError -join "`n"),
		'(event=sample run=[^\n]+ case=structural [^\n]*?retained_high=)16 minimum_retention_margin=99',
		'${1}17 minimum_retention_margin=98')
	if ($SparseExtrema -ceq ($OriginalError -join "`n")) { throw 'sparse native-extrema fixture did not change' }
	[IO.File]::WriteAllText($Server.ErrorPath, $SparseExtrema + "`n")
	[void](Assert-RecoveryRecords -Server $Server -Clients $Clients `
		-ExpectedNonces $ExpectedNonces -ExpectedConnections $Connections)
	# The exact zero-margin boundary is healthy; negative margins remain fatal.
	[IO.File]::WriteAllText($Server.ErrorPath, $SparseExtrema.Replace('minimum_retention_margin=98', 'minimum_retention_margin=0') + "`n")
	[void](Assert-RecoveryRecords -Server $Server -Clients $Clients `
		-ExpectedNonces $ExpectedNonces -ExpectedConnections $Connections)
	[IO.File]::WriteAllLines($Server.ErrorPath, $OriginalError)
	foreach ($Mutation in @(
		@('increasing cumulative minimum', 'minimum_retention_margin=98', 'minimum_retention_margin=99'),
		@('decreasing cumulative high', 'retained_high=17', 'retained_high=16'),
		@('cumulative high exceeds resource cap', 'retained_high=17', 'retained_high=16385'),
		@('negative cumulative minimum', 'minimum_retention_margin=98', 'minimum_retention_margin=-1'),
		@('minimum exceeds observed margin', 'minimum_retention_margin=98', 'minimum_retention_margin=100'),
		@('journal failure remains fatal', 'journal_failures=0', 'journal_failures=1'),
		@('current coverage arithmetic remains fatal', 'required=100 margin=99', 'required=100 margin=98'),
		@('current required history was evicted', 'oldest=1 required=100 margin=99', 'oldest=101 required=100 margin=99')
	)) {
		$Lines = @($SparseExtrema -split '\r?\n')
		$Candidates = @(0..($Lines.Count - 1) | Where-Object { $Lines[$_] -match 'event=sample .* case=structural ' })
		$Target = $Candidates[-1]
		$Lines[$Target] = $Lines[$Target].Replace($Mutation[1], $Mutation[2])
		Assert-RetentionRejected -Reason $Mutation[0] -OutputText $OriginalOutput -ErrorText ($Lines -join "`n")
	}
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
