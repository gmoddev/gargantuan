# Pure offline evidence fixture; never starts a process or contacts an endpoint.
function Save-RecoveryCausalFixture {
	param([string]$Root, [string]$RunId, [string]$Case, [long]$Tail, [long]$CessationMicroseconds)
	$Lines = [Collections.Generic.List[string]]::new()
	$Lines.Add("format=GargantuanRecoveryCausalV1`trun=$RunId`tcase=$Case")
	for ($Peer = 1; $Peer -le 32; $Peer++) {
		$Lines.Add((@('capture', $Peer, 1, 100, 1, ($Tail - 1), $Tail, 2, 1, 0, 0, 0, 0, 0, 0, 0, 100, 100, 100, 0, 0) -join "`t"))
	}
	$At = $CessationMicroseconds + 1000
	for ($Peer = 1; $Peer -le 32; $Peer++) {
		foreach ($Kind in @(0, 1, 8, 9)) {
			$Token = if ($Kind -eq 0) { 0 } else { $Peer }
			$First = if ($Kind -in 8, 9) { 77 } else { 0 }
			$Lines.Add((@('event', $Kind, 0, $At++, $Peer, 1, 2, $Token, 77, ($Tail - 1), $Tail, 1, 2, $First, $First, 0, 0, 0, 0, 0, 100, 1, 1, 100) -join "`t"))
		}
	}
	for ($Peer = 1; $Peer -le 32; $Peer++) { $Lines.Add((@('quote', $Peer, 1, 2, 77, 1, 2, ($Tail - 1), $Tail) -join "`t")) }
	for ($Peer = 1; $Peer -le 32; $Peer++) { $Lines.Add((@('source', $Peer, 1, $Tail, 3) -join "`t")) }
	$Lines.Add("end`t128`t0")
	[IO.File]::WriteAllLines((Join-Path $Root "recovery-$Case.tsv"), $Lines)
	for ($Peer = 1; $Peer -le 32; $Peer++) {
		"[Qualification:Recovery] event=causal_peer run=$RunId case=$Case connection_slot=$Peer connection_generation=1 journal_fence=$Tail cursor=$Tail cut_token=$Peer cut_sequence=2 unresolved=0 planning_unresolved=0 accepted=77 first_sent=77 acked=77 retired=77 converged=1"
	}
	"[Qualification:Recovery] event=causal_result run=$RunId case=$Case contract=causal_fence_v1 events=128 accepted=2464 first_sent=2464 acked=2464 retired=2464 prefix_converged_us=19000000 status=PASS"
}
