#requires -Version 7.0
# No executable launch, network probe, or capture. Builds tiny mock packages,
# invokes the real manifest creator, then uses the endpoint's own validator.

$ErrorActionPreference = 'Stop'
$Creator = Join-Path $PSScriptRoot 'NewPhysicalGameSessionFarmManifest.ps1'
$Endpoint = Join-Path $PSScriptRoot 'PhysicalGameSessionFarmEndpoint.ps1'
foreach ($Path in @($Creator, $Endpoint)) {
	$Tokens = $null; $Errors = $null
	[void][Management.Automation.Language.Parser]::ParseFile($Path, [ref]$Tokens, [ref]$Errors)
	if ($Errors.Count -ne 0) { throw "PowerShell syntax failed: $Path $($Errors[0].Message)" }
}
$Tokens = $null; $Errors = $null
$Ast = [Management.Automation.Language.Parser]::ParseFile($Endpoint, [ref]$Tokens, [ref]$Errors)
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

function Write-MockPackage {
	param([string]$Root, [ValidateSet('Server', 'Player')][string]$Role)
	[void][IO.Directory]::CreateDirectory((Join-Path $Root 'content'))
	$Binary = if ($Role -eq 'Server') { 'GargantuanServer.exe' } else { 'GargantuanPlayer.exe' }
	$Names = @($Binary, 'game.package.json', 'content/content.manifest.json', 'runtime.dll')
	foreach ($Name in $Names) {
		[IO.File]::WriteAllText((Join-Path $Root $Name), "$Role/$Name")
	}
	$Files = @($Names | ForEach-Object {
		$Path = Join-Path $Root $_
		[ordered]@{ Path = $_; Bytes = ([IO.FileInfo]$Path).Length;
			Sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
	})
	[IO.File]::WriteAllText((Join-Path $Root 'deployment-sha256.json'),
		([ordered]@{ Format = 'GargantuanFarmDeployment'; Version = 1;
			SourceCommit = 'a' * 40; Files = $Files } |
		ConvertTo-Json -Depth 5))
}

$Root = Join-Path ([IO.Path]::GetTempPath()) ('farm-manifest-test-' + [Guid]::NewGuid().ToString('N'))
$ServerPackage = Join-Path $Root 'ServerPackage'
$PlayerPackage = Join-Path $Root 'PlayerPackage'
$LocalOutput = Join-Path $Root 'LocalOutput'
$NodeOutput = Join-Path $Root 'NodeOutput'
[void][IO.Directory]::CreateDirectory($Root)
try {
	Write-MockPackage -Root $ServerPackage -Role Server
	Write-MockPackage -Root $PlayerPackage -Role Player
	& $Creator -ServerPackageRoot $ServerPackage -PlayerPackageRoot $PlayerPackage `
		-OutputRoot $LocalOutput -RunId '12345678-1234-4234-8234-123456789abc' -Endpoint '127.0.0.1:39450' `
		-Provider Local -ScaleWorkload -ClientFrames 9000 -ServerTicks 10000 | Out-Null
	$ServerManifest = Join-Path $LocalOutput 'Server/run-manifest.json'
	$ClientManifest = Join-Path $LocalOutput 'Clients/run-manifest.json'
	$ServerBytes = [IO.File]::ReadAllBytes($ServerManifest)
	$ClientBytes = [IO.File]::ReadAllBytes($ClientManifest)
	if ([Convert]::ToHexString($ServerBytes) -cne [Convert]::ToHexString($ClientBytes)) {
		throw 'role-local manifests differ in bytes'
	}
	$Pin = (Get-FileHash -LiteralPath $ServerManifest -Algorithm SHA256).Hash
	$Manifest = Read-PinnedRunManifest -Path $ServerManifest -ExpectedSha256 $Pin
	if ($Manifest.RunId -ne '12345678-1234-4234-8234-123456789abc' -or $Manifest.Provider -ne 'Local' -or
		$Manifest.Nonces.Count -ne 32 -or $Manifest.Contains('NodeEndpoint')) {
		throw 'Local run manifest fields are invalid'
	}
	[void](Assert-LocalPackagePins -LocalRoot $ServerPackage -LocalRole Server -RunManifest $Manifest)
	[void](Assert-LocalPackagePins -LocalRoot $PlayerPackage -LocalRole Clients -RunManifest $Manifest)
	Expect-Rejection {
		& $Creator -ServerPackageRoot $ServerPackage -PlayerPackageRoot $PlayerPackage `
			-OutputRoot $LocalOutput -RunId '12345678-1234-4234-8234-123456789abc' -Endpoint '127.0.0.1:39450' `
			-Provider Local -ScaleWorkload -ClientFrames 9000 -ServerTicks 10000 | Out-Null
	} 'stale output root or reused local output'
	$Certificate = Join-Path $Root 'node-root.pem'
	[IO.File]::WriteAllText($Certificate, 'mock-root-cert')
	& $Creator -ServerPackageRoot $ServerPackage -PlayerPackageRoot $PlayerPackage `
		-OutputRoot $NodeOutput -RunId '12345678-1234-4234-8234-123456789abd' -Endpoint '127.0.0.1:39450' `
		-Provider Node -ScaleWorkload -ClientFrames 9000 -ServerTicks 10000 `
		-NodeEndpoint 'node.example:4433' -NodeRootCertificatePath $Certificate `
		-NodeTokenEnvironment 'GARGANTUAN_ENGINE_ADAPTER_TOKEN' | Out-Null
	$NodeManifestPath = Join-Path $NodeOutput 'Server/run-manifest.json'
	$NodeManifest = Read-PinnedRunManifest -Path $NodeManifestPath `
		-ExpectedSha256 (Get-FileHash -LiteralPath $NodeManifestPath -Algorithm SHA256).Hash
	if ($NodeManifest.NodeEndpoint -ne 'node.example:4433' -or
		$NodeManifest.NodeRootCertificateSha256 -ne
		(Get-FileHash -LiteralPath $Certificate -Algorithm SHA256).Hash.ToLowerInvariant() -or
		$NodeManifest.Contains('NodeToken')) {
		throw 'Node run manifest leaked a token or lost its TLS root pin'
	}
	$DefaultOutput = Join-Path $Root 'DefaultOutput'
	& $Creator -ServerPackageRoot $ServerPackage -PlayerPackageRoot $PlayerPackage `
		-OutputRoot $DefaultOutput -Endpoint '127.0.0.1:39450' -ClientFrames 1800 `
		-ServerTicks 9000 | Out-Null
	$DefaultManifest = Get-Content -LiteralPath (Join-Path $DefaultOutput 'Server/run-manifest.json') `
		-Raw | ConvertFrom-Json -AsHashtable
	$DefaultRunId = [Guid]::Empty
	if (-not [Guid]::TryParseExact($DefaultManifest.RunId, 'D', [ref]$DefaultRunId) -or
		$DefaultRunId.ToString('D') -cne $DefaultManifest.RunId) {
		throw 'default run ID is not a canonical capture-service UUID'
	}
	Expect-Rejection {
		& $Creator -ServerPackageRoot $ServerPackage -PlayerPackageRoot $PlayerPackage `
			-OutputRoot (Join-Path $Root 'InvalidRunId') -RunId 'farm-not-a-uuid' `
			-Endpoint '127.0.0.1:39450' -ClientFrames 1800 -ServerTicks 9000 | Out-Null
	} 'non-UUID physical run ID'
	[IO.File]::WriteAllText((Join-Path $PlayerPackage 'runtime.dll'), 'tampered')
	Expect-Rejection {
		& $Creator -ServerPackageRoot $ServerPackage -PlayerPackageRoot $PlayerPackage `
			-OutputRoot (Join-Path $Root 'TamperedOutput') -Endpoint '127.0.0.1:39450' `
			-Provider Local -ClientFrames 1800 -ServerTicks 9000 | Out-Null
	} 'mismatched listed runtime file'
	[IO.File]::WriteAllText((Join-Path $PlayerPackage 'runtime.dll'), 'Player/runtime.dll')
	[IO.File]::WriteAllText((Join-Path $PlayerPackage 'unlisted.dll'), 'extra')
	Expect-Rejection {
		& $Creator -ServerPackageRoot $ServerPackage -PlayerPackageRoot $PlayerPackage `
			-OutputRoot (Join-Path $Root 'UnlistedOutput') -Endpoint '127.0.0.1:39450' `
			-Provider Local -ClientFrames 1800 -ServerTicks 9000 | Out-Null
	} 'unlisted role package file'
	Remove-Item -LiteralPath (Join-Path $PlayerPackage 'unlisted.dll') -Force
	Remove-Item -LiteralPath (Join-Path $PlayerPackage 'runtime.dll') -Force
	Expect-Rejection {
		& $Creator -ServerPackageRoot $ServerPackage -PlayerPackageRoot $PlayerPackage `
			-OutputRoot (Join-Path $Root 'MissingOutput') -Endpoint '127.0.0.1:39450' `
			-Provider Local -ClientFrames 1800 -ServerTicks 9000 | Out-Null
	} 'missing listed role package file'
	Write-Output '[Qualification:FarmManifest] MOCK_TEST_OK'
} finally {
	$ResolvedRoot = [IO.Path]::GetFullPath($Root)
	$ResolvedTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
	if (-not $ResolvedRoot.StartsWith($ResolvedTemp + [IO.Path]::DirectorySeparatorChar,
		[StringComparison]::OrdinalIgnoreCase) -or
		[IO.Path]::GetFileName($ResolvedRoot) -cnotmatch '^farm-manifest-test-[a-f0-9]{32}$') {
		throw 'refusing recursive test cleanup outside the expected temporary root'
	}
	if (Test-Path -LiteralPath $ResolvedRoot) {
		Remove-Item -LiteralPath $ResolvedRoot -Recurse -Force
	}
}
