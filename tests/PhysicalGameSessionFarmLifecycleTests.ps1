#requires -Version 7.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PhysicalGameSessionFarmLifecycle.ps1')
$Root = Join-Path ([IO.Path]::GetTempPath()) ('farm-lifecycle-' + [Guid]::NewGuid().ToString('N'))
$Server = Join-Path $Root 'server'; $Clients = Join-Path $Root 'clients'
$Manifest = Join-Path $Root 'manifest.json'; $Run = [Guid]::NewGuid().ToString()
$Cases = 0
function Observe { Read-FarmLifecycleObservation -ServerRoot $Server -ClientRoot $Clients -RunManifestPath $Manifest }
function Reject([string]$Name, [string]$Text) {
	[IO.File]::WriteAllText((Join-Path $Server 'server.stdout.log'), $Text)
	$Failed = $false
	try { $null = Observe } catch { $Failed = $true }
	if (-not $Failed) { throw "accepted invalid lifecycle: $Name" }
	$script:Cases++
}
function Line([string]$Role, [int]$Slot, [string]$Nonce) {
	return "[Qualification:Lifecycle] event=post_stop contract=logical_stop_v1 run_id=$Run role=$Role slot=$Slot nonce=$Nonce " +
		'session_measured=1 session_terminal=1 connections=0 journal_readers=0 admission_owners=0 admission_bytes=64 ' +
		'reserved=12 accepted=10 rolled_back=2 retired=10 terminal_release=0 outstanding=0 active_grants=0 content_present=1 ' +
		'requested=0 acquiring=0 prepared=0 completion_reserved=0 completed_bytes=0 decoded_bytes=0 ' +
		'resident=4 cached_bytes=1024 records=8 resident_objects=16 valid=1'
}
try {
	[void][IO.Directory]::CreateDirectory($Server); [void][IO.Directory]::CreateDirectory($Clients)
	[IO.File]::WriteAllText($Manifest, (@{RunId=$Run; Nonces=@(1..32 | ForEach-Object { [string]$_ })} | ConvertTo-Json))
	[IO.File]::WriteAllText((Join-Path $Server 'server.stdout.log'), '')
	foreach ($Slot in 0..31) { [IO.File]::WriteAllText((Join-Path $Clients ('client-{0:D2}.stdout.log' -f $Slot)), '') }
	if ((Observe).State -ne 'NOT_MEASURED') { throw 'historical absence claimed measured' }; $Cases++
	$Good = Line 'server' -1 '0'
	Reject 'partial records' $Good
	foreach ($Slot in 0..31) {
		[IO.File]::WriteAllText((Join-Path $Clients ('client-{0:D2}.stdout.log' -f $Slot)), (Line 'client' $Slot ([string]($Slot+1))))
	}
	[IO.File]::WriteAllText((Join-Path $Server 'server.stdout.log'), $Good)
	$Result = Observe
	if ($Result.State -ne 'MEASURED' -or $Result.RoleCount -ne 33 -or $Result.SourceHashes.Count -ne 33 -or
		$Result.Observations[0].cached_bytes -ne 1024) { throw 'valid full receipt/cache diagnostics rejected' }; $Cases++
	Assert-FarmLifecycleAdmission -Observation $Result -Admission @{accepted=10;retired=10;terminal_release=0}
	$Cases++
	foreach ($Name in @('accepted','retired','terminal_release')) {
		$Admission = @{accepted=10;retired=10;terminal_release=0}
		$Admission[$Name]++
		$Failed = $false
		try { Assert-FarmLifecycleAdmission -Observation $Result -Admission $Admission } catch { $Failed = $true }
		if (-not $Failed) { throw "post-Stop counter reset/contradiction accepted: $Name" }
		$Cases++
	}
	foreach ($Name in @('session_measured','session_terminal','valid','content_present')) {
		Reject $Name ($Good.Replace("$Name=1", "$Name=0"))
	}
	foreach ($Name in @('connections','journal_readers','admission_owners','outstanding','active_grants',
		'requested','acquiring','prepared','completion_reserved','completed_bytes','decoded_bytes')) {
		Reject $Name ($Good.Replace("$Name=0", "$Name=1"))
	}
	Reject 'reservation mismatch' ($Good.Replace('reserved=12', 'reserved=13'))
	Reject 'retirement mismatch' ($Good.Replace('retired=10', 'retired=9'))
	Reject 'stale run' ($Good.Replace($Run, [Guid]::NewGuid().ToString()))
	Reject 'wrong role' ($Good.Replace('role=server', 'role=client'))
	Reject 'wrong slot' ($Good.Replace('slot=-1', 'slot=0'))
	Reject 'wrong nonce' ($Good.Replace('nonce=0', 'nonce=1'))
	Reject 'duplicate field' ($Good + ' accepted=10')
	Reject 'unknown field' ($Good + ' ignored=0')
	Reject 'missing field' ($Good.Replace(' requested=0', ''))
	Reject 'duplicate row' ($Good + "`n" + $Good)
	Reject 'unsigned overflow' ($Good.Replace('accepted=10', 'accepted=18446744073709551616'))
	Reject 'negative' ($Good.Replace('requested=0', 'requested=-1'))
	Reject 'exact unsigned mismatch' ($Good.Replace('reserved=12 accepted=10 rolled_back=2 retired=10',
		'reserved=18446744073709551615 accepted=18446744073709551614 rolled_back=2 retired=18446744073709551614'))
	[IO.File]::WriteAllText((Join-Path $Server 'server.stdout.log'), $Good)
	$ClientPath = Join-Path $Clients 'client-31.stdout.log'
	[IO.File]::WriteAllText($ClientPath, (Line 'client' 31 'wrong'))
	$Failed = $false; try { $null = Observe } catch { $Failed = $true }
	if (-not $Failed) { throw 'client identity mismatch accepted' }; $Cases++
	Write-Output "Physical farm lifecycle tests passed: $Cases cases"
} finally {
	$Resolved = [IO.Path]::GetFullPath($Root)
	if (-not $Resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase)) {
		throw 'scratch path escaped temporary directory'
	}
	Remove-Item -LiteralPath $Resolved -Recurse -Force
}
