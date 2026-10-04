#requires -Version 7.0
# Read-only, role-local inventory for the 32-actual-client physical farm.
# This is an input/headroom preflight, not a provider or capture verdict.

param(
	[Parameter(Mandatory = $true)][ValidateSet('Server', 'Clients')][string]$Role,
	[Parameter(Mandatory = $true)][string]$ManifestPath,
	[Parameter(Mandatory = $true)][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$ManifestSha256,
	[Parameter(Mandatory = $true)][string]$PackageRoot,
	[Parameter(Mandatory = $true)][string]$EvidenceRoot,
	[Parameter(Mandatory = $true)][string]$RunRegistryRoot,
	[Parameter(Mandatory = $true)][string]$ReportPath,
	[string]$NodeRootCertificatePath,
	[string]$NodeTokenFilePath,
	[ValidatePattern('^[a-fA-F0-9]{64}$')][string]$NodeTokenFileSha256
)

$ErrorActionPreference = 'Stop'

function Import-EndpointValidators {
	$Source = Join-Path $PSScriptRoot 'PhysicalGameSessionFarmEndpoint.ps1'
	$Tokens = $null; $Errors = $null
	$Ast = [Management.Automation.Language.Parser]::ParseFile($Source, [ref]$Tokens, [ref]$Errors)
	if ($Errors.Count -ne 0) { throw 'role-local endpoint has a syntax error' }
	$Names = @('Assert-RunManifest', 'Read-PinnedRunManifest', 'Assert-RolePaths',
		'Assert-LocalPackagePins', 'Assert-DeploymentManifest', 'Assert-ProjectionPlainPath',
		'Read-ProjectionJson', 'Get-NativeRuntimePlan', 'Get-RuntimeProjectionPath', 'Assert-RuntimeProjection', 'Assert-PreparedRuntimeProjection')
	$Definitions = @($Ast.FindAll({ param($Node)
		$Node -is [Management.Automation.Language.FunctionDefinitionAst] -and
		$Node.Name -in $Names
	}, $true))
	if ($Definitions.Count -ne $Names.Count) { throw 'role-local endpoint validator set is incomplete' }
	return $Definitions
}

function Assert-PreflightInventory {
	param([System.Collections.IDictionary]$Inventory, [string]$LocalRole,
		[System.Collections.IDictionary]$RunManifest)
	$MinimumMemory = if ($LocalRole -eq 'Clients') { 8GB } else { 4GB }
	if ($Inventory.LogicalProcessors -lt 4 -or $Inventory.AvailableMemoryBytes -lt $MinimumMemory -or
		$Inventory.EvidenceDriveFreeBytes -lt 2GB -or $Inventory.PackageDriveFreeBytes -lt 1GB -or
		$Inventory.ProcessCount -lt 1 -or $Inventory.ThreadCount -lt 1) {
		throw 'role-local CPU, memory, disk, or process baseline lacks safe preflight headroom'
	}
	if ($Inventory.GameInterfaceStatus -cne 'Up' -or
		$Inventory.GameInterfaceLinkSpeed -cne '10 Gbps' -or
		$Inventory.GameInterfaceMtu -ne 1500 -or
		$Inventory.GameInterfaceAddress -cnotin @('10.253.3.1', '10.253.3.2') -or
		$Inventory.GameInterfaceIndex -le 0 -or
		($LocalRole -eq 'Server' -and $Inventory.GameInterfaceAddress -cne
			($RunManifest.Endpoint -split ':')[0]) -or
		($LocalRole -eq 'Clients' -and $Inventory.GameInterfaceAddress -cne '10.253.3.1') -or
		($LocalRole -eq 'Server' -and $Inventory.GameUdpPortOccupied)) {
		throw 'role-local direct fiber interface, route, MTU, or server UDP socket is not ready'
	}
	if ($RunManifest.Provider -eq 'Node' -and $LocalRole -eq 'Server' -and
		(-not $Inventory.NodeRootCertificateMatches -or -not $Inventory.NodeTokenPresent)) {
		throw 'real-TLS Node provider certificate pin or token is missing'
	}
}

function Get-ProcessBaseline {
	$Processes = @(Get-Process)
	$Threads = 0L
	foreach ($Process in $Processes) {
		try { $Threads += $Process.Threads.Count } catch { }
	}
	return [pscustomobject]@{ ProcessCount = $Processes.Count; ThreadCount = $Threads }
}

function Test-NodeTokenFile {
	param([string]$Path, [string]$Sha256, [string]$RunId)
	if ([string]::IsNullOrWhiteSpace($Path) -or [string]::IsNullOrWhiteSpace($Sha256)) { return $false }
	$Resolved = [IO.Path]::GetFullPath($Path)
	if ([IO.Path]::GetFileName($Resolved) -cne 'node-token.secret' -or
		[IO.Path]::GetFileName([IO.Path]::GetDirectoryName($Resolved)) -cne $RunId) { return $false }
	$Parent = [IO.Path]::GetDirectoryName($Resolved)
	$ParentItem = Get-Item -LiteralPath $Parent -ErrorAction Stop
	$File = Get-Item -LiteralPath $Resolved -ErrorAction Stop
	if (($ParentItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
		($File.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
		$File.PSIsContainer -or $File.Length -ne 64 -or
		(Get-FileHash -LiteralPath $Resolved -Algorithm SHA256).Hash -ine $Sha256) { return $false }
	$Acl = Get-Acl -LiteralPath $Parent
	$Sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
	$Allowed = @($Sid, 'S-1-5-18', 'S-1-5-32-544', 'S-1-3-4')
	$Observed = @($Acl.Access | ForEach-Object {
		$_.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value
	})
	if (-not $Acl.AreAccessRulesProtected -or
		$Acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -cne $Sid -or
		$Observed.Count -lt 1 -or $Sid -notin $Observed -or
		@($Observed | Where-Object { $_ -notin $Allowed }).Count -ne 0) { return $false }
	return [IO.File]::ReadAllText($Resolved, [Text.Encoding]::ASCII) -cmatch '^[a-f0-9]{64}$'
}

foreach ($Definition in @(Import-EndpointValidators)) {
	. ([scriptblock]::Create($Definition.Extent.Text))
}
$Manifest = Read-PinnedRunManifest -Path $ManifestPath -ExpectedSha256 $ManifestSha256
$Paths = Assert-RolePaths -LocalPackage $PackageRoot -LocalEvidence $EvidenceRoot `
	-LocalRegistry $RunRegistryRoot
[void](Assert-LocalPackagePins -LocalRoot $Paths.Package -LocalRole $Role -RunManifest $Manifest)
$Prefix = if ($Role -eq 'Server') { 'Server' } else { 'Player' }
$RuntimePlan = Get-NativeRuntimePlan -LocalRoot $Paths.Package -DeploymentSha256 $Manifest["${Prefix}DeploymentSha256"] `
	-SourceCommit $Manifest.SourceCommit -LocalRole $Role
$RuntimeRoot = Assert-PreparedRuntimeProjection -LocalRoot $Paths.Package -Registry $Paths.Registry `
	-LocalRunId $Manifest.RunId -LocalRole $Role -Plan $RuntimePlan
$ReportPath = [IO.Path]::GetFullPath($ReportPath)
if (Test-Path -LiteralPath $ReportPath) { throw 'stale preflight report path already exists' }
if ($ReportPath.StartsWith($Paths.Package + [IO.Path]::DirectorySeparatorChar,
	[StringComparison]::OrdinalIgnoreCase) -or
	$ReportPath.StartsWith($Paths.Evidence + [IO.Path]::DirectorySeparatorChar,
	[StringComparison]::OrdinalIgnoreCase) -or
	$ReportPath.StartsWith($Paths.Registry + [IO.Path]::DirectorySeparatorChar,
	[StringComparison]::OrdinalIgnoreCase)) {
	throw 'preflight report must be outside package, evidence, and registry roots'
}

$Address = ($Manifest.Endpoint -split ':')[0]
if ($Role -eq 'Server') {
	$GameIp = Get-NetIPAddress -IPAddress $Address -AddressFamily IPv4 -ErrorAction Stop |
		Select-Object -First 1
	$InterfaceIndex = [int]$GameIp.InterfaceIndex
} else {
	$Route = Find-NetRoute -RemoteIPAddress $Address -ErrorAction Stop | Select-Object -First 1
	$InterfaceIndex = [int]$Route.InterfaceIndex
	$GameIp = Get-NetIPAddress -InterfaceIndex $InterfaceIndex -AddressFamily IPv4 |
		Where-Object IPAddress -eq '10.253.3.1' | Select-Object -First 1
	if (-not $GameIp) { throw 'client route does not use the qualified fiber address' }
}
$Adapter = Get-NetAdapter -InterfaceIndex $InterfaceIndex -ErrorAction Stop
$IpInterface = Get-NetIPInterface -InterfaceIndex $InterfaceIndex -AddressFamily IPv4 -ErrorAction Stop
$Processors = @(Get-CimInstance Win32_Processor)
$OperatingSystem = Get-CimInstance Win32_OperatingSystem
$Baseline = Get-ProcessBaseline
$NodeCertificateMatches = $false
$NodeTokenPresent = $false
if ($Role -eq 'Server' -and $Manifest.Provider -eq 'Node') {
	$NodeCertificateMatches = -not [string]::IsNullOrWhiteSpace($NodeRootCertificatePath) -and
		(Test-Path -LiteralPath $NodeRootCertificatePath -PathType Leaf) -and
		(Get-FileHash -LiteralPath $NodeRootCertificatePath -Algorithm SHA256).Hash -ieq
		$Manifest.NodeRootCertificateSha256
	$NodeTokenPresent = Test-NodeTokenFile -Path $NodeTokenFilePath `
		-Sha256 $NodeTokenFileSha256 -RunId $Manifest.RunId
}
$Inventory = [ordered]@{
	Format = 'GargantuanPhysicalFarmPreflight'; Version = 1
	RunId = $Manifest.RunId; SourceCommit = $Manifest.SourceCommit
	Role = $Role; Provider = $Manifest.Provider; Endpoint = $Manifest.Endpoint
	ManifestSha256 = $ManifestSha256.ToLowerInvariant()
	DeploymentSha256 = $(if ($Role -eq 'Server') { $Manifest.ServerDeploymentSha256 }
		else { $Manifest.PlayerDeploymentSha256 })
	RuntimeProjectionRoot = $RuntimeRoot
	RuntimeProjectionReceiptSha256 = (Get-FileHash -LiteralPath ($RuntimeRoot + '.json') -Algorithm SHA256).Hash.ToLowerInvariant()
	LogicalProcessors = [long](($Processors | Measure-Object NumberOfLogicalProcessors -Sum).Sum)
	PhysicalCores = [long](($Processors | Measure-Object NumberOfCores -Sum).Sum)
	AvailableMemoryBytes = [long]$OperatingSystem.FreePhysicalMemory * 1024L
	ProcessCount = $Baseline.ProcessCount; ThreadCount = $Baseline.ThreadCount
	GameInterfaceIndex = $InterfaceIndex
	GameInterfaceStatus = [string]$Adapter.Status
	GameInterfaceLinkSpeed = [string]$Adapter.LinkSpeed
	GameInterfaceMtu = [int]$IpInterface.NlMtu
	GameInterfaceAddress = [string]$GameIp.IPAddress
	GameUdpPortOccupied = [bool]($Role -eq 'Server' -and
		(Get-NetUDPEndpoint -LocalPort ([int](($Manifest.Endpoint -split ':')[1])) -ErrorAction SilentlyContinue))
	PackageDriveFreeBytes = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($Paths.Package)).AvailableFreeSpace
	EvidenceDriveFreeBytes = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($Paths.Evidence)).AvailableFreeSpace
	NodeRootCertificateMatches = $NodeCertificateMatches
	NodeTokenPresent = $NodeTokenPresent
	ObservedUtc = [DateTimeOffset]::UtcNow.ToString('O')
	Status = 'INVENTORY_ONLY'
}
Assert-PreflightInventory -Inventory $Inventory -LocalRole $Role -RunManifest $Manifest
$Bytes = [Text.UTF8Encoding]::new($false).GetBytes(($Inventory | ConvertTo-Json -Depth 5))
$Stream = [IO.File]::Open($ReportPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $Stream.Write($Bytes) } finally { $Stream.Dispose() }
Write-Output "[Qualification:FarmPreflight] INVENTORY_READY run=$($Manifest.RunId) role=$Role report=$ReportPath"
