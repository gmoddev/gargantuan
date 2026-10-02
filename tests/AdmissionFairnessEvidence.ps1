# Offline interpretation of the bounded farm admission trace. This validates
# evidence identity and checks accepted-grant waits against the canonical model
# bound; it does not prove sustained fairness or continuous source backlog.
function Read-AdmissionFairnessEvidence {
	param(
		[Parameter(Mandatory = $true)][string]$Path,
		[Parameter(Mandatory = $true)][string]$RunId,
		[string[]]$ExpectedConnections = @()
	)
	$File = Get-Item -LiteralPath $Path -ErrorAction Stop
	if ($File.PSIsContainer -or $File.Length -lt 64 -or $File.Length -gt 33554432) {
		throw 'admission fairness evidence is not a bounded file'
	}
	$Expected = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	foreach ($Connection in $ExpectedConnections) { [void]$Expected.Add($Connection) }
	$Demands = [Collections.Generic.Dictionary[UInt64, object]]::new()
	$Current = [Collections.Generic.Dictionary[string, UInt64]]::new([StringComparer]::Ordinal)
	$Peers = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
	$Tokens = [Collections.Generic.HashSet[UInt64]]::new()
	$PendingReplacement = $null
	$Count = 0
	$GrantCount = 0
	$InterruptedEligibilityEpisodeCount = 0
	$DisposedEligibleCount = 0
	$DisposedEverEligibleCount = 0
	$MaximumWait = [UInt64]0
	$LatestTime = [UInt64]0
	$Ended = $false
	$Stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
	$Reader = [IO.StreamReader]::new($Stream, [Text.UTF8Encoding]::new($false, $true))
	try {
		if ($Reader.ReadLine() -cne "format=GargantuanAdmissionEvidenceV1`trun=$RunId") {
			throw 'admission fairness evidence header or run identity is invalid'
		}
		while ($null -ne ($Line = $Reader.ReadLine())) {
			$Fields = $Line.Split([char]9)
			if ($Fields[0] -ceq 'end') {
				if ($Fields.Count -ne 3 -or $Fields[1] -cne [string]$Count -or $Fields[2] -cne '0' -or
					$null -ne $Reader.ReadLine()) { throw 'admission fairness evidence trailer is invalid' }
				$Ended = $true
				break
			}
			if ($Count -ge 65536 -or $Fields.Count -ne 19 -or $Fields[0] -cne 'event' -or
				$Fields[1] -cnotin @('exact_demand', 'credit_eligible', 'eligibility_interrupted',
					'grant_accepted', 'reservation_rolled_back', 'demand_disposed') -or
				$Fields[2] -cnotin @('none', 'no_work', 'unexamined', 'replaced', 'rollback',
					'generation_removed', 'terminal_release', 'feedback_unavailable')) {
				throw "admission fairness evidence event $Count has an invalid schema"
			}
			try {
				$N = @($Fields[3..18] | ForEach-Object {
					[UInt64]::Parse($_, [Globalization.NumberStyles]::None,
						[Globalization.CultureInfo]::InvariantCulture)
				})
			} catch { throw "admission fairness evidence event $Count has a noninteger field" }
			$Kind = $Fields[1]
			$Reason = $Fields[2]
			$Peer = "$($N[0]):$($N[1])"
			$DemandId = [UInt64]$N[2]
			$Episode = [UInt64]$N[3]
			$Token = [UInt64]$N[4]
			$Bytes = [UInt64]$N[5]
			$At = [UInt64]$N[6]
			$CreditAt = [UInt64]$N[7]
			$EligibleSince = [UInt64]$N[8]
			if ($N[0] -lt 1 -or $N[0] -gt 512 -or $N[1] -eq 0 -or $DemandId -eq 0 -or
				$Bytes -eq 0 -or $Bytes -gt 524288 -or
				$CreditAt -gt $At -or $EligibleSince -gt $At -or
				($Expected.Count -gt 0 -and -not $Expected.Contains($Peer))) {
				throw "admission fairness evidence event $Count has invalid identity, bytes, or chronology"
			}
			# Commit records a fresh ServiceTime() while other admission events
			# may still use the step ledger's earlier Now. File order is causal,
			# but timestamps need only be monotonic within one demand.
			$LatestTime = [Math]::Max($LatestTime, $At)
			if ($PendingReplacement -and ($Kind -cne 'exact_demand' -or $Peer -cne $PendingReplacement)) {
				throw 'replaced admission demand was not followed by the same generation replanning'
			}
			if ($Kind -ceq 'exact_demand') {
				if ($Reason -cne 'none' -or $Episode -ne 0 -or $Token -ne 0 -or
					$CreditAt -ne 0 -or $EligibleSince -ne 0 -or $Demands.ContainsKey($DemandId) -or
					$Current.ContainsKey($Peer)) { throw 'exact admission demand identity is duplicated or malformed' }
				$Demands.Add($DemandId, @{
					Peer = $Peer; Bytes = $Bytes; At = $At; LastAt = $At; Episode = [UInt64]0
					Active = $false; EligibleSince = [UInt64]0; CreditAt = [UInt64]0
					EverEligible = $false; Closed = $false
				})
				$Current.Add($Peer, $DemandId)
				if (-not $Peers.ContainsKey($Peer)) {
					$Peers.Add($Peer, @{ Peer = $Peer; Slot = $N[0]; Generation = $N[1];
						GrantCount = 0; MaximumObservedWaitMicroseconds = [UInt64]0;
						TotalObservedWaitMicroseconds = [UInt64]0 })
				}
				$PendingReplacement = $null
				$Count++
				continue
			}
			if (-not $Demands.ContainsKey($DemandId) -or -not $Current.ContainsKey($Peer) -or
				$Current[$Peer] -ne $DemandId) {
				throw 'admission event refers to an unknown, closed, or different-generation demand'
			}
			$Demand = $Demands[$DemandId]
			if ($Demand.Peer -cne $Peer -or $Demand.Bytes -ne $Bytes -or
				$At -lt $Demand.LastAt -or $Demand.Closed) {
				throw 'admission event changes an exact demand identity or byte count'
			}
			$Demand.LastAt = $At
			switch -CaseSensitive ($Kind) {
				'credit_eligible' {
					if ($Reason -cne 'none' -or $Token -ne 0 -or $Demand.Active -or
						$Episode -ne $Demand.Episode + 1 -or $EligibleSince -ne $At -or
						$CreditAt -lt $Demand.At -or $N[9] -lt $Bytes) {
						throw 'credit eligibility does not belong to a new exact-demand episode'
					}
					$Demand.Episode = $Episode
					$Demand.Active = $true
					$Demand.EverEligible = $true
					$Demand.EligibleSince = $At
					$Demand.CreditAt = $CreditAt
				}
				'eligibility_interrupted' {
					if ($Reason -cne 'feedback_unavailable' -or $Token -ne 0 -or -not $Demand.Active -or
						$Episode -ne $Demand.Episode -or $EligibleSince -ne $Demand.EligibleSince -or
						$CreditAt -ne $Demand.CreditAt) {
						throw 'eligibility interruption has no active exact-demand episode'
					}
					$Demand.Active = $false
					$InterruptedEligibilityEpisodeCount++
				}
				'reservation_rolled_back' {
					if ($Reason -cne 'rollback' -or $Token -eq 0 -or -not $Demand.Active -or
						$Episode -ne $Demand.Episode -or $EligibleSince -ne $Demand.EligibleSince -or
						$CreditAt -ne $Demand.CreditAt -or -not $Tokens.Add($Token)) {
						throw 'reservation rollback has invalid episode or reused grant token'
					}
				}
				'grant_accepted' {
					if ($Reason -cne 'none' -or $Token -eq 0 -or -not $Demand.Active -or
						$Episode -ne $Demand.Episode -or $EligibleSince -ne $Demand.EligibleSince -or
						$CreditAt -ne $Demand.CreditAt -or -not $Tokens.Add($Token)) {
						throw 'accepted grant has invalid episode or duplicate token'
					}
					$Wait = [UInt64]($At - $EligibleSince)
					$Stats = $Peers[$Peer]
					$Stats.GrantCount++
					$Stats.MaximumObservedWaitMicroseconds = [Math]::Max($Stats.MaximumObservedWaitMicroseconds, $Wait)
					$Stats.TotalObservedWaitMicroseconds += $Wait
					$MaximumWait = [Math]::Max($MaximumWait, $Wait)
					$GrantCount++
					$Demand.Closed = $true
					[void]$Current.Remove($Peer)
				}
				'demand_disposed' {
					if ($Reason -cnotin @('no_work', 'unexamined', 'replaced',
						'generation_removed', 'terminal_release') -or $Token -ne 0 -or
						$Episode -ne $Demand.Episode -or
						$EligibleSince -ne $(if ($Demand.Active) { $Demand.EligibleSince } else { 0 })) {
						throw 'exact demand disposition is malformed'
					}
					if ($Demand.Active) { $DisposedEligibleCount++ }
					if ($Demand.EverEligible) { $DisposedEverEligibleCount++ }
					$Demand.Closed = $true
					[void]$Current.Remove($Peer)
					if ($Reason -ceq 'replaced') { $PendingReplacement = $Peer }
				}
			}
			$Count++
		}
		if (-not $Ended -or $Count -eq 0 -or $PendingReplacement) {
			throw 'admission fairness evidence is empty, incomplete, or has an unfinished replacement'
		}
		$Retained = @(
			foreach ($Entry in $Current.GetEnumerator()) {
				$Demand = $Demands[$Entry.Value]
				if ($Demand.Active) {
					[pscustomobject]@{ Peer = $Entry.Key; DemandId = $Entry.Value;
						Episode = $Demand.Episode; EligibleSinceMicroseconds = $Demand.EligibleSince;
						ObservedAgeLowerBoundMicroseconds = [UInt64]($LatestTime - $Demand.EligibleSince);
						Outcome = 'retained_at_end' }
				}
			}
		)
		$EverEligibleCount = 0
		$OpenAfterInterruptionCount = 0
		foreach ($Demand in $Demands.Values) {
			if ($Demand.EverEligible) { $EverEligibleCount++ }
			if (-not $Demand.Closed -and $Demand.EverEligible -and -not $Demand.Active) {
				$OpenAfterInterruptionCount++
			}
		}
		$PeerWaits = @($Peers.Values | Sort-Object Slot, Generation | ForEach-Object {
			[pscustomobject]@{ Peer = $_.Peer; Slot = $_.Slot; Generation = $_.Generation;
				GrantCount = $_.GrantCount;
				MaximumObservedWaitMicroseconds = $_.MaximumObservedWaitMicroseconds;
				TotalObservedWaitMicroseconds = $_.TotalObservedWaitMicroseconds }
		})
		return [pscustomobject]@{
			Classification = 'EVIDENCE_INTEGRITY_AND_OBSERVED_TIMING_ONLY'
			# This is a verdict on recorded, accepted exact-demand episodes only.
			# It does not establish continuous source backlog or universal fairness.
			AcceptedGrantWaitBoundMicroseconds = [UInt64]220500
			AcceptedGrantWaitBound = $(if ($GrantCount -eq 0) { 'NOT_MEASURED' }
				elseif ($MaximumWait -gt 220500) { 'MEASURED_FAIL' }
				else { 'MEASURED_PASS' })
			ExactDemandEpisodeCoverage = $(if ($GrantCount -eq 0) { 'NOT_MEASURED' }
				elseif ($MaximumWait -gt 220500) { 'MEASURED_FAIL' }
				elseif ($InterruptedEligibilityEpisodeCount -gt 0 -or $DisposedEligibleCount -gt 0 -or
					$Current.Count -gt 0 -or $OpenAfterInterruptionCount -gt 0) { 'INCONCLUSIVE_NOT_MEASURED' }
				else { 'MEASURED_PASS' })
			RunId = $RunId; EventCount = $Count; FileBytes = $File.Length
			InterruptedEligibilityEpisodeCount = $InterruptedEligibilityEpisodeCount
			ExactDemandCount = $Demands.Count; EverEligibleDemandCount = $EverEligibleCount
			GrantedCount = $GrantCount; DisposedEverEligibleDemandCount = $DisposedEverEligibleCount
			DisposedWhileEligibleCount = $DisposedEligibleCount
			OpenDemandCount = $Current.Count; RetainedEligibleWaiterCount = $Retained.Count
			OpenAfterInterruptionCount = $OpenAfterInterruptionCount
			RetainedEligibleWaiters = $Retained; PeerWaits = $PeerWaits
			LatestRecordedMicroseconds = $LatestTime
			MaximumObservedEligibilityToGrantMicroseconds = $MaximumWait
		}
	} finally { $Reader.Dispose() }
}
