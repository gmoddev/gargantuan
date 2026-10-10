#requires -Version 7.0
# Creates byte-identical, pinned run manifests for the two role-local endpoint
# supervisors. Inputs are already-built Server/Player packages, each with a
# complete deployment-sha256.json. No package is changed or executed.
# The output root must be new. Its Server/run-manifest.json and
# Clients/run-manifest.json have identical bytes and one printed SHA-256 pin.

param(
	[Parameter(Mandatory = $true)][string]$ServerPackageRoot,
	[Parameter(Mandatory = $true)][string]$PlayerPackageRoot,
	[Parameter(Mandatory = $true)][string]$OutputRoot,
	[ValidatePattern('^[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$')]
	[string]$RunId = [Guid]::NewGuid().ToString(),
	[Parameter(Mandatory = $true)][string]$Endpoint,
	[ValidateSet('Local', 'Node')][string]$Provider = 'Local',
	[switch]$ScaleWorkload,
	[switch]$RecoveryWorkload,
	[ValidateRange(60, 36000)][int]$ClientFrames = 1800,
	[ValidateRange(7200, 48000)][int]$ServerTicks = 9000,
	[string]$NodeEndpoint,
	[string]$NodeRootCertificatePath,
	[ValidatePattern('^[A-Za-z_][A-Za-z0-9_]*$')][string]$NodeTokenEnvironment = 'GARGANTUAN_ENGINE_ADAPTER_TOKEN'
)

$ErrorActionPreference = 'Stop'
$RunId = $RunId.ToLowerInvariant()

# Import only read-only package validators; this creator never creates a runtime
# projection or invokes the endpoint supervisor.
$Tokens = $null; $Errors = $null
$EndpointAst = [Management.Automation.Language.Parser]::ParseFile(
	(Join-Path $PSScriptRoot 'PhysicalGameSessionFarmEndpoint.ps1'), [ref]$Tokens, [ref]$Errors)
if ($Errors.Count -ne 0) { throw 'endpoint validator syntax invalid' }
$ValidatorNames = @('Assert-DeploymentManifest', 'Assert-ProjectionPlainPath', 'Read-ProjectionJson', 'Get-NativeRuntimePlan')
$Definitions = @($EndpointAst.FindAll({ param($Node)
	$Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -in $ValidatorNames
}, $true))
if ($Definitions.Count -ne $ValidatorNames.Count) { throw 'native package validator set incomplete' }
foreach ($Definition in $Definitions) { . ([scriptblock]::Create($Definition.Extent.Text)) }

function Assert-EndpointText {
	param([string]$Text, [switch]$AllowDns)
	$Parts = $Text -split ':'
	$Port = 0
	if ($Parts.Count -ne 2 -or -not [int]::TryParse($Parts[1], [ref]$Port) -or
		$Port -lt 1 -or $Port -gt 65535) { throw 'endpoint must be host:port with a valid port' }
	if ($AllowDns) {
		if ($Parts[0] -cnotmatch '^[A-Za-z0-9.-]{1,253}$') {
			throw 'Node endpoint host is invalid'
		}
	} else {
		$Address = $null
		if (-not [Net.IPAddress]::TryParse($Parts[0], [ref]$Address) -or
			$Address.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) {
			throw 'farm endpoint must be IPv4:port'
		}
	}
}

function Assert-DisjointPaths {
	param([string]$Server, [string]$Player, [string]$Output)
	$Roots = @($Server, $Player, $Output) | ForEach-Object {
		[IO.Path]::GetFullPath($_).TrimEnd('\', '/')
	}
	for ($Left = 0; $Left -lt $Roots.Count; $Left++) {
		for ($Right = $Left + 1; $Right -lt $Roots.Count; $Right++) {
			if ($Roots[$Left].Equals($Roots[$Right], [StringComparison]::OrdinalIgnoreCase) -or
				$Roots[$Left].StartsWith($Roots[$Right] + [IO.Path]::DirectorySeparatorChar,
					[StringComparison]::OrdinalIgnoreCase) -or
				$Roots[$Right].StartsWith($Roots[$Left] + [IO.Path]::DirectorySeparatorChar,
					[StringComparison]::OrdinalIgnoreCase)) {
				throw 'Server, Player, and output roots must be disjoint'
			}
		}
	}
	if (-not (Test-Path -LiteralPath $Roots[0] -PathType Container) -or
		-not (Test-Path -LiteralPath $Roots[1] -PathType Container)) {
		throw 'both role-local packages must already exist'
	}
	if (Test-Path -LiteralPath $Roots[2]) { throw 'stale run-manifest output root already exists' }
	return $Roots
}

function Get-RolePins {
	param([string]$PackageRoot, [ValidateSet('Server', 'Player')][string]$Role)
	$DeploymentPath = Join-Path $PackageRoot 'deployment-sha256.json'
	if (-not (Test-Path -LiteralPath $DeploymentPath -PathType Leaf)) {
		throw "$Role deployment-sha256.json is missing"
	}
	if ((Get-Item -LiteralPath $DeploymentPath).Length -gt 4194304) {
		throw "$Role deployment-sha256.json exceeds the 4 MiB input bound"
	}
	$Deployment = Get-Content -LiteralPath $DeploymentPath -Raw | ConvertFrom-Json -AsHashtable
	if ($Deployment.Format -cne 'GargantuanFarmDeployment' -or $Deployment.Version -ne 1 -or
		$Deployment.SourceCommit -cnotmatch '^[0-9a-f]{40}$' -or
		$Deployment.Files -isnot [array] -or $Deployment.Files.Count -lt 3 -or
		$Deployment.Files.Count -gt 10000) {
		throw "$Role deployment manifest schema is invalid"
	}
	$Expected = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
	foreach ($Entry in $Deployment.Files) {
		$Segments = @([string]$Entry.Path -split '/')
		if ($Entry.Path -isnot [string] -or
			$Entry.Path -cnotmatch '^[^\\/:*?"<>|\x00-\x1f]+(?:/[^\\/:*?"<>|\x00-\x1f]+)*$' -or
			$Segments -contains '..' -or $Segments -contains '.' -or
			$Entry.Path -eq 'deployment-sha256.json' -or
			$Entry.Sha256 -cnotmatch '^[a-fA-F0-9]{64}$' -or $Entry.Bytes -isnot [long] -or
			$Entry.Bytes -lt 0 -or -not $Expected.TryAdd($Entry.Path, $Entry)) {
			throw "$Role deployment manifest contains an invalid or duplicate entry"
		}
	}
	$Found = 0
	foreach ($Item in Get-ChildItem -LiteralPath $PackageRoot -Recurse -Force) {
		if ($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
			throw "$Role package contains a reparse point"
		}
		if ($Item.PSIsContainer) { continue }
		$Relative = [IO.Path]::GetRelativePath($PackageRoot, $Item.FullName).Replace('\', '/')
		if ($Relative -eq 'deployment-sha256.json') { continue }
		$Entry = $null
		if (-not $Expected.TryGetValue($Relative, [ref]$Entry) -or
			$Item.Length -ne $Entry.Bytes -or
			(Get-FileHash -LiteralPath $Item.FullName -Algorithm SHA256).Hash -ine $Entry.Sha256) {
			throw "$Role package contains an unlisted or mismatched file: $Relative"
		}
		$Found += 1
	}
	if ($Found -ne $Expected.Count) { throw "$Role package is missing a listed file" }
	$Binary = if ($Role -eq 'Server') { 'GargantuanServer.exe' } else { 'GargantuanPlayer.exe' }
	$NativeRole = if ($Role -eq 'Server') { 'Server' } else { 'Clients' }
	[void](Get-NativeRuntimePlan -LocalRoot $PackageRoot -DeploymentSha256 `
		(Get-FileHash -LiteralPath $DeploymentPath -Algorithm SHA256).Hash `
		-SourceCommit $Deployment.SourceCommit -LocalRole $NativeRole)
	$Required = @($Binary, 'game.package.json', 'content/content.manifest.json')
	foreach ($Relative in $Required) {
		if (-not $Expected.ContainsKey($Relative)) { throw "$Role package is missing required $Relative" }
	}
	$Hash = { param($Relative)
		(Get-FileHash -LiteralPath (Join-Path $PackageRoot $Relative) -Algorithm SHA256).Hash.ToLowerInvariant()
	}
	return [pscustomobject]@{
		SourceCommit = $Deployment.SourceCommit
		BinarySha256 = (& $Hash $Binary)
		PackageSha256 = (& $Hash 'game.package.json')
		ContentManifestSha256 = (& $Hash 'content/content.manifest.json')
		DeploymentSha256 = (Get-FileHash -LiteralPath $DeploymentPath -Algorithm SHA256).Hash.ToLowerInvariant()
	}
}

Assert-EndpointText -Text $Endpoint
if ($ScaleWorkload -and $ClientFrames -lt 9000) { throw 'ScaleWorkload needs at least 9000 client frames' }
if ($RecoveryWorkload -and (-not $ScaleWorkload -or $ClientFrames -lt 18000 -or $ServerTicks -lt 19000)) {
	throw 'RecoveryWorkload needs ScaleWorkload, 18000 client frames, and 19000 server ticks'
}
if ($ServerTicks -le $ClientFrames + 600) {
	throw 'server tick bound must exceed client frames plus 600 setup/convergence ticks'
}
$Paths = Assert-DisjointPaths -Server $ServerPackageRoot -Player $PlayerPackageRoot -Output $OutputRoot
$ServerPins = Get-RolePins -PackageRoot $Paths[0] -Role Server
$PlayerPins = Get-RolePins -PackageRoot $Paths[1] -Role Player
if ($ServerPins.SourceCommit -cne $PlayerPins.SourceCommit) {
	throw 'role-local packages have different source revisions'
}
if ($Provider -eq 'Node') {
	Assert-EndpointText -Text $NodeEndpoint -AllowDns
	if ([string]::IsNullOrWhiteSpace($NodeRootCertificatePath) -or
		-not (Test-Path -LiteralPath $NodeRootCertificatePath -PathType Leaf)) {
		throw 'Node provider requires a root certificate file'
	}
}

# Only the creator makes nonces. Low 32 bits encode slots 1..32; a random,
# nonzero high 32-bit run prefix prevents per-slot reuse across run identities.
$PrefixBytes = [Security.Cryptography.RandomNumberGenerator]::GetBytes(4)
$Prefix = [BitConverter]::ToUInt32($PrefixBytes, 0)
if ($Prefix -eq 0) { $Prefix = 1 }
$Nonces = @(for ($Slot = 0; $Slot -lt 32; $Slot++) {
	[string](([UInt64]$Prefix -shl 32) -bor [UInt64]($Slot + 1))
})
$Manifest = [ordered]@{
	Format = 'GargantuanPhysicalFarmEndpoint'; Version = 1
	RunId = $RunId; SourceCommit = $ServerPins.SourceCommit
	Endpoint = $Endpoint; Provider = $Provider
	ScaleWorkload = [bool]$ScaleWorkload
	RecoveryWorkload = [bool]$RecoveryWorkload
	ClientFrames = $ClientFrames; ServerTicks = $ServerTicks; Nonces = $Nonces
	ServerSha256 = $ServerPins.BinarySha256
	ServerPackageSha256 = $ServerPins.PackageSha256
	ServerContentManifestSha256 = $ServerPins.ContentManifestSha256
	ServerDeploymentSha256 = $ServerPins.DeploymentSha256
	PlayerSha256 = $PlayerPins.BinarySha256
	PlayerPackageSha256 = $PlayerPins.PackageSha256
	PlayerContentManifestSha256 = $PlayerPins.ContentManifestSha256
	PlayerDeploymentSha256 = $PlayerPins.DeploymentSha256
}
if ($Provider -eq 'Node') {
	$Manifest.NodeEndpoint = $NodeEndpoint
	$Manifest.NodeRootCertificateSha256 = (Get-FileHash -LiteralPath $NodeRootCertificatePath -Algorithm SHA256).Hash.ToLowerInvariant()
	$Manifest.NodeTokenEnvironment = $NodeTokenEnvironment
}
$Json = $Manifest | ConvertTo-Json -Depth 6
$Bytes = [Text.UTF8Encoding]::new($false).GetBytes($Json)
$Hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
$Output = $Paths[2]
[void][IO.Directory]::CreateDirectory($Output)
foreach ($Role in @('Server', 'Clients')) {
	$Directory = Join-Path $Output $Role
	[void][IO.Directory]::CreateDirectory($Directory)
	$Path = Join-Path $Directory 'run-manifest.json'
	$Stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
	try { $Stream.Write($Bytes) } finally { $Stream.Dispose() }
	if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $Hash) {
		throw "written $Role run manifest hash mismatch"
	}
}
Write-Output "[Qualification:FarmManifest] CREATED run=$RunId sha256=$Hash server=$Output\Server\run-manifest.json clients=$Output\Clients\run-manifest.json"
