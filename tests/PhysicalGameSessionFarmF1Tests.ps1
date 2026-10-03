#requires -Version 7.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PhysicalGameSessionFarmF1.ps1')
$Root = Join-Path ([IO.Path]::GetTempPath()) ('farm-f1-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($Root)
$Manifest = Join-Path $Root 'run.json'; $Log = Join-Path $Root 'server.stdout.log'
$Run = 'f1-regression'; $Cases = 0
try {
	[IO.File]::WriteAllText($Manifest, (@{RunId=$Run; Nonces=@(1..32)} | ConvertTo-Json))
	$Ready = @(1..32 | ForEach-Object { "[Qualification:Server] event=ready run=$Run nonce=$_ connection_slot=$_ connection_generation=1 session_epoch=1 player_id=$_" })
	$Rows = @(1..32 | ForEach-Object { "[Qualification:FarmF1] event=peer contract=finite_grant_v1 run=$Run connection=$($_):1 samples=4 completed=2 qualified=2 completed_bytes=154 accepted=154 first_sent=154 acked=154 retired=154 last_token=2 last_bytes=77 last_activated_us=20000 last_first_us=20010 last_completed_us=20010 max_running_byte_us=0 max_finite_shortfall_byte_us=0 failed=0 valid=1" })
	function Read-Case($InputRows) {
		[IO.File]::WriteAllLines($Log, [string[]]($Ready + @($InputRows)))
		Read-FarmF1Observation -ServerRoot $Root -RunManifestPath $Manifest
	}
	$Good = Read-Case $Rows
	if ($Good.State -cne 'MEASURED_PASS' -or $Good.Observations.Count -ne 32) { throw 'valid F1 evidence failed' }; $Cases++
	Assert-FarmF1Admission $Good @{accepted=4928; retired=4928; terminal_release=0}; $Cases++
	if ((Read-Case @()).State -cne 'NOT_MEASURED') { throw 'historical F1 absence promoted' }; $Cases++
	foreach ($Mutation in @(
		@('failed=0','failed=1'), @('qualified=2','qualified=1'), @('max_running_byte_us=0','max_running_byte_us=18025216001'),
		@('max_finite_shortfall_byte_us=0','max_finite_shortfall_byte_us=1'), @('last_first_us=20010','last_first_us=19999'),
		@('acked=154','acked=153'), @('retired=154','retired=0'), @('last_token=2','last_token=0'),
		@('last_bytes=77','last_bytes=524289'), @('connection=32:1','connection=32:2'), @('samples=4','samples=0'),
		@('run=f1-regression','run=stale'), @('completed=2','completed=3'), @('valid=1','valid=0'))) {
		$Changed = @($Rows); $Changed[31] = $Changed[31].Replace($Mutation[0], $Mutation[1])
		$Rejected = $false; try { Read-Case $Changed | Out-Null } catch { $Rejected = $true }
		if (-not $Rejected) { throw "forged F1 accepted: $Mutation" }; $Cases++
	}
	foreach ($Changed in @(@($Rows[0..30]), @($Rows + $Rows[0]))) {
		$Rejected=$false; try { Read-Case $Changed | Out-Null } catch { $Rejected=$true }
		if (-not $Rejected) { throw 'partial/duplicate F1 accepted' }; $Cases++
	}
	$Rejected=$false; try { Assert-FarmF1Admission $Good @{accepted=4929;retired=4929;terminal_release=0} } catch { $Rejected=$true }
	if (-not $Rejected) { throw 'aggregate F1 forgery accepted' }; $Cases++
	Write-Output "[Qualification:FarmF1Tests] PASS cases=$Cases"
} finally {
	$Resolved=[IO.Path]::GetFullPath($Root); $Temp=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/')
	if (-not $Resolved.StartsWith($Temp+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or
		[IO.Path]::GetFileName($Resolved) -cnotmatch '^farm-f1-[a-f0-9]{32}$') { throw 'F1 fixture cleanup escaped temp root' }
	Remove-Item -LiteralPath $Resolved -Recurse -Force
}
