#requires -Version 7.0
# Parser-only recovery evidence tests. No endpoint process or capture is started.
$ErrorActionPreference = 'Stop'
$Runner = Join-Path $PSScriptRoot 'PhysicalGameSessionFarm.ps1'
$Tokens = $null
$Errors = $null
$Ast = [Management.Automation.Language.Parser]::ParseFile($Runner, [ref]$Tokens, [ref]$Errors)
if ($Errors.Count) { throw "farm parser syntax failed: $($Errors[0].Message)" }
$Needed = @('Get-Fields', 'Read-SharedLogLines', 'Get-Records', 'Get-RecoveryDiagnostics',
	'Test-RecoveryQuiescent', 'Assert-RecoveryRecords')
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
		$ServerError.Add("[Qualification:Recovery] event=cessation_barrier run=$RunId case=$Case journal_tail=$Tail")
		for ($Peer = 1; $Peer -le 32; $Peer++) {
			$ServerError.Add("[Runtime:Luau] [Qualification:Recovery] event=probe_ack case=$Case peer_slot=$Peer peer_generation=1")
			if ($Case -ne 'gameplay') {
				$ServerError.Add("[Runtime:Luau] [Qualification:Recovery] event=name_ack case=$Case peer_slot=$Peer peer_generation=1")
			}
		}
		$SampleFields = "outstanding=0 active_grants=0 scheduler_queued=0 native_queued=0 native_observed=32 feedback_observed=32 accepted=100 first_sent=100 acked=100 retired=100 terminal_release=0 journal_backlog=0 materialization_backlog=0 current_tail=$Tail retained=16 oldest=1 required=100 margin=99 journal_failures=0"
		$ServerError.Add("[Qualification:Recovery] event=sample run=$RunId case=$Case elapsed_us=19000000 $SampleFields")
		$ServerError.Add("[Qualification:Recovery] event=sample run=$RunId case=$Case elapsed_us=20000001 $SampleFields")
		$ServerError.Add("[Qualification:Recovery] event=reader run=$RunId case=$Case catalog=1 connection_slot=0 connection_generation=0 next_sequence=$Tail prepared=0 pending_relevance=0")
		for ($Peer = 1; $Peer -le 32; $Peer++) {
			$ServerError.Add("[Qualification:Recovery] event=reader run=$RunId case=$Case catalog=0 connection_slot=$Peer connection_generation=1 next_sequence=$Tail prepared=0 pending_relevance=0")
		}
		$ServerError.Add("[Qualification:Recovery] event=strict_snapshot run=$RunId case=$Case elapsed_us=20000001 cessation_tail=$Tail retained_work_bytes=NOT_MEASURED")
		$ServerError.Add("[Qualification:Recovery] event=strict_deadline_barrier run=$RunId case=$Case elapsed_us=20471000")
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
		@($Positive.Cases | Where-Object StrictConvergenceSufficientProof -ne 'MEASURED_PASS').Count -ne 0 -or
		@($Positive.Cases | Where-Object FixedServiceRecovery -ne 'MEASURED_PASS').Count -ne 0) {
		throw 'valid recovery trace failed strict sufficient proof'
	}
	$OriginalError = [IO.File]::ReadAllLines($Server.ErrorPath)
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
