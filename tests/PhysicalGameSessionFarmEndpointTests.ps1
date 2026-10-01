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
	Write-Output '[Qualification:FarmEndpoint] MOCK_TEST_OK'
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
