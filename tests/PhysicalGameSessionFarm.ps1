#requires -Version 7.0
# Bounded 1/4/32 actual-client GameSession/GNS farm preflight. This script
# qualifies client identity, simultaneous readiness, typed results and cleanup;
# it does not by itself qualify the Foundation 3L Local or Node workload matrix.
# Example:
#   pwsh -File tests/PhysicalGameSessionFarm.ps1 -ServerPackageRoot C:\run\server `
#     -PlayerPackageRoot C:\run\player -EvidenceRoot C:\run\evidence `
#     -Endpoint 127.0.0.1:39450 -Peers 32
# Add -ScaleWorkload -ClientFrames 9000 for the canonical five-phase matrix.
# Stage AdmissionFairnessEvidence.ps1 and RecoveryCausalEvidence.ps1 beside this script.

param(
	[Parameter(Mandatory = $true)][string]$ServerPackageRoot,
	[Parameter(Mandatory = $true)][string]$PlayerPackageRoot,
	[Parameter(Mandatory = $true)][string]$EvidenceRoot,
	[ValidatePattern('^[A-Za-z0-9-]{1,64}$')][string]$RunId = ('farm-' + [Guid]::NewGuid().ToString('N')),
	[ValidatePattern('^[0-9.]+:[0-9]+$')][string]$Endpoint = '127.0.0.1:39450',
	[ValidateSet(1, 4, 32)][int]$Peers = 1,
	[switch]$ScaleWorkload,
	[switch]$RecoveryWorkload,
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
. (Join-Path $PSScriptRoot 'AdmissionFairnessEvidence.ps1')
. (Join-Path $PSScriptRoot 'RecoveryCausalEvidence.ps1')
. (Join-Path $PSScriptRoot 'PhysicalFarmPipeDrain.ps1')
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
$FarmStopEvent = $null
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

function Get-Records {
	param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string]$Kind)
	if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return @() }
	$Prefix = "[Qualification:$Kind] "
	$Rows = [Collections.Generic.List[object]]::new()
	$Lines = @(Read-SharedLogLines -Path $Path)
	for ($Index = 0; $Index -lt $Lines.Count; $Index++) {
		if (-not $Lines[$Index].StartsWith($Prefix, [StringComparison]::Ordinal)) { continue }
		$Fields = Get-Fields -Line $Lines[$Index]
		$Fields['__line'] = $Index
		$Rows.Add($Fields)
	}
	return @($Rows)
}

function Get-RecoveryDiagnostics {
	param([Parameter(Mandatory = $true)][string]$Path)
	if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return @() }
	$Prefix = '[Qualification:Recovery] '
	$Rows = [Collections.Generic.List[object]]::new()
	$Lines = @(Read-SharedLogLines -Path $Path)
	for ($Index = 0; $Index -lt $Lines.Count; $Index++) {
		$Offset = $Lines[$Index].IndexOf($Prefix, [StringComparison]::Ordinal)
		if ($Offset -lt 0) { continue }
		$Fields = Get-Fields -Line $Lines[$Index].Substring($Offset + $Prefix.Length)
		$Fields['__line'] = $Index
		$Rows.Add($Fields)
	}
	return @($Rows)
}

function Test-RecoveryQuiescent {
	param([Parameter(Mandatory = $true)]$Sample, [Parameter(Mandatory = $true)][long]$Tail,
		[switch]$RequireSourceConvergence)
	foreach ($Field in @('outstanding', 'active_grants', 'scheduler_queued', 'native_queued',
		'terminal_release')) {
		if (-not $Sample.ContainsKey($Field) -or [long]$Sample[$Field] -ne 0) { return $false }
	}
	if ([long]$Sample.native_observed -ne 32 -or [long]$Sample.feedback_observed -ne 32 -or
		[long]$Sample.accepted -ne [long]$Sample.first_sent -or
		[long]$Sample.accepted -ne [long]$Sample.acked -or
		[long]$Sample.accepted -ne [long]$Sample.retired) { return $false }
	if ($RequireSourceConvergence -and ([long]$Sample.current_tail -ne $Tail -or
		[long]$Sample.journal_backlog -ne 0 -or [long]$Sample.materialization_backlog -ne 0)) {
		return $false
	}
	return $true
}

function Test-RecoveryServiceHealthy {
	param([Parameter(Mandatory = $true)]$Sample)
	foreach ($Field in @('native_observed', 'feedback_observed', 'accepted', 'first_sent',
		'acked', 'retired', 'terminal_release', 'journal_failures')) {
		if (-not $Sample.ContainsKey($Field) -or $Sample[$Field] -cnotmatch '^[0-9]+$') { return $false }
	}
	return [long]$Sample.native_observed -eq 32 -and [long]$Sample.feedback_observed -eq 32 -and
		[long]$Sample.terminal_release -eq 0 -and [long]$Sample.journal_failures -eq 0 -and
		[long]$Sample.accepted -ge [long]$Sample.first_sent -and
		[long]$Sample.first_sent -ge [long]$Sample.acked -and
		[long]$Sample.acked -ge [long]$Sample.retired
}

function Get-RecoveryUnsigned {
	param([Parameter(Mandatory = $true)]$Row, [Parameter(Mandatory = $true)][string]$Field)
	if (-not $Row.ContainsKey($Field) -or $Row[$Field] -cnotmatch '^[0-9]+$') {
		throw "recovery quote lacks unsigned $Field"
	}
	$Value = 0L
	if (-not [long]::TryParse($Row[$Field], [ref]$Value)) {
		throw "recovery quote $Field exceeds the exact integer range"
	}
	return $Value
}

function Get-RecoveryCeilDiv {
	param([Parameter(Mandatory = $true)][long]$Numerator,
		[Parameter(Mandatory = $true)][long]$Denominator)
	if ($Numerator -lt 0 -or $Denominator -le 0) { throw 'invalid recovery service division' }
	$Remainder = 0L
	$Quotient = [math]::DivRem($Numerator, $Denominator, [ref]$Remainder)
	return [long]($Quotient + [long]($Remainder -ne 0))
}

function Assert-RecoveryQuote {
	param([Parameter(Mandatory = $true)]$Diagnostics, [Parameter(Mandatory = $true)][string]$Case,
		[Parameter(Mandatory = $true)][string]$CaseRunId,
		[Parameter(Mandatory = $true)]$ExpectedPeers,
		[Parameter(Mandatory = $true)]$Barrier,
		[Parameter(Mandatory = $true)]$Snapshot,
		[Parameter(Mandatory = $true)]$Deadline,
		[Parameter(Mandatory = $true)]$Samples,
		[Parameter(Mandatory = $true)][long]$LastProbeLine,
		[Parameter(Mandatory = $true)][long]$Tail,
		[Parameter(Mandatory = $true)][long]$MaximumRunMicroseconds,
		[Parameter(Mandatory = $true)][string]$CausalPath,
		[Parameter(Mandatory = $true)][long]$CessationMicroseconds)
	$Causal = Assert-RecoveryCausalEvidence -Path $CausalPath -RunId $CaseRunId -Case $Case `
		-ExpectedConnections @($ExpectedPeers) -CessationMicroseconds $CessationMicroseconds -JournalFence $Tail
	$Captures = @($Diagnostics | Where-Object { $_.event -eq 'quote_capture' -and $_.case -eq $Case -and $_.run -eq $CaseRunId })
	$Results = @($Diagnostics | Where-Object { $_.event -eq 'quote_result' -and $_.case -eq $Case -and $_.run -eq $CaseRunId })
	$Revoked = @($Diagnostics | Where-Object { $_.event -eq 'quote_revoked' -and $_.case -eq $Case -and $_.run -eq $CaseRunId })
	$Peers = @($Diagnostics | Where-Object { $_.event -eq 'quote_peer' -and $_.case -eq $Case -and $_.run -eq $CaseRunId })
	if ($Captures.Count -ne 1 -or $Results.Count -ne 1 -or $Peers.Count -ne 32 -or $Revoked.Count -ne 0 -or
		$Captures[0].status -cne 'READY' -or $Captures[0].reason -cne 'none' -or
		$Results[0].status -cne 'PASS' -or $Results[0].reason -cne 'none' -or $Results[0].contract -cne 'causal_fence_v1' -or
		[long]$Captures[0].__line -ge [long]$Barrier.__line -or
		[long]$Barrier.__line -ge [long]$Results[0].__line -or
		[long]$Results[0].__line -ge [long]$Deadline.__line) {
		throw "recovery case $Case lacks a complete ordered frozen quote audit"
	}
	$Seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	$Total = 0L
	$FutureTotal = 0L
	$MaximumPeerServiceUs = 0L
	foreach ($Peer in $Peers) {
		$Identity = "$(Get-RecoveryUnsigned -Row $Peer -Field 'connection_slot'):$(Get-RecoveryUnsigned -Row $Peer -Field 'connection_generation')"
		if (-not $ExpectedPeers.Contains($Identity) -or -not $Seen.Add($Identity) -or
			[long]$Peer.__line -le [long]$Barrier.__line -or
			[long]$Peer.__line -ge [long]$Results[0].__line) {
			throw "recovery case $Case quote has a wrong or duplicate peer generation"
		}
		$Debt = Get-RecoveryUnsigned -Row $Peer -Field 'accepted_unretired_complete_bytes'
		$Future = Get-RecoveryUnsigned -Row $Peer -Field 'future_complete_bytes'
		$Work = Get-RecoveryUnsigned -Row $Peer -Field 'w_complete_upper_bytes'
		if ($Debt -gt 524288L -or $Work -ne $Debt + $Future -or $Work -gt 42949672960L -or
			$Debt -ne $Causal.Peers[$Identity].InitialDebt -or $Future -ne $Causal.Peers[$Identity].QuoteBytes) {
			throw "recovery case $Case quote exceeds the accepted peer work envelope"
		}
		$Total += $Work
		$FutureTotal += $Future
		$ServiceUs = Get-RecoveryCeilDiv -Numerator ($Work * 1000000L) -Denominator 2097152L
		$MaximumPeerServiceUs = [math]::Max($MaximumPeerServiceUs, $ServiceUs)
	}
	if ($Seen.Count -ne 32 -or $Total -gt 824633720832L -or
		$Total -ne (Get-RecoveryUnsigned -Row $Results[0] -Field 'w_complete_upper_bytes')) {
		throw "recovery case $Case quote does not conserve complete-message bytes"
	}
	$QuotedFrames = Get-RecoveryUnsigned -Row $Results[0] -Field 'quoted_frames'
	if ($QuotedFrames -gt 65536L -or $QuotedFrames -ne $Causal.QuotedFrames) {
		throw "recovery case $Case quote differs from its immutable reference frames"
	}
	$PoolServiceUs = Get-RecoveryCeilDiv -Numerator ($Total * 1000000L) -Denominator 67108864L
	$BoundUs = 20000000L + [math]::Max(470500L + $MaximumPeerServiceUs, $PoolServiceUs)
	if ($BoundUs -gt $MaximumRunMicroseconds) {
		throw "recovery case $Case retained-work deadline exceeds the run hard timeout"
	}
	if ($BoundUs -ne (Get-RecoveryUnsigned -Row $Results[0] -Field 'bound_us') -or
		$BoundUs -ne (Get-RecoveryUnsigned -Row $Deadline -Field 'bound_us') -or
		(Get-RecoveryUnsigned -Row $Deadline -Field 'elapsed_us') -lt $BoundUs) {
		throw "recovery case $Case uses a noncanonical W_i deadline"
	}
	$CausalResults = @($Diagnostics | Where-Object { $_.event -eq 'causal_result' -and $_.case -eq $Case -and $_.run -eq $CaseRunId })
	$CausalPeers = @($Diagnostics | Where-Object { $_.event -eq 'causal_peer' -and $_.case -eq $Case -and $_.run -eq $CaseRunId })
	if ($CausalResults.Count -ne 1 -or $CausalPeers.Count -ne 32 -or $CausalResults[0].status -cne 'PASS' -or
		$CausalResults[0].contract -cne 'causal_fence_v1' -or
		[long]$CausalResults[0].__line -le [long]$Results[0].__line -or [long]$CausalResults[0].__line -ge [long]$Deadline.__line -or
		(Get-RecoveryUnsigned -Row $CausalResults[0] -Field 'events') -ne $Causal.EventCount) {
		throw "recovery case $Case lacks a complete ordered causal summary"
	}
	foreach ($Field in @('accepted', 'first_sent', 'acked', 'retired')) {
		if ((Get-RecoveryUnsigned -Row $CausalResults[0] -Field $Field) -ne $Causal.Totals[$Field]) { throw "recovery $Case causal total mismatch: $Field" }
	}
	$Seen.Clear()
	foreach ($Row in $CausalPeers) {
		$Identity = "$(Get-RecoveryUnsigned -Row $Row -Field 'connection_slot'):$(Get-RecoveryUnsigned -Row $Row -Field 'connection_generation')"
		if (-not $ExpectedPeers.Contains($Identity) -or -not $Seen.Add($Identity) -or
			[long]$Row.__line -le [long]$Results[0].__line -or [long]$Row.__line -ge [long]$CausalResults[0].__line) { throw "recovery $Case invalid causal peer" }
		$Peer = $Causal.Peers[$Identity]
		foreach ($Pair in @(@('journal_fence', 'Fence'), @('cursor', 'Cursor'), @('cut_token', 'Cut'), @('cut_sequence', 'CutSequence'))) {
			if ((Get-RecoveryUnsigned -Row $Row -Field $Pair[0]) -ne $Peer[$Pair[1]]) { throw "recovery $Case causal peer fence mismatch" }
		}
		if ($Row.unresolved -cne '0' -or $Row.planning_unresolved -cne '0' -or $Row.converged -cne '1') { throw "recovery $Case unresolved causal prefix" }
		foreach ($Field in @('accepted', 'first_sent', 'acked', 'retired')) {
			if ((Get-RecoveryUnsigned -Row $Row -Field $Field) -ne $Peer.Prefix[$Field]) { throw "recovery $Case causal prefix mismatch: $Field" }
		}
	}
	$ConvergedUs = Get-RecoveryUnsigned -Row $CausalResults[0] -Field 'prefix_converged_us'
	if ($ConvergedUs -lt $Causal.EarliestConvergedUs -or $ConvergedUs -gt $BoundUs) { throw "recovery $Case causal prefix missed its retained-work deadline" }
	return [ordered]@{ TotalBytes = $Total; BoundUs = [long]$BoundUs;
		ConvergedUs = [long]$ConvergedUs }
}

function Assert-RecoveryRecords {
	param([Parameter(Mandatory = $true)]$Server, [Parameter(Mandatory = $true)]$Clients,
		[Parameter(Mandatory = $true)]$ExpectedNonces,
		[Parameter(Mandatory = $true)][string[]]$ExpectedConnections,
		[long]$MaximumRunMicroseconds = 900000000L)
	$Output = Get-Records -Path $Server.OutputPath -Kind 'Recovery'
	$Diagnostics = @(Get-RecoveryDiagnostics -Path $Server.ErrorPath)
	$Cases = @('gameplay', 'structural', 'mixed')
	$Objects = @($Output | Where-Object { $_.event -eq 'object' -and $_.run -eq $RunId })
	if ($Objects.Count -ne 32) { throw 'recovery workload lacks 32 authoritative ObjectIds' }
	$ObjectByIndex = @{}
	foreach ($Object in $Objects) {
		$Index = [int]$Object.index
		$Identity = "$($Object.object_slot):$($Object.object_generation)"
		if ($Index -lt 0 -or $Index -ge 32 -or $ObjectByIndex.ContainsKey($Index) -or
			[long]$Object.object_slot -eq 0 -or [long]$Object.object_generation -eq 0) {
			throw 'recovery workload has duplicate or invalid authoritative ObjectId'
		}
		$ObjectByIndex[$Index] = $Identity
	}
	$ExpectedPeers = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	foreach ($Connection in $ExpectedConnections) { [void]$ExpectedPeers.Add($Connection) }
	if ($ExpectedPeers.Count -ne 32) { throw 'recovery workload lacks original peer generations' }
	$CaseObservations = [Collections.Generic.List[object]]::new()
	foreach ($CaseIndex in 0..2) {
		$Case = $Cases[$CaseIndex]
		$Starts = @($Output | Where-Object { $_.event -eq 'case_start' -and $_.case -eq $Case -and $_.run -eq $RunId })
		$Ends = @($Output | Where-Object { $_.event -eq 'all_opportunities' -and $_.case -eq $Case -and $_.run -eq $RunId })
		$Cessations = @($Output | Where-Object { $_.event -eq 'cessation' -and $_.case -eq $Case -and $_.run -eq $RunId })
		$Offers = @($Output | Where-Object { $_.event -eq 'structural_offer' -and $_.case -eq $Case -and $_.run -eq $RunId })
		$Retention = @($Output | Where-Object { $_.event -eq 'retention' -and $_.case -eq $Case -and $_.run -eq $RunId })
		$ExpectedOffers = if ($Case -eq 'gameplay') { 0 } else { 480 }
		if ($Starts.Count -ne 1 -or $Ends.Count -ne 1 -or $Cessations.Count -ne 1 -or
			$Offers.Count -ne $ExpectedOffers -or $Retention.Count -ne $ExpectedOffers -or
			[long]$Ends[0].opportunities -ne 480 -or
			[long]$Ends[0].elapsed_us -lt 7983000 -or
			[long]$Cessations[0].tick -le [long]$Ends[0].tick -or
			[long]$Cessations[0].monotonic_us -le [long]$Starts[0].monotonic_us) {
			throw "recovery case $Case lacks bounded 480-opportunity cessation evidence"
		}
		$Tail = Get-RecoveryUnsigned -Row $Cessations[0] -Field 'journal_tail'
		$MutationSamples = Get-RecoveryUnsigned -Row $Cessations[0] -Field 'retention_mutation_samples'
		if ($MutationSamples -ne 16 * $ExpectedOffers) {
			throw "recovery case $Case lacks mutation-point journal retention coverage"
		}
		if ($ExpectedOffers -gt 0) {
			$MinimumObservedRetentionMargin = [long]::MaxValue
			$MaximumObservedRetained = 0L
			for ($Index = 0; $Index -lt 480; $Index++) {
				$Offer = $Offers[$Index]
				$RetentionRow = $Retention[$Index]
				$Oldest = Get-RecoveryUnsigned -Row $RetentionRow -Field 'oldest'
				$Required = Get-RecoveryUnsigned -Row $RetentionRow -Field 'required'
				$Margin = Get-RecoveryUnsigned -Row $RetentionRow -Field 'margin'
				$Retained = Get-RecoveryUnsigned -Row $RetentionRow -Field 'retained'
				$SampleCount = Get-RecoveryUnsigned -Row $RetentionRow -Field 'mutation_samples'
				if ([int]$Offer.opportunity -ne $Index + 1 -or [int]$Offer.mutations -ne 16 -or
					[int]$Offer.name_bytes -ne 24576 -or
					[int]$RetentionRow.opportunity -ne $Index + 1 -or
					[long]$Offer.__line -ge [long]$RetentionRow.__line -or
					($Index -lt 479 -and [long]$RetentionRow.__line -ge [long]$Offers[$Index + 1].__line) -or
					$Oldest -lt 1 -or $Required -lt $Oldest -or $Required -gt $Tail -or
					$Margin -ne ($Required - $Oldest) -or $Retained -gt 16384 -or
					$SampleCount -ne 16 * ($Index + 1) -or
					($Index -gt 0 -and [long]$Offer.monotonic_us - [long]$Offers[$Index - 1].monotonic_us -lt 16667)) {
					throw "recovery case $Case has an invalid structural offer cadence"
				}
				$MinimumObservedRetentionMargin = [math]::Min($MinimumObservedRetentionMargin, $Margin)
				$MaximumObservedRetained = [math]::Max($MaximumObservedRetained, $Retained)
			}
			if ($MinimumObservedRetentionMargin -lt
				(Get-RecoveryUnsigned -Row $Cessations[0] -Field 'minimum_retention_margin') -or
				$MaximumObservedRetained -ne
				(Get-RecoveryUnsigned -Row $Cessations[0] -Field 'retained_high')) {
				throw "recovery case $Case cessation summary differs from 480 native retention samples"
			}
		}
		$ExpectedRaw = if ($Case -eq 'gameplay') { 0L } else { 188743680L }
		if ($Tail -le 0 -or [long]$Cessations[0].raw_name_bytes -ne $ExpectedRaw -or
			[long]$Cessations[0].minimum_retention_margin -lt 0 -or
			[long]$Cessations[0].retained_high -lt 0 -or
			[long]$Cessations[0].retained_high -gt 16384) {
			throw "recovery case $Case cessation accounting is invalid"
		}
		$Barriers = @($Diagnostics | Where-Object { $_.event -eq 'cessation_barrier' -and $_.case -eq $Case -and $_.run -eq $RunId })
		$Deadlines = @($Diagnostics | Where-Object { $_.event -eq 'strict_deadline_barrier' -and $_.case -eq $Case -and $_.run -eq $RunId })
		$Snapshots = @($Diagnostics | Where-Object { $_.event -eq 'strict_snapshot' -and $_.case -eq $Case -and $_.run -eq $RunId })
		if ($Barriers.Count -ne 1 -or $Deadlines.Count -ne 1 -or $Snapshots.Count -ne 1 -or
			$Barriers[0].journal_tail -ne [string]$Tail -or
			[long]$Barriers[0].__line -ge [long]$Snapshots[0].__line -or
			[long]$Snapshots[0].__line -ge [long]$Deadlines[0].__line -or
			$Snapshots[0].retained_work_bytes -ne 'NOT_MEASURED') {
			throw "recovery case $Case lacks an ordered strict snapshot"
		}
		$Window = @($Diagnostics | Where-Object { [long]$_.__line -gt [long]$Barriers[0].__line -and
			[long]$_.__line -lt [long]$Deadlines[0].__line -and $_.case -eq $Case })
		$Ready = @($Diagnostics | Where-Object { $_.event -eq 'ready_ack' -and $_.case -eq $Case })
		$Offered = @($Diagnostics | Where-Object { $_.event -eq 'offered_ack' -and $_.case -eq $Case })
		foreach ($AckRows in @($Ready, $Offered)) {
			$Seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
			foreach ($Ack in $AckRows) {
				if (-not $Seen.Add("$($Ack.peer_slot):$($Ack.peer_generation)")) { throw "recovery $Case duplicate peer acknowledgement" }
			}
			if ($Seen.Count -ne 32 -or @($Seen | Where-Object { -not $ExpectedPeers.Contains($_) }).Count -ne 0) {
				throw "recovery $Case lacks all original ready/offered peers"
			}
		}
		$ProbeRows = @($Window | Where-Object { $_.event -eq 'probe_ack' })
		$NameRows = @($Window | Where-Object { $_.event -eq 'name_ack' })
		$ProbePeers = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
		foreach ($Ack in $ProbeRows) {
			if (-not $ProbePeers.Add("$($Ack.peer_slot):$($Ack.peer_generation)")) { throw "recovery $Case has duplicate probe ACK" }
		}
		if ($ProbePeers.Count -ne 32 -or @($ProbePeers | Where-Object { -not $ExpectedPeers.Contains($_) }).Count -ne 0) {
			throw "recovery $Case lacks 32 ordinary probe ACKs"
		}
		if ($Case -ne 'gameplay') {
			$NamePeers = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
			foreach ($Ack in $NameRows) {
				if (-not $NamePeers.Add("$($Ack.peer_slot):$($Ack.peer_generation)")) { throw "recovery $Case has duplicate Name ACK" }
			}
			if ($NamePeers.Count -ne 32 -or @($NamePeers | Where-Object { -not $ExpectedPeers.Contains($_) }).Count -ne 0) {
				throw "recovery $Case lacks 32 final Name ACKs"
			}
		} elseif ($NameRows.Count -ne 0) { throw 'gameplay recovery unexpectedly has Name ACKs' }
		$Samples = @($Window | Where-Object { $_.event -eq 'sample' })
		if ($Samples.Count -eq 0) { throw "recovery $Case lacks native service samples" }
		$ObservedRetainedHigh = Get-RecoveryUnsigned -Row $Cessations[0] -Field 'retained_high'
		$ObservedMinimumMargin = Get-RecoveryUnsigned -Row $Cessations[0] -Field 'minimum_retention_margin'
		$PreviousTail = $Tail
		foreach ($Sample in $Samples) {
			$Oldest = Get-RecoveryUnsigned -Row $Sample -Field 'oldest'
			$Required = Get-RecoveryUnsigned -Row $Sample -Field 'required'
			$Margin = Get-RecoveryUnsigned -Row $Sample -Field 'margin'
			$Retained = Get-RecoveryUnsigned -Row $Sample -Field 'retained'
			$ObservedRetainedHigh = [math]::Max($ObservedRetainedHigh, $Retained)
			$ObservedMinimumMargin = [math]::Min($ObservedMinimumMargin, $Margin)
			$CurrentTail = Get-RecoveryUnsigned -Row $Sample -Field 'current_tail'
			if ($Oldest -lt 1 -or $Required -lt $Oldest -or $Required -gt $CurrentTail -or
				$Margin -ne ($Required - $Oldest) -or $Retained -gt 16384 -or
				(Get-RecoveryUnsigned -Row $Sample -Field 'retained_high') -ne $ObservedRetainedHigh -or
				(Get-RecoveryUnsigned -Row $Sample -Field 'minimum_retention_margin') -ne $ObservedMinimumMargin -or
				[long]$Sample.journal_failures -ne 0 -or $CurrentTail -lt $PreviousTail) {
				throw "recovery $Case lost retained journal coverage"
			}
			$PreviousTail = $CurrentTail
		}
		$LastProbeLine = [long]($ProbeRows | Measure-Object -Property __line -Maximum).Maximum
		$Service = @($Samples | Where-Object { [long]$_.__line -gt $LastProbeLine -and
			[long]$_.elapsed_us -le 20000000 -and (Test-RecoveryServiceHealthy -Sample $_) })
		$ServiceState = if ($Service.Count -gt 0) { 'MEASURED_PASS' } else { 'MEASURED_FAIL' }
		$SnapshotSample = @($Samples | Where-Object { [long]$_.__line -lt [long]$Snapshots[0].__line -and
			[long]$_.elapsed_us -eq [long]$Snapshots[0].elapsed_us })
		$Readers = @($Window | Where-Object { $_.event -eq 'reader' -and
			[long]$_.__line -lt [long]$Snapshots[0].__line })
		$SnapshotReaderPeers = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
		$SnapshotCatalog = 0
		$SnapshotReadersPresent = $Readers.Count -eq 33
		foreach ($Reader in $Readers) {
			if ($Reader.catalog -eq '1') { $SnapshotCatalog++; continue }
			$Key = "$($Reader.connection_slot):$($Reader.connection_generation)"
			if (-not $SnapshotReaderPeers.Add($Key) -or -not $ExpectedPeers.Contains($Key)) {
				$SnapshotReadersPresent = $false
			}
		}
		if ($SnapshotCatalog -ne 1 -or $SnapshotReaderPeers.Count -ne 32) {
			$SnapshotReadersPresent = $false
		}
		$LastNameLine = if ($Case -eq 'gameplay') { 0L } else {
			[long]($NameRows | Measure-Object -Property __line -Maximum).Maximum
		}
		$Quote = Assert-RecoveryQuote -Diagnostics $Diagnostics -Case $Case -CaseRunId $RunId `
			-ExpectedPeers $ExpectedPeers -Barrier $Barriers[0] -Snapshot $Snapshots[0] `
			-Deadline $Deadlines[0] -Samples $Samples `
			-LastProbeLine $LastProbeLine -Tail $Tail -MaximumRunMicroseconds $MaximumRunMicroseconds `
			-CausalPath (Join-Path (Split-Path $Server.ErrorPath -Parent) "recovery-$Case.tsv") `
			-CessationMicroseconds ([long]$Cessations[0].monotonic_us)
		$TerminalReaders = @($Window | Where-Object { $_.event -eq 'terminal_reader' -and
			[long]$_.__line -gt [long]$Snapshots[0].__line -and
			[long]$_.__line -lt [long]$Deadlines[0].__line })
		$TerminalReaderPeers = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
		$TerminalCatalog = 0
		$TerminalReadersHealthy = $TerminalReaders.Count -eq 33
		foreach ($Reader in $TerminalReaders) {
			if ($Reader.catalog -eq '1') {
				$TerminalCatalog++
				if ([long]$Reader.next_sequence -lt $Tail) { $TerminalReadersHealthy = $false }
				continue
			}
			$Key = "$($Reader.connection_slot):$($Reader.connection_generation)"
			if (-not $TerminalReaderPeers.Add($Key) -or -not $ExpectedPeers.Contains($Key) -or
				[long]$Reader.next_sequence -lt $Tail) { $TerminalReadersHealthy = $false }
		}
		if ($TerminalCatalog -ne 1 -or $TerminalReaderPeers.Count -ne 32) { $TerminalReadersHealthy = $false }
		$TerminalSamples = @($Samples | Where-Object { [long]$_.__line -gt $LastProbeLine -and
			[long]$_.__line -gt $LastNameLine -and
			[long]$_.elapsed_us -le $Quote.BoundUs -and
			(Test-RecoveryServiceHealthy -Sample $_) } |
			Sort-Object { [long]$_.elapsed_us })
		$Strict = [long]$Snapshots[0].elapsed_us -ge 20000000 -and
			[long]$Snapshots[0].elapsed_us -le 20470500 -and
			$SnapshotSample.Count -eq 1 -and $SnapshotReadersPresent -and
			[long]$Snapshots[0].__line -gt $LastProbeLine -and
			(Test-RecoveryServiceHealthy -Sample $SnapshotSample[0]) -and
			$TerminalReadersHealthy -and $TerminalSamples.Count -gt 0
		$CaseObservations.Add([ordered]@{
			Case = $Case; Opportunities = 480; StructuralOffers = $Offers.Count
			CessationJournalTail = $Tail; RawNameHistoryBytes = $ExpectedRaw
			RetainedHighWaterRecords = [long]$Cessations[0].retained_high
			MinimumRetentionMarginRecords = [long]$Cessations[0].minimum_retention_margin
			RetentionMutationSamples = $MutationSamples
			FixedServiceRecovery = $ServiceState
			StrictConvergenceSufficientProof = $(if ($Strict) { 'MEASURED_PASS' } else { 'INCONCLUSIVE_NOT_MEASURED' })
			StrictSnapshotElapsedUs = [long]$Snapshots[0].elapsed_us
			ExactRetainedWorkBytes = $Quote.TotalBytes
			RetainedWorkBoundUs = $Quote.BoundUs
			ObservedStructuralConvergenceUs = $Quote.ConvergedUs
		})
	}
	foreach ($Slot in 0..31) {
		$Client = $Clients[$Slot]
		$ClientOutput = Get-Records -Path $Client.OutputPath -Kind 'Client'
		$ClientDiagnostics = @(Get-RecoveryDiagnostics -Path $Client.ErrorPath)
		foreach ($Case in $Cases) {
			$Offered = @($ClientDiagnostics | Where-Object { $_.event -eq 'client_offered' -and $_.case -eq $Case })
			$Probes = @($ClientDiagnostics | Where-Object { $_.event -eq 'client_probes' -and $_.case -eq $Case })
			$ExpectedRpc = if ($Case -eq 'structural') { 1 } else { 16 }
			$ExpectedEvents = if ($Case -eq 'structural') { 60 } else { 480 }
			if ($Offered.Count -ne 1 -or $Probes.Count -ne 1 -or
				[int]$Offered[0].opportunities -ne 480 -or
				[int]$Offered[0].rpc_completed + [int]$Offered[0].rpc_errors -ne $ExpectedRpc -or
				($Case -eq 'structural' -and [int]$Offered[0].rpc_errors -ne 0) -or
				[int]$Offered[0].event_attempts -ne $ExpectedEvents -or
				[int]$Offered[0].event_offers -gt $ExpectedEvents -or
				[int]$Offered[0].event_acks -gt [int]$Offered[0].event_offers -or
				[int]$Probes[0].rpc_acks -ne 10 -or [int]$Probes[0].rpc_errors -ne 0 -or
				[int]$Probes[0].event_acks -ne 10 -or [int]$Probes[0].rpc_p95_us -gt 150000 -or
				[int]$Probes[0].rpc_p99_us -gt 250000 -or [int]$Probes[0].rpc_max_us -gt 500000 -or
				[int]$Probes[0].event_max_us -gt 250000) {
				throw "client $Slot lacks canonical $Case overload/ordinary recovery probes"
			}
			if ($Case -eq 'gameplay') { continue }
			$Names = @($ClientOutput | Where-Object { $_.event -eq 'name_object' -and $_.case -eq $Case })
			$Summary = @($ClientOutput | Where-Object { $_.event -eq 'names_observed' -and $_.case -eq $Case })
			$Observed = @($ClientDiagnostics | Where-Object { $_.event -eq 'client_names' -and $_.case -eq $Case })
			if ($Names.Count -ne 32 -or $Summary.Count -ne 1 -or $Observed.Count -ne 1 -or
				$Summary[0].objects -ne '32' -or $Observed[0].objects -ne '32') {
				throw "client $Slot lacks 32 final $Case ObjectId/Name observations"
			}
			$Seen = [Collections.Generic.HashSet[int]]::new()
			$CaseIndex = if ($Case -eq 'structural') { 1 } else { 2 }
			foreach ($Name in $Names) {
				$Index = [int]$Name.index
				$Opportunity = if ($Index -lt 16) { 479 } else { 480 }
				$Letter = [char]([int][char]'a' + (($CaseIndex * 7 + $Opportunity + $Index) % 26))
				if ($Index -lt 0 -or $Index -ge 32 -or -not $Seen.Add($Index) -or
					$Name.run_id -ne $RunId -or $Name.slot -ne [string]$Slot -or
					$Name.nonce -ne $ExpectedNonces[$Slot] -or
					"$($Name.object_slot):$($Name.object_generation)" -ne $ObjectByIndex[$Index] -or
					$Name.letter -cne [string]$Letter -or $Name.name_bytes -ne '24576') {
					throw "client $Slot $Case observed a wrong ObjectId or final Name"
				}
			}
		}
	}
	$ServiceFailed = @($CaseObservations | Where-Object FixedServiceRecovery -ne 'MEASURED_PASS').Count -gt 0
	$ConvergenceIncomplete = @($CaseObservations |
		Where-Object StrictConvergenceSufficientProof -ne 'MEASURED_PASS').Count -gt 0
	$State = if ($ServiceFailed) { 'MEASURED_FAIL' } elseif ($ConvergenceIncomplete) {
		'INCONCLUSIVE_NOT_MEASURED'
	} else { 'MEASURED_PASS' }
	$TotalRetainedAcrossCases = 0L
	foreach ($Observation in $CaseObservations) { $TotalRetainedAcrossCases += [long]$Observation.ExactRetainedWorkBytes }
	return [ordered]@{ State = $State; Cases = @($CaseObservations);
		ExactRetainedWorkBytes = $TotalRetainedAcrossCases }
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
			OutputCopy = [PhysicalFarmPipeDrain]::Start($Process.StandardOutput.BaseStream, $OutputStream)
			ErrorCopy = [PhysicalFarmPipeDrain]::Start($Process.StandardError.BaseStream, $ErrorStream)
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

function Test-SealedFarmPublicationTrace {
	param([Parameter(Mandatory = $true)][string]$Path,
		[Parameter(Mandatory = $true)][string]$ExpectedRunId)
	if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
	$Stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
	try {
		$HeaderBytes = [Collections.Generic.List[byte]]::new()
		while ($HeaderBytes.Count -lt 512) {
			$Value = $Stream.ReadByte()
			if ($Value -lt 0) { return $false }
			$HeaderBytes.Add([byte]$Value)
			if ($Value -eq 10) { break }
		}
		if ($HeaderBytes[$HeaderBytes.Count - 1] -ne 10) { return $false }
		$Header = [Text.Encoding]::ASCII.GetString($HeaderBytes.ToArray())
		$Prefix = "format=GargantuanFarmPublicationV1`trun=$ExpectedRunId`trole=SERVER`tslot=-1`tnonce=0`t"
		if (-not $Header.StartsWith($Prefix, [StringComparison]::Ordinal)) { return $false }
		if ($Header -cnotmatch 'count=([0-9]+)\x09dropped=0\x09decode_failures=0\x0A$') { return $false }
		$Count = [long]$Matches[1]
		return $Stream.Length -eq ($HeaderBytes.Count + 80L * $Count)
	} finally { $Stream.Dispose() }
}

function Stop-FarmProcesses {
	param([bool]$Failed)
	if (-not $Failed) {
		foreach ($Owner in $AllProcesses) { Stop-RunProcess -Owner $Owner }
		return
	}
	# Once a child has failed, the run is already invalid. Let the server observe
	# client departure and seal its bounded native traces before force cleanup.
	$ServerOwner = $null
	foreach ($Owner in $AllProcesses) {
		if ($Owner.Label -ceq 'server') { $ServerOwner = $Owner }
		else { Stop-RunProcess -Owner $Owner }
	}
	if ($ServerOwner) {
		try {
			if (-not $ServerOwner.Process.HasExited) {
				if ($FarmStopEvent -and -not $FarmStopEvent.Set()) {
					throw 'server diagnostic stop event was not signaled'
				}
				if (-not $ServerOwner.Process.WaitForExit(3000)) {
					$CleanupErrors.Add('server diagnostic trace did not seal within 3000 ms')
				}
			}
		} catch {
			$CleanupErrors.Add("server diagnostic grace failed: $($_.Exception.Message)")
		}
		Stop-RunProcess -Owner $ServerOwner
		if ($ScaleWorkload -and -not (Test-SealedFarmPublicationTrace `
			-Path (Join-Path $RunDirectory 'publication-service.bin') -ExpectedRunId $RunId)) {
			$CleanupErrors.Add('server native publication trace was not sealed or is incomplete')
		}
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
	# At 32 clients plus the server, a 420-second run needs at most 6,963
	# process records at two-second intervals; the 900-second maximum needs 14,883.
	if ($ResourceClock.ElapsedMilliseconds - $script:LastResourceSampleMilliseconds -lt 2000) { return }
	if ($ResourceSamples.Count + $AllProcesses.Count -gt 20000) {
		throw 'bounded process resource evidence exceeded 20000 records'
	}
	$script:LastResourceSampleMilliseconds = $ResourceClock.ElapsedMilliseconds
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
		UniqueConnections = $Connections.Count; UniquePlayers = $Players.Count;
		Connections = @($Connections) }
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
		$ContentAcks = @($Records | Where-Object { $_.event -eq 'content_acks' -and $_.phase -eq $Name -and $_.run -eq $RunId })
		$Stops = @($Records | Where-Object { $_.event -eq 'phase_stop_requested' -and $_.phase -eq $Name -and $_.run -eq $RunId })
		$Producer = @($Records | Where-Object { $_.event -eq 'producer_ack' -and $_.phase -eq $Name -and $_.run -eq $RunId })
		if ($Starts.Count -ne 1 -or $Ends.Count -ne 1 -or $Acks.Count -ne 1 -or $ContentAcks.Count -ne 1 -or
			$Stops.Count -ne 1 -or $Producer.Count -ne 1 -or $Acks[0].count -ne '32' -or
			$ContentAcks[0].count -ne '32' -or $Ends[0].phase_acks -ne '32' -or
			$Ends[0].content_acks -ne '32' -or
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
			[long]$ContentAcks[0].tick -lt $StartTick -or [long]$ContentAcks[0].tick -gt [long]$Stops[0].tick -or
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
				[double]$Record.event_max_gap_us -gt 250000 -or
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
	if ($RecoveryWorkload -and (-not $ScaleWorkload -or $ClientFrames -lt 18000)) {
		throw 'RecoveryWorkload requires ScaleWorkload and at least 18000 client frames'
	}
	if ($RecoveryWorkload -and -not $PSBoundParameters.ContainsKey('RunTimeoutMilliseconds')) {
		$RunTimeoutMilliseconds = 420000
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
	if ($RecoveryWorkload) { $ServerTicks = [Math]::Max(19000, $ServerTicks) }
	$Manifest = [ordered]@{
		RunId = $RunId; Purpose = $(if ($RecoveryWorkload) { 'five-phase-plus-recovery-farm-preflight' } elseif ($ScaleWorkload) { 'five-phase-scale-control-preflight' } else { 'actual-GameSession-farm-preflight' }); Provider = $Provider
		RecoveryWorkload = [bool]$RecoveryWorkload
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
	if ($ScaleWorkload) {
		$FarmStopEventName = "Local\GargantuanFarmStop-$RunId"
		$CreatedNew = $false
		$FarmStopEvent = [Threading.EventWaitHandle]::new($false,
			[Threading.EventResetMode]::ManualReset, $FarmStopEventName, [ref]$CreatedNew)
		if (-not $CreatedNew) { throw 'farm diagnostic stop event already exists' }
	}
	$ServerArguments = @('--bind', $Endpoint, '--farm-run-id', $RunId, '--farm-peers', [string]$Peers,
		'--max-ticks', [string]$ServerTicks, '--reliable-mode', 'POOLED_SERVICE',
		'--content-provider', $Provider.ToLowerInvariant())
	if ($ScaleWorkload) { $ServerArguments += @('--farm-scale-workload',
		'--farm-admission-evidence', (Join-Path $RunDirectory 'admission-fairness.tsv'),
		'--farm-publication-evidence', (Join-Path $RunDirectory 'publication-service.bin'),
		'--farm-diagnostic-stop-event', $FarmStopEventName,
		'--content-residency', 'on-demand') }
	if ($RecoveryWorkload) { $ServerArguments += '--farm-recovery-workload' }
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
		if (@(Get-Records -Path $Server.OutputPath -Kind 'Server' | Where-Object {
			$_.event -eq 'start' -and $_.run -eq $RunId }).Count -eq 1) { break }
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
		if ($ScaleWorkload) { $Arguments += @('--farm-scale-workload',
			'--farm-publication-evidence', (Join-Path $RunDirectory ('publication-service-{0}.bin' -f $Slot))) }
		if ($RecoveryWorkload) { $Arguments += '--farm-recovery-workload' }
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
	$RecoveryObservation = if ($RecoveryWorkload) {
		Assert-RecoveryRecords -Server $Server -Clients $Clients -ExpectedNonces $ExpectedNonces `
			-ExpectedConnections $Identity.Connections `
			-MaximumRunMicroseconds ([long]$RunTimeoutMilliseconds * 1000L)
	} else { $null }
	$FairnessObservation = if ($ScaleWorkload) {
		Read-AdmissionFairnessEvidence -Path (Join-Path $RunDirectory 'admission-fairness.tsv') `
			-RunId $RunId -ExpectedConnections $Identity.Connections
	} else { $null }
	$Result = [ordered]@{
		RunId = $RunId; Status = 'PASS'; Gate = $(if ($ScaleWorkload) { 'five-phase-scale-control-preflight' } else { 'actual-GameSession-farm-preflight' })
		Provider = $Provider; Peers = $Peers; Ready = $Identity.Ready
		UniqueNonces = $Identity.UniqueNonces; UniqueConnections = $Identity.UniqueConnections
		UniquePlayers = $Identity.UniquePlayers; ServerPid = $Server.Pid
		ClientPids = @($Clients | ForEach-Object Pid)
		ResourceSamples = $ResourceSamples.Count; ResourceEvidence = 'process-resources.csv'
		CompletedUtc = [DateTimeOffset]::UtcNow.ToString('O')
	}
	if ($ScaleWorkload) { $Result.AdmissionFairnessObservation = $FairnessObservation }
	if ($RecoveryWorkload) {
		$Result.RecoveryObservation = $RecoveryObservation
		if ($RecoveryObservation.State -ne 'MEASURED_PASS') {
			$Result.Status = if ($RecoveryObservation.State -eq 'MEASURED_FAIL') { 'FAIL' } else { 'INCOMPLETE' }
			$Failure = "recovery workload $($RecoveryObservation.State)"
		}
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
	Stop-FarmProcesses -Failed ($Result.Status -ne 'PASS')
	if ($FarmStopEvent) { $FarmStopEvent.Dispose() }
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
