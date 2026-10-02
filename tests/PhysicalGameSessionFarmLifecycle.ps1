#requires -Version 7.0
# Offline replay only. Callers separately verify the immutable role manifests.
function Read-FarmLifecycleObservation {
	param([Parameter(Mandatory)][string]$ServerRoot, [Parameter(Mandatory)][string]$ClientRoot,
		[Parameter(Mandatory)][string]$RunManifestPath)
	$Manifest = Get-Content -LiteralPath $RunManifestPath -Raw | ConvertFrom-Json
	if (-not $Manifest.RunId -or @($Manifest.Nonces).Count -ne 32) { throw 'lifecycle manifest identity missing' }
	$Definitions = @([pscustomobject]@{ Role = 'server'; Slot = -1; Nonce = '0';
		Path = Join-Path $ServerRoot 'server.stdout.log' })
	$Definitions += @(0..31 | ForEach-Object { [pscustomobject]@{ Role = 'client'; Slot = $_;
		Nonce = [string]$Manifest.Nonces[$_]; Path = Join-Path $ClientRoot ('client-{0:D2}.stdout.log' -f $_) } })
	$Numeric = @('session_measured', 'session_terminal', 'connections', 'journal_readers', 'admission_owners',
		'admission_bytes', 'reserved', 'accepted', 'rolled_back', 'retired', 'terminal_release', 'outstanding',
		'active_grants', 'content_present', 'requested', 'acquiring', 'prepared', 'completion_reserved',
		'completed_bytes', 'decoded_bytes', 'resident', 'cached_bytes', 'records', 'resident_objects', 'valid')
	$Expected = @('event', 'contract', 'run_id', 'role', 'slot', 'nonce') + $Numeric
	$Rows = [Collections.Generic.List[object]]::new()
	$Hashes = [Collections.Generic.List[object]]::new()
	foreach ($Definition in $Definitions) {
		$File = Get-Item -LiteralPath $Definition.Path -ErrorAction Stop
		# Existing endpoint stream ceiling, not an added runtime gate.
		if ($File.Length -gt 16777216) { throw 'lifecycle source exceeds endpoint log ceiling' }
		$Lines = @([IO.File]::ReadLines($File.FullName) | Where-Object { $_.StartsWith('[Qualification:Lifecycle]') })
		if ($Lines.Count -gt 1) { throw 'duplicate lifecycle receipt' }
		$Hashes.Add([ordered]@{ Role = $Definition.Role; Slot = $Definition.Slot;
			Sha256 = (Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash.ToLowerInvariant() })
		if ($Lines.Count -eq 0) { continue }
		$Row = [ordered]@{}
		foreach ($Field in $Lines[0].Substring('[Qualification:Lifecycle]'.Length).Trim().Split(' ')) {
			if ($Field -notmatch '^([a-z_]+)=([^ =]+)$' -or $Row.Contains($Matches[1])) { throw 'malformed lifecycle field' }
			$Row[$Matches[1]] = $Matches[2]
		}
		if ($Row.Count -ne $Expected.Count -or @($Expected | Where-Object { -not $Row.Contains($_) }).Count) {
			throw 'lifecycle schema mismatch'
		}
		if ($Row.event -cne 'post_stop' -or $Row.contract -cne 'logical_stop_v1' -or
			$Row.run_id -cne [string]$Manifest.RunId -or $Row.role -cne $Definition.Role -or
			$Row.slot -cne [string]$Definition.Slot -or $Row.nonce -cne $Definition.Nonce) { throw 'lifecycle identity mismatch' }
		foreach ($Name in $Numeric) {
			[UInt64]$Value = 0
			if ($Row[$Name] -notmatch '^(0|[1-9][0-9]*)$' -or -not [UInt64]::TryParse($Row[$Name], [ref]$Value)) {
				throw "invalid lifecycle number $Name"
			}
			$Row[$Name] = $Value
		}
		if ($Row.session_measured -ne 1 -or $Row.session_terminal -ne 1 -or $Row.valid -ne 1 -or
			$Row.content_present -gt 1 -or ($Definition.Role -eq 'server' -and $Row.content_present -ne 1)) {
			throw 'lifecycle observation unavailable or nonterminal'
		}
		foreach ($Name in @('connections','journal_readers','admission_owners','outstanding','active_grants',
			'requested','acquiring','prepared','completion_reserved','completed_bytes','decoded_bytes')) {
			if ($Row[$Name] -ne 0) { throw "lifecycle retained transient ownership $Name" }
		}
		if ($Row.accepted -gt $Row.reserved -or [bigint]$Row.rolled_back -ne [bigint]$Row.reserved - [bigint]$Row.accepted) {
			throw 'lifecycle admission conservation mismatch'
		}
		# The server farm is POOLED_SERVICE. FULL_RESERVATION unit lifetimes have
		# no attributed-retirement equation; the native shared helper does not
		# invent one. Reconcile the pooled server's actual shutdown receipt here.
		if ($Definition.Role -eq 'server' -and ($Row.retired -gt $Row.accepted -or
			[bigint]$Row.terminal_release -ne [bigint]$Row.accepted - [bigint]$Row.retired)) {
			throw 'pooled lifecycle retirement conservation mismatch'
		}
		$Rows.Add($Row)
	}
	if ($Rows.Count -ne 0 -and $Rows.Count -ne 33) { throw 'partial lifecycle evidence cannot qualify cleanup' }
	return [ordered]@{ Contract = 'logical_stop_v1'; RunId = [string]$Manifest.RunId;
		State = $(if ($Rows.Count -eq 33) { 'MEASURED' } else { 'NOT_MEASURED' });
		RoleCount = $Rows.Count; Observations = @($Rows.ToArray()); SourceHashes = @($Hashes.ToArray()) }
}
