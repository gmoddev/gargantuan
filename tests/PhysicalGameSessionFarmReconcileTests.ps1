#requires -Version 7.0
# No process, network, capture, or physical application is started.
$ErrorActionPreference = 'Stop'
$Reconciler = Join-Path $PSScriptRoot 'PhysicalGameSessionFarmReconcile.ps1'
$Tokens = $null
$Errors = $null
[void][Management.Automation.Language.Parser]::ParseFile($Reconciler, [ref]$Tokens, [ref]$Errors)
if ($Errors.Count -ne 0) { throw "reconciler syntax failed: $($Errors[0].Message)" }

function Save-Json {
	param([string]$Path, $Value)
	[IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
}

function Save-Index {
	param([string]$Root, [string]$RunId, [string]$Role)
	$Files = @(Get-ChildItem -LiteralPath $Root -File | Where-Object Name -ne 'evidence-sha256.json' |
		Sort-Object Name | ForEach-Object {
			[ordered]@{ Name = $_.Name; Bytes = $_.Length;
				Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
	})
	Save-Json -Path (Join-Path $Root 'evidence-sha256.json') -Value ([ordered]@{
		RunId = $RunId; Role = $Role; Files = $Files
	})
}

function Assert-Rejected {
	param([string]$Name, [string]$ReportPath)
	$Rejected = $false
	try {
		& $Reconciler -RunManifestPath $ManifestPath -ManifestSha256 $ManifestSha256 `
			-ServerEvidenceRoot $ServerRoot -ClientEvidenceRoot $ClientRoot -ReportPath $ReportPath |
			Out-Null
	} catch { $Rejected = $true }
	if (-not $Rejected -or (Test-Path -LiteralPath $ReportPath)) { throw "$Name was accepted" }
}

$TestRoot = Join-Path ([IO.Path]::GetTempPath()) ('physical-farm-reconcile-test-' + [Guid]::NewGuid().ToString('N'))
$ResolvedTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$ResolvedRoot = [IO.Path]::GetFullPath($TestRoot)
if (-not $ResolvedRoot.StartsWith($ResolvedTemp, [StringComparison]::OrdinalIgnoreCase)) {
	throw 'test scratch root escaped the temporary directory'
}
[void][IO.Directory]::CreateDirectory($TestRoot)
$ServerRoot = Join-Path $TestRoot 'server-evidence'
$ClientRoot = Join-Path $TestRoot 'client-evidence'
[void][IO.Directory]::CreateDirectory($ServerRoot)
[void][IO.Directory]::CreateDirectory($ClientRoot)
$RunId = '7c93e53d-0e0c-4b8d-8a3b-9a761a406ebd'
$ManifestPath = Join-Path $TestRoot 'run-manifest.json'
try {
	$NoncePrefix = [UInt64]123 -shl 32
	$Nonces = @(0..31 | ForEach-Object { [string]($NoncePrefix + [UInt64]($_ + 1)) })
	$HashPin = 'a' * 64
	$Manifest = [ordered]@{
		Format = 'GargantuanPhysicalFarmEndpoint'; Version = 1; RunId = $RunId
		SourceCommit = ('b' * 40)
		Endpoint = '10.253.3.2:39450'; Provider = 'Local'; ScaleWorkload = $true
		ClientFrames = 9000; ServerTicks = 10000; Nonces = $Nonces
		ServerSha256 = $HashPin; ServerPackageSha256 = $HashPin
		PlayerSha256 = $HashPin; PlayerPackageSha256 = $HashPin
		ServerContentManifestSha256 = $HashPin; PlayerContentManifestSha256 = $HashPin
		ServerDeploymentSha256 = $HashPin; PlayerDeploymentSha256 = $HashPin
	}
	Save-Json -Path $ManifestPath -Value $Manifest
	$ManifestSha256 = (Get-FileHash -LiteralPath $ManifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
	[IO.File]::Copy($ManifestPath, (Join-Path $ServerRoot 'run-manifest.json'))
	[IO.File]::Copy($ManifestPath, (Join-Path $ClientRoot 'run-manifest.json'))
	$ServerLines = [Collections.Generic.List[string]]::new()
	$ServerLines.Add("[Qualification:Server] event=start run=$RunId provider=local expected=32")
	for ($Slot = 0; $Slot -lt 32; $Slot++) {
		$ServerLines.Add("[Qualification:Server] event=ready run=$RunId nonce=$($Nonces[$Slot]) connection_slot=$($Slot + 1) connection_generation=1 session_epoch=7 player_id=$($Slot + 1) monotonic_us=$($Slot + 1)")
	}
	$ServerLines.Add("[Qualification:Server] event=result run=$RunId provider=local expected=32 ready_high_water=32 unique_ready=32 identity_conflict=0 exit=0")
	$Names = @('baseline', 'load', 'resident', 'evict', 'reload')
	for ($Index = 0; $Index -lt 5; $Index++) {
		$Phase = $Names[$Index]
		$Tick = 1 + 1000 * $Index
		$TimeUs = 1000000 + 14000000 * $Index
		$ServerLines.Add("[Qualification:Scale] event=phase_start run=$RunId phase=$Phase tick=$Tick monotonic_us=$TimeUs")
		$ServerLines.Add("[Qualification:Scale] event=phase_acks run=$RunId phase=$Phase count=32 tick=$($Tick + 1)")
		$ServerLines.Add("[Qualification:Scale] event=content_acks run=$RunId phase=$Phase count=32 tick=$($Tick + 2)")
		$ServerLines.Add("[Qualification:Scale] event=phase_stop_requested run=$RunId phase=$Phase tick=$($Tick + 3) elapsed_us=13000001")
		$ServerLines.Add("[Qualification:Scale] event=producer_ack run=$RunId phase=$Phase tick=$($Tick + 4)")
		$ServerLines.Add("[Qualification:Scale] event=phase_end run=$RunId phase=$Phase ticks=781 elapsed_us=13000001 monotonic_us=$($TimeUs + 13000001) producer_done=1 phase_acks=32 content_acks=32 tick=$($Tick + 781)")
	}
	$ServerLines.Add("[Qualification:Scale] event=result run=$RunId status=PASS phases=5 peers=32 tick=5001")
	$ServerLines.Add("[Qualification:Admission] event=result run=$RunId accepted=8192 retired=8192 terminal_release=0 outstanding=0 outstanding_high=2048 active_grants=0 grants_high=2 grant_deferrals=3 funded_deferrals=2 credit_deferrals=1 fairness_deferrals=1 max_wait_us=1000 peer_backlog_high=2048 global_backlog_high=8192 peer_credit_high=2048 global_credit_high=8192 fairness_rotations=1 pending_enters=0 pending_leaves=0 materialization_backlog=0 journal_backlog=0 structural_active_peers=0 oldest_pending_ticks=0 backlog_failures=0 journal_failures=0")
	$FairnessPath = Join-Path $ServerRoot 'admission-fairness.tsv'
	$FairnessLines = @(
		"format=GargantuanAdmissionEvidenceV1`trun=$RunId",
		(@('event', 'exact_demand', 'none', 1, 1, 1, 0, 0, 8192, 1000, 0, 0, 0, 0, 0, 0, 0, 0, 0) -join "`t"),
		(@('event', 'credit_eligible', 'none', 1, 1, 1, 1, 0, 8192, 1100, 1050, 1100, 8192, 8192, 0, 0, 0, 0, 0) -join "`t"),
		(@('event', 'grant_accepted', 'none', 1, 1, 1, 1, 1, 8192, 1300, 1050, 1100, 0, 0, 1, 0, 0, 0, 0) -join "`t"),
		"end`t3`t0"
	)
	[IO.File]::WriteAllText($FairnessPath, ($FairnessLines -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
	$FairnessBytes = ([IO.FileInfo]$FairnessPath).Length
	$ServerLines.Add("[Qualification:Admission] event=evidence_result run=$RunId file=admission-fairness.tsv events=3 bytes=$FairnessBytes overflow=0 write_failed=0")
	[IO.File]::WriteAllLines((Join-Path $ServerRoot 'server.stdout.log'), $ServerLines)
	[IO.File]::WriteAllText((Join-Path $ServerRoot 'server.stderr.log'), '')
	$ResourceUtc = [DateTimeOffset]::UtcNow.ToString('O')
	$ServerRows = @(
		[pscustomobject]@{ RunId = $RunId; Label = 'server'; Pid = 777;
			Utc = $ResourceUtc; SupervisorElapsedMilliseconds = 1000
			MonotonicTicks = 1000000; MonotonicFrequency = 1000000
			WorkingSetBytes = 1000000; PrivateBytes = 900000;
			CpuMilliseconds = 1; Threads = 4; Handles = 16 },
		[pscustomobject]@{ RunId = $RunId; Label = 'server'; Pid = 777;
			Utc = $ResourceUtc; SupervisorElapsedMilliseconds = 3000
			MonotonicTicks = 3000000; MonotonicFrequency = 1000000
			WorkingSetBytes = 1000000; PrivateBytes = 900000;
			CpuMilliseconds = 2; Threads = 4; Handles = 16 }
	)
	$ServerRows | Export-Csv -LiteralPath (Join-Path $ServerRoot 'process-resources.csv') -NoTypeInformation
	$ClientRows = [Collections.Generic.List[object]]::new()
	for ($Slot = 0; $Slot -lt 32; $Slot++) {
		$Nonce = $Nonces[$Slot]
		$RootSlot = 100 + $Slot
		$Lines = [Collections.Generic.List[string]]::new()
		$Lines.Add("[Qualification:Client] event=start run_id=$RunId slot=$Slot nonce=$Nonce steady_ns=1")
		$Lines.Add("[Qualification:Client] event=ready run_id=$RunId slot=$Slot nonce=$Nonce connection_slot=$($Slot + 2) connection_generation=1 steady_ns=2")
		foreach ($Index in 0..4) {
			$Phase = $Names[$Index]
			$Objects = if ($Phase -in @('load', 'resident', 'reload')) { 512 } else { 0 }
			$SlotValue = if ($Objects) { $RootSlot } else { 0 }
			$Generation = if ($Phase -eq 'reload') { 2 } elseif ($Objects) { 1 } else { 0 }
			$Lines.Add("[Qualification:Client] event=phase_observed run_id=$RunId slot=$Slot nonce=$Nonce phase=$Phase objects=$Objects root_slot=$SlotValue root_generation=$Generation receive_to_observed_us=1000")
		}
		$Lines.Add("[Qualification:Client] event=scale_complete run_id=$RunId slot=$Slot nonce=$Nonce observed_phases=5 producer_phases=$(if ($Slot -eq 0) { 5 } else { 0 })")
		if ($Slot -eq 0) {
			foreach ($Phase in $Names) {
				$Lines.Add("[Qualification:Producer] event=phase_metrics run_id=$RunId slot=0 nonce=$Nonce phase=$Phase status=PASS metrics_phase_valid=1 producer_healthy=1 remote_samples=100 remote_p95_us=1000 remote_p99_us=2000 remote_max_us=3000 remote_errors=0 remote_timeouts=0 event_offers=1 event_acks=1 event_outstanding=0 event_max_rtt_us=1000 event_max_gap_us=1000 action_requests=1 action_resolutions=1 action_endings=1 action_max_result_us=1000 submission_failures=0 action_rejections=0 unexpected_endings=0")
			}
		}
		$Lines.Add("[Qualification:Client] event=result run_id=$RunId slot=$Slot nonce=$Nonce status=PASS exit_code=0 reason=scale_complete local_player=1 character=1 steady_ns=3")
		$Label = 'client-{0:D2}' -f $Slot
		[IO.File]::WriteAllLines((Join-Path $ClientRoot "$Label.stdout.log"), $Lines)
		[IO.File]::WriteAllText((Join-Path $ClientRoot "$Label.stderr.log"), '')
		$ClientRows.Add([pscustomobject]@{ RunId = $RunId; Label = $Label; Pid = 800 + $Slot;
			Utc = $ResourceUtc; SupervisorElapsedMilliseconds = 1000
			MonotonicTicks = 1000000 + $Slot; MonotonicFrequency = 1000000
			WorkingSetBytes = 1000000; PrivateBytes = 900000;
			CpuMilliseconds = 1; Threads = 4; Handles = 16 })
	}
	$ClientRows | Export-Csv -LiteralPath (Join-Path $ClientRoot 'process-resources.csv') -NoTypeInformation
	foreach ($Role in @('Server', 'Clients')) {
		$Root = if ($Role -eq 'Server') { $ServerRoot } else { $ClientRoot }
		$Samples = if ($Role -eq 'Server') { 2 } else { 32 }
		Save-Json -Path (Join-Path $Root 'result.json') -Value ([ordered]@{
			RunId = $RunId; Role = $Role; Status = 'PASS'; Provider = 'Local'
			Endpoint = $Manifest.Endpoint; ManifestSha256 = $ManifestSha256
			ResourceSamples = $Samples
		})
		Save-Index -Root $Root -RunId $RunId -Role $Role
	}
	$ReportPath = Join-Path $TestRoot 'valid-report.json'
	& $Reconciler -RunManifestPath $ManifestPath -ManifestSha256 $ManifestSha256 `
		-ServerEvidenceRoot $ServerRoot -ClientEvidenceRoot $ClientRoot -ReportPath $ReportPath |
		Out-Null
	$Report = Get-Content -LiteralPath $ReportPath -Raw | ConvertFrom-Json
	if ($Report.Status -ne 'INCOMPLETE' -or $Report.ProviderQualification -ne 'NOT CLAIMED' -or
		$Report.Identity.Ready -ne 32 -or $Report.Admission.accepted -ne 8192 -or
		$Report.AdmissionFairnessObservation.Classification -cne 'EVIDENCE_INTEGRITY_AND_OBSERVED_TIMING_ONLY' -or
		$Report.AdmissionFairnessObservation.MaximumObservedEligibilityToGrantMicroseconds -ne 200 -or
		$Report.AdmissionFairnessObservation.GrantedCount -ne 1 -or
		@($Report.Ledger | Where-Object Gate -eq 'Fairness, backlog and backpressure' |
			Where-Object State -eq 'NOT MEASURED').Count -ne 1 -or
		$Report.MissingGateCount -lt 7 -or
		@($Report.Ledger | Where-Object Gate -eq 'Exact accepted/retired/terminal/debt/grant/journal conservation' |
			Where-Object State -eq 'MEASURED').Count -ne 1) {
		throw 'valid role-local evidence was promoted to provider qualification or lost missing gates'
	}
	$OriginalFairness = [IO.File]::ReadAllText($FairnessPath)
	[IO.File]::WriteAllText($FairnessPath, $OriginalFairness.Replace("credit_eligible`tnone`t1`t1`t1`t1", "credit_eligible`tnone`t1`t1`t1`t2"))
	Assert-Rejected -Name 'tampered immutable fairness evidence' -ReportPath (Join-Path $TestRoot 'tampered-fairness-report.json')
	Save-Index -Root $ServerRoot -RunId $RunId -Role 'Server'
	Assert-Rejected -Name 'rehash of semantically invalid fairness episode' `
		-ReportPath (Join-Path $TestRoot 'bad-fairness-episode-report.json')
	[IO.File]::WriteAllText($FairnessPath, $OriginalFairness)
	Save-Index -Root $ServerRoot -RunId $RunId -Role 'Server'
	$Manifest.Provider = 'Node'
	$Manifest.NodeEndpoint = 'node.example.test:443'
	$Manifest.NodeRootCertificateSha256 = $HashPin
	$Manifest.NodeTokenEnvironment = 'GARGANTUAN_ENGINE_ADAPTER_TOKEN'
	Save-Json -Path $ManifestPath -Value $Manifest
	$ManifestSha256 = (Get-FileHash -LiteralPath $ManifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
	$ServerPath = Join-Path $ServerRoot 'server.stdout.log'
	[IO.File]::WriteAllText($ServerPath, ([IO.File]::ReadAllText($ServerPath)).Replace('provider=local', 'provider=node'))
	foreach ($Root in @($ServerRoot, $ClientRoot)) {
		[IO.File]::Copy($ManifestPath, (Join-Path $Root 'run-manifest.json'), $true)
		$RoleResultPath = Join-Path $Root 'result.json'
		$RoleResult = Get-Content -LiteralPath $RoleResultPath -Raw | ConvertFrom-Json -AsHashtable
		$RoleResult.Provider = 'Node'
		$RoleResult.ManifestSha256 = $ManifestSha256
		Save-Json -Path $RoleResultPath -Value $RoleResult
		Save-Index -Root $Root -RunId $RunId -Role $RoleResult.Role
	}
	$NodeReportPath = Join-Path $TestRoot 'valid-node-report.json'
	& $Reconciler -RunManifestPath $ManifestPath -ManifestSha256 $ManifestSha256 `
		-ServerEvidenceRoot $ServerRoot -ClientEvidenceRoot $ClientRoot -ReportPath $NodeReportPath |
		Out-Null
	$NodeReport = Get-Content -LiteralPath $NodeReportPath -Raw | ConvertFrom-Json
	if ($NodeReport.Provider -ne 'Node' -or $NodeReport.ProviderQualification -ne 'NOT CLAIMED') {
		throw 'Node role-local evidence was rejected or promoted to real-TLS qualification'
	}
	$ResourcePath = Join-Path $ServerRoot 'process-resources.csv'
	$ResourceBaseline = [IO.File]::ReadAllText($ResourcePath)
	$BadResources = @(Import-Csv -LiteralPath $ResourcePath)
	$BadResources[1].MonotonicTicks = '500000'
	$BadResources | Export-Csv -LiteralPath $ResourcePath -NoTypeInformation
	Save-Index -Root $ServerRoot -RunId $RunId -Role 'Server'
	Assert-Rejected -Name 'backwards same-host monotonic resource sample' `
		-ReportPath (Join-Path $TestRoot 'bad-resource-order-report.json')
	[IO.File]::WriteAllText($ResourcePath, $ResourceBaseline)
	Save-Index -Root $ServerRoot -RunId $RunId -Role 'Server'
	$Client0Path = Join-Path $ClientRoot 'client-00.stdout.log'
	$Client0 = [IO.File]::ReadAllText($Client0Path)
	[IO.File]::WriteAllText($Client0Path, $Client0.Replace('event_acks=1', 'event_acks=0'))
	Assert-Rejected -Name 'tampered hashed log' -ReportPath (Join-Path $TestRoot 'tamper-report.json')
	Save-Index -Root $ClientRoot -RunId $RunId -Role 'Clients'
	Assert-Rejected -Name 'rehash of invalid producer conservation' -ReportPath (Join-Path $TestRoot 'bad-producer-report.json')
	[IO.File]::WriteAllText($Client0Path, $Client0)
	Save-Index -Root $ClientRoot -RunId $RunId -Role 'Clients'
	$OriginalServer = [IO.File]::ReadAllText($ServerPath)
	[IO.File]::WriteAllText($ServerPath, $OriginalServer.Replace('event=content_acks run=', 'event=content_acks_missing run='))
	Save-Index -Root $ServerRoot -RunId $RunId -Role 'Server'
	Assert-Rejected -Name 'missing content ACK' -ReportPath (Join-Path $TestRoot 'missing-ack-report.json')
	[IO.File]::WriteAllText($ServerPath, $OriginalServer)
	Save-Index -Root $ServerRoot -RunId $RunId -Role 'Server'
	[IO.File]::WriteAllText($ServerPath, $OriginalServer.Replace('retired=8192', 'retired=8191'))
	Save-Index -Root $ServerRoot -RunId $RunId -Role 'Server'
	Assert-Rejected -Name 'native accepted and retired byte mismatch' -ReportPath (Join-Path $TestRoot 'bad-retired-report.json')
	[IO.File]::WriteAllText($ServerPath, $OriginalServer.Replace('grants_high=2', 'grants_high=5'))
	Save-Index -Root $ServerRoot -RunId $RunId -Role 'Server'
	Assert-Rejected -Name 'native grant bound violation' -ReportPath (Join-Path $TestRoot 'bad-grants-report.json')
	[IO.File]::WriteAllText($ServerPath, $OriginalServer)
	Save-Index -Root $ServerRoot -RunId $RunId -Role 'Server'
	[IO.File]::WriteAllText($ServerPath, $OriginalServer.Replace('pending_leaves=0', 'pending_leaves=1'))
	Save-Index -Root $ServerRoot -RunId $RunId -Role 'Server'
	Assert-Rejected -Name 'native pending conservation mismatch' -ReportPath (Join-Path $TestRoot 'bad-admission-report.json')
	[IO.File]::WriteAllText($ServerPath, $OriginalServer)
	Save-Index -Root $ServerRoot -RunId $RunId -Role 'Server'
	$Client31Path = Join-Path $ClientRoot 'client-31.stdout.log'
	$Client31 = [IO.File]::ReadAllText($Client31Path)
	[IO.File]::WriteAllText($Client31Path, $Client31.Replace("nonce=$($Nonces[31])", "nonce=$($Nonces[30])"))
	Save-Index -Root $ClientRoot -RunId $RunId -Role 'Clients'
	Assert-Rejected -Name 'duplicate client nonce across hosts' -ReportPath (Join-Path $TestRoot 'bad-nonce-report.json')
	[IO.File]::WriteAllText($Client31Path, $Client31)
	Save-Index -Root $ClientRoot -RunId $RunId -Role 'Clients'
	[IO.File]::WriteAllText((Join-Path $ClientRoot 'run-manifest.json'), '{}')
	Save-Index -Root $ClientRoot -RunId $RunId -Role 'Clients'
	Assert-Rejected -Name 'mismatched role manifest' -ReportPath (Join-Path $TestRoot 'bad-manifest-report.json')
	Write-Output '[Qualification:FarmReconcile] MOCK_TEST_OK'
} finally {
	if (-not $ResolvedRoot.StartsWith($ResolvedTemp.TrimEnd('\', '/') +
		[IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
		[IO.Path]::GetFileName($ResolvedRoot) -cnotmatch '^physical-farm-reconcile-test-[a-f0-9]{32}$') {
		throw 'refusing recursive test cleanup outside the expected temporary root'
	}
	if (Test-Path -LiteralPath $ResolvedRoot) {
		Remove-Item -LiteralPath $ResolvedRoot -Recurse -Force
	}
}
