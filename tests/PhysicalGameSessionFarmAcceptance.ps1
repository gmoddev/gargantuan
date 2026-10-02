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
	[string]$NodeTlsMatchReceiptPath,
	[string]$NodeTlsMatchReceiptSha256,
	[string]$NodeStagePath,
	[string]$NodeStageSha256,
	[string]$NodeRunReceiptPath,
	[string]$NodeRunReceiptSha256,
	[Parameter(Mandatory = $true)][string]$OutputPath
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'AdmissionFairnessEvidence.ps1')
. (Join-Path $PSScriptRoot 'PhysicalFarmPublicationEvidence.ps1')

function Import-FarmRecoveryParser {
	$Path = Join-Path $PSScriptRoot 'PhysicalGameSessionFarm.ps1'
	$Tokens = $null
	$Errors = $null
	$Ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$Tokens, [ref]$Errors)
	if ($Errors.Count -ne 0) { throw 'canonical farm recovery parser has a syntax error' }
	$Needed = @('Get-Fields', 'Read-SharedLogLines', 'Get-Records', 'Get-RecoveryDiagnostics',
		'Test-RecoveryQuiescent', 'Assert-RecoveryRecords')
	foreach ($Function in $Ast.FindAll({ param($Node)
		$Node -is [Management.Automation.Language.FunctionDefinitionAst]
	}, $true)) {
		if ($Function.Name -in $Needed) { $Function.Extent.Text }
	}
}

foreach ($Definition in @(Import-FarmRecoveryParser)) {
	. ([scriptblock]::Create($Definition))
}
foreach ($Name in @('Get-Fields', 'Read-SharedLogLines', 'Get-Records', 'Get-RecoveryDiagnostics',
	'Test-RecoveryQuiescent', 'Assert-RecoveryRecords')) {
	if (-not (Get-Command $Name -CommandType Function -ErrorAction SilentlyContinue)) {
		throw "canonical farm recovery parser lacks $Name"
	}
}

$NodeTlsInputs = @($NodeTlsMatchReceiptPath, $NodeTlsMatchReceiptSha256,
	$NodeStagePath, $NodeStageSha256, $NodeRunReceiptPath, $NodeRunReceiptSha256)
if (@($NodeTlsInputs | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }).Count -notin @(0, 6)) {
	throw 'Node TLS acceptance inputs must be supplied together'
}

function Read-BoundedJson {
	param([string]$Path, [long]$MaximumBytes = 1048576)
	$Resolved = [IO.Path]::GetFullPath($Path)
	$Item = Get-Item -LiteralPath $Resolved -ErrorAction Stop
	if ($Item.PSIsContainer -or $Item.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint) -or
		$Item.Length -gt $MaximumBytes) { throw "missing, redirected, or oversized JSON: $Path" }
	return Get-Content -LiteralPath $Resolved -Raw | ConvertFrom-Json -AsHashtable
}

function Read-FarmServerWorkTicks {
	param([string]$ServerRoot, [System.Collections.IDictionary]$ServerIndex, [string]$RunId)
	$Members = @($ServerIndex.Files | Where-Object Name -CEQ 'server-work-ticks.bin')
	if ($Members.Count -eq 0) {
		return [ordered]@{ Status = 'NOT_MEASURED'; Reason = 'indexed server work-tick evidence absent' }
	}
	if ($Members.Count -ne 1) { throw 'indexed server work-tick evidence is duplicated' }
	$ScriptPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../tools/physical-qualifier/farm_server_tick.py'))
	if (-not (Test-Path -LiteralPath $ScriptPath -PathType Leaf)) {
		throw 'bounded server work-tick analyzer is missing'
	}
	$Python = @(Get-Command python -CommandType Application -ErrorAction Stop)[0]
	$Output = @(& $Python.Source $ScriptPath (Join-Path $ServerRoot 'evidence-sha256.json') 2>&1)
	$ExitCode = $LASTEXITCODE
	if ($ExitCode -ne 0 -or $Output.Count -ne 1) {
		$Detail = ((@($Output) | ForEach-Object { [string]$_ }) -join ' ')
		if ($Detail.Length -gt 512) { $Detail = $Detail.Substring(0, 512) }
		throw "bounded server work-tick analysis rejected indexed evidence: $Detail"
	}
	$Observation = [string]$Output[0] | ConvertFrom-Json -AsHashtable
	if ($Observation.Format -cne 'GargantuanFarmServerWorkTicks' -or
		$Observation.Version -ne 1 -or $Observation.RunId -cne $RunId -or
		$Observation.Status -cnotin @('MEASURED_PASS', 'MEASURED_FAIL') -or
		$Observation.Phases.Count -ne 5 -or
		$Observation.CrossHostLatency -cne 'NOT_MEASURED') {
		throw 'bounded server work-tick observation is invalid'
	}
	return $Observation
}

function Read-FarmRemoteCadence {
	param([string]$ClientRoot, [System.Collections.IDictionary]$ClientIndex, [string]$RunId)
	$Members = @($ClientIndex.Files | Where-Object Name -CEQ 'client-00.stdout.log')
	if ($Members.Count -eq 0) {
		return [ordered]@{ Status = 'NOT_MEASURED'; Reason = 'indexed producer Luau log absent'
			OtherClientsScaleRemoteRecipientService = 'NOT_MEASURED'
			CrossHostOneWayLatency = 'NOT_MEASURED' }
	}
	if ($Members.Count -ne 1) { throw 'indexed producer Luau log is duplicated' }
	$LogPath = Assert-IndexedFile -Root $ClientRoot -Index $ClientIndex `
		-Name 'client-00.stdout.log' -MaximumBytes 4194304
	if (-not [IO.File]::ReadAllText($LogPath).Contains('[Qualification:RemoteCadence] ')) {
		return [ordered]@{ Status = 'NOT_MEASURED'; Reason = 'producer Luau offer/completion trace absent'
			OtherClientsScaleRemoteRecipientService = 'NOT_MEASURED'
			CrossHostOneWayLatency = 'NOT_MEASURED' }
	}
	$ScriptPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../tools/physical-qualifier/farm_remote_cadence.py'))
	if (-not (Test-Path -LiteralPath $ScriptPath -PathType Leaf)) {
		throw 'bounded Remote Luau cadence analyzer is missing'
	}
	$Python = @(Get-Command python -CommandType Application -ErrorAction Stop)[0]
	$Output = @(& $Python.Source $ScriptPath --clients-index `
		(Join-Path $ClientRoot 'evidence-sha256.json') --producer-slot 0 2>&1)
	$ExitCode = $LASTEXITCODE
	if ($ExitCode -notin @(0, 1) -or $Output.Count -ne 1) {
		$Detail = ((@($Output) | ForEach-Object { [string]$_ }) -join ' ')
		if ($Detail.Length -gt 512) { $Detail = $Detail.Substring(0, 512) }
		throw "bounded Remote Luau cadence analysis rejected indexed evidence: $Detail"
	}
	$Observation = [string]$Output[0] | ConvertFrom-Json -AsHashtable
	if ($Observation.Format -cne 'GargantuanFarmRemoteLuauCadence' -or
		$Observation.Version -ne 1 -or $Observation.RunId -cne $RunId -or
		$Observation.ProducerSlot -ne 0 -or
		$Observation.Status -cne $(if ($ExitCode -eq 0) { 'MEASURED_PASS' } else { 'MEASURED_FAIL' }) -or
		$Observation.SourceSha256 -ine $Members[0].Sha256 -or
		$Observation.OtherClientsScaleRemoteRecipientService -cne 'NOT_MEASURED' -or
		$Observation.CrossHostOneWayLatency -cne 'NOT_MEASURED' -or
		$Observation.Phases.Count -ne 5 -or
		@($Observation.Phases.Keys | Where-Object { $_ -cnotin @('baseline', 'load', 'resident', 'evict', 'reload') }).Count -ne 0) {
		throw 'bounded Remote Luau cadence observation is invalid'
	}
	return $Observation
}

function Assert-IndexedFile {
	param([string]$Root, [System.Collections.IDictionary]$Index, [string]$Name,
		[long]$MaximumBytes = 16777216)
	$Entries = @($Index.Files | Where-Object Name -CEQ $Name)
	if ($Entries.Count -ne 1) { throw "immutable evidence index lacks one $Name" }
	$Entry = $Entries[0]
	$Path = Join-Path $Root $Name
	$Item = Get-Item -LiteralPath $Path -ErrorAction Stop
	if ($Item.PSIsContainer -or $Item.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint) -or
		$Item.Length -ne [long]$Entry.Bytes -or $Item.Length -gt $MaximumBytes -or
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
		$Index.Files -isnot [array] -or $Index.Files.Count -lt 2 -or $Index.Files.Count -gt 128 -or
		[string]$ExpectedHash -cnotmatch '^[a-fA-F0-9]{64}$' -or
		(Get-FileHash -LiteralPath $IndexPath -Algorithm SHA256).Hash -ine $ExpectedHash) {
		throw "$Role evidence index does not match the reconciliation report"
	}
	$ManifestPath = Assert-IndexedFile -Root $Resolved -Index $Index -Name 'run-manifest.json'
	if ((Get-FileHash -LiteralPath $ManifestPath -Algorithm SHA256).Hash -ine $Report.ManifestSha256) {
		throw "$Role run manifest does not match the reconciliation report"
	}
	$ResourcePath = Assert-IndexedFile -Root $Resolved -Index $Index -Name 'process-resources.csv'
	$ServerLogPath = $null
	$ResultPath = Assert-IndexedFile -Root $Resolved -Index $Index -Name 'result.json'
	$Result = Read-BoundedJson -Path $ResultPath
	if ($Result.RunId -cne $Report.RunId -or $Result.Role -cne $Role -or
		$Result.Status -cne 'PASS') { throw "$Role sealed role result is invalid" }
	$HostResourcePath = Assert-IndexedFile -Root $Resolved -Index $Index -Name 'host-resources.csv'
	$FairnessPath = $null
	$NodeProviderPath = $null
	if ($Role -ceq 'Server') {
		$ServerLogPath = Assert-IndexedFile -Root $Resolved -Index $Index -Name 'server.stdout.log'
		$FairnessPath = Assert-IndexedFile -Root $Resolved -Index $Index `
			-Name 'admission-fairness.tsv' -MaximumBytes 33554432
		if ($Report.Provider -ceq 'Node') {
			$NodeProviderPath = Assert-IndexedFile -Root $Resolved -Index $Index -Name 'node-provider.json'
		} elseif (@($Index.Files | Where-Object Name -CEQ 'node-provider.json').Count -ne 0) {
			throw 'Local role evidence unexpectedly contains a Node provider receipt'
		}
	}
	$IndexedBytes = [long]0
	$Names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
	foreach ($Entry in $Index.Files) {
		if ([string]$Entry.Name -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$' -or
			$Entry.Name -ieq 'evidence-sha256.json' -or
			-not $Names.Add([string]$Entry.Name) -or [long]$Entry.Bytes -lt 0 -or
			[long]$Entry.Bytes -gt $(if ($Role -ceq 'Server' -and
				$Entry.Name -ceq 'admission-fairness.tsv') { 33554432 }
				elseif ($Role -ceq 'Server' -and $Entry.Name -ceq 'publication-service.bin') { 335544832 }
				else { 16777216 })) {
			throw "$Role evidence index has an invalid retained-file bound"
		}
		$EntryPath = Join-Path $Resolved $Entry.Name
		$EntryItem = Get-Item -LiteralPath $EntryPath -ErrorAction Stop
		if ($EntryItem.PSIsContainer -or
			$EntryItem.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint) -or
			$EntryItem.Length -ne [long]$Entry.Bytes -or
			[string]$Entry.Sha256 -cnotmatch '^[a-fA-F0-9]{64}$' -or
			(Get-FileHash -LiteralPath $EntryPath -Algorithm SHA256).Hash -ine $Entry.Sha256) {
			throw "$Role indexed evidence $($Entry.Name) differs from its sealed bytes"
		}
		$IndexedBytes += [long]$Entry.Bytes
	}
	$Actual = @(Get-ChildItem -LiteralPath $Resolved -Force)
	if ($Actual.Count -ne $Names.Count + 1 -or
		@($Actual | Where-Object { $_.Name -ine 'evidence-sha256.json' -and
			-not $Names.Contains($_.Name) }).Count -ne 0) {
		throw "$Role evidence index does not cover the complete role-local root"
	}
	return [pscustomobject]@{
		Root = $Resolved; Manifest = (Read-BoundedJson -Path $ManifestPath); Result = $Result
		ResourcePath = $ResourcePath; HostResourcePath = $HostResourcePath
		FairnessPath = $FairnessPath; NodeProviderPath = $NodeProviderPath
		ServerLogPath = $ServerLogPath; Index = $Index
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

function Read-AdmissionObservation {
	param([System.Collections.IDictionary]$Report, [string]$FairnessPath, [string]$ServerLogPath)
	$Admission = $Report.Admission
	$Names = @('accepted', 'retired', 'terminal_release', 'outstanding', 'outstanding_high',
		'active_grants', 'grants_high', 'grant_deferrals', 'funded_deferrals',
		'credit_deferrals', 'fairness_deferrals', 'max_wait_us', 'peer_backlog_high',
		'global_backlog_high', 'peer_credit_high', 'global_credit_high',
		'fairness_rotations', 'pending_enters', 'pending_leaves',
		'materialization_backlog', 'journal_backlog', 'structural_active_peers',
		'oldest_pending_ticks', 'backlog_failures', 'journal_failures')
	if ($Admission -isnot [System.Collections.IDictionary] -or
		@($Names | Where-Object { -not $Admission.Contains($_) }).Count -ne 0) {
		throw 'reconciliation lacks complete native admission fields'
	}
	foreach ($Name in $Names) {
		if ($Admission[$Name] -isnot [ValueType] -or
			[string]$Admission[$Name] -cnotmatch '^(0|[1-9][0-9]*)$') {
			throw "reconciliation has invalid native admission $Name"
		}
	}
	$Lines = @([IO.File]::ReadAllLines($ServerLogPath) | Where-Object {
		$_.StartsWith('[Qualification:Admission] event=result ', [StringComparison]::Ordinal)
	})
	if ($Lines.Count -ne 1) { throw 'indexed server log lacks one final native admission receipt' }
	$Native = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::Ordinal)
	foreach ($Token in $Lines[0].Substring('[Qualification:Admission] '.Length).Split(' ')) {
		$Parts = $Token.Split('=', 2)
		if ($Parts.Count -ne 2 -or -not $Native.TryAdd($Parts[0], $Parts[1])) {
			throw 'indexed native admission receipt contains duplicate or malformed fields'
		}
	}
	if ($Native['event'] -cne 'result' -or $Native['run'] -cne $Report.RunId) {
		throw 'indexed native admission receipt has a different run identity'
	}
	foreach ($Name in $Names) {
		if (-not $Native.ContainsKey($Name) -or
			$Native[$Name] -cne [string]$Admission[$Name]) {
			throw "reconciled admission $Name differs from indexed native receipt"
		}
	}
	if ($Admission.accepted -le 0 -or $Admission.accepted -ne $Admission.retired -or
		$Admission.terminal_release -ne 0 -or $Admission.outstanding -ne 0 -or
		$Admission.active_grants -ne 0 -or $Admission.grants_high -lt 1 -or
		$Admission.grants_high -gt 4 -or $Admission.outstanding_high -gt 2097152 -or
		$Admission.peer_credit_high -gt 524288 -or
		$Admission.global_credit_high -gt 2097152 -or
		$Admission.materialization_backlog -ne 0 -or $Admission.journal_backlog -ne 0 -or
		$Admission.structural_active_peers -ne 0 -or
		$Admission.backlog_failures -ne 0 -or $Admission.journal_failures -ne 0) {
		throw 'native admission conservation or canonical bounded subset failed'
	}
	$Connections = @($Report.Identity.Connections)
	if ($Connections.Count -ne 32 -or @($Connections | Select-Object -Unique).Count -ne 32) {
		throw 'reconciliation lacks 32 distinct physical connection identities'
	}
	$ObservedFairness = Read-AdmissionFairnessEvidence -Path $FairnessPath `
		-RunId $Report.RunId -ExpectedConnections $Connections
	$RecordedFairness = $Report.AdmissionFairnessObservation
	if ($null -eq $RecordedFairness -or
		($ObservedFairness | ConvertTo-Json -Depth 10 -Compress) -cne
		($RecordedFairness | ConvertTo-Json -Depth 10 -Compress)) {
		throw 'reconciled admission fairness observation differs from sealed native timeline'
	}
	return [ordered]@{
		Classification = 'TERMINAL_CONSERVATION_AND_BOUNDED_ADMISSION_SUBSETS_ONLY'
		AcceptedBytes = [long]$Admission.accepted
		RetiredBytes = [long]$Admission.retired
		TerminalReleaseBytes = [long]$Admission.terminal_release
		OutstandingBytes = [long]$Admission.outstanding
		MaximumActiveGrants = [long]$Admission.grants_high
		MaximumOutstandingBytes = [long]$Admission.outstanding_high
		MaximumPeerCreditBytes = [long]$Admission.peer_credit_high
		MaximumGlobalCreditBytes = [long]$Admission.global_credit_high
		PeerBacklogHighBytes = [long]$Admission.peer_backlog_high
		GlobalBacklogHighBytes = [long]$Admission.global_backlog_high
		GrantDeferrals = [long]$Admission.grant_deferrals
		FundedDeferrals = [long]$Admission.funded_deferrals
		CreditDeferrals = [long]$Admission.credit_deferrals
		FairnessDeferrals = [long]$Admission.fairness_deferrals
		FairnessRotations = [long]$Admission.fairness_rotations
		AcceptedGrantWaitBound = $ObservedFairness.AcceptedGrantWaitBound
		ExactDemandEpisodeCoverage = $ObservedFairness.ExactDemandEpisodeCoverage
		PendingEnters = [long]$Admission.pending_enters
		PendingLeaves = [long]$Admission.pending_leaves
		TerminalMaterializationBacklog = [long]$Admission.materialization_backlog
		TerminalJournalBacklog = [long]$Admission.journal_backlog
		JournalFailures = [long]$Admission.journal_failures
		Fairness = $ObservedFairness
	}
}

function Read-ProviderObservation {
	param([System.Collections.IDictionary]$Report, [System.Collections.IDictionary]$Manifest,
		[string]$NodeProviderPath)
	if ($Report.Provider -ceq 'Local') {
		if ($NodeProviderPath -or
			$Report.NodeAuthenticatedManifest.State -cne 'NOT_APPLICABLE') {
			throw 'Local provider has unexpected Node authentication evidence'
		}
		return [ordered]@{ State = 'LOCAL_PINNED_PACKAGE_ONLY'; RealTls = 'NOT_APPLICABLE' }
	}
	$Receipt = Read-BoundedJson -Path $NodeProviderPath
	$Recorded = $Report.NodeAuthenticatedManifest
	if ($Receipt.Format -cne 'GargantuanFarmNodeAuthenticatedManifest' -or
		$Receipt.Version -ne 2 -or $Receipt.RunId -cne $Report.RunId -or
		$Receipt.Provider -cne 'Node' -or
		$Receipt.RequestId -cnotmatch '^server-content-[1-9][0-9]{0,19}$' -or
		$Receipt.ProjectId -cnotmatch '^[a-f0-9]{32}$' -or
		$Receipt.PackageVersion -isnot [long] -or $Receipt.PackageVersion -le 0 -or
		$Receipt.NodeEndpoint -cne $Manifest.NodeEndpoint -or
		$Receipt.RootCertificateSha256 -ine $Manifest.NodeRootCertificateSha256 -or
		$Receipt.ManifestSha256 -ine $Manifest.ServerContentManifestSha256 -or
		$Receipt.ManifestBytes -isnot [long] -or $Receipt.ManifestBytes -lt 1 -or
		$Receipt.ManifestBytes -gt 4194304 -or
		$Receipt.ChannelCredentials -cne 'grpc_ssl_credentials' -or
		$Receipt.AuthenticatedManifestRpcCount -isnot [long] -or
		$Receipt.AuthenticatedManifestRpcCount -lt 1 -or
		$Receipt.AuthenticatedManifestRpcCount -gt 10000 -or
		$Receipt.TlsSessionDetails -cne 'NOT_MEASURED' -or
		$Receipt.Source -cne 'GargantuanServer/NodeContentProvider' -or
		$Recorded.State -cne 'AUTHENTICATED_MANIFEST_RPC_MEASURED' -or
		$Recorded.RequestId -cne $Receipt.RequestId -or
		$Recorded.ProjectId -cne $Receipt.ProjectId -or
		$Recorded.PackageVersion -ne $Receipt.PackageVersion -or
		$Recorded.ManifestSha256 -ine $Receipt.ManifestSha256 -or
		$Recorded.RootCertificateSha256 -ine $Receipt.RootCertificateSha256 -or
		$Recorded.TlsSessionDetails -cne 'NOT_MEASURED') {
		throw 'Node authenticated manifest subset differs from indexed provider receipt'
	}
	return [ordered]@{
		State = 'AUTHENTICATED_MANIFEST_RPC_MEASURED'
		RequestId = $Receipt.RequestId; ProjectId = $Receipt.ProjectId
		PackageVersion = $Receipt.PackageVersion
		RootCertificateSha256 = $Receipt.RootCertificateSha256
		RealTls = 'NOT_MEASURED'
	}
}

function Import-NodeTlsMatcher {
	$Path = Join-Path $PSScriptRoot 'PhysicalGameSessionFarmNodeTls.ps1'
	$Tokens = $null; $Errors = $null
	$Ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$Tokens, [ref]$Errors)
	if ($Errors.Count -ne 0) { throw 'canonical Node TLS matcher has a syntax error' }
	$Needed = @('Get-NodeTimestamp', 'Get-NodeTlsLogMatchReceipt')
	foreach ($Function in $Ast.FindAll({ param($Node)
		$Node -is [Management.Automation.Language.FunctionDefinitionAst]
	}, $true)) {
		if ($Function.Name -in $Needed) { $Function.Extent.Text }
	}
}

function Read-NodeTlsObservation {
	param([string]$ServerReceiptPath, [string]$MatchReceiptPath,
		[string]$MatchReceiptSha256, [string]$StagePath, [string]$StageSha256,
		[string]$RunReceiptPath, [string]$RunReceiptSha256)
	foreach ($Pin in @($MatchReceiptSha256, $StageSha256, $RunReceiptSha256)) {
		if ($Pin -cnotmatch '^[a-fA-F0-9]{64}$') { throw 'Node TLS evidence pin is invalid' }
	}
	$null = Read-BoundedJson -Path $MatchReceiptPath -MaximumBytes 4096
	if ((Get-FileHash -LiteralPath $MatchReceiptPath -Algorithm SHA256).Hash -ine
		$MatchReceiptSha256) { throw 'Node TLS match receipt differs from independent pin' }
	$Derived = Get-NodeTlsLogMatchReceipt -ServerReceiptPath $ServerReceiptPath `
		-NodeStagePath $StagePath -NodeRunReceiptPath $RunReceiptPath `
		-NodeRunReceiptSha256 $RunReceiptSha256 -NodeStageSha256 $StageSha256
	$ExpectedBytes = [Text.UTF8Encoding]::new($false).GetBytes(($Derived | ConvertTo-Json -Depth 4))
	$ExpectedHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData(
		$ExpectedBytes)).ToLowerInvariant()
	if ($ExpectedHash -ine $MatchReceiptSha256) {
		throw 'Node TLS match receipt differs from pinned source'
	}
	return [ordered]@{
		State = 'NEGOTIATED_TLS_MANIFEST_RPC_MEASURED'
		TlsVersion = $Derived.TlsVersion; CipherSuite = $Derived.CipherSuite
		RequestId = $Derived.RequestId; NodeRunReceiptSha256 = $Derived.NodeRunReceiptSha256
		NodeStageSha256 = $Derived.NodeStageSha256
		NodeTlsMatchReceiptSha256 = $MatchReceiptSha256.ToLowerInvariant()
	}
}

if (-not [string]::IsNullOrWhiteSpace($NodeTlsMatchReceiptPath)) {
	foreach ($Definition in @(Import-NodeTlsMatcher)) { . ([scriptblock]::Create($Definition)) }
	foreach ($Name in @('Get-NodeTimestamp', 'Get-NodeTlsLogMatchReceipt')) {
		if (-not (Get-Command $Name -CommandType Function -ErrorAction SilentlyContinue)) {
			throw "canonical Node TLS matcher lacks $Name"
		}
	}
}

function Read-RecoveryObservation {
	param([System.Collections.IDictionary]$Report, [System.Collections.IDictionary]$Manifest,
		$Server, $Clients)
	if ($Manifest.Contains('RecoveryWorkload') -and $Manifest.RecoveryWorkload -isnot [bool]) {
		throw 'recovery workload flag has invalid type'
	}
	if ($Manifest.RecoveryWorkload -ne $true) {
		if ($null -ne $Report.RecoveryObservation) {
			throw 'non-recovery run has a reconciled recovery claim'
		}
		return [ordered]@{ State = 'NOT MEASURED'; Workload = 'NOT RUN'
			FixedServiceRecovery = 'NOT MEASURED'
			StrictConvergenceSufficientProof = 'NOT MEASURED'
			SampledJournalRetention = 'NOT MEASURED'
			ExactRetainedWorkBytes = 'NOT_MEASURED'; Cases = @() }
	}
	if ($null -eq $Report.RecoveryObservation -or
		$Server.Result.ScaleWorkload -ne $true -or $Clients.Result.ScaleWorkload -ne $true -or
		$Server.Result.ManifestSha256 -ine $Report.ManifestSha256 -or
		$Clients.Result.ManifestSha256 -ine $Report.ManifestSha256 -or
		$Server.Result.Provider -cne $Report.Provider -or
		$Clients.Result.Provider -cne $Report.Provider -or
		$Server.Result.Endpoint -cne $Manifest.Endpoint -or
		$Clients.Result.Endpoint -cne $Manifest.Endpoint) {
		throw 'recovery workload lacks matching reconciled and sealed role results'
	}
	$ServerLog = [pscustomobject]@{
		OutputPath = (Assert-IndexedFile -Root $Server.Root -Index $Server.Index -Name 'server.stdout.log')
		ErrorPath = (Assert-IndexedFile -Root $Server.Root -Index $Server.Index -Name 'server.stderr.log')
	}
	$ClientLogs = @(0..31 | ForEach-Object {
		$Label = 'client-{0:D2}' -f $_
		[pscustomobject]@{
			OutputPath = (Assert-IndexedFile -Root $Clients.Root -Index $Clients.Index -Name "$Label.stdout.log")
			ErrorPath = (Assert-IndexedFile -Root $Clients.Root -Index $Clients.Index -Name "$Label.stderr.log")
		}
	})
	$ExpectedNonces = @($Manifest.Nonces | ForEach-Object { [string]$_ })
	$Connections = @($Report.Identity.Connections)
	if ($ExpectedNonces.Count -ne 32 -or $Connections.Count -ne 32) {
		throw 'recovery workload lacks 32 pinned client and connection identities'
	}
	$PreviousRunId = $script:RunId
	try {
		$script:RunId = [string]$Report.RunId
		$Observed = Assert-RecoveryRecords -Server $ServerLog -Clients $ClientLogs `
			-ExpectedNonces $ExpectedNonces -ExpectedConnections $Connections
	} finally { $script:RunId = $PreviousRunId }
	if (($Observed | ConvertTo-Json -Depth 12 -Compress) -cne
		($Report.RecoveryObservation | ConvertTo-Json -Depth 12 -Compress)) {
		throw 'reconciled recovery observation differs from immutable source logs'
	}
	$Cases = @($Observed.Cases)
	if ($Cases.Count -ne 3 -or
		(@($Cases | ForEach-Object Case) -join ',') -cne 'gameplay,structural,mixed' -or
		$Observed.ExactRetainedWorkBytes -cne 'NOT_MEASURED' -or
		@($Cases | Where-Object ExactRetainedWorkBytes -cne 'NOT_MEASURED').Count -ne 0) {
		throw 'recovery observation has invalid case or retained-work classification'
	}
	$Fixed = if (@($Cases | Where-Object FixedServiceRecovery -ne 'MEASURED_PASS').Count -eq 0) {
		'MEASURED_PASS'
	} else { 'MEASURED_FAIL' }
	$Strict = if (@($Cases | Where-Object StrictConvergenceSufficientProof -ne 'MEASURED_PASS').Count -eq 0) {
		'MEASURED_PASS'
	} else { 'INCONCLUSIVE_NOT_MEASURED' }
	return [ordered]@{
		State = $Observed.State; Workload = 'THREE_CASES_REPLAYED_FROM_INDEXED_LOGS'
		FixedServiceRecovery = $Fixed; StrictConvergenceSufficientProof = $Strict
		SampledJournalRetention = 'MEASURED_PASS'
		MinimumCessationRetentionMarginRecords = [long](@($Cases | ForEach-Object MinimumRetentionMarginRecords |
			Measure-Object -Minimum).Minimum)
		ExactRetainedWorkBytes = 'NOT_MEASURED'; Cases = $Cases
	}
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
	$Admission = Read-AdmissionObservation -Report $Report -FairnessPath $Server.FairnessPath `
		-ServerLogPath $Server.ServerLogPath
	$Publication = Read-FarmPublicationObservation -ServerRoot $Server.Root -ClientRoot $Clients.Root `
		-ServerIndex $Server.Index -ClientIndex $Clients.Index `
		-RunManifestPath (Join-Path $Server.Root 'run-manifest.json') `
		-ScratchParent ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($ReportPath)))
	if ($null -eq $Report.PublicationObservation -or
		($Publication | ConvertTo-Json -Depth 12 -Compress) -cne
		($Report.PublicationObservation | ConvertTo-Json -Depth 12 -Compress)) {
		throw 'reconciled Character publication observation differs from sealed native evidence'
	}
	$ServerWorkTicks = Read-FarmServerWorkTicks -ServerRoot $Server.Root `
		-ServerIndex $Server.Index -RunId $Report.RunId
	$RemoteCadence = Read-FarmRemoteCadence -ClientRoot $Clients.Root `
		-ClientIndex $Clients.Index -RunId $Report.RunId
	$ProviderObservation = Read-ProviderObservation -Report $Report -Manifest $Manifest `
		-NodeProviderPath $Server.NodeProviderPath
	$Recovery = Read-RecoveryObservation -Report $Report -Manifest $Manifest `
		-Server $Server -Clients $Clients
	return [pscustomobject]@{
		Report = $Report; Manifest = $Manifest
		Admission = $Admission; Publication = $Publication; ServerWorkTicks = $ServerWorkTicks
		RemoteCadence = $RemoteCadence
		ProviderObservation = $ProviderObservation; Recovery = $Recovery
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
if (-not [string]::IsNullOrWhiteSpace($NodeTlsMatchReceiptPath)) {
	$NodeTlsObservation = Read-NodeTlsObservation `
		-ServerReceiptPath (Join-Path $NodeServerEvidenceRoot 'node-provider.json') `
		-MatchReceiptPath $NodeTlsMatchReceiptPath -MatchReceiptSha256 $NodeTlsMatchReceiptSha256 `
		-StagePath $NodeStagePath -StageSha256 $NodeStageSha256 `
		-RunReceiptPath $NodeRunReceiptPath -RunReceiptSha256 $NodeRunReceiptSha256
	$Node.ProviderObservation['RealTls'] = $NodeTlsObservation.State
	$Node.ProviderObservation['TlsEvidence'] = $NodeTlsObservation
}
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
		RetiredBytes = $Local.Report.Admission.retired; Admission = $Local.Admission
		Publication = $Local.Publication
		ServerWorkTicks = $Local.ServerWorkTicks
		RemoteCadence = $Local.RemoteCadence
		Recovery = $Local.Recovery
		Provider = $Local.ProviderObservation; Resources = $Local.Resources
		EvidenceRetention = $Local.EvidenceRetention
	}
	Node = [ordered]@{
		Ready = $Node.Report.Identity.Ready; AcceptedBytes = $Node.Report.Admission.accepted
		RetiredBytes = $Node.Report.Admission.retired; Admission = $Node.Admission
		Publication = $Node.Publication
		ServerWorkTicks = $Node.ServerWorkTicks
		RemoteCadence = $Node.RemoteCadence
		Recovery = $Node.Recovery
		Provider = $Node.ProviderObservation; Resources = $Node.Resources
		EvidenceRetention = $Node.EvidenceRetention
	}
	GateObservations = @(
		[ordered]@{ Gate = 'Cross-provider exact workload/deployment pin parity'; State = 'MEASURED' },
		[ordered]@{ Gate = 'Per-provider terminal native admission/debt conservation and bounded grants/credit'; State = 'MEASURED'; Reason = 'sealed final receipt, not an intra-run service or fairness bound' },
		[ordered]@{ Gate = 'Per-provider exact-demand fairness event identity and observed eligibility waits'; State = 'MEASURED'; Reason = 'sealed native timeline; the separate accepted-grant bound remains scoped to recorded episodes' },
		[ordered]@{ Gate = 'Recorded accepted-grant eligibility wait within 220.5 ms'; State = $(if ($Local.Admission.AcceptedGrantWaitBound -eq 'MEASURED_FAIL' -or $Node.Admission.AcceptedGrantWaitBound -eq 'MEASURED_FAIL') { 'MEASURED_FAIL' } elseif ($Local.Admission.AcceptedGrantWaitBound -eq 'MEASURED_PASS' -and $Node.Admission.AcceptedGrantWaitBound -eq 'MEASURED_PASS') { 'MEASURED_PASS' } else { 'NOT MEASURED' }); Reason = 'accepted exact-demand episodes only; interruptions, disposals and continuous semantic backlog remain separate' },
		[ordered]@{ Gate = 'Node authenticated manifest RPC and root/content pins'; State = 'MEASURED'; Reason = 'indexed provider receipt' },
		[ordered]@{ Gate = 'Node negotiated TLS for authenticated manifest RPC'; State = $(if ($Node.ProviderObservation.RealTls -ceq 'NEGOTIATED_TLS_MANIFEST_RPC_MEASURED') { 'MEASURED' } else { 'NOT MEASURED' }); Reason = 'requires independently pinned Node stage/run/log and exact request matcher; full provider parity remains separate' },
		[ordered]@{ Gate = 'Role-local bounded process sampling'; State = 'MEASURED' },
		[ordered]@{ Gate = 'Complete-sweep role-local working set within supervisor limit'; State = $(if ($Local.Resources.Server.CompleteSweepCount -gt 0 -and $Local.Resources.Clients.CompleteSweepCount -gt 0 -and $Node.Resources.Server.CompleteSweepCount -gt 0 -and $Node.Resources.Clients.CompleteSweepCount -gt 0) { 'MEASURED' } else { 'NOT MEASURED' }); Reason = 'sequential per-process sweep; not a synchronized host memory or network headroom result' },
		[ordered]@{ Gate = 'Role-local host CPU, memory, simultaneous owned processes, and fiber NIC counters'; State = 'MEASURED' },
		[ordered]@{ Gate = 'Role-local indexed evidence byte/file bounds'; State = 'MEASURED' },
		[ordered]@{ Gate = 'Five-phase observation and terminal native admission conservation'; State = 'MEASURED' },
		[ordered]@{ Gate = 'Character accepted-state chain and role-local publication delays'; State = $(if ($Local.Publication.Status -ceq 'ACCEPTED_STATE_CHAIN_OBSERVED' -and $Node.Publication.Status -ceq 'ACCEPTED_STATE_CHAIN_OBSERVED') { 'MEASURED' } else { 'NOT MEASURED' }); Reason = 'each provider independently rejoined all hash-sealed native Character traces; cross-host clocks remain separate' },
		[ordered]@{ Gate = 'Server work-tick p95/p99/max in all five phases'; State = $(if ($Local.ServerWorkTicks.Status -ceq 'MEASURED_FAIL' -or $Node.ServerWorkTicks.Status -ceq 'MEASURED_FAIL') { 'MEASURED_FAIL' } elseif ($Local.ServerWorkTicks.Status -ceq 'MEASURED_PASS' -and $Node.ServerWorkTicks.Status -ceq 'MEASURED_PASS') { 'MEASURED_PASS' } else { 'NOT MEASURED' }); Reason = 'hash-indexed FrameBegin-to-FrameEnd work time excludes deliberate 60-Hz pacing sleep; 16.667/33.334/100-ms phase limits' },
		[ordered]@{ Gate = 'Designated producer Luau RPC and Event ACK cadence'; State = $(if ($Local.RemoteCadence.Status -ceq 'MEASURED_FAIL' -or $Node.RemoteCadence.Status -ceq 'MEASURED_FAIL') { 'MEASURED_FAIL' } elseif ($Local.RemoteCadence.Status -ceq 'MEASURED_PASS' -and $Node.RemoteCadence.Status -ceq 'MEASURED_PASS') { 'MEASURED_PASS' } else { 'NOT MEASURED' }); Reason = 'hash-indexed client-00 Luau invoke/return and offer/OnClientEvent callback on one local clock; 150/250/500-ms RPC and 250-ms Event RTT/ACK-gap limits' },
		[ordered]@{ Gate = 'Full Character and Remote recipient cadence'; State = 'NOT MEASURED'; Reason = 'Character cross-host service latency and complete per-recipient cadence remain separate; ScaleEvent/ScaleFunction service for the other 31 clients and cross-host one-way latency are not measured' },
		[ordered]@{ Gate = 'Full fairness, overload backpressure and journal retention margin'; State = 'NOT MEASURED'; Reason = 'terminal counters and exact-demand event waits do not prove saturated service, continuous backlog bounds, or the journal high-water margin' },
		[ordered]@{ Gate = 'CPU, memory, network and transport headroom'; State = 'NOT MEASURED'; Reason = 'bounded host/NIC snapshots describe utilization, but no canonical CPU/memory/NIC pass percentage or concurrent packet-level reserve proof follows from those samples' },
		[ordered]@{ Gate = 'Fixed 20-second service recovery'; State = $(if ($Local.Recovery.FixedServiceRecovery -eq 'MEASURED_PASS' -and $Node.Recovery.FixedServiceRecovery -eq 'MEASURED_PASS') { 'MEASURED_PASS' } elseif ($Local.Recovery.FixedServiceRecovery -eq 'MEASURED_FAIL' -or $Node.Recovery.FixedServiceRecovery -eq 'MEASURED_FAIL') { 'MEASURED_FAIL' } else { 'NOT MEASURED' }); Reason = 'three canonical cases per provider, replayed from indexed server/client recovery logs and matched to sealed reconciliation' },
		[ordered]@{ Gate = 'Strict structural convergence sufficient proof'; State = $(if ($Local.Recovery.StrictConvergenceSufficientProof -eq 'MEASURED_PASS' -and $Node.Recovery.StrictConvergenceSufficientProof -eq 'MEASURED_PASS') { 'MEASURED_PASS' } else { 'NOT MEASURED' }); Reason = 'strict snapshot and client final-Name proof only; exact retained-work service bound remains unmeasured' },
		[ordered]@{ Gate = 'Workload-derived exact structural convergence'; State = 'NOT MEASURED'; Reason = 'phase observations and final debt do not locate final accepted byte versus client observation' },
		[ordered]@{ Gate = 'Journal retention margin and overload'; State = 'NOT MEASURED'; Reason = 'final zero journal backlog lacks retained-history high-water and overload chronology' },
		[ordered]@{ Gate = 'Sampled overload journal retention window'; State = $(if ($Local.Recovery.SampledJournalRetention -eq 'MEASURED_PASS' -and $Node.Recovery.SampledJournalRetention -eq 'MEASURED_PASS') { 'MEASURED_PASS' } else { 'NOT MEASURED' }); Reason = 'indexed opportunity and recovery samples remain within 16,384 records with nonnegative observed reader margin; transient between-sample minimum remains unmeasured' },
		[ordered]@{ Gate = 'Full Local/Node provider parity'; State = 'NOT MEASURED'; Reason = 'application service, real TLS, exact convergence, resource headroom and capture gates remain independent' }
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
if (-not [string]::IsNullOrWhiteSpace($NodeStagePath)) {
	$StageRoot = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($NodeStagePath)).TrimEnd('\', '/')
	if ($OutputPath.StartsWith($StageRoot + [IO.Path]::DirectorySeparatorChar,
		[StringComparison]::OrdinalIgnoreCase)) {
		throw 'acceptance observation must remain outside the pinned Node stage'
	}
}
if (Test-Path -LiteralPath $OutputPath) { throw 'acceptance observation path already exists' }
$Bytes = [Text.UTF8Encoding]::new($false).GetBytes(($Observed | ConvertTo-Json -Depth 12))
$Stream = [IO.File]::Open($OutputPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $Stream.Write($Bytes) } finally { $Stream.Dispose() }
Write-Output "[Qualification:FarmAcceptance] INCOMPLETE local=$($Local.Report.RunId) node=$($Node.Report.RunId) output=$OutputPath"
