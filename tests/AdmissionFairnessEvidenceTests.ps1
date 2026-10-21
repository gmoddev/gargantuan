$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'AdmissionFairnessEvidence.ps1')

$RunId = 'fairness-analyzer-test'
$Root = Join-Path ([IO.Path]::GetTempPath()) ('gargantuan-fairness-analyzer-' + [Guid]::NewGuid().ToString('N'))
$Path = Join-Path $Root 'admission-fairness.tsv'
[void][IO.Directory]::CreateDirectory($Root)

function New-Event {
	param([string]$Kind, [UInt64]$Demand, [UInt64]$At, [UInt64]$Slot = 1,
		[UInt64]$Generation = 1, [string]$Reason = 'none', [UInt64]$Episode = 0,
		[UInt64]$Token = 0, [UInt64]$Bytes = 100, [UInt64]$CreditAt = 0,
		[UInt64]$EligibleAt = 0, [UInt64]$PeerCredit = 0,
		[UInt64]$GlobalCredit = 524288, [UInt64]$ActiveGrants = [UInt64]($Kind -ceq 'grant_accepted'),
		[UInt64]$GrantDeferrals = 0, [UInt64]$FundedDeferrals = 0,
		[UInt64]$CreditDeferrals = 0, [UInt64]$FairnessDeferrals = 0)
	return (@('event', $Kind, $Reason, $Slot, $Generation, $Demand, $Episode, $Token,
		$Bytes, $At, $CreditAt, $EligibleAt, $PeerCredit, $GlobalCredit, $ActiveGrants,
		$GrantDeferrals, $FundedDeferrals, $CreditDeferrals, $FairnessDeferrals) -join "`t")
}

function Write-Trace {
	param([string[]]$Events, [string]$Trailer, [int]$Version = 1)
	$Lines = @("format=GargantuanAdmissionEvidenceV$Version`trun=$RunId") + $Events + @($Trailer)
	if ($Version -eq 3) { $Lines[0] += "`tcontract=HYBRID_F1_V1" }
	[IO.File]::WriteAllText($Path, ($Lines -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
}

function Assert-Rejected {
	param([string[]]$Events, [string]$Case, [int]$Version = 1)
	Write-Trace -Events $Events -Trailer "end`t$($Events.Count)`t0" -Version $Version
	$Rejected = $false
	try {
		[void](Read-AdmissionFairnessEvidence -Path $Path -RunId $RunId `
			-ExpectedConnections @('1:1', '2:1', '3:1', '4:1'))
	} catch { $Rejected = $true }
	if (-not $Rejected) { throw "analyzer accepted invalid $Case" }
}

function Read-HybridTrace([string[]]$Rows) {
	Write-Trace -Events $Rows -Trailer "end`t$($Rows.Count)`t0" -Version 3
	return Read-AdmissionFairnessEvidence -Path $Path -RunId $RunId
}

function New-LineageTrace([UInt64]$GrantAt) {
	return @(
		(New-Event exact_demand 100 1000),
		(New-Event credit_eligible 100 1100 -Episode 1 -CreditAt 1050 -EligibleAt 1100 -PeerCredit 100),
		(New-Event demand_disposed 100 401100 -Reason replaced -Episode 1 -CreditAt 1050 -EligibleAt 1100 -PeerCredit 100),
		(New-Event exact_demand 101 401100 -Bytes 90 -PeerCredit 100),
		(New-Event credit_eligible 101 401100 -Bytes 90 -Episode 1 -CreditAt 401100 -EligibleAt 401100 -PeerCredit 100),
		(New-Event demand_disposed 101 801100 -Bytes 90 -Reason replaced -Episode 1 -CreditAt 401100 -EligibleAt 401100 -PeerCredit 100),
		(New-Event exact_demand 102 801100 -Bytes 80 -PeerCredit 100),
		(New-Event credit_eligible 102 801100 -Bytes 80 -Episode 1 -CreditAt 801100 -EligibleAt 801100 -PeerCredit 100),
		(New-Event grant_accepted 102 $GrantAt -Bytes 80 -Episode 1 -Token 900 -CreditAt 801100 -EligibleAt 801100),
		(New-Event grant_retired 102 ($GrantAt+1) -Bytes 80 -Token 900 -ActiveGrants 1),
		(New-Event grant_released 102 ($GrantAt+2) -Bytes 80 -Token 900 -ActiveGrants 0)
	)
}

try {
	$Events = @(
		(New-Event exact_demand 1 1000),
		(New-Event credit_eligible 1 1100 -Episode 1 -CreditAt 1050 -EligibleAt 1100 -PeerCredit 100),
		(New-Event eligibility_interrupted 1 1200 -Reason feedback_unavailable -Episode 1 -CreditAt 1050 -EligibleAt 1100),
		(New-Event credit_eligible 1 1300 -Episode 2 -CreditAt 1050 -EligibleAt 1300 -PeerCredit 100),
		(New-Event reservation_rolled_back 1 1350 -Reason rollback -Episode 2 -Token 1 -CreditAt 1050 -EligibleAt 1300),
		(New-Event grant_accepted 1 1500 -Episode 2 -Token 2 -CreditAt 1050 -EligibleAt 1300),
		(New-Event exact_demand 2 1600 -Slot 2 -Bytes 77),
		(New-Event credit_eligible 2 1700 -Slot 2 -Bytes 77 -Episode 1 -CreditAt 1650 -EligibleAt 1700 -PeerCredit 77),
		(New-Event demand_disposed 2 1800 -Slot 2 -Bytes 77 -Reason replaced -Episode 1 -CreditAt 1650 -EligibleAt 1700),
		(New-Event exact_demand 3 1801 -Slot 2 -Bytes 77),
		(New-Event credit_eligible 3 1900 -Slot 2 -Bytes 77 -Episode 1 -CreditAt 1850 -EligibleAt 1900 -PeerCredit 77),
		(New-Event demand_disposed 3 2000 -Slot 2 -Bytes 77 -Reason no_work -Episode 1 -CreditAt 1850 -EligibleAt 1900),
		(New-Event exact_demand 4 2100 -Slot 3 -Bytes 90),
		(New-Event credit_eligible 4 2200 -Slot 3 -Bytes 90 -Episode 1 -CreditAt 2150 -EligibleAt 2200 -PeerCredit 90),
		(New-Event exact_demand 5 2500 -Slot 4 -Bytes 80),
		(New-Event demand_disposed 5 2600 -Slot 4 -Bytes 80 -Reason no_work)
	)
	Write-Trace -Events $Events -Trailer "end`t$($Events.Count)`t0"
	$Result = Read-AdmissionFairnessEvidence -Path $Path -RunId $RunId `
		-ExpectedConnections @('1:1', '2:1', '3:1', '4:1')
	if ($Result.Classification -cne 'EVIDENCE_INTEGRITY_AND_OBSERVED_TIMING_ONLY' -or
		$Result.EventCount -ne 16 -or $Result.ExactDemandCount -ne 5 -or
		$Result.EverEligibleDemandCount -ne 4 -or $Result.GrantedCount -ne 1 -or
		$Result.DisposedEverEligibleDemandCount -ne 2 -or
		$Result.DisposedWhileEligibleCount -ne 2 -or $Result.OpenDemandCount -ne 1 -or
		$Result.InterruptedEligibilityEpisodeCount -ne 1 -or
		$Result.AcceptedGrantWaitBound -cne 'MEASURED_PASS' -or
		$Result.ExactDemandEpisodeCoverage -cne 'INCONCLUSIVE_NOT_MEASURED' -or
		$Result.RetainedEligibleWaiterCount -ne 1 -or $Result.OpenAfterInterruptionCount -ne 0 -or
		$Result.RetainedEligibleWaiters[0].ObservedAgeLowerBoundMicroseconds -ne 400 -or
		$Result.MaximumObservedEligibilityToGrantMicroseconds -ne 200 -or
		$Result.TraceCanonicalCounterBounds -cne 'MEASURED_PASS' -or
		$Result.MaximumTraceActiveGrants -ne 1 -or
		$Result.MaximumTraceGlobalCreditBytes -ne 524288 -or
		@($Result.PeerWaits | Where-Object { $_.Peer -ceq '1:1' })[0].GrantCount -ne 1 -or
		@($Result.PeerWaits | Where-Object { $_.Peer -ceq '1:1' })[0].MaximumObservedWaitMicroseconds -ne 200 -or
		@($Result.PeerWaits | Where-Object { $_.Peer -ceq '2:1' })[0].GrantCount -ne 0) {
		throw 'valid trace metrics or retained-waiter outcome are incorrect'
	}
	$Offline = & (Join-Path $PSScriptRoot 'AnalyzeAdmissionFairnessEvidence.ps1') -Path $Path `
		-RunId $RunId -ExpectedConnections @('1:1', '2:1', '3:1', '4:1') | ConvertFrom-Json
	if ($Offline.EventCount -ne $Result.EventCount -or
		$Offline.MaximumObservedEligibilityToGrantMicroseconds -ne 200 -or
		$Offline.Classification -cne 'EVIDENCE_INTEGRITY_AND_OBSERVED_TIMING_ONLY') {
		throw 'offline analyzer result differs from farm reconciliation'
	}
	$Lifecycle = @(
		(New-Event exact_demand 30 1000),
		(New-Event credit_eligible 30 1100 -Episode 1 -CreditAt 1050 -EligibleAt 1100 -PeerCredit 100),
		(New-Event grant_accepted 30 1200 -Episode 1 -Token 30 -CreditAt 1050 -EligibleAt 1100 -ActiveGrants 1),
		(New-Event grant_retired 30 1300 -Token 30 -ActiveGrants 1),
		(New-Event grant_released 30 1400 -Token 30 -ActiveGrants 0),
		(New-Event exact_demand 31 1500),
		(New-Event credit_eligible 31 1600 -Episode 1 -CreditAt 1550 -EligibleAt 1600 -PeerCredit 100),
		(New-Event grant_accepted 31 1700 -Episode 1 -Token 31 -CreditAt 1550 -EligibleAt 1600 -ActiveGrants 1),
		(New-Event grant_retired 31 1800 -Token 31 -ActiveGrants 1),
		(New-Event grant_released 31 1900 -Token 31 -ActiveGrants 0)
	)
	Write-Trace -Events $Lifecycle -Trailer "end`t10`t0" -Version 2
	$LifecycleResult = Read-AdmissionFairnessEvidence -Path $Path -RunId $RunId
	if ($LifecycleResult.GrantLifecycleCoverage -cne 'MEASURED_PASS' -or
		$LifecycleResult.RetiredGrantCount -ne 2 -or $LifecycleResult.ReleasedGrantCount -ne 2 -or
		$LifecycleResult.OpenGrantCount -ne 0) { throw 'complete ACK-gated grant chronology failed' }
	$ZeroDebtTerminal = @($Lifecycle)
	$ZeroDebtTerminal[4] = New-Event grant_terminal_released 30 1400 -Token 30 -ActiveGrants 0
	Write-Trace -Events $ZeroDebtTerminal -Trailer "end`t10`t0" -Version 2
	$ZeroDebtResult = Read-AdmissionFairnessEvidence -Path $Path -RunId $RunId
	if ($ZeroDebtResult.GrantLifecycleCoverage -cne 'MEASURED_PASS' -or
		$ZeroDebtResult.TerminalReleasedGrantCount -ne 1 -or
		$ZeroDebtResult.TerminalDebtReleaseCount -ne 0) {
		throw 'zero-debt terminal owner cleanup was misclassified as a byte release'
	}
	$Invalid = @($Lifecycle)
	$Invalid[3] = New-Event grant_retired 30 1300 -Token 31 -ActiveGrants 1
	Write-Trace -Events $Invalid -Trailer "end`t10`t0" -Version 2
	try { [void](Read-AdmissionFairnessEvidence -Path $Path -RunId $RunId); throw 'wrong retirement token accepted' }
	catch { if ($_.Exception.Message -ceq 'wrong retirement token accepted') { throw } }
	$Invalid = @($Lifecycle)
	$Invalid[3] = New-Event grant_released 30 1300 -Token 30 -ActiveGrants 0
	Write-Trace -Events $Invalid -Trailer "end`t10`t0" -Version 2
	try { [void](Read-AdmissionFairnessEvidence -Path $Path -RunId $RunId); throw 'release before retirement accepted' }
	catch { if ($_.Exception.Message -ceq 'release before retirement accepted') { throw } }
	$Invalid = @($Lifecycle[0..3] + $Lifecycle[5..9])
	Write-Trace -Events $Invalid -Trailer "end`t9`t0" -Version 2
	try { [void](Read-AdmissionFairnessEvidence -Path $Path -RunId $RunId); throw 'second grant before release accepted' }
	catch { if ($_.Exception.Message -ceq 'second grant before release accepted') { throw } }
	$Invalid = @($Lifecycle)
	$Invalid[4] = New-Event grant_terminal_released 30 1400 -Token 30 -Reason terminal_release -ActiveGrants 0
	Write-Trace -Events $Invalid -Trailer "end`t10`t0" -Version 2
	try { [void](Read-AdmissionFairnessEvidence -Path $Path -RunId $RunId); throw 'contradictory terminal debt release accepted' }
	catch { if ($_.Exception.Message -ceq 'contradictory terminal debt release accepted') { throw } }
	$Invalid = @($Lifecycle[0..2])
	$Invalid += New-Event grant_terminal_released 30 1300 -Token 30 -Reason terminal_release -ActiveGrants 0
	Write-Trace -Events $Invalid -Trailer "end`t4`t0" -Version 2
	$TerminalResult = Read-AdmissionFairnessEvidence -Path $Path -RunId $RunId
	if ($TerminalResult.GrantLifecycleCoverage -cne 'MEASURED_FAIL' -or
		$TerminalResult.TerminalDebtReleaseCount -ne 1) {
		throw 'positive-debt terminal release was not classified as failure'
	}
	$CrossPeerClock = @(
		(New-Event exact_demand 1 1000),
		(New-Event credit_eligible 1 1100 -Episode 1 -CreditAt 1050 -EligibleAt 1100 -PeerCredit 100),
		(New-Event exact_demand 2 1200 -Slot 2 -Bytes 77),
		(New-Event grant_accepted 1 1400 -Episode 1 -Token 1 -CreditAt 1050 -EligibleAt 1100),
		(New-Event demand_disposed 2 1300 -Slot 2 -Bytes 77 -Reason no_work)
	)
	Write-Trace -Events $CrossPeerClock -Trailer "end`t5`t0"
	$CrossPeerResult = Read-AdmissionFairnessEvidence -Path $Path -RunId $RunId
	if ($CrossPeerResult.GrantedCount -ne 1 -or
		$CrossPeerResult.MaximumObservedEligibilityToGrantMicroseconds -ne 300 -or
		$CrossPeerResult.LatestRecordedMicroseconds -ne 1400) {
		throw 'cross-peer ledger and fresh ServiceTime timestamps were misordered'
	}
	$Bounded = @(
		(New-Event exact_demand 10 1000),
		(New-Event credit_eligible 10 1100 -Episode 1 -CreditAt 1050 -EligibleAt 1100 -PeerCredit 100),
		(New-Event grant_accepted 10 221600 -Episode 1 -Token 10 -CreditAt 1050 -EligibleAt 1100)
	)
	Write-Trace -Events $Bounded -Trailer "end`t3`t0"
	$BoundedResult = Read-AdmissionFairnessEvidence -Path $Path -RunId $RunId
	if ($BoundedResult.AcceptedGrantWaitBoundMicroseconds -ne 220500 -or
		$BoundedResult.AcceptedGrantWaitBound -cne 'MEASURED_PASS' -or
		$BoundedResult.ExactDemandEpisodeCoverage -cne 'MEASURED_PASS') {
		throw 'exact 220.5-ms accepted-grant bound failed'
	}
	if ($BoundedResult.PSObject.Properties.Name -contains 'ContinuousEligiblePeerEpisodeCoverage') {
		throw 'legacy contract acquired hybrid peer-lineage semantics'
	}
	$OverBound = @($Bounded)
	$OverBound[2] = New-Event grant_accepted 10 221601 -Episode 1 -Token 10 -CreditAt 1050 -EligibleAt 1100
	Write-Trace -Events $OverBound -Trailer "end`t3`t0"
	$OverBoundResult = Read-AdmissionFairnessEvidence -Path $Path -RunId $RunId
	if ($OverBoundResult.AcceptedGrantWaitBound -cne 'MEASURED_FAIL' -or
		$OverBoundResult.ExactDemandEpisodeCoverage -cne 'MEASURED_FAIL') {
		throw 'accepted grant beyond canonical wait was not classified as a measured failure'
	}
	$Changed = @($Events)
	$Changed[1] = New-Event credit_eligible 1 1100 -Episode 1 -CreditAt 1050 -EligibleAt 1100 -PeerCredit 524289
	$HybridBoundary = @($Bounded)
	$HybridBoundary[2] = New-Event grant_accepted 10 1182350 -Episode 1 -Token 10 -CreditAt 1050 -EligibleAt 1100
	Write-Trace -Events $HybridBoundary -Trailer "end`t3`t0" -Version 3
	$HybridResult = Read-AdmissionFairnessEvidence -Path $Path -RunId $RunId
	if ($HybridResult.ServiceContract -cne 'HYBRID_F1_V1' -or
		$HybridResult.AcceptedGrantWaitBoundMicroseconds -ne 1181250 -or
		$HybridResult.AcceptedGrantWaitBound -cne 'MEASURED_PASS') { throw 'hybrid exact fairness boundary failed' }
	$HybridBoundary[2] = New-Event grant_accepted 10 1182351 -Episode 1 -Token 10 -CreditAt 1050 -EligibleAt 1100
	Write-Trace -Events $HybridBoundary -Trailer "end`t3`t0" -Version 3
	if ((Read-AdmissionFairnessEvidence -Path $Path -RunId $RunId).AcceptedGrantWaitBound -cne 'MEASURED_FAIL') {
		throw 'hybrid overdue accepted demand did not fail'
	}
	$Starved = @($Bounded[0..1])
	$Starved += New-Event exact_demand 20 1182351 -Slot 2
	Write-Trace -Events $Starved -Trailer "end`t3`t0" -Version 3
	if ((Read-AdmissionFairnessEvidence -Path $Path -RunId $RunId).ExactDemandEpisodeCoverage -cne 'MEASURED_FAIL') {
		throw 'hybrid retained starvation did not fail'
	}
	$Lineage = @(New-LineageTrace 1182350)
	$LineageResult = Read-HybridTrace $Lineage
	if ($LineageResult.ContinuousEligiblePeerEpisodeCoverage -cne 'MEASURED_PASS' -or
		$LineageResult.MaximumObservedPeerEligibilityToGrantMicroseconds -ne 1181250 -or
		$LineageResult.MaximumObservedEligibilityToGrantMicroseconds -ne 381250 -or
		$LineageResult.ExactDemandEpisodeCoverage -cne 'INCONCLUSIVE_NOT_MEASURED' -or
		$LineageResult.ExactDemandCount -ne 3 -or $LineageResult.GrantedCount -ne 1 -or
		$LineageResult.CarriedEligibleReplacementCount -ne 2 -or
		$LineageResult.ContinuousPeerEligibleEpisodeCount -ne 1 -or
		$LineageResult.CompletedContinuousPeerEligibleEpisodeCount -ne 1 -or
		$LineageResult.GrantLifecycleCoverage -cne 'MEASURED_PASS') {
		throw 'same-clock replacements lost peer age, distinct candidate identity or exact successor retirement'
	}
	$LateLineage = Read-HybridTrace @(New-LineageTrace 1182351)
	if ($LateLineage.AcceptedGrantWaitBound -cne 'MEASURED_PASS' -or
		$LateLineage.ContinuousEligiblePeerEpisodeCoverage -cne 'MEASURED_FAIL') {
		throw 'young successor candidates hid an overdue continuous eligible peer'
	}
	$Gap = @($Lineage)
	$Gap[3] = New-Event exact_demand 101 401101 -Bytes 90 -PeerCredit 100
	$Gap[4] = New-Event credit_eligible 101 401101 -Bytes 90 -Episode 1 -CreditAt 401101 -EligibleAt 401101 -PeerCredit 100
	$Gap[5] = New-Event demand_disposed 101 801100 -Bytes 90 -Reason replaced -Episode 1 -CreditAt 401101 -EligibleAt 401101 -PeerCredit 100
	if ((Read-HybridTrace $Gap).ContinuousEligiblePeerEpisodeCoverage -cne 'INCONCLUSIVE_NOT_MEASURED') {
		throw 'later replacement clock invented continuous peer eligibility'
	}
	$Interleaved = @($Lineage[0..3]) + @(
		(New-Event exact_demand 200 401100 -Slot 2),
		(New-Event demand_disposed 200 401100 -Slot 2 -Reason no_work)
	) + @($Lineage[4..10])
	if ((Read-HybridTrace $Interleaved).ContinuousEligiblePeerEpisodeCoverage -cne 'INCONCLUSIVE_NOT_MEASURED') {
		throw 'per-peer filtering concealed a global replacement bridge gap'
	}
	$LifecycleGap = @(
		(New-Event exact_demand 300 800 -Slot 2),
		(New-Event credit_eligible 300 900 -Slot 2 -Episode 1 -CreditAt 850 -EligibleAt 900 -PeerCredit 100),
		(New-Event grant_accepted 300 950 -Slot 2 -Episode 1 -Token 901 -CreditAt 850 -EligibleAt 900),
		(New-Event exact_demand 100 1000 -ActiveGrants 1),
		(New-Event credit_eligible 100 1100 -Episode 1 -CreditAt 1050 -EligibleAt 1100 -PeerCredit 100 -ActiveGrants 1),
		(New-Event demand_disposed 100 401100 -Reason replaced -Episode 1 -CreditAt 1050 -EligibleAt 1100 -PeerCredit 100 -ActiveGrants 1),
		(New-Event grant_retired 300 401100 -Slot 2 -Token 901 -ActiveGrants 1),
		(New-Event grant_released 300 401100 -Slot 2 -Token 901 -ActiveGrants 0)
	) + @($Lineage[3..10])
	if ((Read-HybridTrace $LifecycleGap).ContinuousEligiblePeerEpisodeCoverage -cne 'INCONCLUSIVE_NOT_MEASURED') {
		throw 'lifecycle early-continue concealed a global replacement bridge gap'
	}
	$InventedCredit = @($Lineage)
	$InventedCredit[3] = New-Event exact_demand 101 401100 -Bytes 120 -PeerCredit 200
	$InventedCredit[4] = New-Event credit_eligible 101 401100 -Bytes 120 -Episode 1 -CreditAt 401100 -EligibleAt 401100 -PeerCredit 200
	$InventedCredit[5] = New-Event demand_disposed 101 801100 -Bytes 120 -Reason replaced -Episode 1 -CreditAt 401100 -EligibleAt 401100 -PeerCredit 200
	$InventedCredit[6] = New-Event exact_demand 102 801100 -Bytes 80 -PeerCredit 200
	$InventedCredit[7] = New-Event credit_eligible 102 801100 -Bytes 80 -Episode 1 -CreditAt 801100 -EligibleAt 801100 -PeerCredit 200
	if ((Read-HybridTrace $InventedCredit).ContinuousEligiblePeerEpisodeCoverage -cne 'INCONCLUSIVE_NOT_MEASURED') {
		throw 'same-clock invented refill qualified a successor exceeding original peer credit'
	}
	$ChangedGlobal = @($Lineage)
	$ChangedGlobal[3] = New-Event exact_demand 101 401100 -Bytes 90 -PeerCredit 100 -GlobalCredit 524289
	if ((Read-HybridTrace $ChangedGlobal).ContinuousEligiblePeerEpisodeCoverage -cne 'INCONCLUSIVE_NOT_MEASURED') {
		throw 'same-clock changed funding witness qualified replacement continuity'
	}
	foreach ($FieldIndex in 14..18) {
		$ChangedWitness = @($Lineage)
		foreach ($RowIndex in 3..10) {
			$Fields = $ChangedWitness[$RowIndex].Split([char]9)
			$Fields[$FieldIndex] = [string]([UInt64]$Fields[$FieldIndex]+1)
			$ChangedWitness[$RowIndex] = $Fields -join "`t"
		}
		$RejectedWitness = $false
		try {
			$WitnessResult = Read-HybridTrace $ChangedWitness
			$RejectedWitness = $WitnessResult.ContinuousEligiblePeerEpisodeCoverage -cne 'MEASURED_PASS'
		} catch { $RejectedWitness = $true }
		if (-not $RejectedWitness) { throw "same-clock owner/counter witness column $FieldIndex invented continuity" }
	}
	$MissingEligible = @($Lineage[0..3]) + @(
		(New-Event demand_disposed 101 401100 -Bytes 90 -Reason no_work)
	)
	if ((Read-HybridTrace $MissingEligible).ContinuousEligiblePeerEpisodeCoverage -cne 'INCONCLUSIVE_NOT_MEASURED') {
		throw 'successor exact demand alone falsely proved eligible continuity'
	}
	foreach ($Reason in @('no_work','unexamined','generation_removed','terminal_release')) {
		foreach ($EndAt in @(1200,1182351)) {
			$Withdrawal = @($Lineage[0..1]) + @(
				(New-Event demand_disposed 100 $EndAt -Reason $Reason -Episode 1 -CreditAt 1050 -EligibleAt 1100)
			)
			$ExpectedState = if ($EndAt -eq 1200) { 'INCONCLUSIVE_NOT_MEASURED' } else { 'MEASURED_FAIL' }
			if ((Read-HybridTrace $Withdrawal).ContinuousEligiblePeerEpisodeCoverage -cne $ExpectedState) {
				throw "eligible withdrawal $Reason reset or hid the peer deadline"
			}
		}
	}
	foreach ($EndAt in @(1182350,1182351)) {
		$SourceEmpty = @($Lineage[0..1]) + @(
			(New-Event demand_disposed 100 $EndAt -Reason source_empty -Episode 1 -CreditAt 1050 -EligibleAt 1100)
		)
		$EmptyResult = Read-HybridTrace $SourceEmpty
		$ExpectedState = if ($EndAt -eq 1182350) { 'MEASURED_PASS' } else { 'MEASURED_FAIL' }
		if ($EmptyResult.ContinuousEligiblePeerEpisodeCoverage -cne $ExpectedState -or
			$EmptyResult.SourceEmptyPeerEligibleEpisodeCount -ne 1 -or
			$EmptyResult.CompletedContinuousPeerEligibleEpisodeCount -ne 0 -or $EmptyResult.GrantedCount -ne 0 -or
			$EmptyResult.MaximumObservedPeerEligibilityToGrantMicroseconds -ne 0 -or
			$EmptyResult.MaximumObservedSourceEmptyPeerEligibleWaitMicroseconds -ne $EndAt-1100 -or
			$EmptyResult.GrantLifecycleCoverage -cne 'NOT_MEASURED' -or
			$EmptyResult.ExactDemandEpisodeCoverage -cne 'NOT_MEASURED') {
			throw 'source-owned empty end hid an overdue wait or invented delivered service'
		}
		Assert-Rejected $SourceEmpty 'legacy source-empty waiver' -Version 1
		Assert-Rejected $SourceEmpty 'legacy lifecycle source-empty waiver' -Version 2
	}
	$EmptyReplacement = @($Lineage[0..7]) + @(
		(New-Event demand_disposed 102 1182350 -Bytes 80 -Reason source_empty -Episode 1 -CreditAt 801100 -EligibleAt 801100)
	)
	$EmptyReplacementResult = Read-HybridTrace $EmptyReplacement
	if ($EmptyReplacementResult.ContinuousEligiblePeerEpisodeCoverage -cne 'MEASURED_PASS' -or
		$EmptyReplacementResult.MaximumObservedSourceEmptyPeerEligibleWaitMicroseconds -ne 1181250 -or
		$EmptyReplacementResult.CompletedContinuousPeerEligibleEpisodeCount -ne 0 -or
		$EmptyReplacementResult.SourceEmptyPeerEligibleEpisodeCount -ne 1) {
		throw 'source-empty end reset the original eligible replacement clock'
	}
	Assert-Rejected ($EmptyReplacement + @($EmptyReplacement[-1])) 'duplicate source-empty end' -Version 3
	$Interrupted = @($Lineage[0..1]) + @(
		(New-Event eligibility_interrupted 100 1182351 -Reason feedback_unavailable -Episode 1 -CreditAt 1050 -EligibleAt 1100),
		(New-Event credit_eligible 100 1182400 -Episode 2 -CreditAt 1050 -EligibleAt 1182400 -PeerCredit 100),
		(New-Event grant_accepted 100 1182500 -Episode 2 -Token 901 -CreditAt 1050 -EligibleAt 1182400),
		(New-Event grant_retired 100 1182501 -Token 901 -ActiveGrants 1),
		(New-Event grant_released 100 1182502 -Token 901 -ActiveGrants 0)
	)
	if ((Read-HybridTrace $Interrupted).ContinuousEligiblePeerEpisodeCoverage -cne 'MEASURED_FAIL') {
		throw 'feedback interruption and fresh eligibility erased an overdue prior episode'
	}
	$OpenLineage = @($Lineage[0..7]) + @(
		(New-Event exact_demand 200 1182351 -Slot 2),
		(New-Event demand_disposed 200 1182351 -Slot 2 -Reason no_work)
	)
	if ((Read-HybridTrace $OpenLineage).ContinuousEligiblePeerEpisodeCoverage -cne 'MEASURED_FAIL') {
		throw 'open replacement chain forgot the first eligible clock'
	}
	$WrongGeneration = @($Lineage); $WrongGeneration[3] = New-Event exact_demand 101 401100 -Generation 2 -Bytes 90
	Assert-Rejected $WrongGeneration 'hybrid replacement generation changed' -Version 3
	Assert-Rejected @($Lineage[0..2]) 'unfinished hybrid replacement' -Version 3
	Assert-Rejected $Changed 'peer credit beyond canonical burst cap'
	$Changed = @($Events)
	$Changed[1] = New-Event credit_eligible 1 1100 -Episode 1 -CreditAt 1050 -EligibleAt 1100 -PeerCredit 100 -GlobalCredit 2097153
	Assert-Rejected $Changed 'global credit beyond canonical burst cap'
	$Changed = @($Events)
	$Changed[5] = New-Event grant_accepted 1 1500 -Episode 2 -Token 2 -CreditAt 1050 -EligibleAt 1300 -ActiveGrants 5
	Assert-Rejected $Changed 'five concurrent native grants'
	$Changed = @($Events)
	$Changed[5] = New-Event grant_accepted 1 1500 -Episode 2 -Token 2 -CreditAt 1050 -EligibleAt 1300 -ActiveGrants 0
	Assert-Rejected $Changed 'accepted grant with no native active grant'
	$Changed = @($Events)
	$Changed[4] = New-Event reservation_rolled_back 1 1350 -Reason rollback -Episode 2 -Token 1 -CreditAt 1050 -EligibleAt 1300 -GrantDeferrals 2
	Assert-Rejected $Changed 'regressing native grant deferrals'
	$Changed = @($Events)
	$Changed[5] = New-Event grant_accepted 1 1500 -Episode 2 -Token 1 -CreditAt 1050 -EligibleAt 1300
	Assert-Rejected $Changed 'reused rollback/grant token'
	$Changed = @($Events)
	$Changed[5] = New-Event grant_accepted 1 1500 -Generation 2 -Episode 2 -Token 2 -CreditAt 1050 -EligibleAt 1300
	Assert-Rejected $Changed 'cross-generation grant'
	$Changed = @($Events)
	$Changed[5] = New-Event grant_accepted 1 1500 -Episode 2 -Token 2 -Bytes 101 -CreditAt 1050 -EligibleAt 1300
	Assert-Rejected $Changed 'altered exact byte count'
	$Changed = @($Events)
	$Changed[3] = New-Event credit_eligible 1 1300 -Episode 1 -CreditAt 1050 -EligibleAt 1300 -PeerCredit 100
	Assert-Rejected $Changed 'reused eligibility episode'
	$Changed = @($Events)
	$Changed[5] = New-Event grant_accepted 1 1320 -Episode 2 -Token 2 -CreditAt 1050 -EligibleAt 1300
	Assert-Rejected $Changed 'backwards same-demand grant after rollback'
	$Changed = @($Events)
	$Changed[9] = New-Event exact_demand 3 1801 -Slot 4 -Bytes 77
	Assert-Rejected $Changed 'replacement in wrong generation'
	$Changed = @($Events)
	$Changed[1] = New-Event grant_accepted 1 1100 -Token 2
	Assert-Rejected $Changed 'grant without eligibility'
	$Changed = @($Events)
	$Changed[9] = New-Event exact_demand 2 1801 -Slot 2 -Bytes 77
	Assert-Rejected $Changed 'duplicate exact demand identity'
	$Changed = @($Events)
	$Changed[6] = New-Event grant_accepted 1 1600 -Episode 2 -Token 2 -CreditAt 1050 -EligibleAt 1300
	Assert-Rejected $Changed 'duplicate accepted grant'
	$Changed = @($Events[0..8])
	Assert-Rejected $Changed 'unfinished replacement disposition'
	Write-Trace -Events $Events -Trailer "end`t15`t0"
	try { [void](Read-AdmissionFairnessEvidence -Path $Path -RunId $RunId); throw 'bad trailer accepted' }
	catch { if ($_.Exception.Message -ceq 'bad trailer accepted') { throw } }
Write-Output '[Qualification:Admission] analyzer_cases=31 peer_lineage_cases=25 source_empty_cases=8 PASS'
} finally {
	$Resolved = [IO.Path]::GetFullPath($Root)
	if ($Resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase) -and
		[IO.Path]::GetFileName($Resolved).StartsWith('gargantuan-fairness-analyzer-', [StringComparison]::Ordinal)) {
		[IO.Directory]::Delete($Resolved, $true)
	}
}
