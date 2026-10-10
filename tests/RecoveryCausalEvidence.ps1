#requires -Version 7.0
# Independent offline replay of source-owned causal recovery facts. Native PASS
# summaries never substitute for this transactional accounting pass.
function Assert-RecoveryCausalEvidence {
	param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$RunId,
		[Parameter(Mandatory)][string]$Case, [Parameter(Mandatory)][string[]]$ExpectedConnections,
		[Parameter(Mandatory)][long]$CessationMicroseconds, [Parameter(Mandatory)][long]$JournalFence)
	function U([string]$Text) {
		if ($Text -cnotmatch '^(0|[1-9][0-9]*)$') { throw 'causal evidence has a noncanonical unsigned integer' }
		$Value = [decimal]::Parse($Text, [Globalization.CultureInfo]::InvariantCulture)
		if ($Value -gt [decimal]18446744073709551615) { throw 'causal evidence integer overflow' }
		return $Value
	}
	function Totals { return @{ accepted = [decimal]0; first_sent = [decimal]0; acked = [decimal]0; retired = [decimal]0 } }
	function Seal($Peer) {
		if (-not $Peer.Sealed -and -not $Peer.Planning -and $Peer.Cursor -ge $Peer.Fence -and -not $Peer.Prepared -and $Peer.Baseline.Count -eq 0) {
			$Peer.Sealed = $true; $Peer.Cut = $Peer.LastGrant; $Peer.CutSequence = $Peer.NextSequence - 1
			$Peer.Prefix = $Peer.All.Clone()
		}
	}
	function Converged($Peer) { return $Peer.Sealed -and (-not $Peer.Grant -or $Peer.Grant.Token -gt $Peer.Cut) }
	function Coverage($Peer, $Before, $After, $Resolved) {
		if ($Before -ne $Peer.Cursor -or $After -lt $Before) { throw 'causal cursor has a gap or rewind' }
		$Seen = @{}
		foreach ($Identity in $Resolved) {
			$Token = [string]$Identity.Token
			if ($Seen.ContainsKey($Token) -or -not $Peer.Pending.ContainsKey($Token) -or $Peer.Pending[$Token] -cne $Identity.Identity) {
				throw 'causal prepared frame resolves a missing or different pending identity'
			}
			$Seen[$Token] = $true
		}
	}
	function Apply-Coverage($Peer, $After, $Resolved) {
		$Peer.Cursor = $After
		foreach ($Identity in $Resolved) {
			$Token = [string]$Identity.Token
			$Peer.Pending.Remove($Token); $Peer.Baseline.Remove($Token); $State.PendingCount--
		}
	}
	function Add-Bytes($Peer, [string]$Field, [decimal]$Bytes, [bool]$Prefix) {
		$Peer.All[$Field] += $Bytes; $State.Totals[$Field] += $Bytes
		if ($Prefix) { $Peer.Prefix[$Field] += $Bytes }
		if ($State.Totals[$Field] -gt [decimal]18446744073709551615) { throw 'causal totals overflow' }
	}
	function Fingerprint($First, $Second) {
		$A = U $First; $B = U $Second
		if ($A -eq 0 -and $B -eq 0) { throw 'causal frame fingerprint is missing' }
		return "$A`:$B"
	}
	function Pending-Identity([string]$Text) {
		$Fields = $Text.Split(':')
		if ($Fields.Count -ne 4) { throw 'causal pending identity is malformed' }
		$Token = U $Fields[0]; $Slot = U $Fields[1]; $Generation = U $Fields[2]; $Enter = U $Fields[3]
		if ($Token -eq 0 -or $Slot -eq 0 -or $Generation -eq 0 -or $Enter -gt 1) { throw 'causal pending identity is invalid' }
		return @{ Token = $Token; Identity = "$Slot`:$Generation`:$Enter"; Object = "$Slot`:$Generation"; Enter = $Enter }
	}
	if (-not (Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -gt 33554432) {
		throw 'causal recovery file is missing or exceeds 32 MiB'
	}
	$Expected = [Collections.Generic.HashSet[string]]::new([string[]]$ExpectedConnections, [StringComparer]::Ordinal)
	if ($Expected.Count -ne 32) { throw 'causal recovery requires 32 original peers' }
	$Lines = [IO.File]::ReadAllLines($Path)
	if ($Lines.Count -lt 66 -or $Lines.Count -gt 131170 -or
		$Lines[0] -cne "format=GargantuanRecoveryCausalV1`trun=$RunId`tcase=$Case") { throw 'causal evidence header or row bound is invalid' }
	$State = @{ Peers = @{}; Slots = @{}; Scope = ''; PendingCount = 0; GrantCount = 0; IdentityCount = 0;
		Events = 0; Quotes = 0; Sources = @{}; Totals = (Totals); LastTime = [decimal]$CessationMicroseconds; ConvergedUs = $null }
	$InEvents = $false; $Ended = $false; $AfterEvents = $false
	for ($Line = 1; $Line -lt $Lines.Count; $Line++) {
		$F = $Lines[$Line].Split("`t")
		if ($Ended) { throw 'causal evidence follows its terminal record' }
		if ($F[0] -ceq 'capture') {
			if ($InEvents -or $F.Count -lt 21 -or $State.Peers.Count -ge 32) { throw 'invalid causal capture order or fields' }
			$N = @(1..20 | ForEach-Object { U $F[$_] })
			$Key = "$($N[0]):$($N[1])"; $Scope = "$($N[2]):$($N[3])"
			if (-not $Expected.Contains($Key) -or $State.Peers.ContainsKey($Key) -or $State.Slots.ContainsKey([string]$N[0]) -or
				$N[2] -eq 0 -or $N[3] -eq 0 -or ($State.Scope -ne '' -and $Scope -cne $State.Scope) -or
				$N[4] -gt $N[5] -or $N[5] -ne $JournalFence -or $N[6] -eq 0 -or $N[7] -eq 0 -or $N[19] -gt 1) { throw 'invalid causal capture generation, source or fence' }
			$State.Scope = $Scope; $State.Slots[[string]$N[0]] = $true
			$Peer = @{ Cursor = $N[4]; Fence = $N[5]; NextSequence = $N[6]; PendingWatermark = $N[7]; SeenTokens = @{};
				QuoteSequence = $N[6]; QuoteCursor = $N[4]; QuoteBytes = [decimal]0; InitialDebt = $N[10]; Planning = ($N[19] -eq 1);
				Pending = @{}; Baseline = @{}; Prepared = $null; Grant = $null; LastGrant = [decimal]0;
				Sealed = $false; Cut = [decimal]0; CutSequence = [decimal]0; All = (Totals); Prefix = (Totals); CumulativeAccepted = $N[15] }
			if ($N[17] -gt $N[16] -or $N[16] -gt $N[15]) { throw 'captured native delivery violates conservation' }
			for ($Index = 21; $Index -lt $F.Count; $Index++) {
				$Pending = Pending-Identity $F[$Index]; $Token = [string]$Pending.Token
				if ($Pending.Token -ge $N[7] -or $Peer.Pending.ContainsKey($Token)) { throw 'invalid captured pending watermark' }
				$Peer.Pending[$Token] = $Pending.Identity; $Peer.Baseline[$Token] = $true; $Peer.SeenTokens[$Token] = $true; $State.PendingCount++
			}
			if ($State.PendingCount -gt 1048576) { throw 'causal pending bound exceeded' }
			if ($N[10] -gt 0) {
				if ($N[8] -eq 0 -or $N[9] -eq 0 -or $N[9] -ge $N[6] -or $N[10] -gt 524288 -or
					$N[14] -gt $N[13] -or $N[13] -gt $N[10] -or $N[18] -gt $N[17] -or
					$N[15] - $N[18] -ne $N[10] -or $N[16] - $N[18] -ne $N[13] -or $N[17] - $N[18] -ne $N[14]) {
					throw 'captured grant identity or native byte basis is invalid'
				}
				$Peer.Grant = @{ Token = $N[8]; Sequence = $N[9]; Bytes = $N[10]; Hash = (Fingerprint $F[12] $F[13]);
					First = $N[13]; Ack = $N[14]; Base = $N[18] }
				$Peer.LastGrant = $N[8]; $State.GrantCount++
				Add-Bytes $Peer accepted $N[10] $false; Add-Bytes $Peer first_sent $N[13] $false; Add-Bytes $Peer acked $N[14] $false
			} elseif ($N[8] -ne 0 -or $N[15] -ne $N[16] -or $N[15] -ne $N[17]) { throw 'debt-free capture has outstanding native work' }
			if ($State.GrantCount -gt 4) { throw 'captured drain grant bound exceeded' }
			Seal $Peer; $State.Peers[$Key] = $Peer
			continue
		}
		if ($State.Peers.Count -ne 32) { throw 'causal evidence lacks all captures' }
		if (-not $InEvents) {
			$InEvents = $true
			if (@($State.Peers.Values | Where-Object { -not (Converged $_) }).Count -eq 0) { $State.ConvergedUs = [decimal]0 }
		}
		if ($F[0] -ceq 'end') {
			if ($F.Count -ne 3 -or (U $F[1]) -ne $State.Events -or $F[2] -cne '0') { throw 'causal terminal count or status is invalid' }
			$Ended = $true; continue
		}
		if ($F[0] -ceq 'quote') {
			$AfterEvents = $true
			if ($F.Count -ne 9 -or ++$State.Quotes -gt 65536 -or $State.Sources.Count -ne 0) { throw 'invalid frozen reference row order or bound' }
			$Key = "$(U $F[1]):$(U $F[2])"; $Sequence = U $F[3]; $Bytes = U $F[4]
			if (-not $State.Peers.ContainsKey($Key)) { throw 'unknown frozen reference peer' }
			$Peer = $State.Peers[$Key]
			if ($Sequence -ne $Peer.QuoteSequence -or $Bytes -eq 0 -or $Bytes -gt 524288) { throw 'invalid frozen reference sequence or bytes' }
			$Before = U $F[7]; $After = U $F[8]
			if ($Before -lt $Peer.QuoteCursor -or $After -lt $Before -or $After -gt $Peer.Fence) { throw 'invalid immutable reference cursor coverage' }
			$Peer.QuoteCursor = $After
			[void](Fingerprint $F[5] $F[6]); $Peer.QuoteSequence++; $Peer.QuoteBytes += $Bytes
			continue
		}
		if ($F[0] -ceq 'source') {
			$AfterEvents = $true
			if ($F.Count -lt 5) { throw 'invalid final source snapshot' }
			$Key = "$(U $F[1]):$(U $F[2])"
			if (-not $State.Peers.ContainsKey($Key) -or $State.Sources.ContainsKey($Key)) { throw 'unknown or duplicate final source peer' }
			$Peer = $State.Peers[$Key]; $Tokens = @{}
			if ((U $F[3]) -ne $Peer.Cursor -or (U $F[4]) -ne $Peer.NextSequence) { throw 'final source cursor/sequence differs from event replay' }
			for ($Index = 5; $Index -lt $F.Count; $Index++) {
				$Token = [string](U $F[$Index])
				if ($Tokens.ContainsKey($Token) -or -not $Peer.Pending.ContainsKey($Token)) { throw 'final source has unknown or duplicate pending token' }
				$Tokens[$Token] = $true
			}
			if ($Tokens.Count -ne $Peer.Pending.Count) { throw 'final source is missing a pending token' }
			$State.Sources[$Key] = $true; continue
		}
		if ($AfterEvents) { throw 'causal event follows reference/final source rows' }
		if ($F[0] -cne 'event' -or $F.Count -lt 24 -or ++$State.Events -gt 65536) { throw 'invalid causal event fields or bound' }
		$N = @(1..23 | ForEach-Object { U $F[$_] })
		$Kind = [int]$N[0]; $Reason = [int]$N[1]; $At = $N[2]; $Key = "$($N[3]):$($N[4])"
		if ($Kind -gt 10 -or $Reason -gt 5 -or $At -lt $State.LastTime -or -not $State.Peers.ContainsKey($Key) -or
			"$($N[19]):$($N[20])" -cne $State.Scope -or $N[21] -ne 1) { throw 'causal event source, time, validity or generation mismatch' }
		$State.LastTime = $At; $Peer = $State.Peers[$Key]
		$Sequence = $N[5]; $Token = $N[6]; $Bytes = $N[7]; $Before = $N[8]; $After = $N[9]
		$Resolved = [Collections.Generic.List[object]]::new(); $Entering = @{}; $Leaving = @{}
		for ($Index = 24; $Index -lt $F.Count; $Index++) {
			if (++$State.IdentityCount -gt 1048576) { throw 'causal identity bound exceeded' }
			if ($F[$Index].StartsWith('p:', [StringComparison]::Ordinal)) { $Resolved.Add((Pending-Identity $F[$Index].Substring(2))); continue }
			$IdentityFields = $F[$Index].Split(':')
			if ($IdentityFields.Count -ne 3 -or $IdentityFields[0] -cnotin @('e', 'l')) { throw 'invalid causal semantic identity' }
			$Id = "$(U $IdentityFields[1]):$(U $IdentityFields[2])"
			if ($IdentityFields[1] -eq '0' -or $IdentityFields[2] -eq '0') { throw 'zero semantic identity' }
			$Set = if ($IdentityFields[0] -ceq 'e') { $Entering } else { $Leaving }
			if ($Set.ContainsKey($Id) -or $Entering.ContainsKey($Id) -or $Leaving.ContainsKey($Id)) { throw 'duplicate causal semantic identity' }
			$Set[$Id] = $true
		}
		if ($Kind -notin 0, 10 -and ($Resolved.Count + $Entering.Count + $Leaving.Count) -ne 0) { throw 'semantic identities outside a preparation/installation' }
		switch ($Kind) {
			0 {
				if ($Peer.Prepared -or $Sequence -ne $Peer.NextSequence -or $Bytes -eq 0 -or $Bytes -gt 524288) { throw 'invalid prepared causal frame' }
				Coverage $Peer $Before $After $Resolved
				foreach ($Pending in $Resolved) {
					$Set = if ($Pending.Enter -eq 1) { $Entering } else { $Leaving }
					if (-not $Set.ContainsKey($Pending.Object)) { throw 'resolved pending token lacks its semantic operation' }
				}
				$Peer.Prepared = @{ Sequence = $Sequence; Bytes = $Bytes; Hash = (Fingerprint $F[11] $F[12]); After = $After; Resolved = @($Resolved) }
			}
			1 {
				$Prepared = $Peer.Prepared
				if (-not $Prepared -or $Sequence -ne $Prepared.Sequence -or $Bytes -ne $Prepared.Bytes -or
					(Fingerprint $F[11] $F[12]) -cne $Prepared.Hash -or $Token -le $Peer.LastGrant -or $Peer.Grant -or
					$State.GrantCount -ge 4 -or $Sequence -eq [decimal]18446744073709551615) { throw 'accepted causal frame differs from preparation or ownership' }
				$Peer.Grant = @{ Token = $Token; Sequence = $Sequence; Bytes = $Bytes; Hash = $Prepared.Hash;
					First = [decimal]0; Ack = [decimal]0; Base = $Peer.CumulativeAccepted }
				$Peer.CumulativeAccepted += $Bytes; $Peer.LastGrant = $Token; $Peer.NextSequence++; $State.GrantCount++
				Add-Bytes $Peer accepted $Bytes $false
				Apply-Coverage $Peer $Prepared.After $Prepared.Resolved; $Peer.Prepared = $null; Seal $Peer
			}
			2 { if (-not $Peer.Prepared -or $Peer.Prepared.Sequence -ne $Sequence) { throw 'unowned causal rejection' }; $Peer.Prepared = $null }
			3 {
				if ($Reason -ne 1 -or $Peer.Prepared -or $After -le $Before) { throw 'unexplained no-frame causal advance' }
				Coverage $Peer $Before $After @(); Apply-Coverage $Peer $After @(); Seal $Peer
			}
			4 {
				$Pending = Pending-Identity "$($N[14]):$($N[16]):$($N[17]):$($N[18])"
				if ($Peer.Prepared -or $Pending.Token -lt $Peer.PendingWatermark -or $Peer.SeenTokens.ContainsKey([string]$Pending.Token) -or $State.PendingCount -ge 1048576) { throw 'invalid new causal pending token' }
				$Peer.Pending[[string]$Pending.Token] = $Pending.Identity; $Peer.SeenTokens[[string]$Pending.Token] = $true; $State.PendingCount++
			}
			{ $_ -in 5, 6 } {
				$Pending = Pending-Identity "$($N[14]):$($N[16]):$($N[17]):$($N[18])"; $Old = [string]$Pending.Token
				if ($Peer.Prepared -or -not $Peer.Pending.ContainsKey($Old) -or $Peer.Pending[$Old] -cne $Pending.Identity) { throw 'unowned causal pending disposition' }
				if ($Kind -eq 5) {
					if ($Reason -notin 2, 3) { throw 'unexplained causal cancellation' }
					$Peer.Pending.Remove($Old); $Peer.Baseline.Remove($Old); $State.PendingCount--; Seal $Peer
				} else {
					$New = $N[15]
					if ($Reason -ne 4 -or $New -lt $Peer.PendingWatermark -or $Peer.SeenTokens.ContainsKey([string]$New)) { throw 'invalid causal token replacement' }
					$Peer.Pending.Remove($Old); $Peer.Pending[[string]$New] = $Pending.Identity; $Peer.SeenTokens[[string]$New] = $true
					if ($Peer.Baseline.ContainsKey($Old)) { $Peer.Baseline.Remove($Old); $Peer.Baseline[[string]$New] = $true }
				}
			}
			7 { throw 'required causal peer generation disconnected' }
			8 {
				$Grant = $Peer.Grant; $First = $N[12]; $Ack = $N[13]
				if (-not $Grant -or $Grant.Token -ne $Token -or $Sequence -ne $Grant.Sequence -or $Bytes -ne $Grant.Bytes -or
					(Fingerprint $F[11] $F[12]) -cne $Grant.Hash -or $N[22] -ne $Grant.Base -or
					$First -lt $Grant.First -or $Ack -lt $Grant.Ack -or $Ack -gt $First -or $First -gt $Bytes) { throw 'invalid native causal delivery' }
				$Prefix = $Peer.Sealed -and $Token -le $Peer.Cut
				Add-Bytes $Peer first_sent ($First - $Grant.First) $Prefix; Add-Bytes $Peer acked ($Ack - $Grant.Ack) $Prefix
				$Grant.First = $First; $Grant.Ack = $Ack
			}
			9 {
				$Grant = $Peer.Grant
				if (-not $Grant -or $Grant.Token -ne $Token -or $Grant.Bytes -ne $Bytes -or $Grant.First -ne $Bytes -or $Grant.Ack -ne $Bytes) { throw 'retirement lacks exact causal first-send and ACK' }
				Add-Bytes $Peer retired $Bytes ($Peer.Sealed -and $Token -le $Peer.Cut); $Peer.Grant = $null; $State.GrantCount--
			}
			10 {
				if ($Peer.Prepared -or $Entering.Count -or $Leaving.Count -or $Resolved.Count -ne $Peer.Pending.Count) { throw 'incomplete causal planning installation' }
				Coverage $Peer $Peer.Cursor $Peer.Cursor $Resolved
				if ($Peer.Planning) {
					foreach ($Pending in $Resolved) { $Peer.Baseline[[string]$Pending.Token] = $true }
					$Peer.Planning = $false; Seal $Peer
				}
			}
		}
		if ($null -eq $State.ConvergedUs -and @($State.Peers.Values | Where-Object { -not (Converged $_) }).Count -eq 0) {
			$State.ConvergedUs = $At - $CessationMicroseconds
		}
	}
	if (-not $Ended -or $State.Sources.Count -ne 32 -or $State.Peers.Count -ne 32 -or $null -eq $State.ConvergedUs -or
		@($State.Peers.Values | Where-Object { -not (Converged $_) }).Count -ne 0) { throw 'causal recovery has no complete finite convergence proof' }
	return @{ Peers = $State.Peers; Totals = $State.Totals; EventCount = $State.Events; QuotedFrames = $State.Quotes; EarliestConvergedUs = $State.ConvergedUs }
}
