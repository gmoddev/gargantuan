#requires -Version 7.0
# Role-local supervisor for an already-created 32-client physical farm run.
# The caller supplies the same manifest bytes and -ManifestSha256 on both PCs.
# Manifest JSON schema (all fields required; no executable paths or commands):
# {
#   "Format": "GargantuanPhysicalFarmEndpoint", "Version": 1,
#   "RunId": "canonical UUID", "SourceCommit": "40 lowercase hex",
#   "Endpoint": "IPv4:UDP-port",
#   "Provider": "Local|Node", "ScaleWorkload": true,
#   "ClientFrames": 9000, "ServerTicks": 10000,
#   "Nonces": ["nonzero-uint64-with-common-high-32-bits-and-low-bits-1", ... 32],
#   "ServerSha256": "64 hex", "ServerPackageSha256": "64 hex",
#   "PlayerSha256": "64 hex", "PlayerPackageSha256": "64 hex",
#   "ServerContentManifestSha256": "64 hex", "PlayerContentManifestSha256": "64 hex",
#   "ServerDeploymentSha256": "64 hex", "PlayerDeploymentSha256": "64 hex",
#   "NodeEndpoint": "host:port", "NodeRootCertificateSha256": "64 hex",
#   "NodeTokenEnvironment": "GARGANTUAN_ENGINE_ADAPTER_TOKEN"
# }
# Node* fields are required only for Provider=Node. Each role reads only its
# own package root. The stable role-local RunRegistryRoot rejects a reused ID.
# Each role package also contains deployment-sha256.json with schema:
# {"Format":"GargantuanFarmDeployment","Version":1,"SourceCommit":"40 lowercase hex",
#  "Files":[{"Path":"relative/forward-slash/path","Bytes":123,"Sha256":"64 hex"}, ...]}
# The manifest lists every package file except itself; its top-level SHA-256
# is the role-specific *DeploymentSha256 field above.
# This script does not start captures, elevate, modify firewall rules, or run
# a command supplied by the manifest.

param(
	[Parameter(Mandatory = $true)][ValidateSet('Server', 'Clients')][string]$Role,
	[Parameter(Mandatory = $true)][string]$ManifestPath,
	[Parameter(Mandatory = $true)][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$ManifestSha256,
	[Parameter(Mandatory = $true)][string]$PackageRoot,
	[Parameter(Mandatory = $true)][string]$EvidenceRoot,
	[Parameter(Mandatory = $true)][string]$RunRegistryRoot,
	[ValidateRange(10000, 180000)][int]$StartupTimeoutMilliseconds = 60000,
	[ValidateRange(30000, 900000)][int]$RunTimeoutMilliseconds = 300000,
	[ValidateRange(0, 250)][int]$StartupStaggerMilliseconds = 75,
	[ValidateRange(1048576, 16777216)][int]$MaximumLogBytesPerStream = 4194304,
	[ValidateRange(16777216, 17179869184)][long]$MaximumClientWorkingSetBytes = 1073741824,
	[ValidateRange(16777216, 17179869184)][long]$MaximumServerWorkingSetBytes = 8589934592,
	[ValidateRange(16777216, 34359738368)][long]$MaximumAggregateWorkingSetBytes = 21474836480,
	[ValidateRange(1, 2000)][int]$MaximumThreadsPerProcess = 512,
	[string]$NodeRootCertificatePath
)

$ErrorActionPreference = 'Stop'
$OwnedProcesses = [Collections.Generic.List[object]]::new()
$ResourceSamples = [Collections.Generic.List[object]]::new()
$HostResourceSamples = [Collections.Generic.List[object]]::new()
$CleanupErrors = [Collections.Generic.List[string]]::new()
$Failure = $null
$Result = $null
$EvidenceCreated = $false
$ResourceSampler = $null
$RunId = 'unvalidated'

function Get-TypedFields {
	param([Parameter(Mandatory = $true)][string]$Line)
	$Fields = @{}
	foreach ($Match in [regex]::Matches($Line, '(?:^|\s)([a-z][a-z0-9_]*)=([^\s]+)')) {
		$Fields[$Match.Groups[1].Value] = $Match.Groups[2].Value
	}
	return $Fields
}

function Read-SharedLogLines {
	param([Parameter(Mandatory = $true)][string]$Path)
	$Stream = [IO.File]::Open($Path, [IO.FileMode]::Open,
		[IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
	try {
		if ($Stream.Length -gt 16777216) { throw 'live farm log exceeds the hard read bound' }
		$Length = [int]$Stream.Length
		if ($Length -eq 0) { return @() }
		$Bytes = [byte[]]::new($Length)
		$Offset = 0
		while ($Offset -lt $Length) {
			$Count = $Stream.Read($Bytes, $Offset, $Length - $Offset)
			if ($Count -le 0) { throw 'live farm log changed during bounded read' }
			$Offset += $Count
		}
		$Content = [Text.UTF8Encoding]::new($false, $true).GetString($Bytes)
		$LastNewline = $Content.LastIndexOf("`n", [StringComparison]::Ordinal)
		if ($LastNewline -lt 0) { return @() }
		return @($Content.Substring(0, $LastNewline + 1) -split '\r?\n' |
			Where-Object Length -gt 0)
	} finally { $Stream.Dispose() }
}

function Get-TypedRecords {
	param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string]$Kind)
	if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return @() }
	$Prefix = "[Qualification:$Kind] "
	return @(Read-SharedLogLines -Path $Path | Where-Object {
		$_.StartsWith($Prefix, [StringComparison]::Ordinal)
	} | ForEach-Object { Get-TypedFields -Line $_ })
}

function Assert-RunManifest {
	param([Parameter(Mandatory = $true)][System.Collections.IDictionary]$Value)
	$Common = @('Format', 'Version', 'RunId', 'SourceCommit', 'Endpoint', 'Provider', 'ScaleWorkload',
		'ClientFrames', 'ServerTicks', 'Nonces', 'ServerSha256', 'ServerPackageSha256',
		'PlayerSha256', 'PlayerPackageSha256', 'ServerContentManifestSha256',
		'PlayerContentManifestSha256', 'ServerDeploymentSha256', 'PlayerDeploymentSha256')
	$NodeFields = @('NodeEndpoint', 'NodeRootCertificateSha256', 'NodeTokenEnvironment')
	$Required = if ($Value.Provider -eq 'Node') { @($Common + $NodeFields) } else { $Common }
	$Allowed = @($Required + 'RecoveryWorkload')
	foreach ($Key in $Required) { if (-not $Value.Contains($Key)) { throw "manifest lacks $Key" } }
	foreach ($Key in $Value.Keys) { if ($Key -notin $Allowed) { throw "manifest has unrecognized field $Key" } }
	if ($Value.Format -cne 'GargantuanPhysicalFarmEndpoint' -or $Value.Version -isnot [long] -or
		$Value.Version -ne 1 -or
		$Value.RunId -isnot [string] -or $Value.SourceCommit -isnot [string] -or
		$Value.Endpoint -isnot [string] -or $Value.Provider -isnot [string] -or
		$Value.RunId -cnotmatch '^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$' -or
		$Value.SourceCommit -cnotmatch '^[0-9a-f]{40}$' -or
		$Value.Provider -cnotin @('Local', 'Node') -or $Value.ScaleWorkload -isnot [bool] -or
		($Value.Contains('RecoveryWorkload') -and
			($Value.RecoveryWorkload -isnot [bool] -or ($Value.RecoveryWorkload -and
				(-not $Value.ScaleWorkload -or $Value.ClientFrames -lt 18000 -or $Value.ServerTicks -lt 19000)))) -or
		$Value.ClientFrames -isnot [long] -or $Value.ClientFrames -lt 60 -or
		$Value.ClientFrames -gt 36000 -or ($Value.ScaleWorkload -and $Value.ClientFrames -lt 9000) -or
		$Value.ServerTicks -isnot [long] -or
		$Value.ServerTicks -lt 7200 -or $Value.ServerTicks -gt 48000 -or
		$Value.ServerTicks -le $Value.ClientFrames + 600 -or
		$Value.Nonces -isnot [array] -or $Value.Nonces.Count -ne 32) {
		throw 'manifest has invalid schema, run, scale, or bounded workload fields'
	}
	$Parts = $Value.Endpoint -split ':'
	$Address = $null
	$Port = 0
	if ($Parts.Count -ne 2 -or -not [Net.IPAddress]::TryParse($Parts[0], [ref]$Address) -or
		$Address.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork -or
		-not [int]::TryParse($Parts[1], [ref]$Port) -or $Port -lt 1 -or $Port -gt 65535) {
		throw 'manifest endpoint must be IPv4:UDP-port'
	}
	$Prefix = $null
	for ($Slot = 0; $Slot -lt 32; $Slot++) {
		$Nonce = [string]$Value.Nonces[$Slot]
		$Parsed = [UInt64]0
		if ($Value.Nonces[$Slot] -isnot [string] -or $Nonce -cnotmatch '^[1-9][0-9]{0,19}$' -or
			-not [UInt64]::TryParse($Nonce, [ref]$Parsed) -or
			($Parsed -band ([UInt64]::MaxValue -shr 32)) -ne [UInt64]($Slot + 1)) {
			throw "manifest nonce assignment $Slot is invalid"
		}
		$ThisPrefix = $Parsed -shr 32
		if ($ThisPrefix -eq 0 -or ($null -ne $Prefix -and $ThisPrefix -ne $Prefix)) {
			throw 'manifest nonces lack one nonzero run-scoped prefix'
		}
		$Prefix = $ThisPrefix
	}
	foreach ($Key in @('ServerSha256', 'ServerPackageSha256', 'PlayerSha256',
		'PlayerPackageSha256', 'ServerContentManifestSha256', 'PlayerContentManifestSha256',
		'ServerDeploymentSha256', 'PlayerDeploymentSha256')) {
		if ($Value[$Key] -isnot [string] -or
			$Value[$Key] -cnotmatch '^[a-fA-F0-9]{64}$') { throw "manifest $Key is not a SHA-256 pin" }
	}
	if ($Value.Provider -eq 'Node') {
		$NodeParts = [string]$Value.NodeEndpoint -split ':'
		$NodePort = 0
		if ($NodeParts.Count -ne 2 -or $NodeParts[0] -notmatch '^[A-Za-z0-9.-]{1,253}$' -or
			-not [int]::TryParse($NodeParts[1], [ref]$NodePort) -or $NodePort -lt 1 -or
			$NodePort -gt 65535 -or
			[string]$Value.NodeRootCertificateSha256 -cnotmatch '^[a-fA-F0-9]{64}$' -or
			[string]$Value.NodeTokenEnvironment -cnotmatch '^[A-Za-z_][A-Za-z0-9_]*$') {
			throw 'manifest Node endpoint, certificate pin, or token environment is invalid'
		}
	}
	return [pscustomobject]@{ Address = $Address; Port = $Port; NoncePrefix = $Prefix }
}

function Read-PinnedRunManifest {
	param([string]$Path, [string]$ExpectedSha256)
	$Resolved = [IO.Path]::GetFullPath($Path)
	if (-not (Test-Path -LiteralPath $Resolved -PathType Leaf) -or
		(Get-Item -LiteralPath $Resolved).Length -gt 1048576 -or
		(Get-FileHash -LiteralPath $Resolved -Algorithm SHA256).Hash -ine $ExpectedSha256) {
		throw 'pre-created run manifest is missing or has a mismatched SHA-256 pin'
	}
	$Value = Get-Content -LiteralPath $Resolved -Raw | ConvertFrom-Json -AsHashtable
	[void](Assert-RunManifest -Value $Value)
	return $Value
}

function Assert-LocalPackagePins {
	param([Parameter(Mandatory = $true)][string]$LocalRoot,
		[Parameter(Mandatory = $true)][string]$LocalRole,
		[Parameter(Mandatory = $true)][System.Collections.IDictionary]$RunManifest)
	$BinaryName = if ($LocalRole -eq 'Server') { 'GargantuanServer.exe' } else { 'GargantuanPlayer.exe' }
	$Prefix = if ($LocalRole -eq 'Server') { 'Server' } else { 'Player' }
	$Paths = @(
		@((Join-Path $LocalRoot $BinaryName), "${Prefix}Sha256"),
		@((Join-Path $LocalRoot 'game.package.json'), "${Prefix}PackageSha256"),
		@((Join-Path $LocalRoot 'content/content.manifest.json'), "${Prefix}ContentManifestSha256")
	)
	foreach ($Entry in $Paths) {
		if (-not (Test-Path -LiteralPath $Entry[0] -PathType Leaf)) { throw "local package lacks $($Entry[0])" }
		$Actual = (Get-FileHash -LiteralPath $Entry[0] -Algorithm SHA256).Hash
		if ($Actual -ine $RunManifest[$Entry[1]]) { throw "local package pin mismatch: $($Entry[1])" }
	}
	Assert-DeploymentManifest -LocalRoot $LocalRoot -ExpectedSha256 $RunManifest["${Prefix}DeploymentSha256"] `
		-ExpectedSourceCommit $RunManifest.SourceCommit
	return $Paths[0][0]
}

function Assert-DeploymentManifest {
	param([string]$LocalRoot, [string]$ExpectedSha256, [string]$ExpectedSourceCommit)
	$Path = Join-Path $LocalRoot 'deployment-sha256.json'
	if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or
		(Get-Item -LiteralPath $Path).Length -gt 4194304 -or
		(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $ExpectedSha256) {
		throw 'role-local deployment manifest is missing or has a mismatched pin'
	}
	$Deployment = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -AsHashtable
	if ($Deployment.Format -cne 'GargantuanFarmDeployment' -or $Deployment.Version -ne 1 -or
		$Deployment.SourceCommit -cne $ExpectedSourceCommit -or
		$Deployment.Files -isnot [array] -or $Deployment.Files.Count -lt 3 -or
		$Deployment.Files.Count -gt 10000) { throw 'deployment manifest schema is invalid' }
	$Expected = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
	foreach ($Entry in $Deployment.Files) {
		$Segments = @([string]$Entry.Path -split '/')
		if ($Entry.Path -isnot [string] -or
			$Entry.Path -cnotmatch '^[^\\/:*?"<>|\x00-\x1f]+(?:/[^\\/:*?"<>|\x00-\x1f]+)*$' -or
			$Segments -contains '..' -or $Segments -contains '.' -or
			$Entry.Path -eq 'deployment-sha256.json' -or
			$Entry.Sha256 -cnotmatch '^[a-fA-F0-9]{64}$' -or $Entry.Bytes -isnot [long] -or
			$Entry.Bytes -lt 0 -or -not $Expected.TryAdd($Entry.Path, $Entry)) {
			throw 'deployment manifest contains an invalid or duplicate file entry'
		}
	}
	$Found = 0
	foreach ($Item in Get-ChildItem -LiteralPath $LocalRoot -Recurse -Force) {
		if ($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
			throw 'role-local package contains a reparse point'
		}
		if ($Item.PSIsContainer) { continue }
		$Relative = [IO.Path]::GetRelativePath($LocalRoot, $Item.FullName).Replace('\', '/')
		if ($Relative -eq 'deployment-sha256.json') { continue }
		$Entry = $null
		if (-not $Expected.TryGetValue($Relative, [ref]$Entry) -or
			$Item.Length -ne $Entry.Bytes -or
			(Get-FileHash -LiteralPath $Item.FullName -Algorithm SHA256).Hash -ine $Entry.Sha256) {
			throw "role-local package file is unlisted or mismatched: $Relative"
		}
		$Found += 1
	}
	if ($Found -ne $Expected.Count) { throw 'role-local package omits a deployment-manifest file' }
}

function Assert-ProjectionPlainPath {
	param([string]$Path)
	if (-not [IO.Path]::IsPathFullyQualified($Path)) { throw 'projection path must be absolute' }
	$Current = [IO.Path]::GetFullPath($Path)
	while ($Current) {
		if (Test-Path -LiteralPath $Current) {
			if ((Get-Item -LiteralPath $Current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
				throw 'projection path contains redirection'
			}
		}
		$Current = [IO.Path]::GetDirectoryName($Current)
	}
}

function Read-ProjectionJson {
	param([string]$Path)
	if ((Get-Item -LiteralPath $Path).Length -gt 4MB) { throw 'projection JSON exceeds bound' }
	$Bytes = [IO.File]::ReadAllBytes($Path)
	$Offset = if ($Bytes.Length -ge 3 -and $Bytes[0] -eq 239 -and $Bytes[1] -eq 187 -and $Bytes[2] -eq 191) { 3 } else { 0 }
	$Text = [Text.UTF8Encoding]::new($false, $true).GetString($Bytes, $Offset, $Bytes.Length - $Offset)
	$Document = [Text.Json.JsonDocument]::Parse($Text)
	try {
		$Pending = [Collections.Generic.Stack[Text.Json.JsonElement]]::new()
		$Pending.Push($Document.RootElement)
		while ($Pending.Count -gt 0) {
			$Element = $Pending.Pop()
			if ($Element.ValueKind -eq [Text.Json.JsonValueKind]::Object) {
				$Names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
				foreach ($Property in $Element.EnumerateObject()) {
					if (-not $Names.Add($Property.Name)) { throw 'duplicate projection JSON property' }
					$Pending.Push($Property.Value)
				}
			} elseif ($Element.ValueKind -eq [Text.Json.JsonValueKind]::Array) {
				foreach ($Value in $Element.EnumerateArray()) { $Pending.Push($Value) }
			}
		}
	} finally { $Document.Dispose() }
	return ConvertFrom-Json -InputObject $Text -AsHashtable
}

function Get-NativeRuntimePlan {
	param([string]$LocalRoot, [string]$DeploymentSha256, [string]$SourceCommit,
		[ValidateSet('Server', 'Clients')][string]$LocalRole)
	Assert-ProjectionPlainPath -Path $LocalRoot
	Assert-DeploymentManifest -LocalRoot $LocalRoot -ExpectedSha256 $DeploymentSha256 -ExpectedSourceCommit $SourceCommit
	$ManifestPath = Join-Path $LocalRoot 'game.package.json'
	if ((Get-Item -LiteralPath $ManifestPath).Length -gt 4MB) { throw 'game manifest exceeds native bound' }
	$Game = Read-ProjectionJson -Path $ManifestPath
	$Deployment = Read-ProjectionJson -Path (Join-Path $LocalRoot 'deployment-sha256.json')
	$Keys = @('Format', 'PackageFormatVersion', 'RuntimeCompatibility', 'ProjectId', 'DisplayName',
		'Configuration', 'Revision', 'UnsavedChanges', 'Player', 'Startup', 'ContentTableSha256', 'Content')
	$Binary = if ($LocalRole -eq 'Server') { 'GargantuanServer.exe' } else { 'GargantuanPlayer.exe' }
	if ($Game.Count -ne 12 -or @($Keys | Where-Object { -not $Game.Contains($_) }).Count -ne 0 -or
		$Game.Format -cne 'GargantuanGamePackage' -or $Game.PackageFormatVersion -ne 2 -or
		$Game.RuntimeCompatibility -ne 1 -or $Game.ProjectId -cnotmatch '^[a-f0-9]{32}$' -or
		$Game.Revision -isnot [long] -or $Game.Revision -le 0 -or $Game.Player -cne $Binary -or
		$Game.Content -isnot [array] -or $Game.Content.Count -lt 1 -or $Game.Content.Count -gt 9999 -or
		$Game.ContentTableSha256 -cnotmatch '^[a-f0-9]{64}$' -or $Game.Startup -isnot [System.Collections.IDictionary] -or
		$Game.Startup.Count -ne 4) { throw 'native runtime manifest schema is invalid' }
	$Expected = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
	$Canonical = [Collections.Generic.List[object]]::new()
	$Prior = ''; $Total = 0L
	foreach ($Entry in $Game.Content) {
		# Uppercase hex preserves native UTF8 byte order (including prefixes),
		# unlike Ordinal comparison of UTF16 text for astral path characters.
		$Segments = @([string]$Entry.Path -split '/')
		if ($Entry -isnot [System.Collections.IDictionary] -or $Entry.Count -ne 4 -or
			@('Path', 'Size', 'Sha256', 'Category' | Where-Object { -not $Entry.Contains($_) }).Count -ne 0 -or
			$Entry.Path -isnot [string] -or [Text.Encoding]::UTF8.GetByteCount($Entry.Path) -gt 512 -or
			$Entry.Path -cnotmatch '^[^\\/:*?"<>|\x00-\x1f]+(?:/[^\\/:*?"<>|\x00-\x1f]+)*$' -or
			$Segments -contains '.' -or $Segments -contains '..' -or
			$Entry.Path -iin @('game.package.json', 'deployment-sha256.json') -or
			($Prior -and [StringComparer]::Ordinal.Compare(
				[Convert]::ToHexString([Text.Encoding]::UTF8.GetBytes($Prior)),
				[Convert]::ToHexString([Text.Encoding]::UTF8.GetBytes($Entry.Path))) -ge 0) -or
			$Entry.Size -isnot [long] -or $Entry.Size -lt 0 -or $Entry.Size -gt 512MB -or
			$Entry.Sha256 -cnotmatch '^[a-f0-9]{64}$' -or
			$Entry.Category -cnotin @('Runtime', 'Project', 'Asset', 'Shader', 'Notice') -or
			-not $Expected.TryAdd($Entry.Path, $Entry)) { throw 'native content entry identity or bound invalid' }
		if ($Total -gt 8GB - $Entry.Size) { throw 'native content aggregate bound exceeded' }
		$Total += $Entry.Size; $Prior = $Entry.Path
		$Canonical.Add([ordered]@{ Path = $Entry.Path; Size = $Entry.Size; Sha256 = $Entry.Sha256; Category = $Entry.Category })
	}
	$TableBytes = [Text.UTF8Encoding]::new($false).GetBytes((ConvertTo-Json -InputObject @($Canonical.ToArray()) -Compress -Depth 8))
	$TableHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($TableBytes)).ToLowerInvariant()
	if ($TableHash -cne $Game.ContentTableSha256) { throw 'native ordered content table hash mismatch' }
	$Required = @($Binary, $Game.Startup.Project, $Game.Startup.AssetCatalog, $Game.Startup.ContentManifest,
		'runtime/DefaultActionMap.luau', 'runtime/DefaultInteractionRuntime.luau', 'runtime/DefaultCharacterRuntime.luau',
		'runtime/DefaultLocomotion.luau', 'runtime/DefaultCamera.luau', 'runtime/DefaultPlayerRuntime.luau',
		'runtime/GargantuanSans.ttf', 'shaders/gui.frag.spv', 'shaders/gui.vert.spv',
		'shaders/opaque.frag.spv', 'shaders/opaque.vert.spv', 'shaders/shadow.frag.spv', 'shaders/shadow.vert.spv',
		'shaders/sky.frag.spv', 'shaders/sky.vert.spv', 'notices/Gargantuan.txt', 'notices/SDL3.txt',
		'notices/SDL3_image.txt', 'notices/SDL3_ttf.txt')
	if ($null -ne $Game.Startup.PreRun) { $Required += $Game.Startup.PreRun }
	foreach ($Name in $Required) {
		if ($Name -isnot [string] -or -not $Expected.ContainsKey($Name)) { throw 'native required startup/runtime entry absent' }
	}
	if ($Deployment.Files.Count -ne $Expected.Count + 1) { throw 'deployment/native membership differs' }
	$Files = [Collections.Generic.List[object]]::new()
	foreach ($Entry in $Deployment.Files) {
		if ($Entry.Path -ceq 'game.package.json') {
			$PackageHash = $Entry.Sha256.ToLowerInvariant()
		} else {
			$Native = $null
			if (-not $Expected.TryGetValue($Entry.Path, [ref]$Native) -or
				$Native.Path -cne $Entry.Path -or $Native.Size -ne $Entry.Bytes -or $Native.Sha256 -ine $Entry.Sha256) {
				throw 'deployment/native size or hash differs'
			}
		}
		$Files.Add([ordered]@{ Path = $Entry.Path; Bytes = $Entry.Bytes; Sha256 = $Entry.Sha256.ToLowerInvariant() })
	}
	if (-not $PackageHash) { throw 'deployment lacks exact game descriptor' }
	return [pscustomobject]@{ Files = @($Files.ToArray()); ContentBytes = $Total; PackageSha256 = $PackageHash;
		DeploymentSha256 = $DeploymentSha256.ToLowerInvariant(); SourceCommit = $SourceCommit; Binary = $Binary }
}

function Get-RuntimeProjectionPath {
	param([string]$Registry, [string]$LocalRunId, [ValidateSet('Server', 'Clients')][string]$LocalRole)
	if ($LocalRunId -cnotmatch '^[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}$') { throw 'projection run identity invalid' }
	Assert-ProjectionPlainPath -Path $Registry
	return Join-Path ([IO.Path]::GetFullPath($Registry)) "$LocalRunId.$LocalRole.runtime"
}

function Assert-RuntimeProjection {
	param([string]$LocalRoot, [string]$RuntimeRoot, [object]$Plan, [switch]$PathsOnly)
	# Match PackageBuilder::ValidateStagingPaths, rather than .NET's broader
	# long-path support: the pinned native host is not longPathAware.
	$MaximumSupportedStagingPath = 240
	$Members = @('game.package.json') + @($Plan.Files | ForEach-Object Path)
	foreach ($Member in $Members) {
		$Current = [IO.Path]::GetFullPath((Join-Path $RuntimeRoot $Member))
		while ($Current) {
			if ($Current.Length -gt $MaximumSupportedStagingPath) {
				throw 'runtime projection path exceeds native Windows 240-character bound'
			}
			$Current = [IO.Path]::GetDirectoryName($Current)
		}
	}
	Assert-ProjectionPlainPath -Path $RuntimeRoot
	if ($PathsOnly) { return }
	if (-not (Test-Path -LiteralPath $RuntimeRoot -PathType Container)) { throw 'runtime projection directory absent' }
	$Expected = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::OrdinalIgnoreCase)
	$Directories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
	foreach ($Entry in $Plan.Files) {
		$Expected.Add($Entry.Path, $Entry)
		$Parent = [IO.Path]::GetDirectoryName($Entry.Path.Replace('/', [IO.Path]::DirectorySeparatorChar))
		while ($Parent) { [void]$Directories.Add($Parent.Replace('\', '/')); $Parent = [IO.Path]::GetDirectoryName($Parent) }
	}
	$Found = 0
	foreach ($Item in Get-ChildItem -LiteralPath $RuntimeRoot -Recurse -Force) {
		if ($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'runtime projection redirected' }
		$Relative = [IO.Path]::GetRelativePath($RuntimeRoot, $Item.FullName).Replace('\', '/')
		if ($Item.PSIsContainer) {
			if (-not $Directories.Contains($Relative)) { throw 'runtime projection undeclared directory' }
			continue
		}
		$Entry = $null
		if (-not $Expected.TryGetValue($Relative, [ref]$Entry) -or $Relative -cne $Entry.Path -or
			$Item.Length -ne $Entry.Bytes -or (Get-FileHash -LiteralPath $Item.FullName -Algorithm SHA256).Hash -ine $Entry.Sha256) {
			throw 'runtime projection content mismatch'
		}
		$Found += 1
	}
	if ($Found -ne $Expected.Count) { throw 'runtime projection missing content' }
}

function New-RuntimeProjection {
	param([string]$LocalRoot, [string]$Registry, [string]$LocalRunId,
		[ValidateSet('Server', 'Clients')][string]$LocalRole, [object]$Plan)
	$RuntimeRoot = Get-RuntimeProjectionPath -Registry $Registry -LocalRunId $LocalRunId -LocalRole $LocalRole
	Assert-RuntimeProjection -LocalRoot $LocalRoot -RuntimeRoot $RuntimeRoot -Plan $Plan -PathsOnly
	$CurrentPlan = Get-NativeRuntimePlan -LocalRoot $LocalRoot -DeploymentSha256 $Plan.DeploymentSha256 `
		-SourceCommit $Plan.SourceCommit -LocalRole $LocalRole
	if ((ConvertTo-Json -InputObject $CurrentPlan.Files -Depth 8 -Compress) -cne
		(ConvertTo-Json -InputObject $Plan.Files -Depth 8 -Compress) -or $CurrentPlan.PackageSha256 -cne $Plan.PackageSha256) {
		throw 'projection plan changed before copy'
	}
	$Source = [IO.Path]::GetFullPath($LocalRoot).TrimEnd('\', '/')
	$Separator = [IO.Path]::DirectorySeparatorChar
	if ($RuntimeRoot.StartsWith($Source + $Separator, [StringComparison]::OrdinalIgnoreCase) -or
		$Source.StartsWith($RuntimeRoot + $Separator, [StringComparison]::OrdinalIgnoreCase) -or $RuntimeRoot -ieq $Source) {
		throw 'projection and source overlap'
	}
	$Attempt = $RuntimeRoot + '.attempt'; $Receipt = $RuntimeRoot + '.json'
	foreach ($Path in @($RuntimeRoot, $Attempt, $Receipt)) {
		Assert-ProjectionPlainPath -Path $Path
		if (Test-Path -LiteralPath $Path) { throw 'stale or partial projection; no retry permitted' }
	}
	$CopyBytes = 0L
	foreach ($Entry in $Plan.Files) { $CopyBytes += $Entry.Bytes }
	if ([IO.DriveInfo]::new([IO.Path]::GetPathRoot($RuntimeRoot)).AvailableFreeSpace -lt $CopyBytes + 1GB) {
		throw 'runtime projection disk headroom insufficient'
	}
	[void][IO.Directory]::CreateDirectory($Registry)
	$Stream = [IO.File]::Open($Attempt, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
	try { $Stream.Write([Text.Encoding]::UTF8.GetBytes("$LocalRunId/$LocalRole")) } finally { $Stream.Dispose() }
	if (Test-Path -LiteralPath $RuntimeRoot) { throw 'projection creation race; preserve attempt' }
	[void][IO.Directory]::CreateDirectory($RuntimeRoot)
	foreach ($Entry in $Plan.Files) {
		$InputPath = Join-Path $Source $Entry.Path; $Target = Join-Path $RuntimeRoot $Entry.Path
		Assert-ProjectionPlainPath -Path $InputPath; Assert-ProjectionPlainPath -Path $Target
		if ((Get-Item -LiteralPath $InputPath).Length -ne $Entry.Bytes -or
			(Get-FileHash -LiteralPath $InputPath -Algorithm SHA256).Hash -ine $Entry.Sha256) { throw 'projection source changed before copy' }
		[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Target))
		[IO.File]::Copy($InputPath, $Target, $false)
	}
	Assert-RuntimeProjection -LocalRoot $Source -RuntimeRoot $RuntimeRoot -Plan $Plan
	Assert-DeploymentManifest -LocalRoot $Source -ExpectedSha256 $Plan.DeploymentSha256 -ExpectedSourceCommit $Plan.SourceCommit
	$Value = [ordered]@{ Format = 'GargantuanFarmRuntimeProjection'; Version = 1; RunId = $LocalRunId; Role = $LocalRole;
		SourceCommit = $Plan.SourceCommit; PackageRoot = $Source; RuntimeRoot = $RuntimeRoot;
		PackageSha256 = $Plan.PackageSha256; DeploymentSha256 = $Plan.DeploymentSha256; Files = $Plan.Files }
	$Stream = [IO.File]::Open($Receipt, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
	try { $Stream.Write([Text.UTF8Encoding]::new($false).GetBytes(($Value | ConvertTo-Json -Depth 8))) } finally { $Stream.Dispose() }
	return $RuntimeRoot
}

function Assert-PreparedRuntimeProjection {
	param([string]$LocalRoot, [string]$Registry, [string]$LocalRunId,
		[ValidateSet('Server', 'Clients')][string]$LocalRole, [object]$Plan)
	$RuntimeRoot = Get-RuntimeProjectionPath -Registry $Registry -LocalRunId $LocalRunId -LocalRole $LocalRole
	Assert-RuntimeProjection -LocalRoot $LocalRoot -RuntimeRoot $RuntimeRoot -Plan $Plan -PathsOnly
	$ReceiptPath = $RuntimeRoot + '.json'
	$AttemptPath = $RuntimeRoot + '.attempt'
	Assert-ProjectionPlainPath -Path $AttemptPath
	if (-not (Test-Path -LiteralPath $AttemptPath -PathType Leaf) -or
		(Get-Item -LiteralPath $AttemptPath).Length -gt 128 -or
		[IO.File]::ReadAllText($AttemptPath, [Text.UTF8Encoding]::new($false, $true)) -cne "$LocalRunId/$LocalRole") {
		throw 'prepared runtime projection one-use attempt identity invalid'
	}
	Assert-ProjectionPlainPath -Path $ReceiptPath
	if (-not (Test-Path -LiteralPath $ReceiptPath -PathType Leaf) -or (Get-Item -LiteralPath $ReceiptPath).Length -gt 4MB) {
		throw 'prepared runtime projection receipt absent or oversized'
	}
	$Receipt = Read-ProjectionJson -Path $ReceiptPath
	$Keys = @('Format', 'Version', 'RunId', 'Role', 'SourceCommit', 'PackageRoot', 'RuntimeRoot',
		'PackageSha256', 'DeploymentSha256', 'Files')
	if ($Receipt.Count -ne $Keys.Count -or @($Keys | Where-Object { -not $Receipt.Contains($_) }).Count -ne 0 -or
		$Receipt.Format -cne 'GargantuanFarmRuntimeProjection' -or $Receipt.Version -isnot [long] -or $Receipt.Version -ne 1 -or
		$Receipt.RunId -cne $LocalRunId -or $Receipt.Role -cne $LocalRole -or
		$Receipt.PackageRoot -cne [IO.Path]::GetFullPath($LocalRoot).TrimEnd('\', '/') -or
		$Receipt.RuntimeRoot -cne $RuntimeRoot -or $Receipt.SourceCommit -cne $Plan.SourceCommit -or
		$Receipt.PackageSha256 -cne $Plan.PackageSha256 -or $Receipt.DeploymentSha256 -cne $Plan.DeploymentSha256 -or
		(ConvertTo-Json -InputObject $Receipt.Files -Depth 8 -Compress) -cne
		(ConvertTo-Json -InputObject $Plan.Files -Depth 8 -Compress)) { throw 'prepared runtime projection receipt mismatch' }
	Assert-RuntimeProjection -LocalRoot $LocalRoot -RuntimeRoot $RuntimeRoot -Plan $Plan
	return $RuntimeRoot
}

function Assert-RolePaths {
	param([string]$LocalPackage, [string]$LocalEvidence, [string]$LocalRegistry)
	$Package = [IO.Path]::GetFullPath($LocalPackage).TrimEnd('\', '/')
	$Evidence = [IO.Path]::GetFullPath($LocalEvidence).TrimEnd('\', '/')
	$Registry = [IO.Path]::GetFullPath($LocalRegistry).TrimEnd('\', '/')
	$StartsWithin = { param($A, $B) $A.StartsWith($B + [IO.Path]::DirectorySeparatorChar,
		[StringComparison]::OrdinalIgnoreCase) }
	if ($Package -ieq $Evidence -or $Package -ieq $Registry -or $Evidence -ieq $Registry -or
		(& $StartsWithin $Package $Evidence) -or (& $StartsWithin $Evidence $Package) -or
		(& $StartsWithin $Registry $Package) -or (& $StartsWithin $Package $Registry) -or
		(& $StartsWithin $Registry $Evidence) -or (& $StartsWithin $Evidence $Registry)) {
		throw 'package, evidence, and run registry roots must be disjoint'
	}
	if (-not (Test-Path -LiteralPath $Package -PathType Container)) { throw 'role-local package root is missing' }
	if (Test-Path -LiteralPath $Evidence) { throw 'stale role-local evidence root already exists' }
	return [pscustomobject]@{ Package = $Package; Evidence = $Evidence; Registry = $Registry }
}

function Start-EndpointProcess {
	param([string]$Executable, [string]$WorkingDirectory, [string[]]$Arguments,
		[string]$Label, [string]$OutputDirectory, [string[]]$RemoveEnvironmentVariables = @(), [switch]$SamplerPipes)
	if ($SamplerPipes -and $Label -cne 'sampler') { throw 'dedicated sampler pipes cannot own a native role process' }
	$Info = [Diagnostics.ProcessStartInfo]::new()
	$Info.FileName = $Executable
	$Info.WorkingDirectory = $WorkingDirectory
	$Info.UseShellExecute = $false
	$Info.CreateNoWindow = $true
	$Info.RedirectStandardOutput = $true
	$Info.RedirectStandardError = $true
	foreach ($Argument in $Arguments) { [void]$Info.ArgumentList.Add($Argument) }
	foreach ($Variable in $RemoveEnvironmentVariables) { [void]$Info.Environment.Remove($Variable) }
	$OutputPath = Join-Path $OutputDirectory "$Label.stdout.log"
	$ErrorPath = Join-Path $OutputDirectory "$Label.stderr.log"
	$Output = [IO.File]::Open($OutputPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
	$ErrorStream = [IO.File]::Open($ErrorPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
	$Process = [Diagnostics.Process]::new()
	$Started = $false
	try {
		$Process.StartInfo = $Info
		if (-not $Process.Start()) { throw "$Label did not start" }
		$Started = $true
		return [pscustomobject]@{
			Label = $Label; Pid = $Process.Id; Process = $Process
			OutputPath = $OutputPath; ErrorPath = $ErrorPath
			OutputStream = $Output; ErrorStream = $ErrorStream
			OutputCopy = $(if ($SamplerPipes) { [FarmSamplerPipeCopy]::Start($Process.StandardOutput.BaseStream, $Output) }
				else { $Process.StandardOutput.BaseStream.CopyToAsync($Output) })
			ErrorCopy = $(if ($SamplerPipes) { [FarmSamplerPipeCopy]::Start($Process.StandardError.BaseStream, $ErrorStream) }
				else { $Process.StandardError.BaseStream.CopyToAsync($ErrorStream) })
		}
	} catch {
		if ($Started -and -not $Process.HasExited) { try { $Process.Kill($true) } catch {} }
		$Output.Dispose(); $ErrorStream.Dispose(); $Process.Dispose()
		throw
	}
}

function Stop-EndpointProcess {
	param([Parameter(Mandatory = $true)]$Owner)
	try {
		if (-not $Owner.Process.HasExited) { $Owner.Process.Kill($true) }
		if (-not $Owner.Process.WaitForExit(5000)) { throw "owned PID $($Owner.Pid) remained live" }
		if (-not $Owner.OutputCopy.Wait(5000) -or -not $Owner.ErrorCopy.Wait(5000) -or
			$Owner.OutputCopy.IsFaulted -or $Owner.ErrorCopy.IsFaulted) {
			throw 'redirected output did not drain'
		}
	} finally {
		$Owner.OutputStream.Dispose(); $Owner.ErrorStream.Dispose(); $Owner.Process.Dispose()
	}
}

function Assert-EndpointBounds {
	param([object[]]$Owners, [long]$MaximumLogBytes, [long]$MaximumMemoryBytes,
		[long]$MaximumAggregateMemoryBytes,
		[int]$MaximumThreads)
	$AggregateWorkingSetBytes = 0L
	foreach ($Owner in $Owners) {
		foreach ($Path in @($Owner.OutputPath, $Owner.ErrorPath)) {
			if (([IO.FileInfo]$Path).Length -gt $MaximumLogBytes) { throw "$($Owner.Label) log limit exceeded" }
		}
		$Owner.Process.Refresh()
		if ($Owner.Process.HasExited) { continue }
		$AggregateWorkingSetBytes += $Owner.Process.WorkingSet64
		if ($Owner.Process.WorkingSet64 -gt $MaximumMemoryBytes -or
			$Owner.Process.Threads.Count -gt $MaximumThreads) { throw "$($Owner.Label) resource limit exceeded" }
	}
	if ($AggregateWorkingSetBytes -gt $MaximumAggregateMemoryBytes) {
		throw 'role-local aggregate working-set limit exceeded'
	}
}

function Add-EndpointResourceSamples {
	param([object[]]$Owners, [string]$LocalRunId,
		[Collections.Generic.List[object]]$Samples, [long]$SupervisorElapsedMilliseconds)
	if ($Samples.Count + $Owners.Count -gt 20000) { throw 'resource evidence limit exceeded' }
	foreach ($Owner in $Owners) {
		$Owner.Process.Refresh()
		if ($Owner.Process.HasExited) { continue }
		$Samples.Add([pscustomobject]@{
			RunId = $LocalRunId; Label = $Owner.Label; Pid = $Owner.Pid
			Utc = [DateTimeOffset]::UtcNow.ToString('O')
			SupervisorElapsedMilliseconds = $SupervisorElapsedMilliseconds
			MonotonicTicks = [Diagnostics.Stopwatch]::GetTimestamp()
			MonotonicFrequency = [Diagnostics.Stopwatch]::Frequency
			WorkingSetBytes = $Owner.Process.WorkingSet64
			PrivateBytes = $Owner.Process.PrivateMemorySize64
			CpuMilliseconds = [math]::Round($Owner.Process.TotalProcessorTime.TotalMilliseconds, 3)
			Threads = $Owner.Process.Threads.Count; Handles = $Owner.Process.HandleCount
		})
	}
}

# Resource evidence is sampled on this role's own host and monotonic clock. The
# process sum is one bounded sweep, not a sum of each process's separate peak.
function Initialize-HostResourceCounters {
	param([string]$PreparationRoot, $Preparation, [string]$ExpectedRoot, [string]$EvidenceDirectory)
	# The fixed source is emitted once before the role clock. A child loads the
	# exact held bytes, rather than compiling again or reopening a verified path.
	$Source = @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
public static class FarmSamplerPipeCopy {
    // Two owned readers avoid queuing behind native owners' idle pipe reads.
    // They do not execute PowerShell, commands, or callbacks.
    public static Task Start(Stream Input, Stream Output) {
        return Task.Factory.StartNew(() => {
            byte[] Buffer = new byte[4096]; long Count = 0;
            int Read;
            while ((Read = Input.Read(Buffer, 0, Buffer.Length)) != 0) {
                if (Count + Read > 4194304) throw new IOException("sampler pipe exceeded four MiB");
                Output.Write(Buffer, 0, Read); Output.Flush(); Count += Read;
            }
        }, CancellationToken.None, TaskCreationOptions.LongRunning, TaskScheduler.Default);
    }
}
public static class FarmHostNativeCounters {
    [StructLayout(LayoutKind.Sequential)]
    public struct MemoryStatus {
        public uint Length, Load;
        public ulong TotalPhysical, AvailablePhysical, TotalPageFile, AvailablePageFile;
        public ulong TotalVirtual, AvailableVirtual, AvailableExtendedVirtual;
    }
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool GetSystemTimes(out ulong idle, out ulong kernel, out ulong user);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool GlobalMemoryStatusEx(ref MemoryStatus status);
}
'@
	$SourceSha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData(
		[Text.UTF8Encoding]::new($false).GetBytes($Source))).ToLowerInvariant()
	$RuntimePath = [IO.Path]::GetFullPath((Get-Process -Id $PID).Path)
	$RuntimeSha256 = (Get-FileHash -LiteralPath $RuntimePath -Algorithm SHA256).Hash.ToLowerInvariant()
	$CounterType = 'FarmHostNativeCounters' -as [type]
	$PipeType = 'FarmSamplerPipeCopy' -as [type]
	$MemoryType = 'FarmHostNativeCounters+MemoryStatus' -as [type]
	if ($PreparationRoot) {
		if ($null -ne $Preparation -or $null -ne $CounterType -or $null -ne $PipeType -or $null -ne $MemoryType) {
			throw 'counter preparation refuses an existing or foreign loaded type'
		}
		$PreparationRoot = [IO.Path]::GetFullPath($PreparationRoot)
		$ExpectedRoot = $PreparationRoot
		if (-not $EvidenceDirectory -or -not [IO.Directory]::Exists($EvidenceDirectory) -or
			([IO.File]::GetAttributes($EvidenceDirectory) -band [IO.FileAttributes]::ReparsePoint)) {
			throw 'counter preparation evidence directory differs'
		}
		if ([IO.Directory]::Exists($PreparationRoot) -or [IO.File]::Exists($PreparationRoot)) {
			throw 'counter preparation directory already exists'
		}
		$Ancestor = [IO.Path]::GetDirectoryName($PreparationRoot)
		while ($Ancestor) {
			if ([IO.File]::Exists($Ancestor) -or ([IO.Directory]::Exists($Ancestor) -and
				([IO.File]::GetAttributes($Ancestor) -band [IO.FileAttributes]::ReparsePoint))) {
				throw 'counter preparation ancestor redirected'
			}
			$Ancestor = [IO.Path]::GetDirectoryName($Ancestor)
		}
		[void][IO.Directory]::CreateDirectory($PreparationRoot)
		$AssemblyPath = Join-Path $PreparationRoot 'counters.dll'
		# OutputAssembly emits without loading the types. No compile retry/cache.
		$CompilationFailure = $null
		try { Add-Type -TypeDefinition $Source -OutputAssembly $AssemblyPath -OutputType Library -ErrorAction Stop }
		catch { $CompilationFailure = $_.Exception.Message; throw }
		finally {
			$Files = @(Get-ChildItem -LiteralPath $PreparationRoot -File | ForEach-Object {
				if ($_.Name -cne 'counters.dll' -or $_.Length -gt 1048576 -or
					($_.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'counter preparation output differs' }
				[ordered]@{ Name = $_.Name; Bytes = $_.Length; Sha256 = (Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant() }
			})
			$CollectedAssembly = Join-Path $EvidenceDirectory 'resource-counter-assembly.dll'
			if ($Files.Count -eq 1) {
				$HeldPartial = [IO.File]::Open($AssemblyPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
				try {
					if ($HeldPartial.Length -ne $Files[0].Bytes) { throw 'counter preparation retention size differs' }
					$Partial = [byte[]]::new([int]$HeldPartial.Length); $HeldPartial.ReadExactly($Partial,0,$Partial.Length)
					if ([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Partial)).ToLowerInvariant() -cne $Files[0].Sha256) {
						throw 'counter preparation retention hash differs'
					}
					$Flat = [IO.File]::Open($CollectedAssembly,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
					try { $Flat.Write($Partial); $Flat.Flush($true) } finally { $Flat.Dispose() }
				} finally { $HeldPartial.Dispose() }
			}
			Write-EndpointSamplerJson -Path (Join-Path $EvidenceDirectory 'resource-counter-preparation.json') -Value ([ordered]@{
				SourceSha256 = $SourceSha256; RuntimePath = $RuntimePath; RuntimeSha256 = $RuntimeSha256;
				Files = $Files; CollectedAssemblyName = 'resource-counter-assembly.dll'; CompilationFailure = $CompilationFailure;
				Scope = 'OWNED_PRE_RUN_COUNTER_ASSEMBLY' })
		}
		$Preparation = [pscustomobject]@{
			Path = $AssemblyPath; Bytes = [long](Get-Item -LiteralPath $AssemblyPath).Length
			Sha256 = (Get-FileHash -LiteralPath $AssemblyPath -Algorithm SHA256).Hash.ToLowerInvariant()
			SourceSha256 = $SourceSha256; RuntimePath = $RuntimePath; RuntimeSha256 = $RuntimeSha256
			FullName = $null; ModuleVersionId = $null; LoadedAssembly = $null
		}
	}
	$Names = @('Path','Bytes','Sha256','SourceSha256','RuntimePath','RuntimeSha256','FullName','ModuleVersionId','LoadedAssembly')
	if ($null -eq $Preparation -or @($Preparation.PSObject.Properties.Name | Where-Object { $_ -notin $Names }).Count -or
		@($Names | Where-Object { $null -eq $Preparation.PSObject.Properties[$_] }).Count -or
		$Preparation.Path -isnot [string] -or -not $ExpectedRoot -or
		[IO.Path]::GetFullPath($Preparation.Path) -ine [IO.Path]::GetFullPath((Join-Path $ExpectedRoot 'counters.dll')) -or
		[IO.Path]::GetFileName($Preparation.Path) -cne 'counters.dll' -or
		[IO.Path]::GetFullPath($Preparation.Path) -cne $Preparation.Path -or
		$Preparation.Bytes -isnot [long] -or $Preparation.Bytes -le 0 -or $Preparation.Bytes -gt 1048576 -or
		$Preparation.Sha256 -cnotmatch '^[a-f0-9]{64}$' -or $Preparation.SourceSha256 -cne $SourceSha256 -or
		$Preparation.RuntimePath -ine $RuntimePath -or $Preparation.RuntimeSha256 -cne $RuntimeSha256 -or
		(-not $PreparationRoot -and ($Preparation.FullName -isnot [string] -or [string]::IsNullOrWhiteSpace($Preparation.FullName) -or
			$Preparation.ModuleVersionId -isnot [string] -or $Preparation.ModuleVersionId -cnotmatch '^[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}$'))) {
		throw 'counter assembly source/runtime/path/size pin differs'
	}
	$Ancestor = $Preparation.Path
	while ($Ancestor) {
		if (([IO.File]::Exists($Ancestor) -or [IO.Directory]::Exists($Ancestor)) -and
			([IO.File]::GetAttributes($Ancestor) -band [IO.FileAttributes]::ReparsePoint)) {
			throw 'counter assembly path redirected'
		}
		$Ancestor = [IO.Path]::GetDirectoryName($Ancestor)
	}
	$Held = [IO.File]::Open($Preparation.Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
	try {
		if ($Held.Length -ne $Preparation.Bytes) { throw 'counter assembly held size differs' }
		$Bytes = [byte[]]::new([int]$Held.Length)
		$Held.ReadExactly($Bytes,0,$Bytes.Length)
		if ([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant() -cne $Preparation.Sha256) {
			throw 'counter assembly held hash differs'
		}
		if ($null -ne $CounterType -or $null -ne $PipeType -or $null -ne $MemoryType) {
			if ($null -eq $Preparation.LoadedAssembly -or $null -eq $CounterType -or $null -eq $PipeType -or $null -eq $MemoryType -or
				-not [object]::ReferenceEquals($CounterType.Assembly,$Preparation.LoadedAssembly) -or
				-not [object]::ReferenceEquals($PipeType.Assembly,$Preparation.LoadedAssembly) -or
				-not [object]::ReferenceEquals($MemoryType.Assembly,$Preparation.LoadedAssembly)) {
				throw 'counter assembly loaded type origin differs'
			}
			$Assembly = $Preparation.LoadedAssembly
		} else {
			if ($null -ne $Preparation.LoadedAssembly) { throw 'counter assembly loaded identity is not visible' }
			$Assembly = [Reflection.Assembly]::Load($Bytes)
		}
		$CounterType = 'FarmHostNativeCounters' -as [type]
		$PipeType = 'FarmSamplerPipeCopy' -as [type]
		$MemoryType = 'FarmHostNativeCounters+MemoryStatus' -as [type]
		if ($null -eq $CounterType -or $null -eq $PipeType -or $null -eq $MemoryType -or
			-not [object]::ReferenceEquals($CounterType.Assembly,$Assembly) -or
			-not [object]::ReferenceEquals($PipeType.Assembly,$Assembly) -or
			-not [object]::ReferenceEquals($MemoryType.Assembly,$Assembly) -or
			(@($Assembly.GetExportedTypes().FullName | Sort-Object) -join ',') -cne
				'FarmHostNativeCounters,FarmHostNativeCounters+MemoryStatus,FarmSamplerPipeCopy' -or
			($Preparation.FullName -and $Preparation.FullName -cne $Assembly.FullName) -or
			($Preparation.ModuleVersionId -and $Preparation.ModuleVersionId -cne $Assembly.ManifestModule.ModuleVersionId.ToString())) {
			throw 'counter assembly type/identity pin differs'
		}
		$Result = [pscustomobject]@{ Path = $Preparation.Path; Bytes = $Preparation.Bytes; Sha256 = $Preparation.Sha256;
			SourceSha256 = $SourceSha256; RuntimePath = $RuntimePath; RuntimeSha256 = $RuntimeSha256;
			FullName = $Assembly.FullName; ModuleVersionId = $Assembly.ManifestModule.ModuleVersionId.ToString(); LoadedAssembly = $Assembly }
		return $Result
	} finally { $Held.Dispose() }
}

function Get-HostResourceInterface {
	param([Net.IPAddress]$ServerAddress, [string]$LocalRole)
	if ($LocalRole -eq 'Server') {
		$GameIp = Get-NetIPAddress -IPAddress $ServerAddress.ToString() -AddressFamily IPv4 -ErrorAction Stop |
			Select-Object -First 1
		$Index = [int]$GameIp.InterfaceIndex
	} else {
		$Route = Find-NetRoute -RemoteIPAddress $ServerAddress.ToString() -ErrorAction Stop |
			Select-Object -First 1
		$Index = [int]$Route.InterfaceIndex
		$GameIp = Get-NetIPAddress -InterfaceIndex $Index -AddressFamily IPv4 -ErrorAction Stop |
			Where-Object IPAddress -eq '10.253.3.1' | Select-Object -First 1
	}
	if (-not $GameIp -or $Index -le 0) { throw 'resource sampler cannot identify the game interface' }
	$Adapter = Get-NetAdapter -InterfaceIndex $Index -ErrorAction Stop
	if ($Adapter.Status -cne 'Up' -or $Adapter.LinkSpeed -cne '10 Gbps' -or
		[string]::IsNullOrWhiteSpace($Adapter.MacAddress)) {
		throw 'resource sampler game interface lost its qualified identity or link'
	}
	return [pscustomobject]@{
		Index = $Index; MacAddress = [string]$Adapter.MacAddress
		Address = [string]$GameIp.IPAddress; Name = [string]$Adapter.Name
		HostName = [Environment]::MachineName
	}
}

function Add-HostResourceSample {
	param([object[]]$Owners, [string]$LocalRunId, [string]$LocalRole,
		[string]$LocalProvider, $Interface, [Collections.Generic.List[object]]$Samples,
		[long]$SupervisorElapsedMilliseconds)
	if ($Samples.Count -ge 1000) { throw 'bounded host resource evidence exceeded 1000 records' }
	$StartTicks = [Diagnostics.Stopwatch]::GetTimestamp()
	$Idle = [ulong]0; $Kernel = [ulong]0; $User = [ulong]0
	if (-not [FarmHostNativeCounters]::GetSystemTimes([ref]$Idle, [ref]$Kernel, [ref]$User)) {
		throw 'host CPU counters are unavailable'
	}
	$WorkingSet = 0L; $Private = 0L; $Live = 0
	foreach ($Owner in $Owners) {
		try {
			$Owner.Process.Refresh()
			if ($Owner.Process.HasExited) { continue }
			$WorkingSet += $Owner.Process.WorkingSet64
			$Private += $Owner.Process.PrivateMemorySize64
			$Live++
		} catch {
			if ($Owner.Process.HasExited) { continue }
			throw "host resource sample failed for $($Owner.Label): $($_.Exception.Message)"
		}
	}
	$Memory = [FarmHostNativeCounters+MemoryStatus]::new()
	$Memory.Length = [Runtime.InteropServices.Marshal]::SizeOf(
		[type][FarmHostNativeCounters+MemoryStatus])
	if (-not [FarmHostNativeCounters]::GlobalMemoryStatusEx([ref]$Memory)) {
		throw 'host memory counters are unavailable'
	}
	$Adapter = Get-NetAdapter -InterfaceIndex $Interface.Index -ErrorAction Stop
	if ($Adapter.Status -cne 'Up' -or $Adapter.LinkSpeed -cne '10 Gbps' -or
		$Adapter.MacAddress -cne $Interface.MacAddress -or $Adapter.Name -cne $Interface.Name) {
		throw 'resource sampler interface changed during the run'
	}
	$Nic = $Adapter | Get-NetAdapterStatistics -ErrorAction Stop
	if (-not $Nic) { throw 'game interface byte/packet counters are unavailable' }
	$EndTicks = [Diagnostics.Stopwatch]::GetTimestamp()
	$Samples.Add([pscustomobject]@{
		RunId = $LocalRunId; Role = $LocalRole; Provider = $LocalProvider
		HostName = $Interface.HostName; InterfaceIndex = $Interface.Index
		InterfaceMacAddress = $Interface.MacAddress; InterfaceAddress = $Interface.Address
		InterfaceLinkSpeed = '10 Gbps'; Utc = [DateTimeOffset]::UtcNow.ToString('O')
		SupervisorElapsedMilliseconds = $SupervisorElapsedMilliseconds
		SampleStartTicks = $StartTicks; SampleEndTicks = $EndTicks
		MonotonicFrequency = [Diagnostics.Stopwatch]::Frequency
		LiveOwnedProcessCount = $Live; OwnedWorkingSetBytes = $WorkingSet
		OwnedPrivateBytes = $Private; HostTotalPhysicalBytes = $Memory.TotalPhysical
		HostAvailablePhysicalBytes = $Memory.AvailablePhysical
		HostCpuIdle100ns = $Idle; HostCpuKernel100ns = $Kernel; HostCpuUser100ns = $User
		NicSentBytes = $Nic.SentBytes; NicReceivedBytes = $Nic.ReceivedBytes
		NicOutboundDiscardedPackets = $Nic.OutboundDiscardedPackets
		NicOutboundPacketErrors = $Nic.OutboundPacketErrors
		NicReceivedDiscardedPackets = $Nic.ReceivedDiscardedPackets
		NicReceivedPacketErrors = $Nic.ReceivedPacketErrors
	})
}

function Add-TimedResourceSamples {
	param([object[]]$Owners)
	if ($null -ne $ResourceSampler) { Assert-EndpointSampler -Sampler $ResourceSampler; return }
	$Elapsed = $RunClock.ElapsedMilliseconds
	if ($Elapsed - $script:LastResourceSampleMilliseconds -lt 2000) { return }
	Add-EndpointResourceSamples -Owners $Owners -LocalRunId $RunId `
		-Samples $ResourceSamples -SupervisorElapsedMilliseconds $Elapsed
	Add-HostResourceSample -Owners $Owners -LocalRunId $RunId -LocalRole $Role `
		-LocalProvider $Manifest.Provider -Interface $HostResourceInterface `
		-Samples $HostResourceSamples -SupervisorElapsedMilliseconds $Elapsed
	$script:LastResourceSampleMilliseconds = $Elapsed
}

# The fixed child is generated from this already hash-pinned endpoint source.
# It owns only resource observation, never a native farm process or a socket.
# Immutable owner publications avoid a blocking pipe or a mutable PID roster.
function Write-EndpointSamplerJson {
	param([string]$Path, $Value, [int]$MaximumBytes = 16384)
	$Bytes = [Text.UTF8Encoding]::new($false).GetBytes(($Value | ConvertTo-Json -Depth 8 -Compress))
	if ($Bytes.Length -gt $MaximumBytes) { throw 'sampler control record exceeds its fixed byte bound' }
	$Temporary = $Path + '.pending'
	$Stream = [IO.File]::Open($Temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
	try { $Stream.Write($Bytes); $Stream.Flush($true) } finally { $Stream.Dispose() }
	[IO.File]::Move($Temporary, $Path)
}

function Publish-EndpointSamplerOwner {
	param($Sampler, $Owner)
	Assert-EndpointSampler -Sampler $Sampler
	if ($Owner.Label -cnotmatch '^(server|client-(0[0-9]|[12][0-9]|3[01]))$') { throw 'unowned sampler label' }
	$Owner.Process.Refresh()
	$StartedTicks = $Owner.Process.StartTime.ToUniversalTime().Ticks
	# Native per-process counters are a single-owner startup baseline. Expensive
	# adapter queries and all-owner sweeps belong exclusively to the sampler.
	$First = [Collections.Generic.List[object]]::new()
	$Elapsed = [long](([Diagnostics.Stopwatch]::GetTimestamp() - $Sampler.StartedTicks) * 1000 / [Diagnostics.Stopwatch]::Frequency)
	Add-EndpointResourceSamples -Owners @($Owner) -LocalRunId $Sampler.RunId -Samples $First -SupervisorElapsedMilliseconds $Elapsed
	if ($First.Count -ne 1) { throw 'new sampler owner lacks its actual startup counter baseline' }
	Write-EndpointSamplerJson -Path (Join-Path $Sampler.Root ($Owner.Label + '.owner.json')) -Value ([ordered]@{
		RunId = $Sampler.RunId; Label = $Owner.Label; Pid = $Owner.Pid; ProcessStartedUtcTicks = $StartedTicks; FirstSample = $First[0] })
}

function Get-EndpointSamplerOwners {
	param([string]$Root, [string]$LocalRunId, [string]$LocalRole)
	$Labels = if ($LocalRole -eq 'Server') { @('server') } else { @(0..31 | ForEach-Object { 'client-{0:D2}' -f $_ }) }
	$Owners = [Collections.Generic.List[object]]::new()
	try {
		foreach ($Label in $Labels) {
			$File = Join-Path $Root ($Label + '.owner.json')
			if (-not [IO.File]::Exists($File)) { continue }
			if ((Get-Item -LiteralPath $File).Length -gt 16384) { throw 'sampler owner record oversized' }
			$Record = Get-Content -LiteralPath $File -Raw | ConvertFrom-Json -AsHashtable
			$SampleNames = @('RunId','Label','Pid','Utc','SupervisorElapsedMilliseconds','MonotonicTicks','MonotonicFrequency',
				'WorkingSetBytes','PrivateBytes','CpuMilliseconds','Threads','Handles')
			if ($Record.Count -ne 5 -or @('RunId','Label','Pid','ProcessStartedUtcTicks','FirstSample' | Where-Object { -not $Record.Contains($_) }).Count -ne 0 -or
				$Record.RunId -cne $LocalRunId -or $Record.Label -cne $Label -or
				$Record.Pid -isnot [long] -or $Record.Pid -le 0 -or
				$Record.ProcessStartedUtcTicks -isnot [long] -or $Record.ProcessStartedUtcTicks -le 0 -or
				$Record.FirstSample.RunId -cne $LocalRunId -or $Record.FirstSample.Label -cne $Label -or
				$Record.FirstSample -isnot [Collections.IDictionary] -or $Record.FirstSample.Count -ne $SampleNames.Count -or
				@($SampleNames | Where-Object { -not $Record.FirstSample.Contains($_) }).Count -ne 0 -or
				$Record.FirstSample.Pid -ne $Record.Pid -or $Record.FirstSample.MonotonicFrequency -ne [Diagnostics.Stopwatch]::Frequency -or
				$Record.FirstSample.MonotonicTicks -isnot [long] -or $Record.FirstSample.MonotonicTicks -le 0 -or
				$Record.FirstSample.SupervisorElapsedMilliseconds -isnot [long] -or $Record.FirstSample.SupervisorElapsedMilliseconds -lt 0) {
				throw 'sampler owner identity record is invalid'
			}
			try { $Process = [Diagnostics.Process]::GetProcessById([int]$Record.Pid) }
			catch [ArgumentException] { continue } # An already exited owner is not replaced by a guessed PID.
			try {
				if ($Process.StartTime.ToUniversalTime().Ticks -ne $Record.ProcessStartedUtcTicks) { throw 'sampler PID generation changed' }
				$Owners.Add([pscustomobject]@{ Label = $Label; Pid = $Record.Pid; Process = $Process; FirstSample = [pscustomobject]$Record.FirstSample })
			} catch { $Process.Dispose(); throw }
		}
		return ,$Owners
	} catch { foreach ($Owner in $Owners) { $Owner.Process.Dispose() }; throw }
}

function Write-EndpointSamplerCsv {
	param([IO.StreamWriter]$Writer, [object[]]$Rows, [ref]$HeaderWritten)
	foreach ($Row in $Rows) {
		$Lines = @($Row | ConvertTo-Csv -NoTypeInformation)
		if (-not $HeaderWritten.Value) { $Writer.WriteLine($Lines[0]); $HeaderWritten.Value = $true }
		$Writer.WriteLine($Lines[1])
	}
	$Writer.Flush()
	if ($Writer.BaseStream.Position -gt 16777216) { throw 'sampler raw evidence exceeds 16 MiB' }
}

function Invoke-EndpointResourceSampler {
	param([string]$ConfigurationPath)
	$EntryTicks = [Diagnostics.Stopwatch]::GetTimestamp()
	$Config = Get-Content -LiteralPath $ConfigurationPath -Raw | ConvertFrom-Json -AsHashtable
	$Names = @('RunId','Role','Provider','Interface','StartedTicks','Frequency','TimeoutMilliseconds','ScriptSha256',
		'CounterAssembly','InitializationStartedTicks')
	$InterfaceNames = @('Index','MacAddress','Address','Name','HostName')
	if ($Config.Count -ne $Names.Count -or @($Names | Where-Object { -not $Config.Contains($_) }).Count -ne 0 -or
		$Config.RunId -cnotmatch '^[a-f0-9-]{36}$' -or $Config.Role -cnotin @('Server','Clients') -or
		$Config.Provider -cnotin @('Local','Node') -or $Config.Frequency -ne [Diagnostics.Stopwatch]::Frequency -or
		$Config.StartedTicks -isnot [long] -or $Config.StartedTicks -le 0 -or $Config.Frequency -isnot [long] -or
		$Config.TimeoutMilliseconds -isnot [long] -or $Config.TimeoutMilliseconds -lt 30000 -or $Config.TimeoutMilliseconds -gt 900000 -or
		$Config.InitializationStartedTicks -isnot [long] -or $Config.InitializationStartedTicks -le 0 -or
		$Config.CounterAssembly -isnot [Collections.IDictionary] -or $null -ne $Config.CounterAssembly.LoadedAssembly -or
		$Config.Interface -isnot [Collections.IDictionary] -or $Config.Interface.Count -ne 5 -or
		@($InterfaceNames | Where-Object { -not $Config.Interface.Contains($_) }).Count -ne 0 -or
		$Config.Interface.Index -isnot [long] -or $Config.Interface.Index -le 0 -or
		@($InterfaceNames[1..4] | Where-Object { $Config.Interface[$_] -isnot [string] -or [string]::IsNullOrWhiteSpace($Config.Interface[$_]) }).Count -ne 0 -or
		(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash -ine $Config.ScriptSha256) {
		throw 'fixed sampler configuration/source differs'
	}
	$Root = Split-Path -Parent $ConfigurationPath
	if ([IO.Path]::GetFileName($ConfigurationPath) -cne 'configuration.json' -or
		[IO.Path]::GetFullPath($PSCommandPath) -ine [IO.Path]::GetFullPath((Join-Path $Root 'sampler.ps1'))) {
		throw 'resource sampler entry paths differ from the fixed generated closure'
	}
	Write-EndpointSamplerJson -Path (Join-Path $Root 'child-entry.json') -Value ([ordered]@{
		RunId = $Config.RunId; Phase = 'CHILD_ENTRY'; StartedTicks = $EntryTicks;
		EndedTicks = [Diagnostics.Stopwatch]::GetTimestamp(); Frequency = $Config.Frequency })
	$ProcessWriter = [IO.StreamWriter]::new([IO.File]::Open((Join-Path $Root 'process-resources.csv'),
		[IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read), [Text.UTF8Encoding]::new($false))
	$HostWriter = $null; $Failure = $null; $ProcessCount = 0; $HostCount = 0
	$ProcessHeader = $false; $HostHeader = $false; $LastElapsed = -2000L
	$FirstSamples = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	try {
		$HostWriter = [IO.StreamWriter]::new([IO.File]::Open((Join-Path $Root 'host-resources.csv'),
			[IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read), [Text.UTF8Encoding]::new($false))
		$LoadStartedTicks = [Diagnostics.Stopwatch]::GetTimestamp()
		Write-EndpointSamplerJson -Path (Join-Path $Root 'child-counter-load.started.json') -Value ([ordered]@{
			RunId = $Config.RunId; Phase = 'VERIFIED_COUNTER_LOAD_STARTED'; StartedTicks = $LoadStartedTicks; Frequency = $Config.Frequency })
		$CounterPreparation = Initialize-HostResourceCounters -Preparation ([pscustomobject]$Config.CounterAssembly) -ExpectedRoot $Root
		Write-EndpointSamplerJson -Path (Join-Path $Root 'child-counter-load.json') -Value ([ordered]@{
			RunId = $Config.RunId; Phase = 'VERIFIED_COUNTER_LOAD'; StartedTicks = $LoadStartedTicks;
			EndedTicks = [Diagnostics.Stopwatch]::GetTimestamp(); Frequency = $Config.Frequency;
			AssemblySha256 = $CounterPreparation.Sha256; SourceSha256 = $CounterPreparation.SourceSha256;
			FullName = $CounterPreparation.FullName; ModuleVersionId = $CounterPreparation.ModuleVersionId })
		while ($true) {
			$Elapsed = [long](([Diagnostics.Stopwatch]::GetTimestamp() - $Config.StartedTicks) * 1000 / $Config.Frequency)
			if ($Elapsed -ge $Config.TimeoutMilliseconds) { throw 'resource sampler runtime deadline elapsed' }
			if ([IO.File]::Exists((Join-Path $Root 'sampler.stop'))) { break }
			if ($Elapsed - $LastElapsed -lt 2000) { Start-Sleep -Milliseconds 25; continue }
			$Sequence = '{0:D4}' -f $HostCount
			Write-EndpointSamplerJson -Path (Join-Path $Root ($Sequence + '.started.json')) -Value ([ordered]@{
				RunId = $Config.RunId; Sequence = $HostCount; StartedTicks = [Diagnostics.Stopwatch]::GetTimestamp() })
			$Owners = Get-EndpointSamplerOwners -Root $Root -LocalRunId $Config.RunId -LocalRole $Config.Role
			try {
				$Processes = [Collections.Generic.List[object]]::new()
				foreach ($Owner in $Owners) {
					if ($FirstSamples.Add($Owner.Label)) { $Processes.Add($Owner.FirstSample) }
				}
				# The roster can grow while it is read. Stamp the actual sweep only
				# after the immutable snapshot, strictly after each startup baseline.
				$Elapsed = [long](([Diagnostics.Stopwatch]::GetTimestamp() - $Config.StartedTicks) * 1000 / $Config.Frequency)
				if (@($Owners | Where-Object { $_.FirstSample.SupervisorElapsedMilliseconds -ge $Elapsed }).Count) {
					Start-Sleep -Milliseconds 1
					$Elapsed = [long](([Diagnostics.Stopwatch]::GetTimestamp() - $Config.StartedTicks) * 1000 / $Config.Frequency)
				}
				if ($ProcessCount + $Processes.Count + $Owners.Count -gt 20000 -or $HostCount -ge 1000) { throw 'bounded sampler record count exceeded' }
				Add-EndpointResourceSamples -Owners @($Owners) -LocalRunId $Config.RunId -Samples $Processes -SupervisorElapsedMilliseconds $Elapsed
				Write-EndpointSamplerCsv -Writer $ProcessWriter -Rows @($Processes) -HeaderWritten ([ref]$ProcessHeader)
				$ProcessCount += $Processes.Count
				$Hosts = [Collections.Generic.List[object]]::new()
				Add-HostResourceSample -Owners @($Owners) -LocalRunId $Config.RunId -LocalRole $Config.Role -LocalProvider $Config.Provider `
					-Interface ([pscustomobject]$Config.Interface) -Samples $Hosts -SupervisorElapsedMilliseconds $Elapsed
				if ($Hosts.Count -ne 1) { throw 'host resource sampler did not produce exactly one bracketed observation' }
				Write-EndpointSamplerCsv -Writer $HostWriter -Rows @($Hosts) -HeaderWritten ([ref]$HostHeader)
				$HostCount += $Hosts.Count
				if (($Hosts[0].SampleEndTicks - $Hosts[0].SampleStartTicks) * 1000 / $Config.Frequency -gt 5000) {
					throw 'host resource snapshot spans over five seconds'
				}
				Write-EndpointSamplerJson -Path (Join-Path $Root ($Sequence + '.completed.json')) -Value ([ordered]@{
					RunId = $Config.RunId; Sequence = $HostCount - 1; EndedTicks = [Diagnostics.Stopwatch]::GetTimestamp() })
				if ($HostCount -eq 1) {
					Write-EndpointSamplerJson -Path (Join-Path $Root 'child-first-baseline.json') -Value ([ordered]@{
						RunId = $Config.RunId; Phase = 'FIRST_REAL_BASELINE'; StartedTicks = $Hosts[0].SampleStartTicks;
						EndedTicks = $Hosts[0].SampleEndTicks; Frequency = $Config.Frequency; LiveOwnedProcessCount = $Owners.Count })
					Write-EndpointSamplerJson -Path (Join-Path $Root 'sampler.ready.json') -Value ([ordered]@{ RunId = $Config.RunId; Ready = $true })
				}
				$LastElapsed = $Elapsed
			} finally { foreach ($Owner in $Owners) { $Owner.Process.Dispose() } }
		}
	} catch { $Failure = $_.Exception.Message }
	finally {
		$ProcessWriter.Dispose(); if ($null -ne $HostWriter) { $HostWriter.Dispose() }
		Write-EndpointSamplerJson -Path (Join-Path $Root 'sampler.result.json') -Value ([ordered]@{
			RunId = $Config.RunId; Role = $Config.Role; Success = ($null -eq $Failure); Failure = $Failure;
			ProcessSamples = $ProcessCount; HostSamples = $HostCount })
	}
	if ($null -ne $Failure) { throw "[Qualification:ResourceSampler] $Failure" }
}

function Assert-EndpointSampler {
	param($Sampler)
	if ($Sampler.Stopped) { return }
	if ($Sampler.OutputCopy.IsFaulted -or $Sampler.ErrorCopy.IsFaulted) { throw 'bounded sampler output reader failed' }
	foreach ($Path in @($Sampler.OutputPath,$Sampler.ErrorPath)) {
		if ((Get-Item -LiteralPath $Path).Length -gt 4194304) { throw 'resource sampler log exceeds four MiB' }
	}
	if ($Sampler.Process.HasExited) { throw "resource sampler exited before owned stop: $($Sampler.Process.ExitCode)" }
	$Now = [Diagnostics.Stopwatch]::GetTimestamp()
	$Latest = @(Get-ChildItem -LiteralPath $Sampler.Root -Filter '*.started.json' -File |
		Where-Object Name -cmatch '^[0-9]{4}\.started\.json$' | Sort-Object Name | Select-Object -Last 1)
	if ($Latest.Count) {
		$Record = Get-Content -LiteralPath $Latest[0].FullName -Raw | ConvertFrom-Json -AsHashtable
		$Completed = Join-Path $Sampler.Root ($Latest[0].Name.Replace('.started.json','.completed.json'))
		if (-not [IO.File]::Exists($Completed) -and ($Now - $Record.StartedTicks) * 1000 / [Diagnostics.Stopwatch]::Frequency -gt 5000) {
			throw 'resource sampler host query exceeded the five-second bound'
		}
	}
}

function Start-EndpointResourceSampler {
	param([string]$EndpointSource, [string]$OutputDirectory, [string]$LocalRunId, [string]$LocalRole,
		[string]$LocalProvider, $Interface, [long]$StartedTicks, [int]$TimeoutMilliseconds, $CounterPreparation,
		[string]$Registry, [string[]]$SecretVariables = @())
	$InitializationClock = [Diagnostics.Stopwatch]::StartNew()
	$InitializationStartedTicks = [Diagnostics.Stopwatch]::GetTimestamp()
	$VerifiedCounters = Initialize-HostResourceCounters -Preparation $CounterPreparation -ExpectedRoot (Split-Path -Parent $CounterPreparation.Path)
	$CountersVerifiedTicks = [Diagnostics.Stopwatch]::GetTimestamp()
	if ($LocalRunId -cnotmatch '^[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}$' -or $LocalRole -cnotin @('Server','Clients') -or -not $Registry) {
		throw 'resource sampler registry identity differs'
	}
	$Registry = [IO.Path]::GetFullPath($Registry)
	if ($Registry -ieq [IO.Path]::GetFullPath($OutputDirectory) -or
		$Registry.StartsWith([IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {
		throw 'resource sampler registry must remain outside flat evidence'
	}
	$Ancestor = $Registry
	while ($Ancestor) {
		if ([IO.File]::Exists($Ancestor) -or ([IO.Directory]::Exists($Ancestor) -and
			([IO.File]::GetAttributes($Ancestor) -band [IO.FileAttributes]::ReparsePoint))) { throw 'resource sampler registry redirected' }
		$Ancestor = [IO.Path]::GetDirectoryName($Ancestor)
	}
	$Root = Join-Path $Registry "$LocalRunId.$LocalRole.resource-sampler"
	if (Test-Path -LiteralPath $Root) { throw 'resource sampler directory already exists' }
	[void][IO.Directory]::CreateDirectory($Root)
	$Tokens = $null; $Errors = $null
	$Ast = [Management.Automation.Language.Parser]::ParseFile($EndpointSource, [ref]$Tokens, [ref]$Errors)
	if ($Errors.Count) { throw 'sampler source syntax is invalid' }
	$Names = @('Write-EndpointSamplerJson','Get-EndpointSamplerOwners','Write-EndpointSamplerCsv',
		'Invoke-EndpointResourceSampler','Initialize-HostResourceCounters','Add-EndpointResourceSamples','Add-HostResourceSample')
	$Definitions = @($Ast.FindAll({ param($Node) $Node -is [Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -cin $Names }, $true))
	if ($Definitions.Count -ne $Names.Count -or @($Definitions.Name | Sort-Object -Unique).Count -ne $Names.Count) {
		throw 'fixed sampler source closure differs'
	}
	$Script = "param([string]`$ConfigurationPath)`n`$ErrorActionPreference = 'Stop'`n" +
		(($Definitions | ForEach-Object { $_.Extent.Text }) -join "`n") + "`nInvoke-EndpointResourceSampler -ConfigurationPath `$ConfigurationPath`n"
	$ScriptPath = Join-Path $Root 'sampler.ps1'
	$Stream = [IO.File]::Open($ScriptPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
	try { $Stream.Write([Text.UTF8Encoding]::new($false).GetBytes($Script)) } finally { $Stream.Dispose() }
	$AssemblyPath = Join-Path $Root 'counters.dll'
	$Held = [IO.File]::Open($VerifiedCounters.Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
	try {
		if ($Held.Length -ne $VerifiedCounters.Bytes) { throw 'counter assembly copy size differs' }
		$AssemblyBytes = [byte[]]::new([int]$Held.Length); $Held.ReadExactly($AssemblyBytes,0,$AssemblyBytes.Length)
		if ([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($AssemblyBytes)).ToLowerInvariant() -cne $VerifiedCounters.Sha256) {
			throw 'counter assembly copy hash differs'
		}
		$Copy = [IO.File]::Open($AssemblyPath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
		try { $Copy.Write($AssemblyBytes); $Copy.Flush($true) } finally { $Copy.Dispose() }
	} finally { $Held.Dispose() }
	$CounterPin = [ordered]@{ Path = $AssemblyPath; Bytes = $VerifiedCounters.Bytes; Sha256 = $VerifiedCounters.Sha256;
		SourceSha256 = $VerifiedCounters.SourceSha256; RuntimePath = $VerifiedCounters.RuntimePath; RuntimeSha256 = $VerifiedCounters.RuntimeSha256;
		FullName = $VerifiedCounters.FullName; ModuleVersionId = $VerifiedCounters.ModuleVersionId; LoadedAssembly = $null }
	$ConfigurationPath = Join-Path $Root 'configuration.json'
	Write-EndpointSamplerJson -Path $ConfigurationPath -Value ([ordered]@{ RunId = $LocalRunId; Role = $LocalRole;
		Provider = $LocalProvider; Interface = $Interface; StartedTicks = $StartedTicks; Frequency = [Diagnostics.Stopwatch]::Frequency;
		TimeoutMilliseconds = $TimeoutMilliseconds; ScriptSha256 = (Get-FileHash -LiteralPath $ScriptPath -Algorithm SHA256).Hash.ToLowerInvariant();
		CounterAssembly = $CounterPin; InitializationStartedTicks = $InitializationStartedTicks })
	$Runtime = (Get-Process -Id $PID).Path
	$Arguments = @('-NoLogo','-NoProfile','-NonInteractive','-File',$ScriptPath,'-ConfigurationPath',$ConfigurationPath)
	$FunctionPins = [ordered]@{}
	foreach ($Definition in $Definitions) {
		$FunctionPins[$Definition.Name] = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData(
			[Text.UTF8Encoding]::new($false).GetBytes($Definition.Extent.Text))).ToLowerInvariant()
	}
	Write-EndpointSamplerJson -Path (Join-Path $OutputDirectory 'resource-sampler-source.json') -Value ([ordered]@{
		Format = 'GargantuanEndpointResourceSamplerSource'; Version = 2; RunId = $LocalRunId; Role = $LocalRole;
		EndpointSha256 = (Get-FileHash -LiteralPath $EndpointSource -Algorithm SHA256).Hash.ToLowerInvariant();
		ScriptSha256 = (Get-FileHash -LiteralPath $ScriptPath -Algorithm SHA256).Hash.ToLowerInvariant();
		RuntimePath = $Runtime; RuntimeSha256 = (Get-FileHash -LiteralPath $Runtime -Algorithm SHA256).Hash.ToLowerInvariant();
		Arguments = $Arguments; ConfigurationSha256 = (Get-FileHash -LiteralPath $ConfigurationPath -Algorithm SHA256).Hash.ToLowerInvariant();
		FunctionPins = $FunctionPins; CounterAssembly = $CounterPin; Scope = 'OWNED_RESOURCE_OBSERVATION_ONLY' })
	Write-EndpointSamplerJson -Path (Join-Path $Root 'parent-closure.json') -Value ([ordered]@{
		RunId = $LocalRunId; Phase = 'PARENT_VERIFIED_SOURCE_CLOSURE'; StartedTicks = $InitializationStartedTicks;
		CountersVerifiedTicks = $CountersVerifiedTicks; EndedTicks = [Diagnostics.Stopwatch]::GetTimestamp();
		Frequency = [Diagnostics.Stopwatch]::Frequency })
	$Owner = Start-EndpointProcess -Executable $Runtime -WorkingDirectory $Root -Label 'sampler' -OutputDirectory $Root `
		-Arguments $Arguments -RemoveEnvironmentVariables $SecretVariables -SamplerPipes
	$Owner | Add-Member -NotePropertyName Root -NotePropertyValue $Root
	$Owner | Add-Member -NotePropertyName EvidenceDirectory -NotePropertyValue $OutputDirectory
	$Owner | Add-Member -NotePropertyName RunId -NotePropertyValue $LocalRunId
	$Owner | Add-Member -NotePropertyName Stopped -NotePropertyValue $false
	$Owner | Add-Member -NotePropertyName StartedTicks -NotePropertyValue $StartedTicks
	$Owner | Add-Member -NotePropertyName Role -NotePropertyValue $LocalRole
	$Owner | Add-Member -NotePropertyName ProcessStartedUtcTicks -NotePropertyValue $Owner.Process.StartTime.ToUniversalTime().Ticks
	try {
		Write-EndpointSamplerJson -Path (Join-Path $Root 'parent-launch.json') -Value ([ordered]@{
			RunId = $LocalRunId; Phase = 'PARENT_CHILD_LAUNCH'; EndedTicks = [Diagnostics.Stopwatch]::GetTimestamp();
			Frequency = [Diagnostics.Stopwatch]::Frequency; Pid = $Owner.Pid })
		while (-not [IO.File]::Exists((Join-Path $Root 'sampler.ready.json'))) {
			Assert-EndpointSampler -Sampler $Owner
			if ($InitializationClock.ElapsedMilliseconds -ge 5000) { throw 'resource sampler initialization exceeded five seconds' }
			Start-Sleep -Milliseconds 25
		}
		$Ready = Get-Content -LiteralPath (Join-Path $Root 'sampler.ready.json') -Raw | ConvertFrom-Json -AsHashtable
		if ($Ready.Count -ne 2 -or $Ready.RunId -cne $LocalRunId -or $Ready.Ready -isnot [bool] -or -not $Ready.Ready) {
			throw 'sampler readiness record is invalid'
		}
		Write-EndpointSamplerJson -Path (Join-Path $Root 'parent-ready.json') -Value ([ordered]@{
			RunId = $LocalRunId; Phase = 'PARENT_OBSERVED_FIRST_REAL_BASELINE'; EndedTicks = [Diagnostics.Stopwatch]::GetTimestamp();
			Frequency = [Diagnostics.Stopwatch]::Frequency; InitializationMilliseconds = $InitializationClock.ElapsedMilliseconds })
		if ($InitializationClock.ElapsedMilliseconds -ge 5000) { throw 'resource sampler initialization exceeded five seconds' }
		$Owner | Add-Member -NotePropertyName InitializationMilliseconds -NotePropertyValue $InitializationClock.ElapsedMilliseconds
		return $Owner
	} catch {
		try { Stop-EndpointResourceSampler -Sampler $Owner -Processes ([Collections.Generic.List[object]]::new()) -Hosts ([Collections.Generic.List[object]]::new()) } catch {}
		throw
	}
}

function Assert-EndpointSamplerArchive {
	param([string]$Path, [object[]]$Files, [switch]$InventoryOnly)
	if ($Files.Count -lt 1 -or $Files.Count -gt 2100) { throw 'sampler archive inventory differs' }
	$Expected = [Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
	foreach ($File in $Files) {
		if ($File.Name -cnotmatch '^(?:(?:[0-9]{4}\.(?:started|completed)|client-(?:0[0-9]|[12][0-9]|3[01])\.owner|server\.owner|configuration|sampler\.(?:ready|result)|parent-(?:closure|launch|ready)|child-(?:entry|counter-load(?:\.started)?|first-baseline))\.json(?:\.pending)?|sampler\.(?:ps1|stop|stdout\.log|stderr\.log)|(?:process|host)-resources\.csv|counters\.dll)$' -or
			$File.Bytes -lt 0 -or $File.Bytes -gt 16777216 -or $File.Sha256 -cnotmatch '^[a-f0-9]{64}$' -or $Expected.ContainsKey($File.Name)) {
			throw 'sampler archive raw-file pin differs'
		}
		$Expected.Add($File.Name,$File)
	}
	if ($InventoryOnly) { return }
	if ([IO.File]::GetAttributes($Path) -band [IO.FileAttributes]::ReparsePoint) { throw 'sampler archive path redirected' }
	$Held = [IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
	try {
		if ($Held.Length -gt 16777216) { throw 'sampler archive exceeds 16 MiB' }
		$Archive = [IO.Compression.ZipArchive]::new($Held,[IO.Compression.ZipArchiveMode]::Read,$true)
		try {
			if ($Archive.Entries.Count -ne $Expected.Count) { throw 'sampler archive complete membership differs' }
			$Seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
			foreach ($Entry in $Archive.Entries) {
				$Pin = $null
				if (-not $Seen.Add($Entry.FullName) -or -not $Expected.TryGetValue($Entry.FullName,[ref]$Pin) -or
					$Entry.Length -ne $Pin.Bytes) { throw 'sampler archive entry identity/size differs' }
				$Input = $Entry.Open(); $Hasher = [Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
				try {
					$Buffer = [byte[]]::new(4096); $Total = 0L
					while (($Count = $Input.Read($Buffer,0,$Buffer.Length)) -ne 0) {
						$Total += $Count
						if ($Total -gt $Pin.Bytes) { throw 'sampler archive entry decompression exceeds pinned size' }
						$Hasher.AppendData($Buffer,0,$Count)
					}
					if ($Total -ne $Pin.Bytes -or [Convert]::ToHexString($Hasher.GetHashAndReset()).ToLowerInvariant() -cne $Pin.Sha256) {
						throw 'sampler archive entry hash differs'
					}
				} finally { $Input.Dispose(); $Hasher.Dispose() }
			}
		} finally { $Archive.Dispose() }
	} finally { $Held.Dispose() }
}

function Stop-EndpointResourceSampler {
	param($Sampler, [Collections.Generic.List[object]]$Processes, [Collections.Generic.List[object]]$Hosts)
	if ($Sampler.Stopped) { return }
	$Failure = $null
	$ExitCode = $null; $Joined = $false
	try {
		Write-EndpointSamplerJson -Path (Join-Path $Sampler.Root 'sampler.stop') -Value @{ RunId = $Sampler.RunId }
		if (-not $Sampler.Process.WaitForExit(5000)) { $Failure = 'resource sampler did not finish within owned stop bound' }
		else { $ExitCode = $Sampler.Process.ExitCode }
	} finally {
		try { Stop-EndpointProcess -Owner $Sampler; $Joined = $true }
		catch { $Failure = $_.Exception.Message }
		finally { $Sampler.Stopped = $true }
		# Index the generated closure and every raw partial even when a blocked
		# sampler required owned termination. This is no native descendant proof.
		if (@(Get-ChildItem -LiteralPath $Sampler.Root -Force | Where-Object {
			$_.PSIsContainer -or ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) }).Count) { throw 'sampler raw root contains redirected or unindexed directories' }
		$Files = @(Get-ChildItem -LiteralPath $Sampler.Root -File -Force | Sort-Object Name | ForEach-Object {
			Assert-EndpointSamplerArchive -Files @([pscustomobject]@{ Name = $_.Name; Bytes = $_.Length; Sha256 = ('0'*64) }) -InventoryOnly
			$Maximum = if ($_.Name -in @('process-resources.csv','host-resources.csv')) { 16777216 }
				elseif ($_.Name -in @('sampler.stdout.log','sampler.stderr.log')) { 4194304 }
				elseif ($_.Name -in @('sampler.ps1','counters.dll')) { 1048576 } else { 16384 }
			if ($_.Length -gt $Maximum) { throw 'sampler raw file exceeds its fixed bound' }
			[ordered]@{ Name = $_.Name; Bytes = $_.Length; Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
		})
		$ArchivePath = Join-Path $Sampler.EvidenceDirectory 'resource-sampler-raw.zip'
		$ArchivePin = $null; $ArchiveFailure = $null
		try {
			$Output = [IO.File]::Open($ArchivePath,[IO.FileMode]::CreateNew,[IO.FileAccess]::ReadWrite,[IO.FileShare]::Read)
			try {
				$Archive = [IO.Compression.ZipArchive]::new($Output,[IO.Compression.ZipArchiveMode]::Create,$true)
				try {
					foreach ($File in $Files) {
						$Input = [IO.File]::Open((Join-Path $Sampler.Root $File.Name),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
						try {
							if ($Input.Length -ne $File.Bytes) { throw 'sampler archive source held size differs' }
							$Entry = $Archive.CreateEntry($File.Name,[IO.Compression.CompressionLevel]::Optimal); $Target = $Entry.Open()
							$Hasher = [Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
							try {
								$Buffer = [byte[]]::new(4096); $Total = 0L
								while (($Count = $Input.Read($Buffer,0,$Buffer.Length)) -ne 0) {
									$Total += $Count
									if ($Total -gt $File.Bytes) { throw 'sampler archive source changed during held read' }
									$Hasher.AppendData($Buffer,0,$Count); $Target.Write($Buffer,0,$Count)
									if ($Output.Length -gt 16777216) { throw 'sampler archive exceeds 16 MiB' }
								}
								if ($Total -ne $File.Bytes -or [Convert]::ToHexString($Hasher.GetHashAndReset()).ToLowerInvariant() -cne $File.Sha256) {
									throw 'sampler archive source held hash differs'
								}
							} finally { $Target.Dispose(); $Hasher.Dispose() }
						} finally { $Input.Dispose() }
						if ($Output.Length -gt 16777216) { throw 'sampler archive exceeds 16 MiB' }
					}
				} finally { $Archive.Dispose() }
				$Output.Flush($true)
				if ($Output.Length -gt 16777216) { throw 'sampler archive exceeds 16 MiB' }
			} finally { $Output.Dispose() }
			Assert-EndpointSamplerArchive -Path $ArchivePath -Files $Files
			$ArchivePin = [ordered]@{ Name = 'resource-sampler-raw.zip'; Bytes = [long](Get-Item -LiteralPath $ArchivePath).Length;
				Sha256 = (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash.ToLowerInvariant() }
		} catch {
			$ArchiveFailure = $_.Exception.Message
			$Failure = if ($Failure) { "$Failure; archive: $ArchiveFailure" } else { $ArchiveFailure }
		}
		Write-EndpointSamplerJson -Path (Join-Path $Sampler.EvidenceDirectory 'resource-sampler-evidence.json') -Value ([ordered]@{
			Format = 'GargantuanEndpointResourceSamplerEvidence'; Version = 2; RunId = $Sampler.RunId; Role = $Sampler.Role;
			Pid = $Sampler.Pid; CooperativeExitCode = $ExitCode; StreamsJoined = $Joined; StopFailure = $Failure;
			ProcessStartedUtcTicks = $Sampler.ProcessStartedUtcTicks;
			InitializationMilliseconds = $Sampler.InitializationMilliseconds;
			Files = $Files; Archive = $ArchivePin; ArchiveFailure = $ArchiveFailure; NativeTreeReaped = 'NOT_MEASURED_BY_SAMPLER' }) -MaximumBytes 1048576
		foreach ($Pair in @(@('process-resources.csv',$Processes),@('host-resources.csv',$Hosts))) {
			$Path = Join-Path $Sampler.Root $Pair[0]
			if ([IO.File]::Exists($Path)) {
				if ((Get-Item -LiteralPath $Path).Length -gt 16777216) { throw 'sampler raw evidence exceeds 16 MiB' }
				foreach ($Row in @(Import-Csv -LiteralPath $Path)) { $Pair[1].Add($Row) }
			}
		}
	}
	if ($Failure) { throw $Failure }
	$ResultPath = Join-Path $Sampler.Root 'sampler.result.json'
	if (-not [IO.File]::Exists($ResultPath)) { throw 'sampler final receipt absent; raw partials preserved' }
	$Result = Get-Content -LiteralPath $ResultPath -Raw | ConvertFrom-Json -AsHashtable
	if ($Result.Count -ne 6 -or @('RunId','Role','Success','Failure','ProcessSamples','HostSamples' | Where-Object { -not $Result.Contains($_) }).Count -ne 0 -or
		$ExitCode -ne 0 -or $Result.RunId -cne $Sampler.RunId -or $Result.Role -cne $Sampler.Role -or
		$Result.Success -isnot [bool] -or -not $Result.Success -or $null -ne $Result.Failure -or
		$Result.ProcessSamples -isnot [long] -or $Result.HostSamples -isnot [long] -or
		$Result.ProcessSamples -ne $Processes.Count -or $Result.HostSamples -ne $Hosts.Count) { throw 'sampler failed or raw counts differ' }
}

function Assert-FairnessEvidence {
	param([string]$Path, [string]$LocalRunId)
	$File = Get-Item -LiteralPath $Path -ErrorAction Stop
	if (-not $File.PSIsContainer -and ($File.Length -lt 64 -or $File.Length -gt 33554432)) {
		throw 'native fairness evidence size is outside the fixed 32 MiB bound'
	}
	if ($File.PSIsContainer) { throw 'native fairness evidence is not a file' }
	$Stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
	$Reader = [IO.StreamReader]::new($Stream, [Text.UTF8Encoding]::new($false, $true))
	try {
		if ($Reader.ReadLine() -cnotin @("format=GargantuanAdmissionEvidenceV1`trun=$LocalRunId",
			"format=GargantuanAdmissionEvidenceV2`trun=$LocalRunId")) {
			throw 'native fairness evidence run/header mismatch'
		}
		$Count = 0
		$Ended = $false
		while ($null -ne ($Line = $Reader.ReadLine())) {
			$Fields = $Line.Split([char]9)
			if ($Fields[0] -ceq 'end') {
				if ($Ended -or $Fields.Count -ne 3 -or $Fields[1] -cne [string]$Count -or
					$Fields[2] -cne '0' -or $null -ne $Reader.ReadLine()) {
					throw 'native fairness evidence end/count/overflow mismatch'
				}
				$Ended = $true
				break
			}
			if ($Ended -or $Fields.Count -ne 19 -or $Fields[0] -cne 'event' -or
				$Fields[1] -cnotin @('exact_demand', 'credit_eligible', 'eligibility_interrupted',
					'grant_accepted', 'reservation_rolled_back', 'demand_disposed',
					'grant_retired', 'grant_released', 'grant_terminal_released') -or
				$Fields[2] -cnotin @('none', 'no_work', 'unexamined', 'replaced', 'rollback',
					'generation_removed', 'terminal_release', 'feedback_unavailable') -or $Count -ge 65536) {
				throw 'native fairness evidence event schema/count is invalid'
			}
			try {
				$Numbers = @($Fields[3..18] | ForEach-Object { [UInt64]::Parse($_, [Globalization.CultureInfo]::InvariantCulture) })
			} catch { throw 'native fairness evidence has a noninteger field' }
			if ($Numbers[0] -lt 1 -or $Numbers[0] -gt 512 -or $Numbers[1] -eq 0 -or
				$Numbers[2] -eq 0 -or $Numbers[5] -eq 0 -or $Numbers[5] -gt 524288 -or
				$Numbers[7] -gt $Numbers[6] -or $Numbers[8] -gt $Numbers[6] -or
				($Fields[1] -cin @('credit_eligible', 'eligibility_interrupted', 'grant_accepted') -and
					$Numbers[3] -eq 0) -or
				($Fields[1] -cin @('grant_accepted', 'reservation_rolled_back',
					'grant_retired', 'grant_released', 'grant_terminal_released') -and $Numbers[4] -eq 0)) {
				throw 'native fairness evidence identity, size, or timestamp is invalid'
			}
			$Count++
		}
		if (-not $Ended -or $Count -eq 0) { throw 'native fairness evidence has no complete measured events' }
		return [pscustomobject]@{ Count = $Count; Bytes = $File.Length }
	} finally { $Reader.Dispose() }
}

function Assert-ServerEvidence {
	param([string]$LogPath, [System.Collections.IDictionary]$RunManifest, [string]$FairnessPath)
	$Records = @(Get-TypedRecords -Path $LogPath -Kind 'Server')
	$Starts = @($Records | Where-Object event -eq 'start')
	$Ready = @($Records | Where-Object event -eq 'ready')
	$Results = @($Records | Where-Object event -eq 'result')
	if ($Starts.Count -ne 1 -or $Ready.Count -ne 32 -or $Results.Count -ne 1 -or
		$Starts[0].run -cne $RunManifest.RunId -or $Results[0].run -cne $RunManifest.RunId -or
		$Starts[0].provider -cne $RunManifest.Provider.ToLowerInvariant() -or
		$Results[0].provider -cne $RunManifest.Provider.ToLowerInvariant() -or
		$Results[0].expected -cne '32' -or $Results[0].ready_high_water -cne '32' -or
		$Results[0].unique_ready -cne '32' -or $Results[0].identity_conflict -cne '0' -or
		$Results[0].exit -cne '0') { throw 'server typed start/ready/result is invalid' }
	$NonceSet = [Collections.Generic.HashSet[string]]::new()
	$ConnectionSet = [Collections.Generic.HashSet[string]]::new()
	$PlayerSet = [Collections.Generic.HashSet[string]]::new()
	foreach ($Record in $Ready) {
		if ($Record.run -cne $RunManifest.RunId -or $Record.nonce -cnotin $RunManifest.Nonces -or
			-not $NonceSet.Add($Record.nonce) -or
			-not $ConnectionSet.Add("$($Record.connection_slot):$($Record.connection_generation)") -or
			-not $PlayerSet.Add($Record.player_id) -or [UInt64]$Record.connection_slot -eq 0 -or
			[UInt64]$Record.connection_generation -eq 0 -or [UInt64]$Record.session_epoch -eq 0 -or
			[UInt64]$Record.player_id -eq 0) { throw 'server ready identity is invalid' }
	}
	if ($RunManifest.ScaleWorkload) {
		$Scale = @(Get-TypedRecords -Path $LogPath -Kind 'Scale' | Where-Object event -eq 'result')
		if ($Scale.Count -ne 1 -or $Scale[0].run -cne $RunManifest.RunId -or
			$Scale[0].status -cne 'PASS' -or $Scale[0].phases -cne '5' -or
			$Scale[0].peers -cne '32') { throw 'server scale result is invalid' }
		$Fairness = Assert-FairnessEvidence -Path $FairnessPath -LocalRunId $RunManifest.RunId
		$Summary = @(Get-TypedRecords -Path $LogPath -Kind 'Admission' | Where-Object event -eq 'evidence_result')
		if ($Summary.Count -ne 1 -or $Summary[0].run -cne $RunManifest.RunId -or
			$Summary[0].file -cne 'admission-fairness.tsv' -or
			$Summary[0].events -cne [string]$Fairness.Count -or
			$Summary[0].bytes -cne [string]$Fairness.Bytes -or
			$Summary[0].overflow -cne '0' -or $Summary[0].write_failed -cne '0') {
			throw 'server fairness evidence summary does not match the bounded native file'
		}
	}
	return [pscustomobject]@{ Ready = $Ready.Count; UniqueNonces = $NonceSet.Count }
}

function Get-AuthenticatedNodeManifestReceipt {
	param([string]$LogPath, [string]$PackageRoot,
		[System.Collections.IDictionary]$RunManifest)
	if ($RunManifest.Provider -cne 'Node') { throw 'authenticated Node receipt requires Node provider' }
	$Records = @(Get-TypedRecords -Path $LogPath -Kind 'NodeProvider' |
		Where-Object event -eq 'authenticated_manifest')
	if ($Records.Count -ne 1) { throw 'server lacks one authenticated Node manifest RPC record' }
	$Record = $Records[0]
	$PackagePath = Join-Path $PackageRoot 'game.package.json'
	$ContentPath = Join-Path $PackageRoot 'content/content.manifest.json'
	$Package = Get-Content -LiteralPath $PackagePath -Raw | ConvertFrom-Json -AsHashtable
	$Content = Get-Content -LiteralPath $ContentPath -Raw | ConvertFrom-Json -AsHashtable
	$Bytes = (Get-Item -LiteralPath $ContentPath).Length
	if ($Package.ProjectId -cnotmatch '^[a-f0-9]{32}$' -or
		$Package.Revision -isnot [long] -or $Package.Revision -le 0 -or
		$Content.ProjectId -cne $Package.ProjectId -or
		$Content.PackageVersion -ne $Package.Revision -or
		$Record.run -cne $RunManifest.RunId -or
		$Record.project -cne $Package.ProjectId -or
		$Record.revision -cne [string]$Package.Revision -or
		$Record.endpoint -cne $RunManifest.NodeEndpoint -or
		$Record.root_sha256 -ine $RunManifest.NodeRootCertificateSha256 -or
		$Record.manifest_sha256 -ine $RunManifest.ServerContentManifestSha256 -or
		$Record.manifest_bytes -cne [string]$Bytes -or $Bytes -lt 1 -or $Bytes -gt 4194304 -or
		$Record.rpc_count -cnotmatch '^[1-9][0-9]{0,4}$' -or
		$Record.request_id -cnotmatch '^server-content-[1-9][0-9]{0,19}$' -or
		[int]$Record.rpc_count -gt 10000 -or
		$Record.channel -cne 'grpc_ssl_credentials' -or
		$Record.authenticated_rpc -cne '1' -or
		$Record.tls_session_details -cne 'not_measured') {
		throw 'authenticated Node manifest RPC differs from pinned package, CA, or channel semantics'
	}
	return [ordered]@{
		Format = 'GargantuanFarmNodeAuthenticatedManifest'; Version = 2
		RunId = $RunManifest.RunId; Provider = 'Node'
		RequestId = $Record.request_id
		ProjectId = $Package.ProjectId; PackageVersion = [long]$Package.Revision
		NodeEndpoint = $RunManifest.NodeEndpoint
		RootCertificateSha256 = $RunManifest.NodeRootCertificateSha256.ToLowerInvariant()
		ManifestSha256 = $RunManifest.ServerContentManifestSha256.ToLowerInvariant()
		ManifestBytes = [long]$Bytes; AuthenticatedManifestRpcCount = [int]$Record.rpc_count
		ChannelCredentials = 'grpc_ssl_credentials'; TlsSessionDetails = 'NOT_MEASURED'
		Source = 'GargantuanServer/NodeContentProvider'
	}
}

function Write-AuthenticatedNodeManifestReceipt {
	param([string]$Path, [System.Collections.IDictionary]$Receipt)
	$Bytes = [Text.UTF8Encoding]::new($false).GetBytes(($Receipt | ConvertTo-Json -Depth 4))
	if ($Bytes.Length -gt 4096) { throw 'authenticated Node receipt exceeds its byte bound' }
	$Stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write,
		[IO.FileShare]::None)
	try { $Stream.Write($Bytes) } finally { $Stream.Dispose() }
}

function Assert-ClientEvidence {
	param([object[]]$Clients, [System.Collections.IDictionary]$RunManifest)
	if ($Clients.Count -ne 32) { throw 'client process count is not 32' }
	for ($Slot = 0; $Slot -lt 32; $Slot++) {
		$Records = @(Get-TypedRecords -Path $Clients[$Slot].OutputPath -Kind 'Client')
		$Starts = @($Records | Where-Object event -eq 'start')
		$Ready = @($Records | Where-Object event -eq 'ready')
		$Results = @($Records | Where-Object event -eq 'result')
		if ($Starts.Count -ne 1 -or $Ready.Count -ne 1 -or $Results.Count -ne 1) {
			throw "client $Slot typed cardinality is invalid"
		}
		foreach ($Record in @($Starts[0], $Ready[0], $Results[0])) {
			if ($Record.run_id -cne $RunManifest.RunId -or $Record.slot -cne [string]$Slot -or
				$Record.nonce -cne $RunManifest.Nonces[$Slot]) { throw "client $Slot identity is invalid" }
		}
		$ExpectedReason = if ($RunManifest.ScaleWorkload) { 'scale_complete' } else { 'completed' }
		$CharacterRequired = -not $RunManifest.ScaleWorkload -or $Slot % 4 -eq 0
		if ($Ready[0].connection_slot -eq '0' -or $Ready[0].connection_generation -eq '0' -or
			$Results[0].status -cne 'PASS' -or $Results[0].exit_code -cne '0' -or
			$Results[0].reason -cne $ExpectedReason -or $Results[0].local_player -cne '1' -or
			($CharacterRequired -and $Results[0].character -cne '1')) {
			throw "client $Slot typed result is invalid"
		}
	}
	return 32
}

function Write-EndpointEvidenceHash {
	param([string]$Directory, [string]$LocalRunId, [string]$LocalRole)
	$Files = @(
		Get-ChildItem -LiteralPath $Directory -File | Where-Object Name -ne 'evidence-sha256.json' |
			Sort-Object Name | ForEach-Object {
				[ordered]@{ Name = $_.Name; Bytes = $_.Length;
					Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
			}
	)
	if ($Files.Count -eq 0) { throw 'no role-local evidence to hash' }
	[IO.File]::WriteAllText((Join-Path $Directory 'evidence-sha256.json'),
		([ordered]@{ RunId = $LocalRunId; Role = $LocalRole; Files = $Files } |
		ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
}

try {
	$ManifestPath = [IO.Path]::GetFullPath($ManifestPath)
	$Manifest = Read-PinnedRunManifest -Path $ManifestPath -ExpectedSha256 $ManifestSha256
	$Network = Assert-RunManifest -Value $Manifest
	$RunId = $Manifest.RunId
	$HostResourceInterface = Get-HostResourceInterface -ServerAddress $Network.Address -LocalRole $Role
	$Paths = Assert-RolePaths -LocalPackage $PackageRoot -LocalEvidence $EvidenceRoot -LocalRegistry $RunRegistryRoot
	[void](Assert-LocalPackagePins -LocalRoot $Paths.Package -LocalRole $Role -RunManifest $Manifest)
	$Prefix = if ($Role -eq 'Server') { 'Server' } else { 'Player' }
	$RuntimePlan = Get-NativeRuntimePlan -LocalRoot $Paths.Package -DeploymentSha256 $Manifest["${Prefix}DeploymentSha256"] `
		-SourceCommit $Manifest.SourceCommit -LocalRole $Role
	$RuntimeRoot = Assert-PreparedRuntimeProjection -LocalRoot $Paths.Package -Registry $Paths.Registry `
		-LocalRunId $Manifest.RunId -LocalRole $Role -Plan $RuntimePlan
	$Executable = Join-Path $RuntimeRoot $RuntimePlan.Binary
	$AvailableMemoryBytes = [long](Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory * 1024L
	if ($AvailableMemoryBytes -lt 4294967296L) { throw 'role-local host lacks 4 GiB available memory' }
	$EffectiveAggregateWorkingSetBytes = [long][math]::Min(
		$MaximumAggregateWorkingSetBytes, [math]::Floor($AvailableMemoryBytes * 0.75))
	$RoleWorkingSetBytes = if ($Role -eq 'Server') {
		[long][math]::Min($MaximumServerWorkingSetBytes, $EffectiveAggregateWorkingSetBytes)
	} else {
		[long][math]::Min($MaximumClientWorkingSetBytes, $EffectiveAggregateWorkingSetBytes)
	}
	if ($Role -eq 'Server' -and $Manifest.Provider -eq 'Node') {
		if ([string]::IsNullOrWhiteSpace($NodeRootCertificatePath) -or
			-not (Test-Path -LiteralPath $NodeRootCertificatePath -PathType Leaf) -or
			(Get-FileHash -LiteralPath $NodeRootCertificatePath -Algorithm SHA256).Hash -ine
			$Manifest.NodeRootCertificateSha256 -or
			[string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($Manifest.NodeTokenEnvironment))) {
			throw 'Node root certificate pin or environment-backed token is missing'
		}
	}
	if ($Role -eq 'Server' -and (Get-NetUDPEndpoint -LocalPort $Network.Port -ErrorAction SilentlyContinue)) {
		throw "UDP port $($Network.Port) already has an owner"
	}
	[void][IO.Directory]::CreateDirectory($Paths.Registry)
	$ClaimPath = Join-Path $Paths.Registry "$RunId.$Role.claim"
	$Claim = [IO.File]::Open($ClaimPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
	try { $Claim.Write([Text.Encoding]::UTF8.GetBytes($ManifestSha256)) } finally { $Claim.Dispose() }
	[void][IO.Directory]::CreateDirectory($Paths.Evidence)
	$EvidenceCreated = $true
	[IO.File]::Copy($ManifestPath, (Join-Path $Paths.Evidence 'run-manifest.json'))
	[IO.File]::Copy(($RuntimeRoot + '.json'), (Join-Path $Paths.Evidence 'runtime-projection.json'))
	$Owners = $OwnedProcesses
	$SecretVariables = @([Environment]::GetEnvironmentVariables().Keys |
		Where-Object { [string]$_ -match '^GARGANTUAN_ENGINE_ADAPTER_.*TOKEN' })
	if ($Manifest.Provider -ceq 'Node') { $SecretVariables += [string]$Manifest.NodeTokenEnvironment }
	$CounterPreparation = Initialize-HostResourceCounters -PreparationRoot (Join-Path $Paths.Registry "$RunId.$Role.counter-preparation") `
		-EvidenceDirectory $Paths.Evidence
	$RunStartedUtc = [DateTimeOffset]::UtcNow.ToString('O')
	$RunStartedTicks = [Diagnostics.Stopwatch]::GetTimestamp()
	$RunClock = [Diagnostics.Stopwatch]::StartNew()
	$script:LastResourceSampleMilliseconds = -2000L
	$ResourceSampler = Start-EndpointResourceSampler -EndpointSource $PSCommandPath -OutputDirectory $Paths.Evidence `
		-LocalRunId $RunId -LocalRole $Role -LocalProvider $Manifest.Provider -Interface $HostResourceInterface `
		-StartedTicks $RunStartedTicks -TimeoutMilliseconds $RunTimeoutMilliseconds -CounterPreparation $CounterPreparation `
		-Registry $Paths.Registry -SecretVariables $SecretVariables
	if ($Role -eq 'Server') {
		$FairnessPath = Join-Path $Paths.Evidence 'admission-fairness.tsv'
		$PublicationPath = Join-Path $Paths.Evidence 'publication-service.bin'
		$Arguments = @('--bind', $Manifest.Endpoint, '--farm-run-id', $RunId,
			'--farm-peers', '32', '--max-ticks', [string]$Manifest.ServerTicks,
			'--reliable-mode', 'POOLED_SERVICE', '--content-provider', $Manifest.Provider.ToLowerInvariant())
		if ($Manifest.ScaleWorkload) { $Arguments += @('--farm-scale-workload',
			'--farm-admission-evidence', $FairnessPath,
			'--farm-publication-evidence', $PublicationPath, '--content-residency', 'on-demand') }
		if ($Manifest.RecoveryWorkload) { $Arguments += '--farm-recovery-workload' }
		if (-not [Net.IPAddress]::IsLoopback($Network.Address)) { $Arguments += '--allow-insecure-development-network' }
		if ($Manifest.Provider -eq 'Node') {
			$Arguments += @('--content-node-endpoint', $Manifest.NodeEndpoint,
				'--content-node-root-ca', [IO.Path]::GetFullPath($NodeRootCertificatePath),
				'--content-node-token-env', $Manifest.NodeTokenEnvironment)
		}
		$Owner = Start-EndpointProcess -Executable $Executable -WorkingDirectory $RuntimeRoot `
			-Arguments $Arguments -Label 'server' -OutputDirectory $Paths.Evidence
		$Owners.Add($Owner)
		Publish-EndpointSamplerOwner -Sampler $ResourceSampler -Owner $Owner
		$Clock = [Diagnostics.Stopwatch]::StartNew()
		while ($Clock.ElapsedMilliseconds -lt $StartupTimeoutMilliseconds -and
			$RunClock.ElapsedMilliseconds -lt $RunTimeoutMilliseconds) {
			Assert-EndpointBounds -Owners @($Owners) -MaximumLogBytes $MaximumLogBytesPerStream `
				-MaximumMemoryBytes $RoleWorkingSetBytes `
				-MaximumAggregateMemoryBytes $EffectiveAggregateWorkingSetBytes -MaximumThreads $MaximumThreadsPerProcess
			Add-TimedResourceSamples -Owners @($Owners)
			if ($Owner.Process.HasExited) { throw "server exited before startup: $($Owner.Process.ExitCode)" }
			$Owner.OutputStream.Flush()
			$Start = @(Get-TypedRecords -Path $Owner.OutputPath -Kind 'Server' |
				Where-Object { $_.event -eq 'start' -and $_.run -eq $RunId })
			if ($Start.Count -eq 1) { break }
			Start-Sleep -Milliseconds 100
		}
		if ($Clock.ElapsedMilliseconds -ge $StartupTimeoutMilliseconds) { throw 'server startup deadline elapsed' }
		[IO.File]::WriteAllText((Join-Path $Paths.Evidence 'server-ready.json'),
			([ordered]@{ RunId = $RunId; Role = $Role; Pid = $Owner.Pid;
				Endpoint = $Manifest.Endpoint; ReadyUtc = [DateTimeOffset]::UtcNow.ToString('O') } |
			ConvertTo-Json), [Text.UTF8Encoding]::new($false))
	} else {
		$Clients = [Collections.Generic.List[object]]::new()
		for ($Slot = 0; $Slot -lt 32; $Slot++) {
			$Arguments = @('--headless', '--connect', $Manifest.Endpoint, '--farm-run-id', $RunId,
				'--farm-slot', [string]$Slot, '--farm-client-nonce', [string]$Manifest.Nonces[$Slot],
				'--max-frames', [string]$Manifest.ClientFrames)
			if ($Manifest.ScaleWorkload) { $Arguments += @('--farm-scale-workload',
				'--farm-publication-evidence', (Join-Path $Paths.Evidence ('publication-service-{0}.bin' -f $Slot))) }
			if ($Manifest.RecoveryWorkload) { $Arguments += '--farm-recovery-workload' }
			if (-not [Net.IPAddress]::IsLoopback($Network.Address)) { $Arguments += '--allow-insecure-development-network' }
			$Owner = Start-EndpointProcess -Executable $Executable -WorkingDirectory $RuntimeRoot `
				-Arguments $Arguments -Label ('client-{0:D2}' -f $Slot) -OutputDirectory $Paths.Evidence `
				-RemoveEnvironmentVariables $SecretVariables
			$Owners.Add($Owner); $Clients.Add($Owner)
			Publish-EndpointSamplerOwner -Sampler $ResourceSampler -Owner $Owner
			Add-TimedResourceSamples -Owners @($Owners)
			if ($Slot -eq 0) {
				$Clock = [Diagnostics.Stopwatch]::StartNew()
				while ($Clock.ElapsedMilliseconds -lt $StartupTimeoutMilliseconds -and
					$RunClock.ElapsedMilliseconds -lt $RunTimeoutMilliseconds) {
					Assert-EndpointBounds -Owners @($Owners) -MaximumLogBytes $MaximumLogBytesPerStream `
						-MaximumMemoryBytes $RoleWorkingSetBytes `
						-MaximumAggregateMemoryBytes $EffectiveAggregateWorkingSetBytes -MaximumThreads $MaximumThreadsPerProcess
					Add-TimedResourceSamples -Owners @($Owners)
					if ($Owner.Process.HasExited) { throw 'producer client exited before readiness' }
					$Owner.OutputStream.Flush()
					$Ready = @(Get-TypedRecords -Path $Owner.OutputPath -Kind 'Client' | Where-Object {
						$_.event -eq 'ready' -and $_.run_id -eq $RunId -and $_.slot -eq '0' -and
						$_.nonce -eq $Manifest.Nonces[0] })
					if ($Ready.Count -eq 1) { break }
					Start-Sleep -Milliseconds 100
				}
				if ($Clock.ElapsedMilliseconds -ge $StartupTimeoutMilliseconds) { throw 'producer readiness deadline elapsed' }
			}
			if ($StartupStaggerMilliseconds -gt 0) { Start-Sleep -Milliseconds $StartupStaggerMilliseconds }
		}
	}
	while ($RunClock.ElapsedMilliseconds -lt $RunTimeoutMilliseconds) {
		Assert-EndpointBounds -Owners @($Owners) -MaximumLogBytes $MaximumLogBytesPerStream `
			-MaximumMemoryBytes $RoleWorkingSetBytes `
			-MaximumAggregateMemoryBytes $EffectiveAggregateWorkingSetBytes -MaximumThreads $MaximumThreadsPerProcess
		Add-TimedResourceSamples -Owners @($Owners)
		foreach ($Owner in $Owners) {
			if ($Owner.Process.HasExited -and $Owner.Process.ExitCode -ne 0) {
				throw "$($Owner.Label) exited $($Owner.Process.ExitCode)"
			}
		}
		if (@($Owners | Where-Object { -not $_.Process.HasExited }).Count -eq 0) { break }
		Start-Sleep -Milliseconds 100
	}
	if ($RunClock.ElapsedMilliseconds -ge $RunTimeoutMilliseconds) { throw 'role-local runtime deadline elapsed' }
	Stop-EndpointResourceSampler -Sampler $ResourceSampler -Processes $ResourceSamples -Hosts $HostResourceSamples
	foreach ($Owner in $Owners) {
		if (-not $Owner.OutputCopy.Wait(5000) -or -not $Owner.ErrorCopy.Wait(5000)) {
			throw "$($Owner.Label) redirected output did not drain"
		}
		$Owner.OutputStream.Flush(); $Owner.ErrorStream.Flush()
	}
	Assert-EndpointBounds -Owners @($Owners) -MaximumLogBytes $MaximumLogBytesPerStream `
		-MaximumMemoryBytes $RoleWorkingSetBytes `
		-MaximumAggregateMemoryBytes $EffectiveAggregateWorkingSetBytes -MaximumThreads $MaximumThreadsPerProcess
	$Verified = if ($Role -eq 'Server') {
		Assert-ServerEvidence -LogPath $Owners[0].OutputPath -RunManifest $Manifest -FairnessPath $FairnessPath
	} else {
		Assert-ClientEvidence -Clients @($Clients) -RunManifest $Manifest
	}
	if ($Role -eq 'Server' -and $Manifest.Provider -eq 'Node') {
		$AuthenticatedNode = Get-AuthenticatedNodeManifestReceipt -LogPath $Owners[0].OutputPath `
			-PackageRoot $Paths.Package -RunManifest $Manifest
		Write-AuthenticatedNodeManifestReceipt -Path (Join-Path $Paths.Evidence 'node-provider.json') `
			-Receipt $AuthenticatedNode
	}
	$Result = [ordered]@{
		RunId = $RunId; Role = $Role; Status = 'PASS'; Provider = $Manifest.Provider
		Endpoint = $Manifest.Endpoint; ScaleWorkload = $Manifest.ScaleWorkload
		ManifestSha256 = $ManifestSha256.ToLowerInvariant(); Verified = $Verified
		Pids = @($Owners | ForEach-Object Pid); ResourceSamples = $ResourceSamples.Count
		HostResourceSamples = $HostResourceSamples.Count
		AvailableMemoryBytes = $AvailableMemoryBytes
		WorkingSetLimitBytes = $RoleWorkingSetBytes
		AggregateWorkingSetLimitBytes = $EffectiveAggregateWorkingSetBytes
		StartedUtc = $RunStartedUtc; CompletedUtc = [DateTimeOffset]::UtcNow.ToString('O')
	}
} catch {
	$Failure = $_.Exception.Message
	$Result = [ordered]@{
		RunId = $RunId; Role = $Role; Status = 'FAIL'; Reason = $Failure
		CompletedUtc = [DateTimeOffset]::UtcNow.ToString('O')
	}
} finally {
	if ($null -ne $ResourceSampler -and -not $ResourceSampler.Stopped) {
		try { Stop-EndpointResourceSampler -Sampler $ResourceSampler -Processes $ResourceSamples -Hosts $HostResourceSamples }
		catch { $CleanupErrors.Add("resource sampler: $($_.Exception.Message)") }
	}
	foreach ($Owner in $OwnedProcesses) {
		try { Stop-EndpointProcess -Owner $Owner }
		catch { $CleanupErrors.Add("$($Owner.Label): $($_.Exception.Message)") }
	}
	if ($EvidenceCreated) {
		try {
			$ResourceSamples | Export-Csv -LiteralPath (Join-Path $Paths.Evidence 'process-resources.csv') -NoTypeInformation
		} catch { $CleanupErrors.Add("resource evidence: $($_.Exception.Message)") }
		try {
			$HostResourceSamples | Export-Csv -LiteralPath (Join-Path $Paths.Evidence 'host-resources.csv') -NoTypeInformation
			if ((Get-Item -LiteralPath (Join-Path $Paths.Evidence 'host-resources.csv')).Length -gt 16777216) {
				throw 'host resource evidence exceeds 16 MiB'
			}
		} catch { $CleanupErrors.Add("host resource evidence: $($_.Exception.Message)") }
		if ($Role -eq 'Server' -and $OwnedProcesses.Count -gt 0 -and
			(Get-NetUDPEndpoint -LocalPort $Network.Port -ErrorAction SilentlyContinue)) {
			$CleanupErrors.Add("UDP port $($Network.Port) remained occupied after owned PID cleanup")
		}
		if ($CleanupErrors.Count -gt 0) {
			$Result.Status = 'FAIL'
			$Result.CleanupErrors = @($CleanupErrors)
			if (-not $Failure) { $Failure = 'role-local cleanup failed' }
		}
		[IO.File]::WriteAllText((Join-Path $Paths.Evidence 'result.json'),
			($Result | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
		try { Write-EndpointEvidenceHash -Directory $Paths.Evidence -LocalRunId $RunId -LocalRole $Role }
		catch {
			$Failure = "evidence hashing failed: $($_.Exception.Message)"
			$Result.Status = 'FAIL'; $Result.EvidenceError = $Failure
			[IO.File]::WriteAllText((Join-Path $Paths.Evidence 'result.json'),
				($Result | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
		}
	}
}

if ($Failure) { throw "[Qualification:FarmEndpoint] $Failure; evidence=$EvidenceRoot" }
Write-Output "[Qualification:FarmEndpoint] ROLE_PASS run=$RunId role=$Role evidence=$EvidenceRoot"
