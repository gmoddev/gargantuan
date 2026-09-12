param([Parameter(Mandatory)][string]$ServerPath, [Parameter(Mandatory)][string]$PlayerPath)
$ErrorActionPreference = 'Stop'
$Culture = [Globalization.CultureInfo]::InvariantCulture
function Get-Distribution([double[]]$Values) {
	if (!$Values.Count) { return $null }
	$Sorted = @($Values | Sort-Object)
	[ordered]@{ Count=$Sorted.Count; Sum=($Sorted | Measure-Object -Sum).Sum
		P50=$Sorted[[int][math]::Floor(($Sorted.Count-1)*.5)]
		P95=$Sorted[[int][math]::Floor(($Sorted.Count-1)*.95)]
		P99=$Sorted[[int][math]::Floor(($Sorted.Count-1)*.99)]; Max=$Sorted[-1] }
}
function Read-Trace([string]$Path) {
	$Rows = [Collections.Generic.List[object]]::new()
	$Invalid = 0
	foreach ($Line in [IO.File]::ReadLines((Resolve-Path -LiteralPath $Path))) {
		if (!$Line.StartsWith('[Network:Service] ')) { continue }
		$Record = @{}
		foreach ($Match in [regex]::Matches($Line, '(\w+)=([^ ]+)')) {
			$Key = $Match.Groups[1].Value; $Value = $Match.Groups[2].Value
			$Record[$Key] = if ($Key -in @('host','stage')) { $Value } else { [double]::Parse($Value,$Culture) }
		}
		if ($Record.Count -ne 28) { ++$Invalid; continue }
		$Rows.Add([pscustomobject]$Record)
	}
	if (!$Rows.Count) { throw "No complete trace rows: $Path" }
	if (@($Rows | ForEach-Object {"$($_.peer):$($_.gen)"} | Sort-Object -Unique).Count -ne 1) {
		throw "This official-fixture analyzer requires one peer generation per host: $Path"
	}
	[pscustomobject]@{Rows=$Rows; Invalid=$Invalid; End=@(Select-String -LiteralPath $Path -Pattern '^\[Network:ServiceEnd\]' | ForEach-Object Line)}
}
function Get-Class($Kind) {
	switch ([int]$Kind) {
		1 {'CharacterBind'} 2 {'CharacterUnbind'} 4 {'ActionRequest'} 6 {'CharacterReliableState'} 7 {'ActionResult'}
		100 {'ReliableEvent'} 103 {'RpcRequest'} 104 {'RpcResponse'} 105 {'RpcError'} 106 {'RpcCancel'}
		200 {'GRPL'} 300 {'Bootstrap'} default {'Other'}
	}
}
function Get-Summary($Trace) {
	$Rows = $Trace.Rows
	$Sends = @($Rows | Where-Object { $_.stage -eq 'GnsQueued' -and $_.delivery -eq 0 -and $_.result -eq 1 })
	$Class = foreach ($Group in ($Sends | Group-Object { Get-Class $_.kind })) {
		[ordered]@{ Name=$Group.Name; Messages=$Group.Count; PayloadBytes=($Group.Group|Measure-Object bytes -Sum).Sum
			EnvelopeBytes=$Group.Count*32; Sizes=Get-Distribution @($Group.Group.bytes)
			QueueEstimateMs=Get-Distribution @($Group.Group|ForEach-Object { $_.queueUs/1000 }) }
	}
	$Poll = @($Rows | Where-Object { $_.stage -eq 'GnsPoll' -and $_.pending -ge 0 })
	$Peak = $Rows | Sort-Object pending -Descending | Select-Object -First 1
	$Above1M = 0.0
	for ($I=1; $I -lt $Poll.Count; ++$I) {
		if ($Poll[$I-1].pending -ge 1000000) { $Above1M += ($Poll[$I].ns-$Poll[$I-1].ns)/1e9 }
	}
	$Timeline = [Collections.Generic.List[object]]::new(); $Last = 0
	foreach ($R in $Poll) {
		if ($R.ns-$Last -lt 5e8) {continue}; $Last=$R.ns
		$Timeline.Add([ordered]@{Seconds=($R.ns-$Rows[0].ns)/1e9; Pending=$R.pending; Unacked=$R.unacked
			PendingUnreliable=$R.pendingUnreliable; QueueEstimateMs=$R.queueUs/1000; OutBps=$R.outBps; InBps=$R.inBps})
	}
	$Receive = @($Rows | Where-Object stage -eq 'GnsReceive')
	$Gaps = foreach ($Kind in @(100,104,6)) {
		$Group = @($Receive|Where-Object kind -eq $Kind); $Values=@()
		for($I=1;$I -lt $Group.Count;++$I){$Values+=($Group[$I].ns-$Group[$I-1].ns)/1e6}
		[ordered]@{Kind=$Kind; ReceiveGapMs=Get-Distribution $Values}
	}
	[ordered]@{Records=$Rows.Count; InvalidRows=$Trace.Invalid; End=$Trace.End; Classes=@($Class)
		ReliablePayloadBytes=($Sends|Measure-Object bytes -Sum).Sum; ReliableMessages=$Sends.Count
		SendFailures=@($Rows|Where-Object {$_.stage -eq 'GnsQueued' -and $_.result -ne 1}).Count
		RateMin=@($Poll.rateMin|Sort-Object -Unique); RateMax=@($Poll.rateMax|Sort-Object -Unique)
		Rate=@($Poll.rate|Sort-Object -Unique); Peak=$Peak; Above1MSeconds=$Above1M
		ReceiveAgeMs=Get-Distribution @($Receive|ForEach-Object {$_.receiveAgeUs/1000})
		ReceiveGaps=@($Gaps); Timeline=$Timeline}
}
$Server = Read-Trace $ServerPath; $Player = Read-Trace $PlayerPath
# Correlate within each process. No subtraction of sender and receiver clocks.
$Rpc = foreach ($Group in ($Server.Rows | Where-Object { $_.kind -in @(103,104) } | Group-Object peer,gen,object,objectGen,id)) {
	$S=@{}; foreach($R in $Group.Group){$S[$R.stage + ':' + $R.kind]=$R}
	$First=$Group.Group[0]
	$P=@{}; foreach($R in ($Player.Rows | Where-Object {$_.object -eq $First.object -and $_.objectGen -eq $First.objectGen -and $_.id -eq $First.id -and $_.kind -in @(103,104)})) {
		$P[$R.stage + ':' + $R.kind]=$R
	}
	function Delta($Map,[string]$A,[string]$B) {if ($Map.ContainsKey($A) -and $Map.ContainsKey($B)){return ($Map[$B].ns-$Map[$A].ns)/1e6}; return $null}
	[ordered]@{Id=$First.id; Object=$First.object
		RequestAcceptToGnsMs=Delta $P 'SchedulerAccepted:103' 'GnsQueued:103'
		RequestQueueEstimateMs=if($P.ContainsKey('GnsBefore:103')){$P['GnsBefore:103'].queueUs/1000}else{$null}
		ServerReceiveToCallbackMs=Delta $S 'GnsReceive:103' 'ServerCallback:103'
		ServerCallbackToHandlerMs=Delta $S 'ServerCallback:103' 'RpcHandler:103'
		HandlerToResponseMs=Delta $S 'RpcHandler:103' 'RpcResponseProduced:104'
		ResponseProducedToAcceptMs=Delta $S 'RpcResponseProduced:104' 'SchedulerAccepted:104'
		ResponseAcceptToGnsBeforeMs=Delta $S 'SchedulerAccepted:104' 'GnsBefore:104'
		ResponseGnsCallMs=Delta $S 'GnsBefore:104' 'GnsQueued:104'
		ResponseQueueEstimateMs=if($S.ContainsKey('GnsBefore:104')){$S['GnsBefore:104'].queueUs/1000}else{$null}
		ResponsePending=if($S.ContainsKey('GnsBefore:104')){$S['GnsBefore:104'].pending}else{$null}
		BackendSendId=if($S.ContainsKey('GnsQueued:104')){$S['GnsQueued:104'].backendId}else{$null}
		BackendReceiveId=if($P.ContainsKey('GnsReceive:104')){$P['GnsReceive:104'].backendId}else{$null}
		PlayerGnsReceiveAgeMs=if($P.ContainsKey('GnsReceive:104')){$P['GnsReceive:104'].receiveAgeUs/1000}else{$null}
		PlayerReceiveToCallbackMs=Delta $P 'GnsReceive:104' 'ClientCallback:104'
		PlayerCallbackToCompletionMs=Delta $P 'ClientCallback:104' 'RpcCompletion:104'
		ClientLocalAcceptToReceiveMs=Delta $P 'SchedulerAccepted:103' 'GnsReceive:104'
		ClientLocalAcceptToCompletionMs=Delta $P 'SchedulerAccepted:103' 'RpcCompletion:104'}
}
$FrameLines = @(Get-Content -LiteralPath $ServerPath | Where-Object {$_.StartsWith('[Runtime:ServerFrame] ')} | ForEach-Object {$_.Substring(22)})
if (!$FrameLines.Count -or !$FrameLines[0].StartsWith('unix_us,')) { throw 'Missing server frame header' }
$FieldCount = $FrameLines[0].Split(',').Count
$CompletePattern = '^\d+(,\d+){' + ($FieldCount - 1) + '}$'
$CompleteFrameLines = @($FrameLines | Select-Object -Skip 1 | Where-Object {$_ -match $CompletePattern})
$Frames = @(@($FrameLines[0]) + $CompleteFrameLines | ConvertFrom-Csv)
$FrameSummary = @{}
foreach($Field in @('wire_bytes','selected','committed','pending','interval_ns','poll_ns','session_ns')) {
	$FrameSummary[$Field]=Get-Distribution @($Frames|ForEach-Object{[double]$_.$Field})
}
$Bursts = @($Server.Rows|Where-Object {$_.stage -eq 'GnsQueued' -and $_.kind -eq 200 -and $_.bytes -ge 100000} | ForEach-Object{
	[ordered]@{Seconds=($_.ns-$Server.Rows[0].ns)/1e9; Bytes=$_.bytes; Operations=$_.ops; Sequence=$_.id; BackendId=$_.backendId; Pending=$_.pending}})
$RpcDistributions=@{}
if($Rpc.Count){foreach($Key in $Rpc[0].Keys){if($Key.EndsWith('Ms')){$RpcDistributions[$Key]=Get-Distribution @($Rpc|ForEach-Object{if($null -ne $_[$Key]){$_[$Key]}})}}}
[ordered]@{Server=Get-Summary $Server; Player=Get-Summary $Player; Rpc=$Rpc; RpcDistributions=$RpcDistributions
	ServerFrames=$FrameSummary; IncompleteServerFrames=$FrameLines.Count-1-$CompleteFrameLines.Count
	LargeGrpl=$Bursts} | ConvertTo-Json -Depth 12
