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
$RunId = 'unvalidated'

function Get-TypedFields {
	param([Parameter(Mandatory = $true)][string]$Line)
	$Fields = @{}
	foreach ($Match in [regex]::Matches($Line, '(?:^|\s)([a-z][a-z0-9_]*)=([^\s]+)')) {
		$Fields[$Match.Groups[1].Value] = $Match.Groups[2].Value
	}
	return $Fields
}

function Get-TypedRecords {
	param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string]$Kind)
	if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return @() }
	$Prefix = "[Qualification:$Kind] "
	return @([IO.File]::ReadAllLines($Path) | Where-Object {
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
		[string]$Label, [string]$OutputDirectory, [string[]]$RemoveEnvironmentVariables = @())
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
			OutputCopy = $Process.StandardOutput.BaseStream.CopyToAsync($Output)
			ErrorCopy = $Process.StandardError.BaseStream.CopyToAsync($ErrorStream)
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
	if ('FarmHostNativeCounters' -as [type]) { return }
	$Source = @'
using System;
using System.Runtime.InteropServices;
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
	Add-Type -TypeDefinition $Source -ErrorAction Stop
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
	$Elapsed = $RunClock.ElapsedMilliseconds
	if ($Elapsed - $script:LastResourceSampleMilliseconds -lt 2000) { return }
	Add-EndpointResourceSamples -Owners $Owners -LocalRunId $RunId `
		-Samples $ResourceSamples -SupervisorElapsedMilliseconds $Elapsed
	Add-HostResourceSample -Owners $Owners -LocalRunId $RunId -LocalRole $Role `
		-LocalProvider $Manifest.Provider -Interface $HostResourceInterface `
		-Samples $HostResourceSamples -SupervisorElapsedMilliseconds $Elapsed
	$script:LastResourceSampleMilliseconds = $Elapsed
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
		if ($Reader.ReadLine() -cne "format=GargantuanAdmissionEvidenceV1`trun=$LocalRunId") {
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
					'grant_accepted', 'reservation_rolled_back', 'demand_disposed') -or
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
				($Fields[1] -cin @('grant_accepted', 'reservation_rolled_back') -and $Numbers[4] -eq 0)) {
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
	Initialize-HostResourceCounters
	$HostResourceInterface = Get-HostResourceInterface -ServerAddress $Network.Address -LocalRole $Role
	$Paths = Assert-RolePaths -LocalPackage $PackageRoot -LocalEvidence $EvidenceRoot -LocalRegistry $RunRegistryRoot
	$Executable = Assert-LocalPackagePins -LocalRoot $Paths.Package -LocalRole $Role -RunManifest $Manifest
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
	$Owners = $OwnedProcesses
	$SecretVariables = @([Environment]::GetEnvironmentVariables().Keys |
		Where-Object { [string]$_ -match '^GARGANTUAN_ENGINE_ADAPTER_.*TOKEN' })
	$RunStartedUtc = [DateTimeOffset]::UtcNow.ToString('O')
	$RunClock = [Diagnostics.Stopwatch]::StartNew()
	$script:LastResourceSampleMilliseconds = -2000L
	if ($Role -eq 'Server') {
		$FairnessPath = Join-Path $Paths.Evidence 'admission-fairness.tsv'
		$Arguments = @('--bind', $Manifest.Endpoint, '--farm-run-id', $RunId,
			'--farm-peers', '32', '--max-ticks', [string]$Manifest.ServerTicks,
			'--reliable-mode', 'POOLED_SERVICE', '--content-provider', $Manifest.Provider.ToLowerInvariant())
		if ($Manifest.ScaleWorkload) { $Arguments += @('--farm-scale-workload',
			'--farm-admission-evidence', $FairnessPath, '--content-residency', 'on-demand') }
		if ($Manifest.RecoveryWorkload) { $Arguments += '--farm-recovery-workload' }
		if (-not [Net.IPAddress]::IsLoopback($Network.Address)) { $Arguments += '--allow-insecure-development-network' }
		if ($Manifest.Provider -eq 'Node') {
			$Arguments += @('--content-node-endpoint', $Manifest.NodeEndpoint,
				'--content-node-root-ca', [IO.Path]::GetFullPath($NodeRootCertificatePath),
				'--content-node-token-env', $Manifest.NodeTokenEnvironment)
		}
		$Owner = Start-EndpointProcess -Executable $Executable -WorkingDirectory $Paths.Package `
			-Arguments $Arguments -Label 'server' -OutputDirectory $Paths.Evidence
		$Owners.Add($Owner)
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
			if ($Manifest.ScaleWorkload) { $Arguments += '--farm-scale-workload' }
			if ($Manifest.RecoveryWorkload) { $Arguments += '--farm-recovery-workload' }
			if (-not [Net.IPAddress]::IsLoopback($Network.Address)) { $Arguments += '--allow-insecure-development-network' }
			$Owner = Start-EndpointProcess -Executable $Executable -WorkingDirectory $Paths.Package `
				-Arguments $Arguments -Label ('client-{0:D2}' -f $Slot) -OutputDirectory $Paths.Evidence `
				-RemoveEnvironmentVariables $SecretVariables
			$Owners.Add($Owner); $Clients.Add($Owner)
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
