#requires -Version 7.0
function Read-FarmRemoteOwnershipObservation {
	param([Parameter(Mandatory)][string]$ServerRoot, [Parameter(Mandatory)][string]$ClientRoot,
		[Parameter(Mandatory)][string]$RunManifestPath)
	$Manifest = Get-Content -LiteralPath $RunManifestPath -Raw | ConvertFrom-Json
	if (-not $Manifest.RunId -or @($Manifest.Nonces).Count -ne 32) { throw 'Remote ownership manifest identity missing' }
	$Definitions = @([pscustomobject]@{ Role='server'; Slot=-1; Nonce='0'; Path=Join-Path $ServerRoot 'server.stdout.log' })
	$Definitions += @(0..31 | ForEach-Object { [pscustomobject]@{ Role='client'; Slot=$_; Nonce=[string]$Manifest.Nonces[$_];
		Path=Join-Path $ClientRoot ('client-{0:D2}.stdout.log' -f $_) } })
	$Numeric = @('observed','dispatch_current','dispatch_bytes_current','deferred_current','deferred_bytes_current',
		'outgoing_current','handlers_current','dispatch_high','dispatch_bytes_high','deferred_high','deferred_bytes_high',
		'outgoing_high','handlers_high','peer_dispatch_high','peer_dispatch_bytes_high','peer_deferred_bytes_high',
		'peer_outgoing_high','peer_handlers_high','dispatch_accepted','dispatch_released','deferred_accepted','deferred_released',
		'handlers_started','handlers_released','handlers_expired','deferred_expired','requests_started','requests_completed',
		'requests_timed_out','requests_cancelled','handler_errors','resource_rejections','bound_violations',
		'dispatch_residence_us','deferred_residence_us','outgoing_residence_us','handler_residence_us','deadline_overshoot_us','valid')
	$Expected = @('event','contract','run_id','role','slot','nonce') + $Numeric
	$Rows = [Collections.Generic.List[object]]::new(); $Hashes = [Collections.Generic.List[object]]::new()
	foreach ($Definition in $Definitions) {
		$File = Get-Item -LiteralPath $Definition.Path -ErrorAction Stop
		if ($File.Length -gt 16777216) { throw 'Remote ownership source exceeds endpoint log ceiling' }
		$Lines = @([IO.File]::ReadLines($File.FullName) | Where-Object { $_.StartsWith('[Qualification:RemoteOwnership]') })
		if ($Lines.Count -gt 1) { throw 'duplicate Remote ownership receipt' }
		$Hashes.Add([ordered]@{Role=$Definition.Role; Slot=$Definition.Slot;
			Sha256=(Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash.ToLowerInvariant()})
		if ($Lines.Count -eq 0) { continue }
		$Row = [ordered]@{}
		foreach ($Field in $Lines[0].Substring('[Qualification:RemoteOwnership]'.Length).Trim().Split(' ')) {
			if ($Field -notmatch '^([a-z_]+)=([^ =]+)$' -or $Row.Contains($Matches[1])) { throw 'malformed Remote ownership field' }
			$Row[$Matches[1]] = $Matches[2]
		}
		if ($Row.Count -ne $Expected.Count -or @($Expected | Where-Object { -not $Row.Contains($_) }).Count) { throw 'Remote ownership schema mismatch' }
		if ($Row.event -cne 'post_stop' -or $Row.contract -cne 'remote_ownership_v1' -or
			$Row.run_id -cne [string]$Manifest.RunId -or $Row.role -cne $Definition.Role -or
			$Row.slot -cne [string]$Definition.Slot -or $Row.nonce -cne $Definition.Nonce) { throw 'Remote ownership identity mismatch' }
		foreach ($Name in $Numeric) {
			[UInt64]$Value=0
			if ($Row[$Name] -notmatch '^(0|[1-9][0-9]*)$' -or -not [UInt64]::TryParse($Row[$Name],[ref]$Value)) { throw "invalid Remote ownership number $Name" }
			$Row[$Name]=$Value
		}
		if ($Row.observed -ne 1 -or $Row.valid -ne 1) { throw 'Remote ownership observation unavailable' }
		foreach ($Name in @('dispatch_current','dispatch_bytes_current','deferred_current','deferred_bytes_current',
			'outgoing_current','handlers_current','bound_violations')) {
			if ($Row[$Name] -ne 0) { throw "Remote retained ownership or violated bound $Name" }
		}
		# Existing RemoteManager safety ceilings. Negotiated per-peer ceilings are
		# also checked at each native insertion (bound_violations), not inferred
		# from a maximum collected after a peer has been removed.
		$Limits = @{dispatch_high=8192; dispatch_bytes_high=33554432; deferred_high=8192;
			deferred_bytes_high=33554432; outgoing_high=8192; handlers_high=4096; peer_handlers_high=64}
		foreach ($Name in $Limits.Keys) { if ($Row[$Name] -gt $Limits[$Name]) { throw "Remote high-water exceeds canonical bound $Name" } }
		foreach ($Pair in @(@('dispatch_accepted','dispatch_released'),@('deferred_accepted','deferred_released'),
			@('handlers_started','handlers_released'),@('requests_started','requests_completed'))) {
			if ($Row[$Pair[0]] -ne $Row[$Pair[1]]) { throw "Remote terminal conservation mismatch $($Pair[0])" }
		}
		foreach ($Pair in @(@('dispatch_high','dispatch_accepted'),@('deferred_high','deferred_accepted'),
			@('handlers_high','handlers_started'),@('outgoing_high','requests_started'),
			@('handlers_expired','handlers_released'),@('deferred_expired','deferred_released'),
			@('requests_timed_out','requests_completed'),@('requests_cancelled','requests_completed'),
			@('peer_dispatch_high','dispatch_high'),@('peer_dispatch_bytes_high','dispatch_bytes_high'),
			@('peer_deferred_bytes_high','deferred_bytes_high'),@('peer_outgoing_high','outgoing_high'),@('peer_handlers_high','handlers_high'))) {
			if ($Row[$Pair[0]] -gt $Row[$Pair[1]]) { throw "Remote ownership hierarchy mismatch $($Pair[0])" }
		}
		# Residence/expiry overshoot are measured diagnostics. A work lease is
		# observed at the next Pump; there is no new instantaneous 30s gate.
		$Rows.Add($Row)
	}
	if ($Rows.Count -ne 0 -and $Rows.Count -ne 33) { throw 'partial Remote ownership evidence' }
	return [ordered]@{Contract='remote_ownership_v1'; RunId=[string]$Manifest.RunId;
		State=$(if($Rows.Count -eq 33){'MEASURED'}else{'NOT_MEASURED'}); RoleCount=$Rows.Count;
		Observations=@($Rows.ToArray()); SourceHashes=@($Hashes.ToArray())}
}
