#requires -Version 7.0
# Harmless source-level tests for the role-local endpoint supervisor. No farm
# executable, socket, capture, or privileged service is started.

$ErrorActionPreference = 'Stop'
$Source = Join-Path $PSScriptRoot 'PhysicalGameSessionFarmEndpoint.ps1'
$Tokens = $null
$Errors = $null
$Ast = [Management.Automation.Language.Parser]::ParseFile($Source, [ref]$Tokens, [ref]$Errors)
if ($Errors.Count -ne 0) { throw "endpoint syntax failed: $($Errors[0].Message)" }
foreach ($Function in $Ast.FindAll({ param($Node)
	$Node -is [Management.Automation.Language.FunctionDefinitionAst]
}, $true)) {
	. ([scriptblock]::Create($Function.Extent.Text))
}

function Expect-Rejection {
	param([scriptblock]$Case, [string]$Reason)
	$Rejected = $false
	try { & $Case } catch { $Rejected = $true }
	if (-not $Rejected) { throw "expected rejection: $Reason" }
}

$Root = Join-Path ([IO.Path]::GetTempPath()) ('farm-endpoint-test-' + [Guid]::NewGuid().ToString('N'))
$Package = Join-Path $Root 'Package'
$Evidence = Join-Path $Root 'Evidence'
$Registry = Join-Path $Root 'Registry'
[void][IO.Directory]::CreateDirectory($Package)
try {
	$LiveLogPath = Join-Path $Root 'live-server.log'
	$LiveWriter = [IO.File]::Open($LiveLogPath, [IO.FileMode]::CreateNew,
		[IO.FileAccess]::Write, [IO.FileShare]::Read)
	try {
		$First = [Text.Encoding]::UTF8.GetBytes("[Qualification:Server] event=start run=live`n[Qualification:Server] event=res")
		$LiveWriter.Write($First, 0, $First.Length)
		$LiveWriter.Flush()
		$Records = @(Get-TypedRecords -Path $LiveLogPath -Kind 'Server')
		if ($Records.Count -ne 1 -or $Records[0].event -cne 'start') {
			throw 'live endpoint log reader exposed an incomplete line or failed sharing'
		}
		$Tail = [Text.Encoding]::UTF8.GetBytes("ult run=live`n")
		$LiveWriter.Write($Tail, 0, $Tail.Length)
		$LiveWriter.Flush()
		$Records = @(Get-TypedRecords -Path $LiveLogPath -Kind 'Server')
		if ($Records.Count -ne 2 -or $Records[1].event -cne 'result') {
			throw 'live endpoint log reader lost a completed line'
		}
	} finally { $LiveWriter.Dispose() }

	$Prefix = [UInt64]4294967296
	$Nonces = @(for ($Slot = 0; $Slot -lt 32; $Slot++) { [string]($Prefix + [UInt64]($Slot + 1)) })
	$Manifest = [ordered]@{
		Format = 'GargantuanPhysicalFarmEndpoint'; Version = 1L
		RunId = '12345678-1234-4234-8234-123456789abc'
		SourceCommit = 'a' * 40; Endpoint = '127.0.0.1:39450'
		Provider = 'Local'; ScaleWorkload = $true
		ClientFrames = 9000L; ServerTicks = 10000L; Nonces = $Nonces
		ServerSha256 = 'a' * 64; ServerPackageSha256 = 'b' * 64
		PlayerSha256 = 'c' * 64; PlayerPackageSha256 = 'd' * 64
		ServerContentManifestSha256 = 'e' * 64; PlayerContentManifestSha256 = 'f' * 64
		ServerDeploymentSha256 = '1' * 64; PlayerDeploymentSha256 = '2' * 64
	}
	$Parsed = ($Manifest | ConvertTo-Json -Depth 6 | ConvertFrom-Json -AsHashtable)
	$ManifestFile = Join-Path $Root 'run-manifest.json'
	[IO.File]::WriteAllText($ManifestFile, ($Manifest | ConvertTo-Json -Depth 6))
	$ManifestPin = (Get-FileHash -LiteralPath $ManifestFile -Algorithm SHA256).Hash
	if ((Read-PinnedRunManifest -Path $ManifestFile -ExpectedSha256 $ManifestPin).RunId -ne $Manifest.RunId) {
		throw 'correctly pinned run manifest was rejected'
	}
	Expect-Rejection {
		Read-PinnedRunManifest -Path $ManifestFile -ExpectedSha256 ('0' * 64)
	} 'wrong pre-created manifest SHA-256 pin'
	$Network = Assert-RunManifest -Value $Parsed
	if ($Network.Port -ne 39450 -or $Network.NoncePrefix -ne 1) {
		throw 'valid shared manifest or fixed nonces were rejected'
	}
	$Bad = ($Manifest | ConvertTo-Json -Depth 6 | ConvertFrom-Json -AsHashtable)
	$Bad.Nonces[31] = $Bad.Nonces[30]
	Expect-Rejection { Assert-RunManifest -Value $Bad } 'duplicate/reassigned nonce'
	$Bad = ($Manifest | ConvertTo-Json -Depth 6 | ConvertFrom-Json -AsHashtable)
	$Bad.ClientFrames = 1800L
	Expect-Rejection { Assert-RunManifest -Value $Bad } 'short scale client frame budget'
	$Bad.ScaleWorkload = $false
	[void](Assert-RunManifest -Value $Bad)
	$Bad = ($Manifest | ConvertTo-Json -Depth 6 | ConvertFrom-Json -AsHashtable)
	$Bad.ServerSha256 = '0' * 63
	Expect-Rejection { Assert-RunManifest -Value $Bad } 'invalid binary SHA-256 pin'
	$Bad = ($Manifest | ConvertTo-Json -Depth 6 | ConvertFrom-Json -AsHashtable)
	$Bad.Provider = 'Node'
	$Bad.NodeEndpoint = '127.0.0.1:4433'
	$Bad.NodeRootCertificateSha256 = 'a' * 64
	$Bad.NodeTokenEnvironment = 'GARGANTUAN_ENGINE_ADAPTER_TOKEN'
	[void](Assert-RunManifest -Value $Bad)
	$Bad.NodeEndpoint = 'https://127.0.0.1:4433'
	Expect-Rejection { Assert-RunManifest -Value $Bad } 'Node CLI endpoint cannot be URL syntax'
	$Bad = ($Manifest | ConvertTo-Json -Depth 6 | ConvertFrom-Json -AsHashtable)
	$Bad.ServerCommand = 'powershell'
	Expect-Rejection { Assert-RunManifest -Value $Bad } 'arbitrary manifest command'

	$Paths = Assert-RolePaths -LocalPackage $Package -LocalEvidence $Evidence -LocalRegistry $Registry
	if ($Paths.Package -ne $Package) { throw 'role-local package path changed' }
	Expect-Rejection {
		Assert-RolePaths -LocalPackage $Package -LocalEvidence (Join-Path $Package 'Evidence') -LocalRegistry $Registry
	} 'evidence nested under package'
	[void][IO.Directory]::CreateDirectory($Evidence)
	Expect-Rejection {
		Assert-RolePaths -LocalPackage $Package -LocalEvidence $Evidence -LocalRegistry $Registry
	} 'stale evidence root'

	$Files = @('GargantuanPlayer.exe', 'game.package.json', 'content/content.manifest.json', 'runtime.dll')
	foreach ($Relative in $Files) {
		$Path = Join-Path $Package $Relative
		[void][IO.Directory]::CreateDirectory((Split-Path $Path -Parent))
		[IO.File]::WriteAllText($Path, "mock-$Relative")
	}
	$DeploymentFiles = @($Files | ForEach-Object {
		$Path = Join-Path $Package $_
		[ordered]@{ Path = $_.Replace('\', '/'); Bytes = ([IO.FileInfo]$Path).Length;
			Sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
	})
	$DeploymentPath = Join-Path $Package 'deployment-sha256.json'
	[IO.File]::WriteAllText($DeploymentPath,
		([ordered]@{ Format = 'GargantuanFarmDeployment'; Version = 1;
			SourceCommit = 'a' * 40; Files = $DeploymentFiles } |
		ConvertTo-Json -Depth 5))
	$Pinned = ($Manifest | ConvertTo-Json -Depth 6 | ConvertFrom-Json -AsHashtable)
	$Pinned.PlayerSha256 = (Get-FileHash -LiteralPath (Join-Path $Package 'GargantuanPlayer.exe') -Algorithm SHA256).Hash
	$Pinned.PlayerPackageSha256 = (Get-FileHash -LiteralPath (Join-Path $Package 'game.package.json') -Algorithm SHA256).Hash
	$Pinned.PlayerContentManifestSha256 = (Get-FileHash -LiteralPath (Join-Path $Package 'content/content.manifest.json') -Algorithm SHA256).Hash
	$Pinned.PlayerDeploymentSha256 = (Get-FileHash -LiteralPath $DeploymentPath -Algorithm SHA256).Hash
	$ExpectedPlayer = Assert-LocalPackagePins -LocalRoot $Package -LocalRole Clients -RunManifest $Pinned
	if ($ExpectedPlayer -ne (Join-Path $Package 'GargantuanPlayer.exe')) { throw 'client executable was not fixed' }
	$Pinned.PlayerPackageSha256 = '0' * 64
	Expect-Rejection {
		Assert-LocalPackagePins -LocalRoot $Package -LocalRole Clients -RunManifest $Pinned
	} 'role-specific package descriptor pin mismatch'
	$Pinned.PlayerPackageSha256 = (Get-FileHash -LiteralPath (Join-Path $Package 'game.package.json') -Algorithm SHA256).Hash
	[IO.File]::WriteAllText((Join-Path $Package 'runtime.dll'), 'tampered')
	Expect-Rejection {
		Assert-LocalPackagePins -LocalRoot $Package -LocalRole Clients -RunManifest $Pinned
	} 'unlisted package binary changed despite the three direct pins'

	$LogA = Join-Path $Evidence 'a.log'; $LogB = Join-Path $Evidence 'b.log'
	[IO.File]::WriteAllText($LogA, 'a'); [IO.File]::WriteAllText($LogB, 'b')
	$ProcessA = [pscustomobject]@{ HasExited = $false; WorkingSet64 = 100L; Threads = @(1) }
	$ProcessB = [pscustomobject]@{ HasExited = $false; WorkingSet64 = 100L; Threads = @(1) }
	$ProcessA | Add-Member ScriptMethod Refresh { }
	$ProcessB | Add-Member ScriptMethod Refresh { }
	$Owners = @(
		[pscustomobject]@{ Label = 'a'; Process = $ProcessA; OutputPath = $LogA; ErrorPath = $LogB },
		[pscustomobject]@{ Label = 'b'; Process = $ProcessB; OutputPath = $LogB; ErrorPath = $LogA }
	)
	Expect-Rejection {
		Assert-EndpointBounds -Owners $Owners -MaximumLogBytes 10 -MaximumMemoryBytes 150 `
			-MaximumAggregateMemoryBytes 150 -MaximumThreads 2
	} 'two clients exceed aggregate memory though each is individually below cap'

	$MockProcess = [pscustomobject]@{ HasExited = $false; Killed = $false; Disposed = $false }
	$MockProcess | Add-Member ScriptMethod Kill { param($Tree) $this.Killed = $true; $this.HasExited = $true }
	$MockProcess | Add-Member ScriptMethod WaitForExit { param($Milliseconds) return $this.HasExited }
	$MockProcess | Add-Member ScriptMethod Dispose { $this.Disposed = $true }
	$Output = [IO.MemoryStream]::new(); $ErrorStream = [IO.MemoryStream]::new()
	$Owner = [pscustomobject]@{
		Pid = 123; Process = $MockProcess; OutputStream = $Output; ErrorStream = $ErrorStream
		OutputCopy = [Threading.Tasks.Task]::CompletedTask
		ErrorCopy = [Threading.Tasks.Task]::CompletedTask
	}
	Stop-EndpointProcess -Owner $Owner
	if (-not $MockProcess.Killed -or -not $MockProcess.Disposed -or $Output.CanWrite -or $ErrorStream.CanWrite) {
		throw 'owned-PID cleanup did not terminate and dispose the mock process and streams'
	}
	$FairnessPath = Join-Path $Evidence 'admission-fairness.tsv'
	$FairnessLines = @(
		"format=GargantuanAdmissionEvidenceV1`trun=$($Manifest.RunId)",
		"event`texact_demand`tnone`t1`t1`t1`t0`t0`t77`t250000`t250000`t250000`t524288`t2097152`t4`t0`t0`t0`t0",
		"end`t1`t0")
	[IO.File]::WriteAllText($FairnessPath, ($FairnessLines -join "`n") + "`n")
	$Fairness = Assert-FairnessEvidence -Path $FairnessPath -LocalRunId $Manifest.RunId
	if ($Fairness.Count -ne 1 -or $Fairness.Bytes -ne ([IO.FileInfo]$FairnessPath).Length) {
		throw 'bounded native fairness evidence was not accepted'
	}
	Write-EndpointEvidenceHash -Directory $Evidence -LocalRunId $Manifest.RunId -LocalRole Server
	$Index = Get-Content -LiteralPath (Join-Path $Evidence 'evidence-sha256.json') -Raw | ConvertFrom-Json
	if (@($Index.Files | Where-Object Name -eq 'admission-fairness.tsv').Count -ne 1) {
		throw 'native fairness evidence was omitted from the immutable role-local index'
	}
	[IO.File]::WriteAllText($FairnessPath, ($FairnessLines[0..1] -join "`n") + "`n")
	Expect-Rejection {
		Assert-FairnessEvidence -Path $FairnessPath -LocalRunId $Manifest.RunId
	} 'missing native fairness evidence end record'
	[IO.File]::WriteAllText($FairnessPath, ($FairnessLines[0..1] + "end`t1`t1" -join "`n") + "`n")
	Expect-Rejection {
		Assert-FairnessEvidence -Path $FairnessPath -LocalRunId $Manifest.RunId
	} 'native fairness evidence overflow marker'
	Write-Output '[Qualification:FarmEndpoint] MOCK_TEST_OK'
	# Both roles must reject paths the .NET copier can handle but the native
	# package reader cannot, before publishing even an attempt or registry.
	. (Join-Path $PSScriptRoot 'PhysicalFarmPackageTestFixture.ps1')
	$NativePathCases = 0
	foreach ($ProjectionRole in @('Server', 'Clients')) {
		$ProjectionSource = Join-Path $Root ("PathEnvelope-$ProjectionRole")
		$FixtureRole = if ($ProjectionRole -eq 'Server') { 'Server' } else { 'Player' }
		Write-ProjectionTestEnvelope -Root $ProjectionSource -Role $FixtureRole
		$ProjectionPin = (Get-FileHash -LiteralPath (Join-Path $ProjectionSource 'deployment-sha256.json')).Hash
		$ProjectionPlan = Get-NativeRuntimePlan -LocalRoot $ProjectionSource -DeploymentSha256 $ProjectionPin `
			-SourceCommit ('a' * 40) -LocalRole $ProjectionRole
		$MaximumRelativeLength = ($ProjectionPlan.Files | ForEach-Object { $_.Path.Length } | Measure-Object -Maximum).Maximum
		$ProjectionRun = '12345678-1234-4234-8234-123456789abc'
		$ProjectionParent = Join-Path $Root ("PathRegistry-$ProjectionRole")
		foreach ($AbsoluteLength in @(240, 241, 270)) {
			$RuntimeSuffix = "$ProjectionRun.$ProjectionRole.runtime"
			$RegistryLength = $AbsoluteLength - 1 - $MaximumRelativeLength - 1 - $RuntimeSuffix.Length
			$RegistryLeafLength = $RegistryLength - $ProjectionParent.Length - 1
			if ($RegistryLeafLength -lt 1 -or $RegistryLeafLength -gt 255) { throw 'native path fixture geometry invalid' }
			$ProjectionRegistry = Join-Path $ProjectionParent ('r' * $RegistryLeafLength)
			$ProjectionRuntime = Get-RuntimeProjectionPath -Registry $ProjectionRegistry -LocalRunId $ProjectionRun -LocalRole $ProjectionRole
			$ActualMaximum = ($ProjectionPlan.Files | ForEach-Object {
				(Join-Path $ProjectionRuntime $_.Path).Length
			} | Measure-Object -Maximum).Maximum
			if ($ActualMaximum -ne $AbsoluteLength) { throw 'native path boundary fixture differs' }
			if ($AbsoluteLength -eq 240) {
				# Exercise the actual seven-function AST extraction, without
				# launching any of the copied harmless mock host bytes.
				& (Join-Path $PSScriptRoot 'NewPhysicalGameSessionFarmProjection.ps1') `
					-PackageRoot $ProjectionSource -RunRegistryRoot $ProjectionRegistry -RunId $ProjectionRun `
					-Role $ProjectionRole -DeploymentSha256 $ProjectionPin -SourceCommit ('a' * 40) | Out-Null
				$PreparedRuntime = Assert-PreparedRuntimeProjection -LocalRoot $ProjectionSource -Registry $ProjectionRegistry `
					-LocalRunId $ProjectionRun -LocalRole $ProjectionRole -Plan $ProjectionPlan
				if ($PreparedRuntime -cne $ProjectionRuntime) {
					throw 'native boundary projection did not publish and validate'
				}
				$NativePathCases++
			} else {
				foreach ($Operation in @('New', 'Helper', 'Prepared', 'Content')) {
					$PathRejected = $false
					try {
						switch ($Operation) {
							'New' { New-RuntimeProjection -LocalRoot $ProjectionSource -Registry $ProjectionRegistry `
								-LocalRunId $ProjectionRun -LocalRole $ProjectionRole -Plan $ProjectionPlan }
							'Helper' { & (Join-Path $PSScriptRoot 'NewPhysicalGameSessionFarmProjection.ps1') `
								-PackageRoot $ProjectionSource -RunRegistryRoot $ProjectionRegistry -RunId $ProjectionRun `
								-Role $ProjectionRole -DeploymentSha256 $ProjectionPin -SourceCommit ('a' * 40) }
							'Prepared' { Assert-PreparedRuntimeProjection -LocalRoot $ProjectionSource -Registry $ProjectionRegistry `
								-LocalRunId $ProjectionRun -LocalRole $ProjectionRole -Plan $ProjectionPlan }
							'Content' { Assert-RuntimeProjection -LocalRoot $ProjectionSource -RuntimeRoot $ProjectionRuntime -Plan $ProjectionPlan }
						}
					} catch {
						if ($_.Exception.Message -cne 'runtime projection path exceeds native Windows 240-character bound') { throw }
						$PathRejected = $true
					}
					if (-not $PathRejected) { throw "native path accepted by $Operation at $AbsoluteLength" }
					$NativePathCases++
				}
				foreach ($Unpublished in @($ProjectionRegistry, $ProjectionRuntime, ($ProjectionRuntime + '.attempt'), ($ProjectionRuntime + '.json'))) {
					if (Test-Path -LiteralPath $Unpublished) { throw 'long projection published state before rejection' }
				}
			}
		}
	}
	Write-Output "[Qualification:FarmEndpoint] NATIVE_PATH_TEST_OK Cases=$NativePathCases"
	& (Join-Path $PSScriptRoot 'NewPhysicalGameSessionFarmProjectionTests.ps1')
	& (Join-Path $PSScriptRoot 'PhysicalGameSessionFarmSamplerTests.ps1')
} finally {
	$ResolvedRoot = [IO.Path]::GetFullPath($Root)
	$ResolvedTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
	if (-not $ResolvedRoot.StartsWith($ResolvedTemp + [IO.Path]::DirectorySeparatorChar,
		[StringComparison]::OrdinalIgnoreCase) -or
		[IO.Path]::GetFileName($ResolvedRoot) -cnotmatch '^farm-endpoint-test-[a-f0-9]{32}$') {
		throw 'refusing recursive test cleanup outside the expected temporary root'
	}
	if (Test-Path -LiteralPath $ResolvedRoot) {
		Remove-Item -LiteralPath $ResolvedRoot -Recurse -Force
	}
}
