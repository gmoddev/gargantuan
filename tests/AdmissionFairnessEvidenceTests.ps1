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
	param([string[]]$Events, [string]$Trailer)
	$Lines = @("format=GargantuanAdmissionEvidenceV1`trun=$RunId") + $Events + @($Trailer)
	[IO.File]::WriteAllText($Path, ($Lines -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
}

function Assert-Rejected {
	param([string[]]$Events, [string]$Case)
	Write-Trace -Events $Events -Trailer "end`t$($Events.Count)`t0"
	$Rejected = $false
	try {
		[void](Read-AdmissionFairnessEvidence -Path $Path -RunId $RunId `
			-ExpectedConnections @('1:1', '2:1', '3:1', '4:1'))
	} catch { $Rejected = $true }
	if (-not $Rejected) { throw "analyzer accepted invalid $Case" }
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
Write-Output '[Qualification:Admission] analyzer_cases=21 PASS'
} finally {
	$Resolved = [IO.Path]::GetFullPath($Root)
	if ($Resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase) -and
		[IO.Path]::GetFileName($Resolved).StartsWith('gargantuan-fairness-analyzer-', [StringComparison]::Ordinal)) {
		[IO.Directory]::Delete($Resolved, $true)
	}
}
