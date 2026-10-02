#requires -Version 7.0
# Offline cross-provider observation over two already-reconciled Farm32 runs.
# This cannot promote either provider or Foundation 3L to PASS.

param(
	[Parameter(Mandatory = $true)][string]$LocalReportPath,
	[Parameter(Mandatory = $true)][string]$LocalServerEvidenceRoot,
	[Parameter(Mandatory = $true)][string]$LocalClientEvidenceRoot,
	[Parameter(Mandatory = $true)][string]$NodeReportPath,
	[Parameter(Mandatory = $true)][string]$NodeServerEvidenceRoot,
	[Parameter(Mandatory = $true)][string]$NodeClientEvidenceRoot,
	[Parameter(Mandatory = $true)][string]$OutputPath
)

$ErrorActionPreference = 'Stop'

function Read-BoundedJson {
	param([string]$Path, [long]$MaximumBytes = 1048576)
	$Resolved = [IO.Path]::GetFullPath($Path)
	$Item = Get-Item -LiteralPath $Resolved -ErrorAction Stop
	if ($Item.PSIsContainer -or $Item.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint) -or
		$Item.Length -gt $MaximumBytes) { throw "missing, redirected, or oversized JSON: $Path" }
	return Get-Content -LiteralPath $Resolved -Raw | ConvertFrom-Json -AsHashtable
}

function Assert-IndexedFile {
	param([string]$Root, [System.Collections.IDictionary]$Index, [string]$Name)
	$Entries = @($Index.Files | Where-Object Name -CEQ $Name)
	if ($Entries.Count -ne 1) { throw "immutable evidence index lacks one $Name" }
	$Entry = $Entries[0]
	$Path = Join-Path $Root $Name
	$Item = Get-Item -LiteralPath $Path -ErrorAction Stop
	if ($Item.PSIsContainer -or $Item.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint) -or
		$Item.Length -ne [long]$Entry.Bytes -or $Item.Length -gt 16777216 -or
		[string]$Entry.Sha256 -cnotmatch '^[a-fA-F0-9]{64}$' -or
		(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $Entry.Sha256) {
		throw "immutable evidence $Name does not match its index"
	}
	return $Path
}

function Read-RoleEvidence {
	param([string]$Root, [string]$Role, [System.Collections.IDictionary]$Report)
	$Resolved = [IO.Path]::GetFullPath($Root)
	$Directory = Get-Item -LiteralPath $Resolved -ErrorAction Stop
	if (-not $Directory.PSIsContainer -or
		$Directory.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) {
		throw "$Role evidence root is missing or redirected"
	}
	$IndexPath = Join-Path $Resolved 'evidence-sha256.json'
	$Index = Read-BoundedJson -Path $IndexPath
	$ExpectedHash = if ($Role -ceq 'Server') { $Report.ServerEvidenceSha256 } else { $Report.ClientEvidenceSha256 }
	if ($Index.RunId -cne $Report.RunId -or $Index.Role -cne $Role -or
		$Index.Files -isnot [array] -or $Index.Files.Count -lt 2 -or $Index.Files.Count -gt 80 -or
		[string]$ExpectedHash -cnotmatch '^[a-fA-F0-9]{64}$' -or
		(Get-FileHash -LiteralPath $IndexPath -Algorithm SHA256).Hash -ine $ExpectedHash) {
		throw "$Role evidence index does not match the reconciliation report"
	}
	$ManifestPath = Assert-IndexedFile -Root $Resolved -Index $Index -Name 'run-manifest.json'
	if ((Get-FileHash -LiteralPath $ManifestPath -Algorithm SHA256).Hash -ine $Report.ManifestSha256) {
		throw "$Role run manifest does not match the reconciliation report"
	}
	$ResourcePath = Assert-IndexedFile -Root $Resolved -Index $Index -Name 'process-resources.csv'
	$ResultPath = Assert-IndexedFile -Root $Resolved -Index $Index -Name 'result.json'
	$Result = Read-BoundedJson -Path $ResultPath
	if ($Result.RunId -cne $Report.RunId -or $Result.Role -cne $Role -or
		$Result.Status -cne 'PASS') { throw "$Role sealed role result is invalid" }
	$HostResourcePath = Assert-IndexedFile -Root $Resolved -Index $Index -Name 'host-resources.csv'
	$IndexedBytes = [long]0
	foreach ($Entry in $Index.Files) {
		if ([string]$Entry.Name -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$' -or
			[long]$Entry.Bytes -lt 0 -or [long]$Entry.Bytes -gt 33554432) {
			throw "$Role evidence index has an invalid retained-file bound"
		}
		$IndexedBytes += [long]$Entry.Bytes
	}
	return [pscustomobject]@{
		Root = $Resolved; Manifest = (Read-BoundedJson -Path $ManifestPath); Result = $Result
		ResourcePath = $ResourcePath; HostResourcePath = $HostResourcePath; Index = $Index
		IndexedFiles = $Index.Files.Count; IndexedBytes = $IndexedBytes
	}
}

function Read-ResourceObservation {
	param([string]$Path, [string]$RunId, [string[]]$ExpectedLabels, [long]$ExpectedCount,
		[long]$AggregateWorkingSetLimitBytes)
	if ($AggregateWorkingSetLimitBytes -le 0 -or $AggregateWorkingSetLimitBytes -gt 34359738368L) {
		throw 'role-local aggregate working-set limit is invalid'
	}
	$Rows = @(Import-Csv -LiteralPath $Path)
	if ($Rows.Count -ne $ExpectedCount -or $Rows.Count -gt 20000 -or
		$Rows.Count -lt $ExpectedLabels.Count) { throw 'resource row count does not match the role receipt' }
	$Groups = @($Rows | Group-Object Label)
	if ($Groups.Count -ne $ExpectedLabels.Count) { throw 'resource process-label set is incomplete' }
	$Processes = [Collections.Generic.List[object]]::new()
	$TotalPeakWorkingSet = [long]0
	$TotalPeakPrivate = [long]0
	foreach ($Label in $ExpectedLabels) {
		$Samples = @($Rows | Where-Object Label -CEQ $Label)
		if ($Samples.Count -lt 1) { throw "resource samples lack $Label process; labels=$(@($Rows | ForEach-Object Label | Select-Object -Unique) -join ',')" }
		$Prior = $null
		$PeakWorkingSet = [long]0
		$PeakPrivate = [long]0
		$PeakThreads = [long]0
		$PeakHandles = [long]0
		foreach ($Row in $Samples) {
			$Unsigned = @('Pid', 'SupervisorElapsedMilliseconds', 'MonotonicTicks',
				'MonotonicFrequency', 'WorkingSetBytes', 'PrivateBytes', 'Threads', 'Handles')
			$Parsed = @{}
			foreach ($Name in $Unsigned) {
				$Value = [long]0
				if ([string]$Row.$Name -cnotmatch '^(0|[1-9][0-9]*)$' -or
					-not [long]::TryParse([string]$Row.$Name, [ref]$Value)) {
					throw "invalid $Label resource $Name"
				}
				$Parsed[$Name] = $Value
			}
			$Cpu = [double]0
			if (-not [double]::TryParse([string]$Row.CpuMilliseconds,
				[Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture,
				[ref]$Cpu) -or [double]::IsNaN($Cpu) -or [double]::IsInfinity($Cpu) -or $Cpu -lt 0) {
				throw "invalid $Label CPU sample"
			}
			$Utc = [DateTimeOffset]::MinValue
			if ($Row.RunId -cne $RunId -or $Parsed.Pid -le 0 -or
				$Parsed.MonotonicTicks -le 0 -or $Parsed.MonotonicFrequency -le 0 -or
				$Parsed.WorkingSetBytes -le 0 -or $Parsed.Threads -le 0 -or
				-not [DateTimeOffset]::TryParse($Row.Utc, [ref]$Utc)) {
				throw "invalid $Label resource identity or timestamp"
			}
			if ($null -ne $Prior -and ($Parsed.Pid -ne $Prior.Pid -or
				$Parsed.MonotonicFrequency -ne $Prior.Frequency -or
				$Parsed.MonotonicTicks -le $Prior.Ticks -or
				$Parsed.SupervisorElapsedMilliseconds -lt $Prior.Elapsed -or $Cpu -lt $Prior.Cpu)) {
				throw "nonmonotonic $Label resource history"
			}
			$PeakWorkingSet = [Math]::Max($PeakWorkingSet, $Parsed.WorkingSetBytes)
			$PeakPrivate = [Math]::Max($PeakPrivate, $Parsed.PrivateBytes)
			$PeakThreads = [Math]::Max($PeakThreads, $Parsed.Threads)
			$PeakHandles = [Math]::Max($PeakHandles, $Parsed.Handles)
			if ($null -eq $Prior) { $FirstCpu = $Cpu; $FirstElapsed = $Parsed.SupervisorElapsedMilliseconds }
			$Prior = [pscustomobject]@{
				Pid = $Parsed.Pid; Frequency = $Parsed.MonotonicFrequency
				Ticks = $Parsed.MonotonicTicks; Elapsed = $Parsed.SupervisorElapsedMilliseconds
				Cpu = $Cpu
			}
		}
		$Processes.Add([ordered]@{
			Label = $Label; Pid = $Prior.Pid; Samples = $Samples.Count
			PeakWorkingSetBytes = $PeakWorkingSet; PeakPrivateBytes = $PeakPrivate
			PeakThreads = $PeakThreads; PeakHandles = $PeakHandles
			ObservedCpuMilliseconds = [Math]::Round($Prior.Cpu - $FirstCpu, 3)
			ObservedElapsedMilliseconds = $Prior.Elapsed - $FirstElapsed
		})
		$TotalPeakWorkingSet += $PeakWorkingSet
		$TotalPeakPrivate += $PeakPrivate
	}
	# All owners in one supervisor sweep share the same elapsed-millisecond
	# stamp. Reconstruct only complete sweeps; readings within a sweep are
	# sequential, so this is not a synchronized host-memory high-water.
	$CompleteSweeps = 0L
	$PartialSweeps = 0L
	$MaximumSweepWorkingSet = [decimal]0
	$MaximumSweepPrivate = [decimal]0
	$MaximumSweepSkewMicroseconds = [decimal]0
	foreach ($Batch in @($Rows | Group-Object SupervisorElapsedMilliseconds)) {
		$BatchLabels = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
		$SweepWorkingSet = [decimal]0
		$SweepPrivate = [decimal]0
		$FirstTick = [long]::MaxValue
		$LastTick = 0L
		$Frequency = 0L
		foreach ($Row in $Batch.Group) {
			if (-not $BatchLabels.Add($Row.Label)) { throw 'duplicate process in one resource sweep' }
			$SweepWorkingSet += [decimal][long]$Row.WorkingSetBytes
			$SweepPrivate += [decimal][long]$Row.PrivateBytes
			$Tick = [long]$Row.MonotonicTicks
			$RowFrequency = [long]$Row.MonotonicFrequency
			if ($Frequency -ne 0 -and $Frequency -ne $RowFrequency) {
				throw 'resource sweep mixes monotonic clock frequencies'
			}
			$Frequency = $RowFrequency
			$FirstTick = [Math]::Min($FirstTick, $Tick)
			$LastTick = [Math]::Max($LastTick, $Tick)
		}
		if ($BatchLabels.Count -eq $ExpectedLabels.Count) {
			$CompleteSweeps++
			if ($SweepWorkingSet -gt $AggregateWorkingSetLimitBytes) {
				throw 'complete resource sweep exceeds the role-local aggregate limit'
			}
			$MaximumSweepWorkingSet = [Math]::Max($MaximumSweepWorkingSet, $SweepWorkingSet)
			$MaximumSweepPrivate = [Math]::Max($MaximumSweepPrivate, $SweepPrivate)
			$Skew = [decimal]($LastTick - $FirstTick) * 1000000 / $Frequency
			$MaximumSweepSkewMicroseconds = [Math]::Max($MaximumSweepSkewMicroseconds, $Skew)
		} else { $PartialSweeps++ }
	}
	return [ordered]@{
		Classification = 'ROLE_LOCAL_PROCESS_SAMPLES_ONLY'
		SampleCount = $Rows.Count; ProcessCount = $Processes.Count
		CompleteSweepCount = $CompleteSweeps; PartialSweepCount = $PartialSweeps
		CompleteSweepMaximumWorkingSetBytes = [long]$MaximumSweepWorkingSet
		CompleteSweepMaximumPrivateBytes = [long]$MaximumSweepPrivate
		MaximumSweepSkewMicroseconds = [math]::Round($MaximumSweepSkewMicroseconds, 3)
		AggregateWorkingSetLimitBytes = $AggregateWorkingSetLimitBytes
		CompleteSweepLimitObservation = $(if ($CompleteSweeps -gt 0) { 'WITHIN_ROLE_LIMIT' } else { 'NOT_MEASURED' })
		SumOfPerProcessPeakWorkingSetBytes = $TotalPeakWorkingSet
		SumOfPerProcessPeakPrivateBytes = $TotalPeakPrivate
		Processes = @($Processes)
	}
}

function Read-HostResourceObservation {
	param([string]$Path, [string]$RunId, [string]$Role, [string]$Provider,
		[int]$ExpectedLiveProcesses, [long]$ExpectedCount)
	$Rows = @(Import-Csv -LiteralPath $Path)
	if ($Rows.Count -lt 2 -or $Rows.Count -gt 1000 -or $Rows.Count -ne $ExpectedCount) {
		throw "$Role host resource sample count is invalid"
	}
	$Unsigned = @('InterfaceIndex', 'SupervisorElapsedMilliseconds', 'SampleStartTicks',
		'SampleEndTicks', 'MonotonicFrequency', 'LiveOwnedProcessCount', 'OwnedWorkingSetBytes',
		'OwnedPrivateBytes', 'HostTotalPhysicalBytes', 'HostAvailablePhysicalBytes',
		'HostCpuIdle100ns', 'HostCpuKernel100ns', 'HostCpuUser100ns', 'NicSentBytes',
		'NicReceivedBytes', 'NicOutboundDiscardedPackets', 'NicOutboundPacketErrors',
		'NicReceivedDiscardedPackets', 'NicReceivedPacketErrors')
	$Identity = $null; $Previous = $null; $Intervals = [Collections.Generic.List[object]]::new()
	$FullRole = 0; $MinAvailable = [ulong]::MaxValue; $MaxOwnedWorkingSet = [ulong]0
	$MaxOwnedPrivate = [ulong]0; $MaxSkew = [double]0; $MaxCpu = [double]0
	$MaxSendRate = [double]0; $MaxReceiveRate = [double]0
	foreach ($Row in $Rows) {
		$Parsed = @{}
		foreach ($Name in $Unsigned) {
			$Value = [ulong]0
			if ([string]$Row.$Name -cnotmatch '^(0|[1-9][0-9]*)$' -or
				-not [ulong]::TryParse([string]$Row.$Name, [ref]$Value)) {
				throw "invalid $Role host resource $Name"
			}
			$Parsed[$Name] = $Value
		}
		$Utc = [DateTimeOffset]::MinValue
		if (-not [DateTimeOffset]::TryParse($Row.Utc, [ref]$Utc) -or
			$Row.RunId -cne $RunId -or $Row.Role -cne $Role -or
			$Row.Provider -cne $Provider -or $Row.HostName -cnotmatch '^[A-Za-z0-9][A-Za-z0-9_-]{0,62}$' -or
			$Row.InterfaceMacAddress -cnotmatch '^(?:[0-9A-F]{2}-){5}[0-9A-F]{2}$' -or
			$Row.InterfaceAddress -cnotin @('10.253.3.1', '10.253.3.2') -or
			$Row.InterfaceLinkSpeed -cne '10 Gbps' -or $Parsed.InterfaceIndex -eq 0 -or
			$Parsed.MonotonicFrequency -eq 0 -or $Parsed.SampleStartTicks -eq 0 -or
			$Parsed.SampleEndTicks -lt $Parsed.SampleStartTicks -or
			$Parsed.LiveOwnedProcessCount -gt $ExpectedLiveProcesses -or
			($Parsed.LiveOwnedProcessCount -eq 0 -and
				($Parsed.OwnedWorkingSetBytes -ne 0 -or $Parsed.OwnedPrivateBytes -ne 0)) -or
			($Parsed.LiveOwnedProcessCount -gt 0 -and $Parsed.OwnedWorkingSetBytes -eq 0) -or
			$Parsed.HostTotalPhysicalBytes -eq 0 -or
			$Parsed.HostAvailablePhysicalBytes -gt $Parsed.HostTotalPhysicalBytes -or
			$Parsed.HostCpuIdle100ns -gt $Parsed.HostCpuKernel100ns) {
			throw "$Role host resource identity, counter, or physical range is invalid"
		}
		$SnapshotSkewMs = [double](1000.0 *
			([decimal]$Parsed.SampleEndTicks - [decimal]$Parsed.SampleStartTicks) /
			[decimal]$Parsed.MonotonicFrequency)
		if ($SnapshotSkewMs -gt 5000) { throw "$Role host resource snapshot spans over five seconds" }
		$ThisIdentity = "$($Row.HostName)|$($Parsed.InterfaceIndex)|$($Row.InterfaceMacAddress)|$($Row.InterfaceAddress)"
		if ($null -eq $Identity) { $Identity = $ThisIdentity }
		elseif ($ThisIdentity -cne $Identity) { throw "$Role host/interface identity changed" }
		if ($null -ne $Previous) {
			if ($Parsed.MonotonicFrequency -ne $Previous.Frequency -or
				$Parsed.SampleStartTicks -le $Previous.EndTicks -or
				$Parsed.SupervisorElapsedMilliseconds -le $Previous.Elapsed -or
				$Utc -lt $Previous.Utc) {
				throw "$Role host sample order or clock changed"
			}
			foreach ($Counter in @('HostCpuIdle100ns', 'HostCpuKernel100ns', 'HostCpuUser100ns',
				'NicSentBytes', 'NicReceivedBytes', 'NicOutboundDiscardedPackets',
				'NicOutboundPacketErrors', 'NicReceivedDiscardedPackets', 'NicReceivedPacketErrors')) {
				if ($Parsed[$Counter] -lt $Previous.Counters[$Counter]) {
					throw "$Role host $Counter counter reset or wrapped"
				}
			}
			$ElapsedSeconds = [double](
				([decimal]$Parsed.SampleStartTicks - [decimal]$Previous.StartTicks) /
				[decimal]$Parsed.MonotonicFrequency)
			$TotalCpu = ([decimal]$Parsed.HostCpuKernel100ns - [decimal]$Previous.Counters.HostCpuKernel100ns) +
				([decimal]$Parsed.HostCpuUser100ns - [decimal]$Previous.Counters.HostCpuUser100ns)
			$IdleCpu = [decimal]$Parsed.HostCpuIdle100ns - [decimal]$Previous.Counters.HostCpuIdle100ns
			if ($ElapsedSeconds -le 0 -or $TotalCpu -le 0 -or $IdleCpu -gt $TotalCpu) {
				throw "$Role host CPU interval is invalid"
			}
			$CpuPercent = [double](100 * ($TotalCpu - $IdleCpu) / $TotalCpu)
			$SendBytes = [decimal]$Parsed.NicSentBytes - [decimal]$Previous.Counters.NicSentBytes
			$ReceiveBytes = [decimal]$Parsed.NicReceivedBytes - [decimal]$Previous.Counters.NicReceivedBytes
			$SendBps = [double]($SendBytes / [decimal]$ElapsedSeconds)
			$ReceiveBps = [double]($ReceiveBytes / [decimal]$ElapsedSeconds)
			if ($SendBps -gt 1250000000 -or $ReceiveBps -gt 1250000000) {
				throw "$Role NIC counters exceed the pinned 10 Gbps link"
			}
			$MaxCpu = [math]::Max($MaxCpu, $CpuPercent)
			$MaxSendRate = [math]::Max($MaxSendRate, $SendBps)
			$MaxReceiveRate = [math]::Max($MaxReceiveRate, $ReceiveBps)
			$Intervals.Add([ordered]@{ ElapsedSeconds = [math]::Round($ElapsedSeconds, 3)
				HostCpuPercent = [math]::Round($CpuPercent, 3)
				NicSentBytesPerSecond = [math]::Round($SendBps, 3)
				NicReceivedBytesPerSecond = [math]::Round($ReceiveBps, 3) })
		}
		if ($Parsed.LiveOwnedProcessCount -eq $ExpectedLiveProcesses) { $FullRole++ }
		$MinAvailable = [math]::Min($MinAvailable, $Parsed.HostAvailablePhysicalBytes)
		$MaxOwnedWorkingSet = [math]::Max($MaxOwnedWorkingSet, $Parsed.OwnedWorkingSetBytes)
		$MaxOwnedPrivate = [math]::Max($MaxOwnedPrivate, $Parsed.OwnedPrivateBytes)
		$MaxSkew = [math]::Max($MaxSkew, $SnapshotSkewMs)
		$Previous = [pscustomobject]@{ Frequency = $Parsed.MonotonicFrequency
			StartTicks = $Parsed.SampleStartTicks; EndTicks = $Parsed.SampleEndTicks
			Elapsed = $Parsed.SupervisorElapsedMilliseconds; Utc = $Utc; Counters = $Parsed }
	}
	if ($FullRole -lt 1) { throw "$Role host resource trace never observed all owned processes together" }
	return [ordered]@{ Classification = 'BOUNDED_SAME_HOST_SNAPSHOT_SERIES'
		HostName = $Rows[0].HostName; InterfaceIndex = [long]$Rows[0].InterfaceIndex
		InterfaceMacAddress = $Rows[0].InterfaceMacAddress
		InterfaceAddress = $Rows[0].InterfaceAddress; LinkSpeed = '10 Gbps'
		SampleCount = $Rows.Count; FullRoleSampleCount = $FullRole
		MaxSnapshotSkewMilliseconds = [math]::Round($MaxSkew, 3)
		MinimumObservedAvailablePhysicalBytes = $MinAvailable
		MaximumObservedSimultaneousOwnedWorkingSetBytes = $MaxOwnedWorkingSet
		MaximumObservedSimultaneousOwnedPrivateBytes = $MaxOwnedPrivate
		MaximumObservedHostCpuPercent = [math]::Round($MaxCpu, 3)
		MaximumObservedNicSentBytesPerSecond = [math]::Round($MaxSendRate, 3)
		MaximumObservedNicReceivedBytesPerSecond = [math]::Round($MaxReceiveRate, 3)
		NicCounterDeltas = [ordered]@{
			OutboundDiscardedPackets = [decimal]$Previous.Counters.NicOutboundDiscardedPackets - [decimal]$Rows[0].NicOutboundDiscardedPackets
			OutboundPacketErrors = [decimal]$Previous.Counters.NicOutboundPacketErrors - [decimal]$Rows[0].NicOutboundPacketErrors
			ReceivedDiscardedPackets = [decimal]$Previous.Counters.NicReceivedDiscardedPackets - [decimal]$Rows[0].NicReceivedDiscardedPackets
			ReceivedPacketErrors = [decimal]$Previous.Counters.NicReceivedPacketErrors - [decimal]$Rows[0].NicReceivedPacketErrors
		}
		Intervals = @($Intervals) }
}

function Read-ProviderRun {
	param([string]$ReportPath, [string]$ServerRoot, [string]$ClientRoot, [string]$ExpectedProvider)
	$Report = Read-BoundedJson -Path $ReportPath
	if ($Report.Format -cne 'GargantuanPhysicalFarmReconciliation' -or $Report.Version -ne 1 -or
		$Report.Provider -cne $ExpectedProvider -or $Report.Status -cne 'INCOMPLETE' -or
		$Report.RoleLocalEvidence -cne 'VALIDATED' -or $Report.ProviderQualification -cne 'NOT CLAIMED' -or
		$Report.RunId -cnotmatch '^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$' -or
		$Report.ManifestSha256 -cnotmatch '^[a-fA-F0-9]{64}$' -or
		$Report.Identity.Ready -ne 32 -or $Report.Admission.accepted -ne $Report.Admission.retired -or
		$Report.Admission.terminal_release -ne 0 -or $Report.Admission.outstanding -ne 0 -or
		$Report.Admission.active_grants -ne 0) {
		throw "$ExpectedProvider reconciled role evidence is invalid or incomplete"
	}
	$Server = Read-RoleEvidence -Root $ServerRoot -Role 'Server' -Report $Report
	$Clients = Read-RoleEvidence -Root $ClientRoot -Role 'Clients' -Report $Report
	$Manifest = $Server.Manifest
	if ($Manifest -ne $null -and ($Manifest | ConvertTo-Json -Depth 10) -cne
		($Clients.Manifest | ConvertTo-Json -Depth 10)) { throw 'two role manifests differ' }
	if ($Manifest.RunId -cne $Report.RunId -or $Manifest.Provider -cne $ExpectedProvider -or
		$Manifest.ScaleWorkload -ne $true -or $Manifest.Nonces.Count -ne 32) {
		throw "$ExpectedProvider manifest is not the expected physical farm workload"
	}
	$Labels = @(0..31 | ForEach-Object { 'client-{0:D2}' -f $_ })
	$ServerResources = Read-ResourceObservation -Path $Server.ResourcePath -RunId $Report.RunId `
		-ExpectedLabels @('server') -ExpectedCount ([long]$Report.ServerResourceSamples) `
		-AggregateWorkingSetLimitBytes ([long]$Server.Result.AggregateWorkingSetLimitBytes)
	$ClientResources = Read-ResourceObservation -Path $Clients.ResourcePath -RunId $Report.RunId `
		-ExpectedLabels $Labels -ExpectedCount ([long]$Report.ClientResourceSamples) `
		-AggregateWorkingSetLimitBytes ([long]$Clients.Result.AggregateWorkingSetLimitBytes)
	$ServerHost = Read-HostResourceObservation -Path $Server.HostResourcePath -RunId $Report.RunId `
		-Role 'Server' -Provider $ExpectedProvider -ExpectedLiveProcesses 1 `
		-ExpectedCount ([long]$Report.ServerHostResourceSamples)
	$ClientHost = Read-HostResourceObservation -Path $Clients.HostResourcePath -RunId $Report.RunId `
		-Role 'Clients' -Provider $ExpectedProvider -ExpectedLiveProcesses 32 `
		-ExpectedCount ([long]$Report.ClientHostResourceSamples)
	if ($ServerHost.HostName -ceq $ClientHost.HostName -or
		$ServerHost.InterfaceMacAddress -ceq $ClientHost.InterfaceMacAddress -or
		$ServerHost.InterfaceAddress -cne '10.253.3.2' -or
		$ClientHost.InterfaceAddress -cne '10.253.3.1') {
		throw 'Server/Clients host or fiber identity is invalid'
	}
	return [pscustomobject]@{
		Report = $Report; Manifest = $Manifest
		Resources = [ordered]@{ Server = $ServerResources; Clients = $ClientResources
			ServerHost = $ServerHost; ClientHost = $ClientHost }
		EvidenceRetention = [ordered]@{
			Classification = 'RECONCILED_ROLE_LOCAL_INDEX_BOUNDS_ONLY'
			ServerIndexedFiles = $Server.IndexedFiles; ServerIndexedBytes = $Server.IndexedBytes
			ClientIndexedFiles = $Clients.IndexedFiles; ClientIndexedBytes = $Clients.IndexedBytes
		}
	}
}

$Local = Read-ProviderRun -ReportPath $LocalReportPath -ServerRoot $LocalServerEvidenceRoot `
	-ClientRoot $LocalClientEvidenceRoot -ExpectedProvider 'Local'
$Node = Read-ProviderRun -ReportPath $NodeReportPath -ServerRoot $NodeServerEvidenceRoot `
	-ClientRoot $NodeClientEvidenceRoot -ExpectedProvider 'Node'
if ($Local.Report.RunId -ceq $Node.Report.RunId) { throw 'Local and Node reused a physical run identity' }
foreach ($Name in @('ServerHost', 'ClientHost')) {
	foreach ($Field in @('HostName', 'InterfaceIndex', 'InterfaceMacAddress', 'InterfaceAddress')) {
		if ($Local.Resources[$Name][$Field] -cne $Node.Resources[$Name][$Field]) {
			throw "Local/Node $Name physical host identity changed: $Field"
		}
	}
}
$ComparableFields = @('SourceCommit', 'Endpoint', 'ClientFrames', 'ServerTicks',
	'ServerSha256', 'ServerPackageSha256', 'PlayerSha256', 'PlayerPackageSha256',
	'ServerContentManifestSha256', 'PlayerContentManifestSha256',
	'ServerDeploymentSha256', 'PlayerDeploymentSha256')
foreach ($Name in $ComparableFields) {
	if ($Local.Manifest[$Name] -cne $Node.Manifest[$Name]) {
		throw "Local/Node workload or deployment pin mismatch: $Name"
	}
}
$Observed = [ordered]@{
	Format = 'GargantuanPhysicalFarmAcceptanceObservation'; Version = 1
	Status = 'INCOMPLETE'; Foundation3LQualification = 'NOT CLAIMED'
	LocalRunId = $Local.Report.RunId; NodeRunId = $Node.Report.RunId
	LocalReportSha256 = (Get-FileHash -LiteralPath $LocalReportPath -Algorithm SHA256).Hash.ToLowerInvariant()
	NodeReportSha256 = (Get-FileHash -LiteralPath $NodeReportPath -Algorithm SHA256).Hash.ToLowerInvariant()
	WorkloadPinParity = [ordered]@{
		State = 'MEASURED'; SourceCommit = $Local.Manifest.SourceCommit
		ComparableFields = $ComparableFields
	}
	Local = [ordered]@{
		Ready = $Local.Report.Identity.Ready; AcceptedBytes = $Local.Report.Admission.accepted
		RetiredBytes = $Local.Report.Admission.retired; Resources = $Local.Resources
		EvidenceRetention = $Local.EvidenceRetention
	}
	Node = [ordered]@{
		Ready = $Node.Report.Identity.Ready; AcceptedBytes = $Node.Report.Admission.accepted
		RetiredBytes = $Node.Report.Admission.retired; Resources = $Node.Resources
		EvidenceRetention = $Node.EvidenceRetention
	}
	GateObservations = @(
		[ordered]@{ Gate = 'Cross-provider exact workload/deployment pin parity'; State = 'MEASURED' },
		[ordered]@{ Gate = 'Role-local bounded process sampling'; State = 'MEASURED' },
		[ordered]@{ Gate = 'Complete-sweep role-local working set within supervisor limit'; State = $(if ($Local.Resources.Server.CompleteSweepCount -gt 0 -and $Local.Resources.Clients.CompleteSweepCount -gt 0 -and $Node.Resources.Server.CompleteSweepCount -gt 0 -and $Node.Resources.Clients.CompleteSweepCount -gt 0) { 'MEASURED' } else { 'NOT MEASURED' }); Reason = 'sequential per-process sweep; not a synchronized host memory or network headroom result' },
		[ordered]@{ Gate = 'Role-local host CPU, memory, simultaneous owned processes, and fiber NIC counters'; State = 'MEASURED' },
		[ordered]@{ Gate = 'Role-local indexed evidence byte/file bounds'; State = 'MEASURED' },
		[ordered]@{ Gate = 'Five-phase observation and terminal native admission conservation'; State = 'MEASURED' },
		[ordered]@{ Gate = 'CPU, memory, network and transport headroom'; State = 'NOT MEASURED'; Reason = 'bounded host/NIC snapshots describe utilization, but no canonical CPU/memory/NIC pass percentage or concurrent packet-level reserve proof follows from those samples' },
		[ordered]@{ Gate = 'Fixed 20-second service recovery'; State = 'NOT MEASURED'; Reason = 'no independently timed recovery workload or native recovery trace' },
		[ordered]@{ Gate = 'Workload-derived exact structural convergence'; State = 'NOT MEASURED'; Reason = 'phase observations and final debt do not locate final accepted byte versus client observation' },
		[ordered]@{ Gate = 'Journal retention margin and overload'; State = 'NOT MEASURED'; Reason = 'final zero journal backlog lacks retained-history high-water and overload chronology' },
		[ordered]@{ Gate = 'Full Local/Node provider parity'; State = 'NOT MEASURED'; Reason = 'application service, real TLS, recovery, resource headroom and capture gates remain independent' }
	)
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
foreach ($Root in @($LocalServerEvidenceRoot, $LocalClientEvidenceRoot,
	$NodeServerEvidenceRoot, $NodeClientEvidenceRoot)) {
	$Resolved = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
	if ($OutputPath.StartsWith($Resolved + [IO.Path]::DirectorySeparatorChar,
		[StringComparison]::OrdinalIgnoreCase)) {
		throw 'acceptance observation must remain outside immutable role-local roots'
	}
}
if (Test-Path -LiteralPath $OutputPath) { throw 'acceptance observation path already exists' }
$Bytes = [Text.UTF8Encoding]::new($false).GetBytes(($Observed | ConvertTo-Json -Depth 12))
$Stream = [IO.File]::Open($OutputPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $Stream.Write($Bytes) } finally { $Stream.Dispose() }
Write-Output "[Qualification:FarmAcceptance] INCOMPLETE local=$($Local.Report.RunId) node=$($Node.Report.RunId) output=$OutputPath"
