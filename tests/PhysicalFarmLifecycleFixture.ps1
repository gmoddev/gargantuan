# Mock evidence only; never imported by a live endpoint or acceptance parser.
function Add-FarmLifecycleFixture {
	param([string]$ServerRoot, [string]$ClientRoot, $Manifest)
	foreach ($Slot in -1..31) {
		$Role = if ($Slot -eq -1) { 'server' } else { 'client' }
		$Nonce = if ($Slot -eq -1) { '0' } else { [string]$Manifest.Nonces[$Slot] }
		$Path = if ($Slot -eq -1) { Join-Path $ServerRoot 'server.stdout.log' } else {
			Join-Path $ClientRoot ('client-{0:D2}.stdout.log' -f $Slot) }
		$Line = "[Qualification:Lifecycle] event=post_stop contract=logical_stop_v1 run_id=$($Manifest.RunId) role=$Role slot=$Slot nonce=$Nonce " +
			'session_measured=1 session_terminal=1 connections=0 journal_readers=0 admission_owners=0 admission_bytes=64 ' +
			'reserved=12 accepted=10 rolled_back=2 retired=10 terminal_release=0 outstanding=0 active_grants=0 content_present=1 ' +
			'requested=0 acquiring=0 prepared=0 completion_reserved=0 completed_bytes=0 decoded_bytes=0 ' +
			'resident=4 cached_bytes=1024 records=8 resident_objects=16 valid=1'
		if ($Role -ceq 'server') {
			$Admission = @([IO.File]::ReadLines($Path) | Where-Object { $_.StartsWith('[Qualification:Admission] event=result ') })
			if ($Admission.Count -ne 1 -or $Admission[0] -notmatch ' accepted=([0-9]+) ') { throw 'fixture missing final admission' }
			$Accepted = $Matches[1]
			$Line = $Line.Replace('reserved=12 accepted=10 rolled_back=2 retired=10',
				"reserved=$Accepted accepted=$Accepted rolled_back=0 retired=$Accepted")
		}
		[IO.File]::AppendAllText($Path, "`n$Line`n")
		$Remote = "[Qualification:RemoteOwnership] event=post_stop contract=remote_ownership_v1 run_id=$($Manifest.RunId) role=$Role slot=$Slot nonce=$Nonce " +
			'observed=1 dispatch_current=0 dispatch_bytes_current=0 deferred_current=0 deferred_bytes_current=0 outgoing_current=0 handlers_current=0 ' +
			'dispatch_high=2 dispatch_bytes_high=512 deferred_high=1 deferred_bytes_high=128 outgoing_high=1 handlers_high=1 ' +
			'peer_dispatch_high=2 peer_dispatch_bytes_high=512 peer_deferred_bytes_high=128 peer_outgoing_high=1 peer_handlers_high=1 ' +
			'dispatch_accepted=4 dispatch_released=4 deferred_accepted=2 deferred_released=2 handlers_started=2 handlers_released=2 ' +
			'handlers_expired=0 deferred_expired=0 requests_started=2 requests_completed=2 requests_timed_out=0 requests_cancelled=0 ' +
			'handler_errors=0 resource_rejections=0 bound_violations=0 dispatch_residence_us=1000 deferred_residence_us=1000 ' +
			'outgoing_residence_us=1000 handler_residence_us=1000 deadline_overshoot_us=0 valid=1'
		[IO.File]::AppendAllText($Path, "$Remote`n")
	}
}
