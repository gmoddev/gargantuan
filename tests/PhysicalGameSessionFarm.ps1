#requires -Version 7.0
# Bounded 1/4/32 actual-client GameSession/GNS farm preflight. This script
# qualifies client identity, simultaneous readiness, typed results and cleanup;
# it does not by itself qualify the Foundation 3L Local or Node workload matrix.
# Example:
#   pwsh -File tests/PhysicalGameSessionFarm.ps1 -ServerPackageRoot C:\run\server `
#     -PlayerPackageRoot C:\run\player -EvidenceRoot C:\run\evidence `
#     -Endpoint 127.0.0.1:39450 -Peers 32
# Add -ScaleWorkload -ClientFrames 9000 for the canonical five-phase matrix.

param(
	[Parameter(Mandatory = $true)][string]$ServerPackageRoot,
	[Parameter(Mandatory = $true)][string]$PlayerPackageRoot,
	[Parameter(Mandatory = $true)][string]$EvidenceRoot,
	[ValidatePattern('^[A-Za-z0-9-]{1,64}$')][string]$RunId = ('farm-' + [Guid]::NewGuid().ToString('N')),
	[ValidatePattern('^[0-9.]+:[0-9]+$')][string]$Endpoint = '127.0.0.1:39450',
	[ValidateSet(1, 4, 32)][int]$Peers = 1,
	[switch]$ScaleWorkload,
	[ValidateRange(60, 36000)][int]$ClientFrames = 1800,
	[ValidateRange(0, 250)][int]$StartupStaggerMilliseconds = 75,
	[ValidateRange(10000, 180000)][int]$StartupTimeoutMilliseconds = 60000,
	[ValidateRange(30000, 900000)][int]$RunTimeoutMilliseconds = 240000,
	[ValidateRange(1048576, 16777216)][int]$MaximumLogBytesPerStream = 4194304,
	[ValidateSet('Local', 'Node')][string]$Provider = 'Local',
	[string]$NodeEndpoint,
	[string]$NodeRootCertificate,
	[ValidatePattern('^[A-Za-z_][A-Za-z0-9_]*$')][string]$NodeTokenEnvironment = 'GARGANTUAN_ENGINE_ADAPTER_TOKEN'
)

$ErrorActionPreference = 'Stop'
$ServerPackageRoot = [IO.Path]::GetFullPath($ServerPackageRoot)
$PlayerPackageRoot = [IO.Path]::GetFullPath($PlayerPackageRoot)
$EvidenceRoot = [IO.Path]::GetFullPath($EvidenceRoot)
$ServerExecutable = Join-Path $ServerPackageRoot 'GargantuanServer.exe'
$PlayerExecutable = Join-Path $PlayerPackageRoot 'GargantuanPlayer.exe'
$RunDirectory = Join-Path $EvidenceRoot $RunId
$AllProcesses = [System.Collections.Generic.List[object]]::new()
$ResourceSamples = [System.Collections.Generic.List[object]]::new()
$ResourceClock = [Diagnostics.Stopwatch]::StartNew()
$LastResourceSampleMilliseconds = -2000L
$CleanupErrors = [System.Collections.Generic.List[string]]::new()
$Failure = $null
$StartedUtc = [DateTimeOffset]::UtcNow
$Result = $null

function Get-Fields {
	param([Parameter(Mandatory = $true)][string]$Line)
	$Fields = @{}
	foreach ($Match in [regex]::Matches($Line, '(?:^|\s)([a-z][a-z0-9_]*)=([^\s]+)')) {
		$Fields[$Match.Groups[1].Value] = $Match.Groups[2].Value
	}
	return $Fields
}

function Get-Records {
	param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string]$Kind)
	if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return @() }
	$Prefix = "[Qualification:$Kind] "
	return @([IO.File]::ReadAllLines($Path) | Where-Object { $_.StartsWith($Prefix, [StringComparison]::Ordinal) } |
		ForEach-Object { Get-Fields -Line $_ })
}

function Start-LoggedProcess {
	param(
		[Parameter(Mandatory = $true)][string]$Executable,
		[Parameter(Mandatory = $true)][string]$WorkingDirectory,
		[Parameter(Mandatory = $true)][string[]]$Arguments,
		[Parameter(Mandatory = $true)][string]$Label,
		[string[]]$RemoveEnvironmentVariables = @()
	)
	$StartInfo = [Diagnostics.ProcessStartInfo]::new()
	$StartInfo.FileName = $Executable
	$StartInfo.WorkingDirectory = $WorkingDirectory
	$StartInfo.UseShellExecute = $false
	$StartInfo.CreateNoWindow = $true
	$StartInfo.RedirectStandardOutput = $true
	$StartInfo.RedirectStandardError = $true
	foreach ($Argument in $Arguments) { [void]$StartInfo.ArgumentList.Add($Argument) }
	foreach ($Variable in $RemoveEnvironmentVariables) { [void]$StartInfo.Environment.Remove($Variable) }
	$OutputPath = Join-Path $RunDirectory "$Label.stdout.log"
	$ErrorPath = Join-Path $RunDirectory "$Label.stderr.log"
	$OutputStream = [IO.File]::Open($OutputPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
	$ErrorStream = [IO.File]::Open($ErrorPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
	$Process = [Diagnostics.Process]::new()
	$Started = $false
	try {
		$Process.StartInfo = $StartInfo
		if (-not $Process.Start()) { throw "[$Label] process did not start" }
		$Started = $true
		$Owner = [pscustomobject]@{
			Label = $Label; Process = $Process; Pid = $Process.Id
			OutputPath = $OutputPath; ErrorPath = $ErrorPath
			OutputStream = $OutputStream; ErrorStream = $ErrorStream
			OutputCopy = $Process.StandardOutput.BaseStream.CopyToAsync($OutputStream)
			ErrorCopy = $Process.StandardError.BaseStream.CopyToAsync($ErrorStream)
		}
		$AllProcesses.Add($Owner)
		return $Owner
	} catch {
		if ($Started -and -not $Process.HasExited) { try { $Process.Kill($true) } catch {} }
		$OutputStream.Dispose()
		$ErrorStream.Dispose()
		$Process.Dispose()
		throw
	}
}

function Stop-RunProcess {
	param([Parameter(Mandatory = $true)]$Owner)
	try {
		if (-not $Owner.Process.HasExited) { $Owner.Process.Kill($true) }
		if (-not $Owner.Process.WaitForExit(5000)) { throw "process $($Owner.Pid) remained live" }
		if (-not $Owner.OutputCopy.Wait(5000) -or -not $Owner.ErrorCopy.Wait(5000)) {
			throw 'redirected output did not drain'
		}
		if ($Owner.OutputCopy.IsFaulted -or $Owner.ErrorCopy.IsFaulted) { throw 'redirected output failed' }
	} catch {
		$CleanupErrors.Add("$($Owner.Label): $($_.Exception.Message)")
	} finally {
		$Owner.OutputStream.Dispose()
		$Owner.ErrorStream.Dispose()
		$Owner.Process.Dispose()
	}
}

function Assert-LogBounds {
	foreach ($Owner in $AllProcesses) {
		foreach ($Path in @($Owner.OutputPath, $Owner.ErrorPath)) {
			if (([IO.FileInfo]$Path).Length -gt $MaximumLogBytesPerStream) {
				throw "[$($Owner.Label)] output exceeded $MaximumLogBytesPerStream bytes: $Path"
			}
		}
	}
}

function Sample-RunResources {
	if ($ResourceClock.ElapsedMilliseconds - $LastResourceSampleMilliseconds -lt 2000) { return }
	if ($ResourceSamples.Count + $AllProcesses.Count -gt 20000) {
		throw 'bounded process resource evidence exceeded 20000 records'
	}
	$LastResourceSampleMilliseconds = $ResourceClock.ElapsedMilliseconds
	$SampledUtc = [DateTimeOffset]::UtcNow.ToString('O')
	foreach ($Owner in $AllProcesses) {
		try {
			$Owner.Process.Refresh()
			if ($Owner.Process.HasExited) { continue }
			$ResourceSamples.Add([pscustomobject]@{
				RunId = $RunId; Label = $Owner.Label; Pid = $Owner.Pid
				SampledUtc = $SampledUtc; ElapsedMilliseconds = $ResourceClock.ElapsedMilliseconds
				WorkingSetBytes = $Owner.Process.WorkingSet64
				PrivateBytes = $Owner.Process.PrivateMemorySize64
				CpuMilliseconds = [math]::Round($Owner.Process.TotalProcessorTime.TotalMilliseconds, 3)
				Threads = $Owner.Process.Threads.Count; Handles = $Owner.Process.HandleCount
			})
		} catch {
			if ($Owner.Process.HasExited) { continue }
			throw "[$($Owner.Label)] resource sample failed: $($_.Exception.Message)"
		}
	}
}

function Write-EvidenceManifest {
	param([Parameter(Mandatory = $true)][string]$Directory)
	$Entries = @(
		Get-ChildItem -LiteralPath $Directory -File | Where-Object {
			$_.Name -notin @('evidence-sha256.json', 'evidence-sha256.json.tmp')
		} | Sort-Object Name | ForEach-Object {
			[ordered]@{ Name = $_.Name; Bytes = $_.Length;
				Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
		}
	)
	if ($Entries.Count -eq 0) { throw 'farm evidence directory contains no files' }
	$Temporary = Join-Path $Directory 'evidence-sha256.json.tmp'
	$Destination = Join-Path $Directory 'evidence-sha256.json'
	try {
		[IO.File]::WriteAllText($Temporary, ([ordered]@{ RunId = $RunId; Files = $Entries } |
			ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
		Move-Item -LiteralPath $Temporary -Destination $Destination -Force
	} finally {
		if (Test-Path -LiteralPath $Temporary) { Remove-Item -LiteralPath $Temporary -Force }
	}
	return $Entries.Count
}

function Assert-Records {
	param([Parameter(Mandatory = $true)]$Clients, [Parameter(Mandatory = $true)]$Server,
		[Parameter(Mandatory = $true)]$ExpectedNonces)
	$ServerRecords = Get-Records -Path $Server.OutputPath -Kind 'Server'
	$ServerStarts = @($ServerRecords | Where-Object { $_.event -eq 'start' })
	$ServerReady = @($ServerRecords | Where-Object { $_.event -eq 'ready' })
	$ServerResults = @($ServerRecords | Where-Object { $_.event -eq 'result' })
	if ($ServerStarts.Count -ne 1 -or $ServerResults.Count -ne 1 -or $ServerReady.Count -ne $Peers) {
		throw 'server typed start/ready/result cardinality is invalid'
	}
	$ServerResult = $ServerResults[0]
	if ($ServerStarts[0].run -ne $RunId -or $ServerResult.run -ne $RunId -or
		$ServerResult.provider -ne $Provider.ToLowerInvariant() -or
		$ServerResult.expected -ne [string]$Peers -or $ServerResult.ready_high_water -ne [string]$Peers -or
		$ServerResult.unique_ready -ne [string]$Peers -or $ServerResult.identity_conflict -ne '0' -or
		$ServerResult.exit -ne '0') {
		throw 'server typed result failed the simultaneous readiness or identity gate'
	}
	$Connections = [System.Collections.Generic.HashSet[string]]::new()
	$Players = [System.Collections.Generic.HashSet[string]]::new()
	$ServerNonces = [System.Collections.Generic.HashSet[string]]::new()
	foreach ($Record in $ServerReady) {
		if ($Record.run -ne $RunId -or -not $ExpectedNonces.Contains($Record.nonce) -or
			-not $ServerNonces.Add($Record.nonce) -or
			-not $Connections.Add("$($Record.connection_slot):$($Record.connection_generation)") -or
			-not $Players.Add($Record.player_id) -or
			[UInt64]$Record.connection_slot -eq 0 -or [UInt64]$Record.connection_generation -eq 0 -or
			[UInt64]$Record.session_epoch -eq 0 -or [UInt64]$Record.player_id -eq 0) {
			throw 'server typed readiness has a missing, duplicate, or invalid client identity'
		}
	}
	foreach ($Slot in 0..($Peers - 1)) {
		$Records = Get-Records -Path $Clients[$Slot].OutputPath -Kind 'Client'
		$Starts = @($Records | Where-Object { $_.event -eq 'start' })
		$Ready = @($Records | Where-Object { $_.event -eq 'ready' })
		$Results = @($Records | Where-Object { $_.event -eq 'result' })
		if ($Starts.Count -ne 1 -or $Ready.Count -ne 1 -or $Results.Count -ne 1) {
			throw "client $Slot typed start/ready/result cardinality is invalid"
		}
		foreach ($Record in @($Starts[0], $Ready[0], $Results[0])) {
			if ($Record.run_id -ne $RunId -or $Record.slot -ne [string]$Slot -or
				$Record.nonce -ne $ExpectedNonces[$Slot]) {
				throw "client $Slot typed identity differs from its assignment"
			}
		}
		# GNS connection IDs are endpoint-local. Correlate both sides by the
		# run-scoped nonce, while checking each endpoint's identity separately.
		$ExpectedReason = if ($ScaleWorkload) { 'scale_complete' } else { 'completed' }
		$CharacterRequired = -not $ScaleWorkload -or ($Slot % 4 -eq 0)
		if ($Results[0].status -ne 'PASS' -or $Results[0].exit_code -ne '0' -or
			$Results[0].reason -ne $ExpectedReason -or $Results[0].local_player -ne '1' -or
			($CharacterRequired -and $Results[0].character -ne '1') -or
			-not $ServerNonces.Contains($ExpectedNonces[$Slot]) -or
			[UInt64]$Ready[0].connection_slot -eq 0 -or
			[UInt64]$Ready[0].connection_generation -eq 0) {
			throw "client $Slot typed result or GNS identity is invalid"
		}
	}
	return [pscustomobject]@{ Ready = $Peers; UniqueNonces = $ServerNonces.Count;
		UniqueConnections = $Connections.Count; UniquePlayers = $Players.Count }
}

function Assert-ScaleRecords {
	param([Parameter(Mandatory = $true)]$Server, [Parameter(Mandatory = $true)]$Clients,
		[Parameter(Mandatory = $true)]$ExpectedNonces)
	$Records = Get-Records -Path $Server.OutputPath -Kind 'Scale'
	$Results = @($Records | Where-Object { $_.event -eq 'result' })
	if ($Results.Count -ne 1 -or $Results[0].run -ne $RunId -or $Results[0].status -ne 'PASS' -or
		$Results[0].phases -ne '5' -or $Results[0].peers -ne '32') {
		throw 'scale controller did not report one complete five-phase PASS'
	}
	$Names = @('baseline', 'load', 'resident', 'evict', 'reload')
	$PreviousEndTick = 0L
	foreach ($Name in $Names) {
		$Starts = @($Records | Where-Object { $_.event -eq 'phase_start' -and $_.phase -eq $Name -and $_.run -eq $RunId })
		$Ends = @($Records | Where-Object { $_.event -eq 'phase_end' -and $_.phase -eq $Name -and $_.run -eq $RunId })
		$Acks = @($Records | Where-Object { $_.event -eq 'phase_acks' -and $_.phase -eq $Name -and $_.run -eq $RunId })
		$Stops = @($Records | Where-Object { $_.event -eq 'phase_stop_requested' -and $_.phase -eq $Name -and $_.run -eq $RunId })
		$Producer = @($Records | Where-Object { $_.event -eq 'producer_ack' -and $_.phase -eq $Name -and $_.run -eq $RunId })
		if ($Starts.Count -ne 1 -or $Ends.Count -ne 1 -or $Acks.Count -ne 1 -or $Stops.Count -ne 1 -or
			$Producer.Count -ne 1 -or $Acks[0].count -ne '32' -or $Ends[0].phase_acks -ne '32' -or
			$Ends[0].producer_done -ne '1') {
			throw "scale phase $Name lacks complete typed acknowledgement or convergence evidence"
		}
		if (-not $Starts[0].ContainsKey('monotonic_us') -or
			-not $Stops[0].ContainsKey('elapsed_us') -or
			[long]$Stops[0].elapsed_us -le 13000000 -or
			-not $Ends[0].ContainsKey('monotonic_us') -or
			-not $Ends[0].ContainsKey('elapsed_us') -or
			[long]$Ends[0].elapsed_us -le 13000000 -or
			[long]$Ends[0].monotonic_us - [long]$Starts[0].monotonic_us -le 13000000) {
			throw "scale phase $Name did not retain the required >13-second boundary"
		}
		$StartTick = [long]$Starts[0].tick
		$EndTick = [long]$Ends[0].tick
		if ($StartTick -le $PreviousEndTick -or $EndTick -le $StartTick -or
			[long]$Ends[0].ticks -ne $EndTick - $StartTick -or
			[long]$Acks[0].tick -lt $StartTick -or [long]$Acks[0].tick -gt [long]$Stops[0].tick -or
			[long]$Stops[0].tick -lt [long]$Acks[0].tick -or [long]$Stops[0].tick -gt [long]$Producer[0].tick -or
			[long]$Producer[0].tick -gt $EndTick) {
			throw "scale phase $Name has an invalid authoritative tick sequence"
		}
		$PreviousEndTick = $EndTick
	}
	if ([long]$Results[0].tick -le $PreviousEndTick) {
		throw 'scale controller result precedes final phase completion'
	}
	foreach ($Slot in 0..31) {
		$ClientRecords = Get-Records -Path $Clients[$Slot].OutputPath -Kind 'Client'
		$Observed = @($ClientRecords | Where-Object { $_.event -eq 'phase_observed' })
		$Completed = @($ClientRecords | Where-Object { $_.event -eq 'scale_complete' })
		if ($Observed.Count -ne 5 -or $Completed.Count -ne 1 -or
			$Completed[0].observed_phases -ne '5' -or
			($Slot -eq 0 -and $Completed[0].producer_phases -ne '5')) {
			throw "client $Slot lacks complete five-phase content observation"
		}
		$FirstRoot = $null
		foreach ($Index in 0..4) {
			$Record = $Observed[$Index]
			$Name = $Names[$Index]
			$ExpectedObjects = if ($Name -in @('load', 'resident', 'reload')) { '512' } else { '0' }
			$RootIdentity = "$($Record.root_slot):$($Record.root_generation)"
			if ($Record.run_id -ne $RunId -or $Record.slot -ne [string]$Slot -or
				$Record.nonce -ne $ExpectedNonces[$Slot] -or $Record.phase -ne $Name -or
				$Record.objects -ne $ExpectedObjects -or
				-not $Record.ContainsKey('receive_to_observed_us') -or
				[long]$Record.receive_to_observed_us -lt 0) {
				throw "client $Slot has invalid $Name content observation"
			}
			if ($ExpectedObjects -eq '0' -and $RootIdentity -ne '0:0') {
				throw "client $Slot retained the content root in $Name"
			}
			if ($ExpectedObjects -eq '512' -and ($Record.root_slot -eq '0' -or $Record.root_generation -eq '0')) {
				throw "client $Slot has no resident root identity in $Name"
			}
			if ($Name -eq 'load') { $FirstRoot = $RootIdentity }
			if ($Name -eq 'resident' -and $RootIdentity -ne $FirstRoot) {
				throw "client $Slot lost the resident root identity"
			}
			if ($Name -eq 'reload' -and $RootIdentity -eq $FirstRoot) {
				throw "client $Slot did not observe a fresh reload root"
			}
		}
	}
	$ProducerRecords = @(Get-Records -Path $Clients[0].OutputPath -Kind 'Producer' |
		Where-Object { $_.event -eq 'phase_metrics' })
	if ($ProducerRecords.Count -ne 5) { throw 'single producer lacks five gameplay metric receipts' }
	foreach ($Index in 0..4) {
		$Record = $ProducerRecords[$Index]
		$RequiredMetrics = @('remote_samples', 'remote_p95_us', 'remote_p99_us',
			'remote_max_us', 'remote_errors', 'remote_timeouts', 'event_offers',
			'event_acks', 'event_outstanding', 'event_max_rtt_us', 'event_max_gap_us',
			'action_requests', 'action_resolutions', 'action_endings', 'action_max_result_us',
			'submission_failures', 'action_rejections', 'unexpected_endings')
		foreach ($Metric in $RequiredMetrics) {
			if (-not $Record.ContainsKey($Metric)) { throw "producer $($Names[$Index]) lacks $Metric" }
		}
		if ($Record.run_id -ne $RunId -or $Record.slot -ne '0' -or
			$Record.nonce -ne $ExpectedNonces[0] -or $Record.phase -ne $Names[$Index] -or
			$Record.status -ne 'PASS' -or $Record.metrics_phase_valid -ne '1' -or
			$Record.producer_healthy -ne '1' -or $Record.remote_samples -ne '100' -or
			[double]$Record.remote_p95_us -gt 150000 -or [double]$Record.remote_p99_us -gt 250000 -or
			[double]$Record.remote_max_us -gt 500000 -or [double]$Record.remote_errors -ne 0 -or
			[double]$Record.remote_timeouts -ne 0 -or
			[double]$Record.event_offers -le 0 -or
			[double]$Record.event_offers -ne [double]$Record.event_acks -or
			[double]$Record.event_outstanding -ne 0 -or [double]$Record.event_max_rtt_us -gt 250000 -or
			[double]$Record.action_requests -le 0 -or
			[double]$Record.action_requests -ne [double]$Record.action_resolutions -or
			[double]$Record.action_requests -ne [double]$Record.action_endings -or
			[double]$Record.action_max_result_us -gt 250000 -or
			[double]$Record.submission_failures -ne 0 -or [double]$Record.action_rejections -ne 0 -or
			[double]$Record.unexpected_endings -ne 0) {
			throw "producer gameplay metrics failed in $($Names[$Index])"
		}
	}
}

try {
	if ($ScaleWorkload -and -not $PSBoundParameters.ContainsKey('RunTimeoutMilliseconds')) {
		$RunTimeoutMilliseconds = 300000
	}
	if ($ScaleWorkload -and ($Peers -ne 32 -or $ClientFrames -lt 9000)) {
		throw 'ScaleWorkload requires 32 clients and at least 9000 client frames'
	}
	if (-not (Test-Path -LiteralPath $ServerExecutable -PathType Leaf) -or
		-not (Test-Path -LiteralPath $PlayerExecutable -PathType Leaf)) {
		throw 'packaged GargantuanServer.exe or GargantuanPlayer.exe is missing'
	}
	if ($Provider -eq 'Node') {
		if ([string]::IsNullOrWhiteSpace($NodeEndpoint) -or
			-not (Test-Path -LiteralPath $NodeRootCertificate -PathType Leaf) -or
			[string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($NodeTokenEnvironment))) {
			throw 'Node farm preflight requires endpoint, root certificate, and an environment-backed token'
		}
	}
	$EndpointParts = $Endpoint.Split(':')
	$BindAddress = $null
	$Port = 0
	if (-not [Net.IPAddress]::TryParse($EndpointParts[0], [ref]$BindAddress) -or
		$BindAddress.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork -or
		-not [int]::TryParse($EndpointParts[1], [ref]$Port) -or $Port -lt 1 -or $Port -gt 65535) {
		throw 'farm endpoint must be an IPv4 address and valid UDP port'
	}
	if (Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue) {
		throw "UDP port $Port is already occupied"
	}
	if (Test-Path -LiteralPath $RunDirectory) { throw "run evidence directory already exists: $RunDirectory" }
	[void][IO.Directory]::CreateDirectory($EvidenceRoot)
	[void][IO.Directory]::CreateDirectory($RunDirectory)
	$NonceBytes = [Security.Cryptography.RandomNumberGenerator]::GetBytes(8)
	$NoncePrefix = [UInt64](([BitConverter]::ToUInt64($NonceBytes, 0) -shr 32) -shl 32)
	if ($NoncePrefix -eq 0) { $NoncePrefix = [UInt64]0x0100000000000000 }
	$OperatingSystem = Get-CimInstance Win32_OperatingSystem
	$Processors = @(Get-CimInstance Win32_Processor)
	$EvidenceDrive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($EvidenceRoot))
	$BoundAddress = Get-NetIPAddress -IPAddress $BindAddress.IPAddressToString -AddressFamily IPv4 -ErrorAction SilentlyContinue |
		Select-Object -First 1
	$BoundAdapter = if ($BoundAddress) { Get-NetAdapter -InterfaceIndex $BoundAddress.InterfaceIndex -ErrorAction SilentlyContinue }
	$BoundInterface = if ($BoundAddress) {
		Get-NetIPInterface -InterfaceIndex $BoundAddress.InterfaceIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue
	}
	$InterfaceBaseline = if ($BoundAdapter) { Get-NetAdapterStatistics -Name $BoundAdapter.Name }
	$ExpectedNonces = [System.Collections.Generic.List[string]]::new()
	foreach ($Slot in 0..($Peers - 1)) {
		$ExpectedNonces.Add([string]($NoncePrefix -bor [UInt64]($Slot + 1)))
	}
	if (@($ExpectedNonces | Select-Object -Unique).Count -ne $Peers) { throw 'run-scoped client nonces are not unique' }
	$ServerTicks = $ClientFrames + [int][Math]::Ceiling($Peers * $StartupStaggerMilliseconds / 16.667) + 600
	$Manifest = [ordered]@{
		RunId = $RunId; Purpose = $(if ($ScaleWorkload) { 'five-phase-scale-control-preflight' } else { 'actual-GameSession-farm-preflight' }); Provider = $Provider
		Endpoint = $Endpoint; Peers = $Peers; ClientFrames = $ClientFrames; ServerTicks = $ServerTicks
		StartedUtc = $StartedUtc.ToString('O'); Nonces = @($ExpectedNonces)
		ServerExecutable = $ServerExecutable; PlayerExecutable = $PlayerExecutable
		ServerSha256 = (Get-FileHash -LiteralPath $ServerExecutable -Algorithm SHA256).Hash
		PlayerSha256 = (Get-FileHash -LiteralPath $PlayerExecutable -Algorithm SHA256).Hash
		HostPreflight = [ordered]@{
			Machine = [Environment]::MachineName
			PhysicalCores = ($Processors | Measure-Object -Property NumberOfCores -Sum).Sum
			LogicalProcessors = ($Processors | Measure-Object -Property NumberOfLogicalProcessors -Sum).Sum
			TotalMemoryBytes = [long]$OperatingSystem.TotalVisibleMemorySize * 1024
			AvailableMemoryBytes = [long]$OperatingSystem.FreePhysicalMemory * 1024
			EvidenceFreeBytes = $EvidenceDrive.AvailableFreeSpace
			Interface = if ($BoundAdapter) { $BoundAdapter.Name } else { 'loopback-or-unresolved' }
			LinkSpeed = if ($BoundAdapter) { [string]$BoundAdapter.LinkSpeed } else { 'not-applicable' }
			InterfaceMtu = if ($BoundInterface) { $BoundInterface.NlMtu } else { 0 }
			InterfaceStart = if ($InterfaceBaseline) {
				[ordered]@{
					ReceivedBytes = $InterfaceBaseline.ReceivedBytes
					SentBytes = $InterfaceBaseline.SentBytes
					ReceivedDiscardedPackets = $InterfaceBaseline.ReceivedDiscardedPackets
					ReceivedPacketErrors = $InterfaceBaseline.ReceivedPacketErrors
					OutboundDiscardedPackets = $InterfaceBaseline.OutboundDiscardedPackets
					OutboundPacketErrors = $InterfaceBaseline.OutboundPacketErrors
				}
			} else { $null }
		}
	}
	foreach ($Package in @(
		@('ServerPackage', (Join-Path $ServerPackageRoot 'game.package.json')),
		@('PlayerPackage', (Join-Path $PlayerPackageRoot 'game.package.json'))
	)) {
		if (-not (Test-Path -LiteralPath $Package[1] -PathType Leaf)) {
			throw "$($Package[0]) descriptor is missing"
		}
		$Manifest["$($Package[0])Sha256"] = (Get-FileHash -LiteralPath $Package[1] -Algorithm SHA256).Hash
	}
	$ContentManifest = Join-Path $ServerPackageRoot 'content/content.manifest.json'
	if (Test-Path -LiteralPath $ContentManifest -PathType Leaf) {
		$Manifest.ContentManifestSha256 = (Get-FileHash -LiteralPath $ContentManifest -Algorithm SHA256).Hash
	}
	if ($Provider -eq 'Node') {
		$Manifest.NodeEndpoint = $NodeEndpoint
		$Manifest.NodeRootCertificateSha256 = (Get-FileHash -LiteralPath $NodeRootCertificate -Algorithm SHA256).Hash
		$Manifest.NodeTokenEnvironment = $NodeTokenEnvironment
	}
	[IO.File]::WriteAllText((Join-Path $RunDirectory 'manifest.json'),
		($Manifest | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
	$ServerArguments = @('--bind', $Endpoint, '--farm-run-id', $RunId, '--farm-peers', [string]$Peers,
		'--max-ticks', [string]$ServerTicks, '--reliable-mode', 'POOLED_SERVICE',
		'--content-provider', $Provider.ToLowerInvariant())
	if ($ScaleWorkload) { $ServerArguments += @('--farm-scale-workload', '--content-residency', 'on-demand') }
	if (-not [Net.IPAddress]::IsLoopback($BindAddress)) {
		$ServerArguments += '--allow-insecure-development-network'
	}
	if ($Provider -eq 'Node') {
		$ServerArguments += @('--content-node-endpoint', $NodeEndpoint,
			'--content-node-root-ca', $NodeRootCertificate, '--content-node-token-env', $NodeTokenEnvironment)
		if (-not $ScaleWorkload) { $ServerArguments += @('--content-residency', 'on-demand') }
	}
	$Server = Start-LoggedProcess -Executable $ServerExecutable -WorkingDirectory $ServerPackageRoot `
		-Arguments $ServerArguments -Label 'server'
	$Clock = [Diagnostics.Stopwatch]::StartNew()
	while ($Clock.ElapsedMilliseconds -lt $StartupTimeoutMilliseconds) {
		Assert-LogBounds
		if ($Server.Process.HasExited) { throw "server exited before farm start, exit $($Server.Process.ExitCode)" }
		$Server.OutputStream.Flush()
		if ((Get-Records -Path $Server.OutputPath -Kind 'Server' | Where-Object { $_.event -eq 'start' -and $_.run -eq $RunId }).Count -eq 1) { break }
		Start-Sleep -Milliseconds 100
	}
	if ($Clock.ElapsedMilliseconds -ge $StartupTimeoutMilliseconds) { throw 'server farm startup timed out' }
	$Clients = [System.Collections.Generic.List[object]]::new()
	$SecretEnvironmentNames = @([Environment]::GetEnvironmentVariables().Keys |
		Where-Object { [string]$_ -match '^GARGANTUAN_ENGINE_ADAPTER_.*TOKEN' }) + @($NodeTokenEnvironment)
	foreach ($Slot in 0..($Peers - 1)) {
		$Arguments = @('--headless', '--connect', $Endpoint, '--farm-run-id', $RunId,
			'--farm-slot', [string]$Slot, '--farm-client-nonce', $ExpectedNonces[$Slot],
			'--max-frames', [string]$ClientFrames)
		if ($ScaleWorkload) { $Arguments += '--farm-scale-workload' }
		if (-not [Net.IPAddress]::IsLoopback($BindAddress)) { $Arguments += '--allow-insecure-development-network' }
		$Clients.Add((Start-LoggedProcess -Executable $PlayerExecutable -WorkingDirectory $PlayerPackageRoot `
			-Arguments $Arguments -Label ('client-{0:D2}' -f $Slot) -RemoveEnvironmentVariables $SecretEnvironmentNames))
		if ($Slot -eq 0 -and $Peers -gt 1) {
			$FirstClientClock = [Diagnostics.Stopwatch]::StartNew()
			while ($FirstClientClock.ElapsedMilliseconds -lt $StartupTimeoutMilliseconds) {
				Assert-LogBounds
				if ($Server.Process.HasExited -or $Clients[0].Process.HasExited) {
					throw 'server or producer client exited before producer readiness'
				}
				$Clients[0].OutputStream.Flush()
				$FirstReady = @(Get-Records -Path $Clients[0].OutputPath -Kind 'Client' |
					Where-Object { $_.event -eq 'ready' -and $_.run_id -eq $RunId -and
						$_.slot -eq '0' -and $_.nonce -eq $ExpectedNonces[0] })
				if ($FirstReady.Count -eq 1) { break }
				Start-Sleep -Milliseconds 100
			}
			if ($FirstClientClock.ElapsedMilliseconds -ge $StartupTimeoutMilliseconds) {
				throw 'producer client readiness timed out'
			}
		}
		if ($StartupStaggerMilliseconds -gt 0) { Start-Sleep -Milliseconds $StartupStaggerMilliseconds }
	}
	$Clock.Restart()
	while ($Clock.ElapsedMilliseconds -lt $RunTimeoutMilliseconds) {
		Assert-LogBounds
		Sample-RunResources
		foreach ($Client in $Clients) {
			if ($Client.Process.HasExited -and $Client.Process.ExitCode -ne 0) {
				throw "$($Client.Label) exited $($Client.Process.ExitCode)"
			}
		}
		if ($Server.Process.HasExited -and $Server.Process.ExitCode -ne 0) {
			throw "server exited $($Server.Process.ExitCode)"
		}
		if ($Server.Process.HasExited -and @($Clients | Where-Object { -not $_.Process.HasExited }).Count -eq 0) { break }
		Start-Sleep -Milliseconds 100
	}
	if ($Clock.ElapsedMilliseconds -ge $RunTimeoutMilliseconds) { throw 'farm runtime deadline elapsed' }
	Sample-RunResources
	foreach ($Owner in $AllProcesses) {
		if (-not @($ResourceSamples | Where-Object { $_.Label -eq $Owner.Label }).Count) {
			throw "[$($Owner.Label)] has no process resource sample"
		}
	}
	foreach ($Owner in $AllProcesses) {
		if (-not $Owner.OutputCopy.Wait(5000) -or -not $Owner.ErrorCopy.Wait(5000)) {
			throw "$($Owner.Label) redirected output did not drain"
		}
		$Owner.OutputStream.Flush()
		$Owner.ErrorStream.Flush()
	}
	Assert-LogBounds
	$Identity = Assert-Records -Clients $Clients -Server $Server -ExpectedNonces $ExpectedNonces
	if ($ScaleWorkload) { Assert-ScaleRecords -Server $Server -Clients $Clients -ExpectedNonces $ExpectedNonces }
	$Result = [ordered]@{
		RunId = $RunId; Status = 'PASS'; Gate = $(if ($ScaleWorkload) { 'five-phase-scale-control-preflight' } else { 'actual-GameSession-farm-preflight' })
		Provider = $Provider; Peers = $Peers; Ready = $Identity.Ready
		UniqueNonces = $Identity.UniqueNonces; UniqueConnections = $Identity.UniqueConnections
		UniquePlayers = $Identity.UniquePlayers; ServerPid = $Server.Pid
		ClientPids = @($Clients | ForEach-Object Pid)
		ResourceSamples = $ResourceSamples.Count; ResourceEvidence = 'process-resources.csv'
		CompletedUtc = [DateTimeOffset]::UtcNow.ToString('O')
	}
} catch {
	$Failure = $_.Exception.Message
	$Result = [ordered]@{
		RunId = $RunId; Status = 'FAIL'; Gate = $(if ($ScaleWorkload) { 'five-phase-scale-control-preflight' } else { 'actual-GameSession-farm-preflight' })
		Provider = $Provider; Peers = $Peers; Reason = $Failure
		CompletedUtc = [DateTimeOffset]::UtcNow.ToString('O')
	}
} finally {
	if (Test-Path -LiteralPath $RunDirectory -PathType Container) {
		try {
			$ResourceSamples | Export-Csv -LiteralPath (Join-Path $RunDirectory 'process-resources.csv') -NoTypeInformation
		} catch {
			$CleanupErrors.Add("resource evidence write failed: $($_.Exception.Message)")
		}
	}
	foreach ($Owner in $AllProcesses) { Stop-RunProcess -Owner $Owner }
	if ($InterfaceBaseline) {
		try {
			$InterfaceEnd = Get-NetAdapterStatistics -Name $BoundAdapter.Name
			$Result.InterfaceDelta = [ordered]@{
				Name = $BoundAdapter.Name
				ReceivedBytes = [long]$InterfaceEnd.ReceivedBytes - [long]$InterfaceBaseline.ReceivedBytes
				SentBytes = [long]$InterfaceEnd.SentBytes - [long]$InterfaceBaseline.SentBytes
				ReceivedDiscardedPackets = [long]$InterfaceEnd.ReceivedDiscardedPackets - [long]$InterfaceBaseline.ReceivedDiscardedPackets
				ReceivedPacketErrors = [long]$InterfaceEnd.ReceivedPacketErrors - [long]$InterfaceBaseline.ReceivedPacketErrors
				OutboundDiscardedPackets = [long]$InterfaceEnd.OutboundDiscardedPackets - [long]$InterfaceBaseline.OutboundDiscardedPackets
				OutboundPacketErrors = [long]$InterfaceEnd.OutboundPacketErrors - [long]$InterfaceBaseline.OutboundPacketErrors
			}
		} catch {
			$CleanupErrors.Add("interface counter sample failed: $($_.Exception.Message)")
		}
	}
	if ($AllProcesses.Count -gt 0 -and $Port -gt 0) {
		if (Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue) {
			$CleanupErrors.Add("UDP port $Port remained occupied after run-owned process cleanup")
		}
	}
	if ($CleanupErrors.Count -gt 0) {
		$Result.Status = 'FAIL'
		$Result.CleanupErrors = @($CleanupErrors)
		if (-not $Failure) { $Failure = 'farm cleanup failed' }
	}
	if (Test-Path -LiteralPath $RunDirectory -PathType Container) {
		[IO.File]::WriteAllText((Join-Path $RunDirectory 'result.json'),
			($Result | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
		try {
			[void](Write-EvidenceManifest -Directory $RunDirectory)
		} catch {
			$Result.Status = 'FAIL'
			$Result.EvidenceError = $_.Exception.Message
			if (-not $Failure) { $Failure = 'farm evidence manifest failed' }
			[IO.File]::WriteAllText((Join-Path $RunDirectory 'result.json'),
				($Result | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
		}
	}
}

if ($Failure) { throw "[Qualification:Farm] $Failure; evidence: $RunDirectory" }
Write-Output "[Qualification:Farm] FARM_PREFLIGHT_OK run=$RunId peers=$Peers provider=$Provider evidence=$RunDirectory"
