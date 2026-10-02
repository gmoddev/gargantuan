#requires -Version 7.0
# Socket-free acceptance-observation tests; no provider or physical run starts.
$ErrorActionPreference = 'Stop'
$Analyzer = Join-Path $PSScriptRoot 'PhysicalGameSessionFarmAcceptance.ps1'
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

function New-RunFixture {
	param([string]$Prefix, [string]$Provider, [string]$RunId)
	$ServerRoot = Join-Path $TestRoot "$Prefix-server"
	$ClientRoot = Join-Path $TestRoot "$Prefix-clients"
	[void][IO.Directory]::CreateDirectory($ServerRoot)
	[void][IO.Directory]::CreateDirectory($ClientRoot)
	$Manifest = [ordered]@{
		Format = 'GargantuanPhysicalFarmEndpoint'; Version = 1
		RunId = $RunId; SourceCommit = ('b' * 40)
		Endpoint = '10.253.3.2:39450'; Provider = $Provider; ScaleWorkload = $true
		ClientFrames = 9000; ServerTicks = 10000
		Nonces = @(0..31 | ForEach-Object { [string](([UInt64]123 -shl 32) + [UInt64]($_ + 1)) })
		ServerSha256 = ('a' * 64); ServerPackageSha256 = ('a' * 64)
		PlayerSha256 = ('a' * 64); PlayerPackageSha256 = ('a' * 64)
		ServerContentManifestSha256 = ('a' * 64); PlayerContentManifestSha256 = ('a' * 64)
		ServerDeploymentSha256 = ('a' * 64); PlayerDeploymentSha256 = ('a' * 64)
	}
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
	foreach ($RoleRoot in @($ServerRoot, $ClientRoot)) {
		$RoleName = if ($RoleRoot -eq $ServerRoot) { 'Server' } else { 'Clients' }
		Save-Json -Path (Join-Path $RoleRoot 'result.json') -Value ([ordered]@{
			RunId = $RunId; Role = $RoleName; Status = 'PASS'
			AggregateWorkingSetLimitBytes = 21474836480L
		})
	}
	Save-Index -Root $ServerRoot -RunId $RunId -Role 'Server'
	Save-Index -Root $ClientRoot -RunId $RunId -Role 'Clients'
	$Report = [ordered]@{
		Format = 'GargantuanPhysicalFarmReconciliation'; Version = 1
		RunId = $RunId; Provider = $Provider
		ManifestSha256 = (Get-FileHash -LiteralPath (Join-Path $ServerRoot 'run-manifest.json') -Algorithm SHA256).Hash.ToLowerInvariant()
		ServerEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $ServerRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
		ClientEvidenceSha256 = (Get-FileHash -LiteralPath (Join-Path $ClientRoot 'evidence-sha256.json') -Algorithm SHA256).Hash.ToLowerInvariant()
		Status = 'INCOMPLETE'; RoleLocalEvidence = 'VALIDATED'; ProviderQualification = 'NOT CLAIMED'
		Identity = @{ Ready = 32 }; Admission = @{
			accepted = 8192; retired = 8192; terminal_release = 0; outstanding = 0; active_grants = 0
		}
		ServerResourceSamples = $ServerSamples; ClientResourceSamples = $ClientSamples
	}
	$ReportPath = Join-Path $TestRoot "$Prefix-report.json"
	Save-Json -Path $ReportPath -Value $Report
	return [pscustomobject]@{
		ServerRoot = $ServerRoot; ClientRoot = $ClientRoot
		ReportPath = $ReportPath; Report = $Report; Manifest = $Manifest
	}
}

function Invoke-Analyzer {
	param([string]$OutputPath)
	& $Analyzer -LocalReportPath $Local.ReportPath -LocalServerEvidenceRoot $Local.ServerRoot `
		-LocalClientEvidenceRoot $Local.ClientRoot -NodeReportPath $Node.ReportPath `
		-NodeServerEvidenceRoot $Node.ServerRoot -NodeClientEvidenceRoot $Node.ClientRoot `
		-OutputPath $OutputPath | Out-Null
}

function Assert-Rejected {
	param([string]$Name, [string]$OutputPath)
	$Rejected = $false
	try { Invoke-Analyzer -OutputPath $OutputPath } catch { $Rejected = $true }
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
		@($Observed.GateObservations | Where-Object { $_.State -eq 'NOT MEASURED' }).Count -ne 5) {
		throw "resource/parity observation promoted a missing physical gate or lost resource evidence: status=$($Observed.Status) claim=$($Observed.Foundation3LQualification) parity=$($Observed.WorkloadPinParity.State) clients=$($Observed.Local.Resources.Clients.ProcessCount) server=$($Observed.Node.Resources.Server.ProcessCount) ws=$($Observed.Local.Resources.Clients.SumOfPerProcessPeakWorkingSetBytes) missing=$(@($Observed.GateObservations | Where-Object { $_.State -eq 'NOT MEASURED' }).Count)"
	}
	$OriginalOutput = [IO.File]::ReadAllText($OutputPath)
	$OverwriteRejected = $false
	try { Invoke-Analyzer -OutputPath $OutputPath } catch { $OverwriteRejected = $true }
	if (-not $OverwriteRejected -or [IO.File]::ReadAllText($OutputPath) -cne $OriginalOutput) {
		throw 'existing acceptance observation was overwritten'
	}
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
	Write-Output '[Qualification:FarmAcceptance] MOCK_TEST_OK'
} finally {
	if (-not $ResolvedRoot.StartsWith($ResolvedTemp + [IO.Path]::DirectorySeparatorChar,
		[StringComparison]::OrdinalIgnoreCase) -or
		[IO.Path]::GetFileName($ResolvedRoot) -cnotmatch '^physical-farm-acceptance-test-[a-f0-9]{32}$') {
		throw 'refusing recursive test cleanup outside expected temporary root'
	}
	if (Test-Path -LiteralPath $ResolvedRoot) { Remove-Item -LiteralPath $ResolvedRoot -Recurse -Force }
}
