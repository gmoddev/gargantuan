#requires -Version 7.0
# Socket-free acceptance-observation tests; no provider or physical run starts.
$ErrorActionPreference = 'Stop'
$Analyzer = Join-Path $PSScriptRoot 'PhysicalGameSessionFarmAcceptance.ps1'
. (Join-Path $PSScriptRoot 'AdmissionFairnessEvidence.ps1')
$Tokens = $null
$Errors = $null
[void][Management.Automation.Language.Parser]::ParseFile($Analyzer, [ref]$Tokens, [ref]$Errors)
if ($Errors.Count -ne 0) { throw "acceptance analyzer syntax failed: $($Errors[0].Message)" }

function Save-Json {
	param([string]$Path, $Value)
	[IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
}

function Save-Index {
	param([string]$Root, [string]$RunId, [string]$Role)
	$Files = @(Get-ChildItem -LiteralPath $Root -File |
		Where-Object Name -ne 'evidence-sha256.json' | Sort-Object Name | ForEach-Object {
			[ordered]@{ Name = $_.Name; Bytes = $_.Length
				Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
		})
	Save-Json -Path (Join-Path $Root 'evidence-sha256.json') -Value ([ordered]@{
		RunId = $RunId; Role = $Role; Files = $Files
	})
}

function Save-ResourceRows {
	param([string]$Root, [string]$RunId, [string]$Role)
	$Labels = @(if ($Role -eq 'Server') { 'server' } else {
		0..31 | ForEach-Object { 'client-{0:D2}' -f $_ }
	})
	$Rows = [Collections.Generic.List[object]]::new()
	$Now = [DateTimeOffset]::UtcNow.ToString('O')
	for ($Index = 0; $Index -lt $Labels.Count; $Index++) {
		foreach ($Sample in @(0, 1)) {
			$Rows.Add([pscustomobject]@{
				RunId = $RunId; Label = $Labels[$Index]; Pid = 1000 + $Index
				Utc = $Now; SupervisorElapsedMilliseconds = 1000 + 2000 * $Sample
				MonotonicTicks = 1000000 + 2000000 * $Sample + $Index
				MonotonicFrequency = 1000000
				WorkingSetBytes = if ($Role -eq 'Clients') {
					1000000 + 50000 * (($Sample + $Index) % 2)
				} else { 1000000 + 50000 * $Sample }
				PrivateBytes = 900000 + 25000 * $Sample
				CpuMilliseconds = 10 + 10 * $Sample
				Threads = 4; Handles = 16
			})
		}
	}
	$Rows | Export-Csv -LiteralPath (Join-Path $Root 'process-resources.csv') -NoTypeInformation
	return $Rows.Count
}

function Save-HostRows {
	param([string]$Root, [string]$RunId, [string]$Role, [string]$Provider)
	$HostName = if ($Role -eq 'Server') { 'WORKER' } else { 'CLIENT' }
	$Address = if ($Role -eq 'Server') { '10.253.3.2' } else { '10.253.3.1' }
	$Mac = if ($Role -eq 'Server') { 'AA-BB-CC-DD-EE-02' } else { 'AA-BB-CC-DD-EE-01' }
	$Live = if ($Role -eq 'Server') { 1 } else { 32 }
	$Rows = @(0, 1 | ForEach-Object {
		$Sample = $_
		[pscustomobject]@{
			RunId = $RunId; Role = $Role; Provider = $Provider
			HostName = $HostName; InterfaceIndex = 22; InterfaceMacAddress = $Mac
			InterfaceAddress = $Address; InterfaceLinkSpeed = '10 Gbps'
			Utc = [DateTimeOffset]::UtcNow.ToString('O')
			SupervisorElapsedMilliseconds = 1000 + 2000 * $Sample
			SampleStartTicks = 1000000 + 2000000 * $Sample
			SampleEndTicks = 1001000 + 2000000 * $Sample
			MonotonicFrequency = 1000000; LiveOwnedProcessCount = $Live
			OwnedWorkingSetBytes = 1000000 * $Live + 50000 * $Sample
			OwnedPrivateBytes = 900000 * $Live + 25000 * $Sample
			HostTotalPhysicalBytes = 34359738368
			HostAvailablePhysicalBytes = 17179869184 - 1048576 * $Sample
			HostCpuIdle100ns = 1000000000 + 1000000000 * $Sample
			HostCpuKernel100ns = 2000000000 + 2000000000 * $Sample
			HostCpuUser100ns = 1000000000 + 1000000000 * $Sample
			NicSentBytes = 1000000 + 20000000 * $Sample
			NicReceivedBytes = 2000000 + 10000000 * $Sample
			NicOutboundDiscardedPackets = 0; NicOutboundPacketErrors = 0
			NicReceivedDiscardedPackets = 0; NicReceivedPacketErrors = 0
		}
	})
	$Rows | Export-Csv -LiteralPath (Join-Path $Root 'host-resources.csv') -NoTypeInformation
}

function Save-FairnessRows {
	param([string]$Root, [string]$RunId, [string[]]$Connections)
	$Path = Join-Path $Root 'admission-fairness.tsv'
	$Lines = @(
		"format=GargantuanAdmissionEvidenceV1`trun=$RunId",
		(@('event', 'exact_demand', 'none', 1, 1, 1, 0, 0, 8192, 1000, 0, 0, 0, 0, 0, 0, 0, 0, 0) -join "`t"),
		(@('event', 'credit_eligible', 'none', 1, 1, 1, 1, 0, 8192, 1100, 1050, 1100, 8192, 8192, 0, 0, 0, 0, 0) -join "`t"),
		(@('event', 'grant_accepted', 'none', 1, 1, 1, 1, 1, 8192, 1300, 1050, 1100, 0, 0, 1, 0, 0, 0, 0) -join "`t"),
		"end`t3`t0"
	)
	[IO.File]::WriteAllText($Path, ($Lines -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
	return (Read-AdmissionFairnessEvidence -Path $Path -RunId $RunId -ExpectedConnections $Connections)
}

function Import-RecoveryParser {
	$Path = Join-Path $PSScriptRoot 'PhysicalGameSessionFarm.ps1'
	$Tokens = $null; $Errors = $null
	$Ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$Tokens, [ref]$Errors)
	if ($Errors.Count -ne 0) { throw 'recovery fixture parser has invalid syntax' }
	$Needed = @('Get-Fields', 'Read-SharedLogLines', 'Get-Records', 'Get-RecoveryDiagnostics',
		'Test-RecoveryQuiescent', 'Test-RecoveryServiceHealthy', 'Get-RecoveryUnsigned',
		'Get-RecoveryCeilDiv', 'Assert-RecoveryQuote', 'Assert-RecoveryRecords')
	foreach ($Function in $Ast.FindAll({ param($Node)
		$Node -is [Management.Automation.Language.FunctionDefinitionAst]
	}, $true)) {
		if ($Function.Name -in $Needed) { $Function.Extent.Text }
	}
}
foreach ($Definition in @(Import-RecoveryParser)) { . ([scriptblock]::Create($Definition)) }

function Save-RecoveryLogs {
	param([string]$ServerRoot, [string]$ClientRoot, [string]$RunId,
		[string[]]$Nonces, [string[]]$Connections)
	$Cases = @('gameplay', 'structural', 'mixed')
	$ServerOutputPath = Join-Path $ServerRoot 'server.stdout.log'
	$ServerOutput = [Collections.Generic.List[string]]::new()
	$ServerError = [Collections.Generic.List[string]]::new()
	for ($Index = 0; $Index -lt 32; $Index++) {
		$ServerOutput.Add("[Qualification:Recovery] event=object run=$RunId index=$Index object_slot=$(100 + $Index) object_generation=1")
	}
	foreach ($CaseIndex in 0..2) {
		$Case = $Cases[$CaseIndex]; $Tick = 1000 + 1000 * $CaseIndex
		$Tail = 10000 + 10000 * $CaseIndex
		$ServerOutput.Add("[Qualification:Recovery] event=case_start run=$RunId case=$Case tick=$Tick monotonic_us=$(100000000 * ($CaseIndex + 1))")
		for ($Peer = 1; $Peer -le 32; $Peer++) {
			$ServerError.Add("[Qualification:Recovery] event=ready_ack case=$Case peer_slot=$Peer peer_generation=1")
			$ServerError.Add("[Qualification:Recovery] event=offered_ack case=$Case peer_slot=$Peer peer_generation=1")
		}
		if ($Case -ne 'gameplay') {
			for ($Opportunity = 1; $Opportunity -le 480; $Opportunity++) {
				$Time = 100000000 * ($CaseIndex + 1) + $Opportunity * 16667
				$ServerOutput.Add("[Qualification:Recovery] event=structural_offer run=$RunId case=$Case opportunity=$Opportunity mutations=16 name_bytes=24576 monotonic_us=$Time")
				$ServerOutput.Add("[Qualification:Recovery] event=retention run=$RunId case=$Case opportunity=$Opportunity retained=16 margin=99")
			}
		}
		$ServerOutput.Add("[Qualification:Recovery] event=all_opportunities run=$RunId case=$Case opportunities=480 elapsed_us=8000000 tick=$($Tick + 480)")
		$RawBytes = if ($Case -eq 'gameplay') { 0 } else { 188743680 }
		$ServerOutput.Add("[Qualification:Recovery] event=cessation run=$RunId case=$Case tick=$($Tick + 481) monotonic_us=$(100000000 * ($CaseIndex + 1) + 8000001) journal_tail=$Tail raw_name_bytes=$RawBytes retained_high=16 minimum_retention_margin=99")
		$ServerError.Add("[Qualification:Recovery] event=quote_capture run=$RunId case=$Case status=READY reason=none tick=$($Tick + 481)")
		$ServerError.Add("[Qualification:Recovery] event=cessation_barrier run=$RunId case=$Case journal_tail=$Tail")
		for ($Peer = 1; $Peer -le 32; $Peer++) {
			$ServerError.Add("[Qualification:Recovery] event=probe_ack case=$Case peer_slot=$Peer peer_generation=1")
			if ($Case -ne 'gameplay') {
				$ServerError.Add("[Qualification:Recovery] event=name_ack case=$Case peer_slot=$Peer peer_generation=1")
			}
		}
		for ($Peer = 1; $Peer -le 32; $Peer++) {
			$ServerError.Add("[Qualification:Recovery] event=quote_peer run=$RunId case=$Case connection_slot=$Peer connection_generation=1 accepted_unretired_complete_bytes=0 future_complete_bytes=77 w_complete_upper_bytes=77")
		}
		$ServerError.Add("[Qualification:Recovery] event=quote_result run=$RunId case=$Case status=PASS w_complete_upper_bytes=2464 quoted_frames=32 audited_frames=32 audited_accepted_bytes=2464 bound_us=20470537 reason=none tick=$($Tick + 482)")
		$Sample = "outstanding=0 active_grants=0 scheduler_queued=0 native_queued=0 native_observed=32 feedback_observed=32 accepted=100 first_sent=100 acked=100 retired=100 terminal_release=0 journal_backlog=0 materialization_backlog=0 current_tail=$Tail retained=16 oldest=1 required=100 margin=99 journal_failures=0"
		$ServerError.Add("[Qualification:Recovery] event=sample run=$RunId case=$Case elapsed_us=19000000 $Sample")
		$ServerError.Add("[Qualification:Recovery] event=sample run=$RunId case=$Case elapsed_us=20000001 $Sample")
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
	[IO.File]::AppendAllLines($ServerOutputPath, $ServerOutput)
	[IO.File]::WriteAllLines((Join-Path $ServerRoot 'server.stderr.log'), $ServerError)
	$ClientLogs = @(
		for ($Slot = 0; $Slot -lt 32; $Slot++) {
			$Output = [Collections.Generic.List[string]]::new()
			$ErrorLines = [Collections.Generic.List[string]]::new()
			foreach ($Case in $Cases) {
				$Rpc = if ($Case -eq 'structural') { 1 } else { 16 }
				$Events = if ($Case -eq 'structural') { 60 } else { 480 }
				$ErrorLines.Add("[Qualification:Recovery] event=client_offered case=$Case opportunities=480 rpc_completed=$Rpc rpc_errors=0 event_attempts=$Events event_offers=$Events event_acks=$Events")
				$ErrorLines.Add("[Qualification:Recovery] event=client_probes case=$Case rpc_acks=10 rpc_errors=0 event_acks=10 rpc_p95_us=1000 rpc_p99_us=1000 rpc_max_us=1000 event_max_us=1000")
				if ($Case -eq 'gameplay') { continue }
				$CaseIndex = if ($Case -eq 'structural') { 1 } else { 2 }
				for ($Index = 0; $Index -lt 32; $Index++) {
					$Opportunity = if ($Index -lt 16) { 479 } else { 480 }
					$Letter = [char]([int][char]'a' + (($CaseIndex * 7 + $Opportunity + $Index) % 26))
					$Output.Add("[Qualification:Client] event=name_object run_id=$RunId slot=$Slot nonce=$($Nonces[$Slot]) case=$Case index=$Index object_slot=$(100 + $Index) object_generation=1 name_bytes=24576 letter=$Letter")
				}
				$Output.Add("[Qualification:Client] event=names_observed run_id=$RunId slot=$Slot nonce=$($Nonces[$Slot]) case=$Case objects=32")
				$ErrorLines.Add("[Qualification:Recovery] event=client_names case=$Case objects=32")
			}
			$Label = 'client-{0:D2}' -f $Slot
			$OutputPath = Join-Path $ClientRoot "$Label.stdout.log"
			$ErrorPath = Join-Path $ClientRoot "$Label.stderr.log"
			[IO.File]::WriteAllLines($OutputPath, $Output)
			[IO.File]::WriteAllLines($ErrorPath, $ErrorLines)
			[pscustomobject]@{ OutputPath = $OutputPath; ErrorPath = $ErrorPath }
		}
	)
	$ServerLog = [pscustomobject]@{ OutputPath = $ServerOutputPath
		ErrorPath = (Join-Path $ServerRoot 'server.stderr.log') }
	return Assert-RecoveryRecords -Server $ServerLog -Clients $ClientLogs `
		-ExpectedNonces $Nonces -ExpectedConnections $Connections
}

function New-RunFixture {
	param([string]$Prefix, [string]$Provider, [string]$RunId, [switch]$RecoveryWorkload)
	$ServerRoot = Join-Path $TestRoot "$Prefix-server"
	$ClientRoot = Join-Path $TestRoot "$Prefix-clients"
	[void][IO.Directory]::CreateDirectory($ServerRoot)
	[void][IO.Directory]::CreateDirectory($ClientRoot)
	$Manifest = [ordered]@{
		Format = 'GargantuanPhysicalFarmEndpoint'; Version = 1
		RunId = $RunId; SourceCommit = ('b' * 40)
		Endpoint = '10.253.3.2:39450'; Provider = $Provider; ScaleWorkload = $true
		ClientFrames = $(if ($RecoveryWorkload) { 18000 } else { 9000 })
		ServerTicks = $(if ($RecoveryWorkload) { 19000 } else { 10000 })
		Nonces = @(0..31 | ForEach-Object { [string](([UInt64]123 -shl 32) + [UInt64]($_ + 1)) })
		ServerSha256 = ('a' * 64); ServerPackageSha256 = ('a' * 64)
		PlayerSha256 = ('a' * 64); PlayerPackageSha256 = ('a' * 64)
		ServerContentManifestSha256 = ('a' * 64); PlayerContentManifestSha256 = ('a' * 64)
		ServerDeploymentSha256 = ('a' * 64); PlayerDeploymentSha256 = ('a' * 64)
	}
	if ($RecoveryWorkload) { $Manifest.RecoveryWorkload = $true }
	if ($Provider -eq 'Node') {
		$Manifest.NodeEndpoint = '127.0.0.1:39452'
		$Manifest.NodeRootCertificateSha256 = ('c' * 64)
		$Manifest.NodeTokenEnvironment = 'GARGANTUAN_ENGINE_ADAPTER_TOKEN'
	}
	foreach ($Root in @($ServerRoot, $ClientRoot)) {
		Save-Json -Path (Join-Path $Root 'run-manifest.json') -Value $Manifest
	}
	$ServerSamples = Save-ResourceRows -Root $ServerRoot -RunId $RunId -Role 'Server'
	$ClientSamples = Save-ResourceRows -Root $ClientRoot -RunId $RunId -Role 'Clients'
	$Connections = @(1..32 | ForEach-Object { "$_`:1" })
	$Fairness = Save-FairnessRows -Root $ServerRoot -RunId $RunId -Connections $Connections
	$NodeAuthentication = [ordered]@{ State = 'NOT_APPLICABLE' }
	if ($Provider -eq 'Node') {
		$NodeReceipt = [ordered]@{
			Format = 'GargantuanFarmNodeAuthenticatedManifest'; Version = 2
			RunId = $RunId; Provider = 'Node'; RequestId = 'server-content-1'
			ProjectId = '0123456789abcdef0123456789abcdef'; PackageVersion = 37L
			NodeEndpoint = $Manifest.NodeEndpoint
			RootCertificateSha256 = $Manifest.NodeRootCertificateSha256
			ManifestSha256 = $Manifest.ServerContentManifestSha256; ManifestBytes = 512L
			AuthenticatedManifestRpcCount = 1L; ChannelCredentials = 'grpc_ssl_credentials'
			TlsSessionDetails = 'NOT_MEASURED'; Source = 'GargantuanServer/NodeContentProvider'
		}
		Save-Json -Path (Join-Path $ServerRoot 'node-provider.json') -Value $NodeReceipt
		$NodeAuthentication = [ordered]@{
			State = 'AUTHENTICATED_MANIFEST_RPC_MEASURED'
			RequestId = $NodeReceipt.RequestId; ProjectId = $NodeReceipt.ProjectId
			PackageVersion = $NodeReceipt.PackageVersion
			ManifestSha256 = $NodeReceipt.ManifestSha256
			RootCertificateSha256 = $NodeReceipt.RootCertificateSha256
			TlsSessionDetails = 'NOT_MEASURED'
		}
	}
	foreach ($RoleRoot in @($ServerRoot, $ClientRoot)) {
		$RoleName = if ($RoleRoot -eq $ServerRoot) { 'Server' } else { 'Clients' }
		Save-Json -Path (Join-Path $RoleRoot 'result.json') -Value ([ordered]@{
			RunId = $RunId; Role = $RoleName; Status = 'PASS'; ScaleWorkload = $true
			ManifestSha256 = (Get-FileHash -LiteralPath (Join-Path $RoleRoot 'run-manifest.json') -Algorithm SHA256).Hash.ToLowerInvariant()
			Provider = $Provider; Endpoint = $Manifest.Endpoint
			AggregateWorkingSetLimitBytes = 21474836480L
		})
	}
	Save-HostRows -Root $ServerRoot -RunId $RunId -Role 'Server' -Provider $Provider
	Save-HostRows -Root $ClientRoot -RunId $RunId -Role 'Clients' -Provider $Provider
	$AdmissionLine = "[Qualification:Admission] event=result run=$RunId accepted=8192 retired=8192 terminal_release=0 outstanding=0 outstanding_high=2048 active_grants=0 grants_high=2 grant_deferrals=3 funded_deferrals=2 credit_deferrals=1 fairness_deferrals=1 max_wait_us=1000 peer_backlog_high=2048 global_backlog_high=8192 peer_credit_high=2048 global_credit_high=8192 fairness_rotations=1 pending_enters=0 pending_leaves=0 materialization_backlog=0 journal_backlog=0 structural_active_peers=0 oldest_pending_ticks=0 backlog_failures=0 journal_failures=0"
	[IO.File]::WriteAllText((Join-Path $ServerRoot 'server.stdout.log'), "$AdmissionLine`n")
	$Recovery = if ($RecoveryWorkload) {
		Save-RecoveryLogs -ServerRoot $ServerRoot -ClientRoot $ClientRoot `
			-RunId $RunId -Nonces $Manifest.Nonces -Connections $Connections
	} else { $null }
	Save-Index -Root $ServerRoot -RunId $RunId -Role 'Server'
	Save-Index -Root $ClientRoot -RunId $RunId -Role 'Clients'
	$Report = [ordered]@{
		Format = 'GargantuanPhysicalFarmReconciliation'; Version = 1
		RunId = $RunId; Provider = $Provider
		ManifestSha256 = (Get-FileHash -LiteralPath (Join-Path $ServerRoot 'run-manifest.json') -Algorithm SHA256).Hash.ToLowerInvariant()
		ServerEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $ServerRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
		ClientEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $ClientRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
		Status = 'INCOMPLETE'; RoleLocalEvidence = 'VALIDATED'; ProviderQualification = 'NOT CLAIMED'
		Identity = @{ Ready = 32; Connections = $Connections }
		Admission = @{
			accepted = 8192; retired = 8192; terminal_release = 0; outstanding = 0
			outstanding_high = 2048; active_grants = 0; grants_high = 2
			grant_deferrals = 3; funded_deferrals = 2; credit_deferrals = 1
			fairness_deferrals = 1; max_wait_us = 1000; peer_backlog_high = 2048
			global_backlog_high = 8192; peer_credit_high = 2048; global_credit_high = 8192
			fairness_rotations = 1; pending_enters = 0; pending_leaves = 0
			materialization_backlog = 0; journal_backlog = 0; structural_active_peers = 0
			oldest_pending_ticks = 0; backlog_failures = 0; journal_failures = 0
		}
		AdmissionFairnessObservation = $Fairness
		PublicationObservation = [ordered]@{
			State = 'NOT_MEASURED'; Reason = 'indexed native Character publication traces absent'
		}
		RecoveryObservation = $Recovery
		NodeAuthenticatedManifest = $NodeAuthentication
		ServerResourceSamples = $ServerSamples; ClientResourceSamples = $ClientSamples
		ServerHostResourceSamples = 2; ClientHostResourceSamples = 2
	}
	$ReportPath = Join-Path $TestRoot "$Prefix-report.json"
	Save-Json -Path $ReportPath -Value $Report
	return [pscustomobject]@{
		ServerRoot = $ServerRoot; ClientRoot = $ClientRoot
		ReportPath = $ReportPath; Report = $Report; Manifest = $Manifest
	}
}

function Save-RemoteCadenceLog {
	param($Run, [switch]$SlowEvent)
	$RunId = $Run.Report.RunId
	$Nonce = $Run.Manifest.Nonces[0]
	$Lines = [Collections.Generic.List[string]]::new()
	$Lines.Add("[Qualification:Client] event=ready run_id=$RunId slot=0 nonce=$Nonce connection_slot=1 connection_generation=1")
	$Sequence = 0
	foreach ($Phase in @('baseline', 'load', 'resident', 'evict', 'reload')) {
		$Rpc = [Collections.Generic.List[string]]::new()
		for ($Index = 1; $Index -le 100; $Index++) {
			$Started = [long]$Index * 51000000L
			$Rpc.Add("$Index`:$Started`:$($Started + 50000000L):1")
		}
		$Events = [Collections.Generic.List[string]]::new()
		for ($Index = 1; $Index -le 4; $Index++) {
			$Sequence++
			$Received = [long]$Index * 80000000L
			$Offered = $Received - 40000000L
			if ($SlowEvent -and $Phase -ceq 'reload' -and $Index -eq 4) {
				$Offered = 300000000L; $Received = 550000001L
			}
			$Events.Add("$Sequence`:$Offered`:$Received")
		}
		foreach ($Kind in @('rpc', 'event')) {
			$Rows = if ($Kind -ceq 'rpc') { $Rpc } else { $Events }
			$Chunks = [int][Math]::Ceiling($Rows.Count / 32.0)
			$Lines.Add("[Qualification:RemoteCadence] event=summary version=1 kind=$Kind phase=$Phase records=$($Rows.Count) chunks=$Chunks")
			for ($Chunk = 1; $Chunk -le $Chunks; $Chunk++) {
				$First = ($Chunk - 1) * 32
				$Last = [Math]::Min($Rows.Count - 1, $First + 31)
				$Joined = [string]::Join(',', @($Rows[$First..$Last]))
				$Lines.Add("[Qualification:RemoteCadence] event=chunk version=1 kind=$Kind phase=$Phase index=$Chunk records=$Joined")
			}
		}
		$Lines.Add("[Qualification:Producer] event=phase_metrics run_id=$RunId slot=0 nonce=$Nonce phase=$Phase status=PASS remote_samples=100 event_offers=4 event_acks=4 event_outstanding=0")
	}
	[IO.File]::WriteAllLines((Join-Path $Run.ClientRoot 'client-00.stdout.log'), $Lines)
	Save-Index -Root $Run.ClientRoot -RunId $RunId -Role 'Clients'
	$Run.Report.ClientEvidenceSha256 = (Get-FileHash -LiteralPath `
		(Join-Path $Run.ClientRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Run.ReportPath -Value $Run.Report
}

function New-NodeTlsFixture {
	param($NodeRun)
	$StageRoot = Join-Path $TestRoot 'node-tls-stage'
	[void][IO.Directory]::CreateDirectory($StageRoot)
	$ProviderReceipt = Get-Content -LiteralPath (Join-Path $NodeRun.ServerRoot 'node-provider.json') `
		-Raw | ConvertFrom-Json -AsHashtable
	$NodeLogPath = Join-Path $StageRoot 'node.stdout.log'
	$Record = [ordered]@{
		time = '2026-10-01T00:00:00Z'; level = 'INFO'
		msg = '[Content:TLS] authenticated manifest RPC'
		request_id = $ProviderReceipt.RequestId; project_id = $ProviderReceipt.ProjectId
		package_version = $ProviderReceipt.PackageVersion; principal_id = 'farm-server'
		tls_version = 'TLSv1.3'; cipher_suite = 'TLS_AES_128_GCM_SHA256'
		transport = 'grpc_tls'
	}
	[IO.File]::WriteAllText($NodeLogPath, ($Record | ConvertTo-Json -Compress) + "`n")
	$StagePath = Join-Path $StageRoot 'node-stage.json'
	Save-Json -Path $StagePath -Value ([ordered]@{
		Format = 'GargantuanFarmNodeStage'; Version = 1
		RunId = $NodeRun.Manifest.RunId; ProjectId = $ProviderReceipt.ProjectId
		Revision = $ProviderReceipt.PackageVersion; NodeEndpoint = $NodeRun.Manifest.NodeEndpoint
		RootCertificateSha256 = $NodeRun.Manifest.NodeRootCertificateSha256
		NodeExecutableSha256 = ('d' * 64); ConfigSha256 = ('e' * 64)
		CertificateSha256 = ('f' * 64)
	})
	$StagePin = (Get-FileHash -LiteralPath $StagePath -Algorithm SHA256).Hash.ToLowerInvariant()
	$RunReceiptPath = Join-Path $StageRoot 'node-run.json'
	Save-Json -Path $RunReceiptPath -Value ([ordered]@{
		Format = 'GargantuanFarmNodeRun'; Version = 1
		RunId = $NodeRun.Manifest.RunId; StageSha256 = $StagePin
		NodeExecutableSha256 = ('d' * 64); ConfigSha256 = ('e' * 64)
		CertificateSha256 = ('f' * 64)
		RootCertificateSha256 = $NodeRun.Manifest.NodeRootCertificateSha256
		Pid = 1234L; StartedUtc = '2026-10-01T00:00:00Z'
		EndedUtc = '2026-10-01T00:00:01Z'; TcpReady = $true
		TlsProven = $false; LogsDiscarded = $false; ChildReaped = $true
		Reason = 'STOP_REQUESTED'; StdoutPath = $NodeLogPath
		StdoutSha256 = (Get-FileHash -LiteralPath $NodeLogPath -Algorithm SHA256).Hash.ToLowerInvariant()
		StdoutBytes = (Get-Item -LiteralPath $NodeLogPath).Length
		StderrPath = (Join-Path $StageRoot 'node.stderr.log')
		StderrSha256 = ('a' * 64); StderrBytes = 0L
	})
	$RunPin = (Get-FileHash -LiteralPath $RunReceiptPath -Algorithm SHA256).Hash.ToLowerInvariant()
	$MatchPath = Join-Path $TestRoot 'node-tls-match.json'
	& (Join-Path $PSScriptRoot 'PhysicalGameSessionFarmNodeTls.ps1') `
		-ServerReceiptPath (Join-Path $NodeRun.ServerRoot 'node-provider.json') `
		-NodeStagePath $StagePath -NodeRunReceiptPath $RunReceiptPath `
		-NodeRunReceiptSha256 $RunPin -NodeStageSha256 $StagePin -OutputPath $MatchPath | Out-Null
	return @{
		NodeTlsMatchReceiptPath = $MatchPath
		NodeTlsMatchReceiptSha256 = (Get-FileHash -LiteralPath $MatchPath -Algorithm SHA256).Hash.ToLowerInvariant()
		NodeStagePath = $StagePath; NodeStageSha256 = $StagePin
		NodeRunReceiptPath = $RunReceiptPath; NodeRunReceiptSha256 = $RunPin
	}
}

function Invoke-Analyzer {
	param([string]$OutputPath, [hashtable]$TlsInputs = @{})
	& $Analyzer -LocalReportPath $Local.ReportPath -LocalServerEvidenceRoot $Local.ServerRoot `
		-LocalClientEvidenceRoot $Local.ClientRoot -NodeReportPath $Node.ReportPath `
		-NodeServerEvidenceRoot $Node.ServerRoot -NodeClientEvidenceRoot $Node.ClientRoot `
		-OutputPath $OutputPath @TlsInputs | Out-Null
}

function Assert-Rejected {
	param([string]$Name, [string]$OutputPath, [hashtable]$TlsInputs = @{})
	$Rejected = $false
	try { Invoke-Analyzer -OutputPath $OutputPath -TlsInputs $TlsInputs } catch { $Rejected = $true }
	if (-not $Rejected -or (Test-Path -LiteralPath $OutputPath)) { throw "$Name was accepted" }
}

$TestRoot = Join-Path ([IO.Path]::GetTempPath()) ('physical-farm-acceptance-test-' + [Guid]::NewGuid().ToString('N'))
$ResolvedTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
$ResolvedRoot = [IO.Path]::GetFullPath($TestRoot)
if (-not $ResolvedRoot.StartsWith($ResolvedTemp + [IO.Path]::DirectorySeparatorChar,
	[StringComparison]::OrdinalIgnoreCase)) { throw 'test scratch root escaped temporary directory' }
[void][IO.Directory]::CreateDirectory($TestRoot)
try {
	$Local = New-RunFixture -Prefix 'local' -Provider 'Local' `
		-RunId '7c93e53d-0e0c-4b8d-8a3b-9a761a406ebd'
	$Node = New-RunFixture -Prefix 'node' -Provider 'Node' `
		-RunId '8d93e53d-0e0c-4b8d-8a3b-9a761a406ebd'
	$OutputPath = Join-Path $TestRoot 'observed.json'
	Invoke-Analyzer -OutputPath $OutputPath
	$Observed = Get-Content -LiteralPath $OutputPath -Raw | ConvertFrom-Json
	if ($Observed.Status -cne 'INCOMPLETE' -or $Observed.Foundation3LQualification -cne 'NOT CLAIMED' -or
		$Observed.WorkloadPinParity.State -cne 'MEASURED' -or
		$Observed.Local.Resources.Clients.ProcessCount -ne 32 -or
		@($Observed.Local.Resources.Clients.Processes).Count -ne 32 -or
		$Observed.Local.Resources.Clients.Processes[0].Label -cne 'client-00' -or
		$Observed.Node.Resources.Server.ProcessCount -ne 1 -or
		$Observed.Local.Resources.Clients.SumOfPerProcessPeakWorkingSetBytes -ne 33600000 -or
		$Observed.Local.Resources.Clients.CompleteSweepCount -ne 2 -or
		$Observed.Local.Resources.Clients.CompleteSweepMaximumWorkingSetBytes -ne 32800000 -or
		$Observed.Local.Resources.Clients.CompleteSweepLimitObservation -cne 'WITHIN_ROLE_LIMIT' -or
		$Observed.Local.Resources.ClientHost.FullRoleSampleCount -ne 2 -or
		$Observed.Local.Resources.ClientHost.MaximumObservedSimultaneousOwnedWorkingSetBytes -ne 32050000 -or
		$Observed.Node.Resources.ServerHost.MaximumObservedHostCpuPercent -lt 66 -or
		$Observed.Local.Resources.ServerHost.MaximumObservedNicSentBytesPerSecond -ne 10000000 -or
		$Observed.Local.Admission.MaximumActiveGrants -ne 2 -or
		$Observed.Local.Admission.Fairness.MaximumObservedEligibilityToGrantMicroseconds -ne 200 -or
		$Observed.Local.Admission.AcceptedGrantWaitBound -cne 'MEASURED_PASS' -or
		@($Observed.GateObservations | Where-Object {
			$_.Gate -ceq 'Recorded accepted-grant eligibility wait within 220.5 ms' -and
			$_.State -ceq 'MEASURED_PASS' }).Count -ne 1 -or
		$Observed.Node.Provider.State -cne 'AUTHENTICATED_MANIFEST_RPC_MEASURED' -or
		$Observed.Node.Provider.RealTls -cne 'NOT_MEASURED' -or
		$Observed.Local.Publication.State -cne 'NOT_MEASURED' -or
		$Observed.Local.ServerWorkTicks.Status -cne 'NOT_MEASURED' -or
		$Observed.Local.RemoteCadence.Status -cne 'NOT_MEASURED' -or
		@($Observed.GateObservations | Where-Object { $_.State -eq 'NOT MEASURED' }).Count -ne 14 -or
		$Observed.Local.Recovery.FixedServiceRecovery -cne 'NOT MEASURED' -or
		$Observed.Node.Recovery.ExactRetainedWorkBytes -cne 'NOT_MEASURED') {
		throw "resource/parity observation promoted a missing physical gate or lost resource evidence: status=$($Observed.Status) claim=$($Observed.Foundation3LQualification) parity=$($Observed.WorkloadPinParity.State) clients=$($Observed.Local.Resources.Clients.ProcessCount) server=$($Observed.Node.Resources.Server.ProcessCount) ws=$($Observed.Local.Resources.Clients.SumOfPerProcessPeakWorkingSetBytes) missing=$(@($Observed.GateObservations | Where-Object { $_.State -eq 'NOT MEASURED' }).Count)"
	}
	$TlsInputs = New-NodeTlsFixture -NodeRun $Node
	$TlsObservedPath = Join-Path $TestRoot 'tls-observed.json'
	Invoke-Analyzer -OutputPath $TlsObservedPath -TlsInputs $TlsInputs
	$TlsObserved = Get-Content -LiteralPath $TlsObservedPath -Raw | ConvertFrom-Json
	if ($TlsObserved.Node.Provider.RealTls -cne 'NEGOTIATED_TLS_MANIFEST_RPC_MEASURED' -or
		$TlsObserved.Node.Provider.TlsEvidence.TlsVersion -cne 'TLSv1.3' -or
		$TlsObserved.Node.Provider.TlsEvidence.CipherSuite -cne 'TLS_AES_128_GCM_SHA256' -or
		@($TlsObserved.GateObservations | Where-Object {
			$_.Gate -ceq 'Node negotiated TLS for authenticated manifest RPC' -and
			$_.State -ceq 'MEASURED' }).Count -ne 1 -or
		$TlsObserved.Status -cne 'INCOMPLETE' -or
		$TlsObserved.Foundation3LQualification -cne 'NOT CLAIMED' -or
		@($TlsObserved.GateObservations | Where-Object {
			$_.Gate -ceq 'Full Local/Node provider parity' -and
			$_.State -ceq 'NOT MEASURED' }).Count -ne 1) {
		throw 'pinned TLS match was not adopted with bounded proof scope'
	}
	$WrongPin = @{} + $TlsInputs
	$WrongPin.NodeTlsMatchReceiptSha256 = '0' * 64
	Assert-Rejected -Name 'wrong TLS match receipt pin' -TlsInputs $WrongPin `
		-OutputPath (Join-Path $TestRoot 'tls-wrong-pin.json')
	$WrongStage = @{} + $TlsInputs
	$WrongStage.NodeStageSha256 = '0' * 64
	Assert-Rejected -Name 'wrong Node stage pin' -TlsInputs $WrongStage `
		-OutputPath (Join-Path $TestRoot 'tls-wrong-stage.json')
	$IncompleteTls = @{} + $TlsInputs
	$IncompleteTls.Remove('NodeRunReceiptSha256')
	Assert-Rejected -Name 'partial Node TLS inputs' -TlsInputs $IncompleteTls `
		-OutputPath (Join-Path $TestRoot 'tls-partial.json')
	$OriginalMatch = [IO.File]::ReadAllText($TlsInputs.NodeTlsMatchReceiptPath)
	$ForgedMatch = $OriginalMatch | ConvertFrom-Json -AsHashtable
	$ForgedMatch.CipherSuite = 'TLS_AES_256_GCM_SHA384'
	Save-Json -Path $TlsInputs.NodeTlsMatchReceiptPath -Value $ForgedMatch
	$ForgedInput = @{} + $TlsInputs
	$ForgedInput.NodeTlsMatchReceiptSha256 = (Get-FileHash -LiteralPath `
		$TlsInputs.NodeTlsMatchReceiptPath -Algorithm SHA256).Hash.ToLowerInvariant()
	Assert-Rejected -Name 'rehashed TLS match differs from raw Node log' -TlsInputs $ForgedInput `
		-OutputPath (Join-Path $TestRoot 'tls-forged.json')
	[IO.File]::WriteAllText($TlsInputs.NodeTlsMatchReceiptPath, $OriginalMatch)
	$OriginalOutput = [IO.File]::ReadAllText($OutputPath)
	$OverwriteRejected = $false
	try { Invoke-Analyzer -OutputPath $OutputPath } catch { $OverwriteRejected = $true }
	if (-not $OverwriteRejected -or [IO.File]::ReadAllText($OutputPath) -cne $OriginalOutput) {
		throw 'existing acceptance observation was overwritten'
	}
	$Local = New-RunFixture -Prefix 'remote-local' -Provider 'Local' `
		-RunId '6c93e53d-0e0c-4b8d-8a3b-9a761a406ebd'
	$Node = New-RunFixture -Prefix 'remote-node' -Provider 'Node' `
		-RunId '6d93e53d-0e0c-4b8d-8a3b-9a761a406ebd'
	Save-RemoteCadenceLog -Run $Local
	Save-RemoteCadenceLog -Run $Node
	$RemotePassPath = Join-Path $TestRoot 'remote-pass.json'
	Invoke-Analyzer -OutputPath $RemotePassPath
	$RemotePass = Get-Content -LiteralPath $RemotePassPath -Raw | ConvertFrom-Json
	if ($RemotePass.Status -cne 'INCOMPLETE' -or
		$RemotePass.Foundation3LQualification -cne 'NOT CLAIMED' -or
		$RemotePass.Local.RemoteCadence.Status -cne 'MEASURED_PASS' -or
		$RemotePass.Node.RemoteCadence.Phases.reload.RpcP95Ns -ne 50000000 -or
		$RemotePass.Local.RemoteCadence.OtherClientsScaleRemoteRecipientService -cne 'NOT_MEASURED' -or
		$RemotePass.Local.RemoteCadence.CrossHostOneWayLatency -cne 'NOT_MEASURED' -or
		@($RemotePass.GateObservations | Where-Object {
			$_.Gate -ceq 'Designated producer Luau RPC and Event ACK cadence' -and
			$_.State -ceq 'MEASURED_PASS' }).Count -ne 1 -or
		@($RemotePass.GateObservations | Where-Object {
			$_.Gate -ceq 'Full Character and Remote recipient cadence' -and
			$_.State -ceq 'NOT MEASURED' }).Count -ne 1) {
		throw 'producer Remote trace was not adopted with partial coverage'
	}
	$NodeLogPath = Join-Path $Node.ClientRoot 'client-00.stdout.log'
	$OriginalNodeLog = [IO.File]::ReadAllText($NodeLogPath)
	$NodeLogRows = @([IO.File]::ReadAllLines($NodeLogPath) | Where-Object {
		-not ($_ -match 'event=chunk version=1 kind=rpc phase=baseline index=1 ') })
	[IO.File]::WriteAllLines($NodeLogPath, $NodeLogRows)
	Save-Index -Root $Node.ClientRoot -RunId $Node.Report.RunId -Role 'Clients'
	$Node.Report.ClientEvidenceSha256 = (Get-FileHash -LiteralPath `
		(Join-Path $Node.ClientRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	Assert-Rejected -Name 'rehashed missing Luau RPC trace chunk' `
		-OutputPath (Join-Path $TestRoot 'remote-missing-chunk.json')
	[IO.File]::WriteAllText($NodeLogPath, $OriginalNodeLog)
	Save-RemoteCadenceLog -Run $Node -SlowEvent
	$RemoteFailPath = Join-Path $TestRoot 'remote-fail.json'
	Invoke-Analyzer -OutputPath $RemoteFailPath
	$RemoteFail = Get-Content -LiteralPath $RemoteFailPath -Raw | ConvertFrom-Json
	if ($RemoteFail.Node.RemoteCadence.Status -cne 'MEASURED_FAIL' -or
		$RemoteFail.Node.RemoteCadence.Phases.reload.EventMaxRttNs -ne 250000001 -or
		@($RemoteFail.GateObservations | Where-Object {
			$_.Gate -ceq 'Designated producer Luau RPC and Event ACK cadence' -and
			$_.State -ceq 'MEASURED_FAIL' }).Count -ne 1 -or
		@($RemoteFail.GateObservations | Where-Object {
			$_.Gate -ceq 'Full Character and Remote recipient cadence' -and
			$_.State -ceq 'NOT MEASURED' }).Count -ne 1 -or
		$RemoteFail.Status -cne 'INCOMPLETE') {
		throw 'measured Luau Remote violation did not remain scoped and fail closed'
	}
	$RecoveryLocal = New-RunFixture -Prefix 'recovery-local' -Provider 'Local' `
		-RunId '9e93e53d-0e0c-4b8d-8a3b-9a761a406ebd' -RecoveryWorkload
	$RecoveryNode = New-RunFixture -Prefix 'recovery-node' -Provider 'Node' `
		-RunId 'ae93e53d-0e0c-4b8d-8a3b-9a761a406ebd' -RecoveryWorkload
	$Local = $RecoveryLocal; $Node = $RecoveryNode
	$RecoveryPath = Join-Path $TestRoot 'recovery-observed.json'
	Invoke-Analyzer -OutputPath $RecoveryPath
	$RecoveryObserved = Get-Content -LiteralPath $RecoveryPath -Raw | ConvertFrom-Json
	if ($RecoveryObserved.Local.Recovery.FixedServiceRecovery -cne 'MEASURED_PASS' -or
		$RecoveryObserved.Node.Recovery.FixedServiceRecovery -cne 'MEASURED_PASS' -or
		$RecoveryObserved.Local.Recovery.SampledJournalRetention -cne 'MEASURED_PASS' -or
		$RecoveryObserved.Node.Recovery.MinimumCessationRetentionMarginRecords -ne 99 -or
		@($RecoveryObserved.GateObservations | Where-Object {
			$_.Gate -ceq 'Sampled overload journal retention window' -and
			$_.State -ceq 'MEASURED_PASS' }).Count -ne 1 -or
		$RecoveryObserved.Local.Recovery.StrictConvergenceSufficientProof -cne 'MEASURED_PASS' -or
		$RecoveryObserved.Node.Recovery.ExactRetainedWorkBytes -ne 7392 -or
		@($RecoveryObserved.GateObservations | Where-Object {
			$_.Gate -ceq 'Workload-derived exact structural convergence' -and
			$_.State -ceq 'MEASURED_PASS' }).Count -ne 1 -or
		@($RecoveryObserved.GateObservations | Where-Object {
			$_.Gate -ceq 'Fixed 20-second service recovery' -and $_.State -ceq 'MEASURED_PASS'
		}).Count -ne 1 -or
		$RecoveryObserved.Status -cne 'INCOMPLETE') {
		throw 'two sealed recovery workloads did not prove only the fixed service gate'
	}
	$RecoveryServerErrorPath = Join-Path $Node.ServerRoot 'server.stderr.log'
	$OriginalRecoveryServerError = [IO.File]::ReadAllText($RecoveryServerErrorPath)
	Remove-Item -LiteralPath $RecoveryServerErrorPath
	Assert-Rejected -Name 'missing indexed native recovery log' -OutputPath (Join-Path $TestRoot 'missing-recovery.json')
	[IO.File]::WriteAllText($RecoveryServerErrorPath, $OriginalRecoveryServerError)
	$ClientRecoveryPath = Join-Path $Local.ClientRoot 'client-00.stdout.log'
	$OriginalClientRecovery = [IO.File]::ReadAllText($ClientRecoveryPath)
	[IO.File]::WriteAllText($ClientRecoveryPath, $OriginalClientRecovery.Replace('object_slot=100', 'object_slot=999'))
	Assert-Rejected -Name 'tampered indexed client ObjectId' -OutputPath (Join-Path $TestRoot 'tampered-client-recovery.json')
	Save-Index -Root $Local.ClientRoot -RunId $Local.Report.RunId -Role 'Clients'
	$Local.Report.ClientEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Local.ClientRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Local.ReportPath -Value $Local.Report
	Assert-Rejected -Name 'rehashed wrong client ObjectId' -OutputPath (Join-Path $TestRoot 'rehashed-client-identity.json')
	[IO.File]::WriteAllText($ClientRecoveryPath, $OriginalClientRecovery)
	Save-Index -Root $Local.ClientRoot -RunId $Local.Report.RunId -Role 'Clients'
	$Local.Report.ClientEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Local.ClientRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Local.ReportPath -Value $Local.Report
	$Node.Report.RecoveryObservation.Cases[0].FixedServiceRecovery = 'MEASURED_FAIL'
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	Assert-Rejected -Name 'forged recovery case in reconciliation' -OutputPath (Join-Path $TestRoot 'forged-recovery-report.json')
	$Node.Report.RecoveryObservation.Cases[0].FixedServiceRecovery = 'MEASURED_PASS'
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	$WrongCase = $OriginalRecoveryServerError.Replace('event=probe_ack case=mixed peer_slot=32',
		'event=probe_ack case=other peer_slot=32')
	[IO.File]::WriteAllText($RecoveryServerErrorPath, $WrongCase)
	Save-Index -Root $Node.ServerRoot -RunId $Node.Report.RunId -Role 'Server'
	$Node.Report.ServerEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ServerRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	Assert-Rejected -Name 'rehashed recovery case mismatch' -OutputPath (Join-Path $TestRoot 'rehashed-recovery-case.json')
	[IO.File]::WriteAllText($RecoveryServerErrorPath, $OriginalRecoveryServerError)
	Save-Index -Root $Node.ServerRoot -RunId $Node.Report.RunId -Role 'Server'
	$Node.Report.ServerEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ServerRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	$Local = New-RunFixture -Prefix 'local-restored' -Provider 'Local' `
		-RunId '7c93e53d-0e0c-4b8d-8a3b-9a761a406ebd'
	$Node = New-RunFixture -Prefix 'node-restored' -Provider 'Node' `
		-RunId '8d93e53d-0e0c-4b8d-8a3b-9a761a406ebd'
	$OriginalNodeReport = [IO.File]::ReadAllText($Node.ReportPath)
	$Node.Report.Admission.grants_high = 5
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	Assert-Rejected -Name 'forged five-grant high-water' -OutputPath (Join-Path $TestRoot 'five-grants.json')
	$Node.Report.Admission.grants_high = 2
	$Node.Report.Admission.fairness_deferrals = 2
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	Assert-Rejected -Name 'forged admission counter within bounds' -OutputPath (Join-Path $TestRoot 'false-counter.json')
	$Node.Report.Admission.fairness_deferrals = 1
	$Node.Report.AdmissionFairnessObservation.MaximumObservedEligibilityToGrantMicroseconds = 0
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	Assert-Rejected -Name 'forged fairness wait' -OutputPath (Join-Path $TestRoot 'false-fairness.json')
	$Node.Report.AdmissionFairnessObservation.MaximumObservedEligibilityToGrantMicroseconds = 200
	[IO.File]::WriteAllText($Node.ReportPath, $OriginalNodeReport)
	$FairnessPath = Join-Path $Node.ServerRoot 'admission-fairness.tsv'
	$OriginalFairness = [IO.File]::ReadAllText($FairnessPath)
	[IO.File]::WriteAllText($FairnessPath, $OriginalFairness.Replace("grant_accepted`tnone`t1`t1`t1`t1`t1`t8192`t1300", "grant_accepted`tnone`t1`t1`t1`t1`t1`t8192`t1400"))
	Save-Index -Root $Node.ServerRoot -RunId $Node.Report.RunId -Role 'Server'
	$Node.Report.ServerEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ServerRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	Assert-Rejected -Name 'rehashed native fairness timeline differs from report' -OutputPath (Join-Path $TestRoot 'rehashed-fairness.json')
	[IO.File]::WriteAllText($FairnessPath, $OriginalFairness)
	Save-Index -Root $Node.ServerRoot -RunId $Node.Report.RunId -Role 'Server'
	$Node.Report.ServerEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ServerRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	$SlowFairness = $OriginalFairness.Replace("grant_accepted`tnone`t1`t1`t1`t1`t1`t8192`t1300",
		"grant_accepted`tnone`t1`t1`t1`t1`t1`t8192`t221601")
	if ($SlowFairness -ceq $OriginalFairness) { throw 'slow fairness fixture did not change' }
	[IO.File]::WriteAllText($FairnessPath, $SlowFairness)
	Save-Index -Root $Node.ServerRoot -RunId $Node.Report.RunId -Role 'Server'
	$Node.Report.ServerEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ServerRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	$Node.Report.AdmissionFairnessObservation = Read-AdmissionFairnessEvidence -Path $FairnessPath `
		-RunId $Node.Report.RunId -ExpectedConnections $Node.Report.Identity.Connections
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	$SlowPath = Join-Path $TestRoot 'slow-fairness.json'
	Invoke-Analyzer -OutputPath $SlowPath
	$SlowObserved = Get-Content -LiteralPath $SlowPath -Raw | ConvertFrom-Json
	if ($SlowObserved.Node.Admission.AcceptedGrantWaitBound -cne 'MEASURED_FAIL' -or
		@($SlowObserved.GateObservations | Where-Object {
			$_.Gate -ceq 'Recorded accepted-grant eligibility wait within 220.5 ms' -and
			$_.State -ceq 'MEASURED_FAIL' }).Count -ne 1 -or
		@($SlowObserved.GateObservations | Where-Object {
			$_.Gate -ceq 'Full fairness, overload backpressure and journal retention margin' -and
			$_.State -ceq 'NOT MEASURED' }).Count -ne 1) {
		throw 'accepted-grant fairness violation was not scoped as a measured failure'
	}
	[IO.File]::WriteAllText($FairnessPath, $OriginalFairness)
	Save-Index -Root $Node.ServerRoot -RunId $Node.Report.RunId -Role 'Server'
	$Node.Report.ServerEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ServerRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	$Node.Report.AdmissionFairnessObservation = Read-AdmissionFairnessEvidence -Path $FairnessPath `
		-RunId $Node.Report.RunId -ExpectedConnections $Node.Report.Identity.Connections
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	$ProviderPath = Join-Path $Node.ServerRoot 'node-provider.json'
	$OriginalProvider = [IO.File]::ReadAllText($ProviderPath)
	$ProviderReceipt = Get-Content -LiteralPath $ProviderPath -Raw | ConvertFrom-Json -AsHashtable
	$ProviderReceipt.RequestId = 'server-content-2'
	Save-Json -Path $ProviderPath -Value $ProviderReceipt
	Save-Index -Root $Node.ServerRoot -RunId $Node.Report.RunId -Role 'Server'
	$Node.Report.ServerEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ServerRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	Assert-Rejected -Name 'rehashed Node provider receipt differs from report' -OutputPath (Join-Path $TestRoot 'rehashed-provider.json')
	[IO.File]::WriteAllText($ProviderPath, $OriginalProvider)
	Save-Index -Root $Node.ServerRoot -RunId $Node.Report.RunId -Role 'Server'
	$Node.Report.ServerEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ServerRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	$LimitPath = Join-Path $Node.ClientRoot 'result.json'
	$OriginalLimit = [IO.File]::ReadAllText($LimitPath)
	$Limited = Get-Content -LiteralPath $LimitPath -Raw | ConvertFrom-Json -AsHashtable
	$Limited.AggregateWorkingSetLimitBytes = 32799999L
	Save-Json -Path $LimitPath -Value $Limited
	Save-Index -Root $Node.ClientRoot -RunId $Node.Report.RunId -Role 'Clients'
	$Node.Report.ClientEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ClientRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	Assert-Rejected -Name 'complete resource sweep over role limit' -OutputPath (Join-Path $TestRoot 'over-limit.json')
	[IO.File]::WriteAllText($LimitPath, $OriginalLimit)
	Save-Index -Root $Node.ClientRoot -RunId $Node.Report.RunId -Role 'Clients'
	$Node.Report.ClientEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ClientRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	$SamplesPath = Join-Path $Node.ClientRoot 'process-resources.csv'
	$OriginalSamples = [IO.File]::ReadAllText($SamplesPath)
	$Sparse = @(Import-Csv -LiteralPath $SamplesPath)
	for ($Index = 0; $Index -lt $Sparse.Count; $Index++) {
		$Sparse[$Index].SupervisorElapsedMilliseconds = [string](1000 + 100 * [math]::Floor($Index / 2) + 10 * ($Index % 2))
	}
	$Sparse | Export-Csv -LiteralPath $SamplesPath -NoTypeInformation
	Save-Index -Root $Node.ClientRoot -RunId $Node.Report.RunId -Role 'Clients'
	$Node.Report.ClientEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ClientRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	$SparseOutput = Join-Path $TestRoot 'sparse-sweeps.json'
	Invoke-Analyzer -OutputPath $SparseOutput
	$SparseObservation = Get-Content -LiteralPath $SparseOutput -Raw | ConvertFrom-Json
	if ($SparseObservation.Node.Resources.Clients.CompleteSweepCount -ne 0 -or
		$SparseObservation.Node.Resources.Clients.CompleteSweepLimitObservation -cne 'NOT_MEASURED' -or
		@($SparseObservation.GateObservations | Where-Object {
			$_.Gate -ceq 'Complete-sweep role-local working set within supervisor limit' -and
			$_.State -ceq 'NOT MEASURED' }).Count -ne 1) {
		throw 'sparse process samples falsely proved a complete-sweep resource bound'
	}
	[IO.File]::WriteAllText($SamplesPath, $OriginalSamples)
	$HostPath = Join-Path $Node.ClientRoot 'host-resources.csv'
	$OriginalHost = [IO.File]::ReadAllText($HostPath)
	Remove-Item -LiteralPath $HostPath
	Assert-Rejected -Name 'missing sealed client host observation' -OutputPath (Join-Path $TestRoot 'missing-host.json')
	[IO.File]::WriteAllText($HostPath, $OriginalHost)
	$HostRows = @(Import-Csv -LiteralPath $HostPath)
	$HostRows[1].NicSentBytes = '1'
	$HostRows | Export-Csv -LiteralPath $HostPath -NoTypeInformation
	Save-Index -Root $Node.ClientRoot -RunId $Node.Report.RunId -Role 'Clients'
	$Node.Report.ClientEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ClientRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	Assert-Rejected -Name 'rehashed NIC counter reset' -OutputPath (Join-Path $TestRoot 'counter-reset.json')
	$HostRows[1].NicSentBytes = '21000000'
	$HostRows[1].HostName = 'OTHERHOST'
	$HostRows | Export-Csv -LiteralPath $HostPath -NoTypeInformation
	Save-Index -Root $Node.ClientRoot -RunId $Node.Report.RunId -Role 'Clients'
	$Node.Report.ClientEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ClientRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	Assert-Rejected -Name 'rehashed host identity drift' -OutputPath (Join-Path $TestRoot 'host-drift.json')
	$HostRows[0].HostName = 'OTHERHOST'
	$HostRows[0].SampleEndTicks = '7000000'
	$HostRows[1].SampleEndTicks = '8000000'
	$HostRows | Export-Csv -LiteralPath $HostPath -NoTypeInformation
	Save-Index -Root $Node.ClientRoot -RunId $Node.Report.RunId -Role 'Clients'
	$Node.Report.ClientEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ClientRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	Assert-Rejected -Name 'overskewed host snapshot' -OutputPath (Join-Path $TestRoot 'host-skew.json')
	$HostRows[0].HostName = 'CLIENT'
	$HostRows[1].HostName = 'CLIENT'
	$HostRows[0].SampleEndTicks = '1001000'
	$HostRows[1].SampleEndTicks = '3001000'
	$HostRows[1].LiveOwnedProcessCount = '31'
	$HostRows | Export-Csv -LiteralPath $HostPath -NoTypeInformation
	Save-Index -Root $Node.ClientRoot -RunId $Node.Report.RunId -Role 'Clients'
	$Node.Report.ClientEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ClientRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	# One earlier full-role sample still establishes overlap; remove it too.
	$HostRows[0].LiveOwnedProcessCount = '31'
	$HostRows | Export-Csv -LiteralPath $HostPath -NoTypeInformation
	Save-Index -Root $Node.ClientRoot -RunId $Node.Report.RunId -Role 'Clients'
	$Node.Report.ClientEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ClientRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	Assert-Rejected -Name 'no simultaneous 32-client observation' -OutputPath (Join-Path $TestRoot 'host-underfill.json')
	$HostRows[0].LiveOwnedProcessCount = '32'
	$HostRows[1].LiveOwnedProcessCount = '32'
	$HostRows[1].NicSentBytes = '4000000000'
	$HostRows | Export-Csv -LiteralPath $HostPath -NoTypeInformation
	Save-Index -Root $Node.ClientRoot -RunId $Node.Report.RunId -Role 'Clients'
	$Node.Report.ClientEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ClientRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	Assert-Rejected -Name 'impossible 10-Gbps NIC sample' -OutputPath (Join-Path $TestRoot 'host-nic-rate.json')
	[IO.File]::WriteAllText($HostPath, $OriginalHost)
	Save-Index -Root $Node.ClientRoot -RunId $Node.Report.RunId -Role 'Clients'
	$Node.Report.ClientEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ClientRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	$OriginalNodeReport = [IO.File]::ReadAllText($Node.ReportPath)
	$Node.Report.ProviderQualification = 'PASS'
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	Assert-Rejected -Name 'forged provider qualification' -OutputPath (Join-Path $TestRoot 'forged-provider.json')
	[IO.File]::WriteAllText($Node.ReportPath, $OriginalNodeReport)
	$OriginalNodeManifest = [IO.File]::ReadAllText((Join-Path $Node.ServerRoot 'run-manifest.json'))
	$Node.Manifest.ClientFrames = 9001
	Save-Json -Path (Join-Path $Node.ServerRoot 'run-manifest.json') -Value $Node.Manifest
	Assert-Rejected -Name 'tampered indexed workload' -OutputPath (Join-Path $TestRoot 'tampered-workload.json')
	Save-Index -Root $Node.ServerRoot -RunId $Node.Report.RunId -Role 'Server'
	Assert-Rejected -Name 'rehashed but mismatched workload pin' -OutputPath (Join-Path $TestRoot 'mismatched-workload.json')
	Save-Json -Path (Join-Path $Node.ClientRoot 'run-manifest.json') -Value $Node.Manifest
	Save-Index -Root $Node.ClientRoot -RunId $Node.Report.RunId -Role 'Clients'
	$Node.Report.ManifestSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ServerRoot 'run-manifest.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	$Node.Report.ServerEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ServerRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	$Node.Report.ClientEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ClientRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	Assert-Rejected -Name 'two well-indexed runs with different workload pins' -OutputPath (Join-Path $TestRoot 'different-workload.json')
	[IO.File]::WriteAllText((Join-Path $Node.ServerRoot 'run-manifest.json'), $OriginalNodeManifest)
	[IO.File]::WriteAllText((Join-Path $Node.ClientRoot 'run-manifest.json'), $OriginalNodeManifest)
	Save-Index -Root $Node.ServerRoot -RunId $Node.Report.RunId -Role 'Server'
	Save-Index -Root $Node.ClientRoot -RunId $Node.Report.RunId -Role 'Clients'
	$Node.Report.ManifestSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ServerRoot 'run-manifest.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	$Node.Report.ServerEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ServerRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	$Node.Report.ClientEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ClientRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	$ResourcePath = Join-Path $Node.ClientRoot 'process-resources.csv'
	$OriginalResources = [IO.File]::ReadAllText($ResourcePath)
	$Rows = @(Import-Csv -LiteralPath $ResourcePath)
	$Rows[1].CpuMilliseconds = '9'
	$Rows | Export-Csv -LiteralPath $ResourcePath -NoTypeInformation
	Save-Index -Root $Node.ClientRoot -RunId $Node.Report.RunId -Role 'Clients'
	$Node.Report.ClientEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ClientRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	Assert-Rejected -Name 'nonmonotonic CPU after index refresh' -OutputPath (Join-Path $TestRoot 'bad-cpu.json')
	[IO.File]::WriteAllText($ResourcePath, $OriginalResources)
	Save-Index -Root $Node.ClientRoot -RunId $Node.Report.RunId -Role 'Clients'
	$Node.Report.ClientEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Node.ClientRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	$Node.Report.RunId = $Local.Report.RunId
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	Assert-Rejected -Name 'reused provider run identity' -OutputPath (Join-Path $TestRoot 'reused-run.json')
	$Node.Report.RunId = $Node.Manifest.RunId
	$Node.Report.ProviderQualification = 'NOT CLAIMED'
	$PublicationFixture = Join-Path $PSScriptRoot '../tools/physical-qualifier/tests/make_farm_publication_fixture.py'
	. (Join-Path $PSScriptRoot 'PhysicalFarmPublicationEvidence.ps1')
	foreach ($Run in @($Local, $Node)) {
		$RunId = $Run.Manifest.RunId
		$ServerReadyPath = Join-Path $Run.ServerRoot 'server.stdout.log'
		for ($Slot = 0; $Slot -lt 32; $Slot++) {
			$Nonce = $Run.Manifest.Nonces[$Slot]
			[IO.File]::AppendAllText($ServerReadyPath,
				"[Qualification:Server] event=ready run=$RunId nonce=$Nonce connection_slot=$($Slot + 1) connection_generation=1 session_epoch=1 player_id=$($Slot + 1) monotonic_us=$($Slot + 1)`n")
			[IO.File]::AppendAllText((Join-Path $Run.ClientRoot ('client-{0:D2}.stdout.log' -f $Slot)),
				"[Qualification:Client] event=ready run_id=$RunId slot=$Slot nonce=$Nonce connection_slot=$($Slot + 2) connection_generation=1 steady_ns=$($Slot + 1)`n")
		}
		& python $PublicationFixture (Join-Path $Run.ServerRoot 'run-manifest.json') `
			$Run.ServerRoot $Run.ClientRoot
		if ($LASTEXITCODE -ne 0) { throw 'cross-provider publication fixture generation failed' }
		Save-Index -Root $Run.ServerRoot -RunId $RunId -Role 'Server'
		Save-Index -Root $Run.ClientRoot -RunId $RunId -Role 'Clients'
		$Run.Report.ServerEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Run.ServerRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
		$Run.Report.ClientEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $Run.ClientRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
		$Run.Report.PublicationObservation = Read-FarmPublicationObservation `
			-ServerRoot $Run.ServerRoot -ClientRoot $Run.ClientRoot `
			-ServerIndex (Get-Content -LiteralPath (Join-Path $Run.ServerRoot 'evidence-sha256.json') -Raw | ConvertFrom-Json -AsHashtable) `
			-ClientIndex (Get-Content -LiteralPath (Join-Path $Run.ClientRoot 'evidence-sha256.json') -Raw | ConvertFrom-Json -AsHashtable) `
			-RunManifestPath (Join-Path $Run.ServerRoot 'run-manifest.json') -ScratchParent $TestRoot
		Save-Json -Path $Run.ReportPath -Value $Run.Report
	}
	$PublicationAcceptancePath = Join-Path $TestRoot 'publication-acceptance.json'
	Invoke-Analyzer -OutputPath $PublicationAcceptancePath
	$PublicationAcceptance = Get-Content -LiteralPath $PublicationAcceptancePath -Raw | ConvertFrom-Json
	if ($PublicationAcceptance.Status -cne 'INCOMPLETE' -or
		$PublicationAcceptance.Local.Publication.Server.Accepted -ne 32 -or
		$PublicationAcceptance.Node.Publication.Client.ClientHandled -ne 32 -or
		$PublicationAcceptance.Local.Publication.AnalyzerSha256 -cnotmatch '^[a-f0-9]{64}$' -or
		@($PublicationAcceptance.GateObservations | Where-Object Gate -eq 'Character accepted-state chain and role-local publication delays' |
			Where-Object State -eq 'MEASURED').Count -ne 1 -or
		@($PublicationAcceptance.GateObservations | Where-Object Gate -eq 'Tracked-root recipient Character cadence' |
			Where-Object State -eq 'NOT MEASURED').Count -ne 1 -or
		@($PublicationAcceptance.GateObservations | Where-Object Gate -eq 'Full Character and Remote recipient cadence' |
			Where-Object State -eq 'NOT MEASURED').Count -ne 1) {
		throw 'cross-provider Character publication subset was promoted or lost'
	}
	$Node.Report.PublicationObservation.Server.Accepted = 31
	Save-Json -Path $Node.ReportPath -Value $Node.Report
	Assert-Rejected -Name 'forged reconciled Character publication count' `
		-OutputPath (Join-Path $TestRoot 'forged-publication.json')
	Write-Output '[Qualification:FarmAcceptance] MOCK_TEST_OK'
} finally {
	if (-not $ResolvedRoot.StartsWith($ResolvedTemp + [IO.Path]::DirectorySeparatorChar,
		[StringComparison]::OrdinalIgnoreCase) -or
		[IO.Path]::GetFileName($ResolvedRoot) -cnotmatch '^physical-farm-acceptance-test-[a-f0-9]{32}$') {
		throw 'refusing recursive test cleanup outside expected temporary root'
	}
	if (Test-Path -LiteralPath $ResolvedRoot) { Remove-Item -LiteralPath $ResolvedRoot -Recurse -Force }
}
