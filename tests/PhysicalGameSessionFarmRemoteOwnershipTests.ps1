#requires -Version 7.0
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PhysicalGameSessionFarmRemoteOwnership.ps1')
$Root = Join-Path ([IO.Path]::GetTempPath()) ('farm-remote-ownership-' + [Guid]::NewGuid().ToString('N'))
$Server = Join-Path $Root 'server'; $Clients = Join-Path $Root 'clients'
$Manifest = Join-Path $Root 'manifest.json'; $Run = [Guid]::NewGuid().ToString(); $Cases=0
function Observe { Read-FarmRemoteOwnershipObservation -ServerRoot $Server -ClientRoot $Clients -RunManifestPath $Manifest }
function Reject([string]$Name, [string]$Text) {
	[IO.File]::WriteAllText((Join-Path $Server 'server.stdout.log'), $Text)
	$Failed=$false; try { $null=Observe } catch { $Failed=$true }
	if(-not $Failed){throw "accepted invalid Remote ownership: $Name"}; $script:Cases++
}
function Line([string]$Role,[int]$Slot,[string]$Nonce) {
	return "[Qualification:RemoteOwnership] event=post_stop contract=remote_ownership_v1 run_id=$Run role=$Role slot=$Slot nonce=$Nonce " +
		'observed=1 dispatch_current=0 dispatch_bytes_current=0 deferred_current=0 deferred_bytes_current=0 outgoing_current=0 handlers_current=0 ' +
		'dispatch_high=2 dispatch_bytes_high=512 deferred_high=1 deferred_bytes_high=128 outgoing_high=1 handlers_high=1 ' +
		'peer_dispatch_high=2 peer_dispatch_bytes_high=512 peer_deferred_bytes_high=128 peer_outgoing_high=1 peer_handlers_high=1 ' +
		'dispatch_accepted=4 dispatch_released=4 deferred_accepted=2 deferred_released=2 handlers_started=2 handlers_released=2 ' +
		'handlers_expired=1 deferred_expired=1 requests_started=2 requests_completed=2 requests_timed_out=1 requests_cancelled=1 ' +
		'handler_errors=0 resource_rejections=0 bound_violations=0 dispatch_residence_us=1000 deferred_residence_us=1000 ' +
		'outgoing_residence_us=1000 handler_residence_us=30002000 deadline_overshoot_us=2000 valid=1'
}
try {
	[void][IO.Directory]::CreateDirectory($Server); [void][IO.Directory]::CreateDirectory($Clients)
	[IO.File]::WriteAllText($Manifest,(@{RunId=$Run;Nonces=@(1..32|ForEach-Object{[string]$_})}|ConvertTo-Json))
	[IO.File]::WriteAllText((Join-Path $Server 'server.stdout.log'),'')
	foreach($Slot in 0..31){[IO.File]::WriteAllText((Join-Path $Clients ('client-{0:D2}.stdout.log' -f $Slot)),'')}
	if((Observe).State -ne 'NOT_MEASURED'){throw 'historical absence claimed measured'}; $Cases++
	$Good=Line 'server' -1 '0'
	Reject 'partial' $Good
	foreach($Slot in 0..31){[IO.File]::WriteAllText((Join-Path $Clients ('client-{0:D2}.stdout.log' -f $Slot)),(Line 'client' $Slot ([string]($Slot+1))))}
	[IO.File]::WriteAllText((Join-Path $Server 'server.stdout.log'),$Good)
	$Result=Observe
	if($Result.State -ne 'MEASURED' -or $Result.RoleCount -ne 33 -or $Result.SourceHashes.Count -ne 33 -or
		$Result.Observations[0].handler_residence_us -ne 30002000){throw 'valid ownership/actual lease overshoot rejected'}; $Cases++
	foreach($Name in @('dispatch_current','dispatch_bytes_current','deferred_current','deferred_bytes_current','outgoing_current','handlers_current','bound_violations')){
		Reject $Name ($Good.Replace("$Name=0","$Name=1"))
	}
	foreach($Name in @('observed','valid')){Reject $Name ($Good.Replace("$Name=1","$Name=0"))}
	foreach($Pair in @(@('dispatch_high=2','dispatch_high=8193'),@('dispatch_bytes_high=512','dispatch_bytes_high=33554433'),
		@('deferred_high=1','deferred_high=8193'),@('deferred_bytes_high=128','deferred_bytes_high=33554433'),
		@('outgoing_high=1','outgoing_high=8193'),@('handlers_high=1','handlers_high=4097'),@('peer_handlers_high=1','peer_handlers_high=65'),
		@('dispatch_released=4','dispatch_released=3'),@('deferred_released=2','deferred_released=1'),
		@('handlers_released=2','handlers_released=1'),@('requests_completed=2','requests_completed=1'),
		@('handlers_expired=1','handlers_expired=3'),@('deferred_expired=1','deferred_expired=3'),
		@('requests_timed_out=1','requests_timed_out=3'),@('peer_dispatch_high=2','peer_dispatch_high=3'))){
		Reject $Pair[0] ($Good.Replace($Pair[0],$Pair[1]))
	}
	Reject 'missingfield' ($Good.Replace(' dispatch_current=0',''))
	Reject 'unknownfield' ($Good+' new_bound=0')
	Reject 'duplicatefield' ($Good+' observed=1')
	Reject 'duplicaterow' ($Good+"`n"+$Good)
	Reject 'run' ($Good.Replace($Run,[Guid]::NewGuid().ToString()))
	Reject 'role' ($Good.Replace('role=server','role=client'))
	Reject 'slot' ($Good.Replace('slot=-1','slot=0'))
	Reject 'nonce' ($Good.Replace('nonce=0','nonce=1'))
	Reject 'negative' ($Good.Replace('dispatch_residence_us=1000','dispatch_residence_us=-1'))
	Reject 'overflow' ($Good.Replace('dispatch_residence_us=1000','dispatch_residence_us=18446744073709551616'))
	Write-Output "Physical farm Remote ownership tests passed: $Cases cases"
} finally {
	$Resolved=[IO.Path]::GetFullPath($Root)
	if(-not $Resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()),[StringComparison]::OrdinalIgnoreCase)){throw 'scratch escaped temporary directory'}
	Remove-Item -LiteralPath $Resolved -Recurse -Force
}
