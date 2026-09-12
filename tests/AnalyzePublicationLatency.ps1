param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Output)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $Output) { throw 'Do not overwrite retained attribution' }
function Values([string]$Line) {
    $Result = @{}
    foreach ($Match in [regex]::Matches($Line, '(\w+)=([^\s]+)')) { $Result[$Match.Groups[1].Value] = $Match.Groups[2].Value }
    return $Result
}
function Distribution($InputValues) {
    $Sorted = @($InputValues | Sort-Object)
    if (!$Sorted.Count) { return $null }
    return [ordered]@{ Count=$Sorted.Count; P50=$Sorted[[int][math]::Floor(($Sorted.Count-1)*.5)]; P95=$Sorted[[int][math]::Floor(($Sorted.Count-1)*.95)]; P99=$Sorted[[int][math]::Floor(($Sorted.Count-1)*.99)]; Max=$Sorted[-1] }
}
function Interval($Group, [string]$From, [string]$To) {
    if (!$Group.ContainsKey($From) -or !$Group.ContainsKey($To)) { return $null }
    return ([long]$Group[$To].ns - [long]$Group[$From].ns) / 1e6
}
$Groups = @{}; $Engine = @{}; $Histograms = @(); $Limits = $null; $Duplicates = @{}; $ClientTicks = @(); $ServerTicks = @(); $Roots = @{}
foreach ($Line in [IO.File]::ReadLines((Resolve-Path -LiteralPath $Path))) {
    if ($Line.Contains('[Content:LatencyLimit]')) { $Limits = Values $Line; continue }
    if ($Line.Contains('[Content:LatencyRoot]')) { $V=Values $Line; $Roots["$($V.object)/$($V.generation)"]=$true; continue }
    if ($Line.Contains('[Content:Tick]') -and $Line.Contains('side=client')) { $ClientTicks += ,(Values $Line); continue }
    if ($Line.Contains('[Content:Tick]') -and $Line.Contains('side=server')) { $ServerTicks += ,(Values $Line); continue }
    if ($Line.Contains('[Content:RecipientHistogram]')) { $Histograms += ,(Values $Line); continue }
    if (!$Line.Contains('[Content:Latency]')) { continue }
    $V = Values $Line
    if ($V.kind -eq '300') { $Engine["$($V.phase)/$($V.peer)/$($V.tick)"] = [long]$V.ns; continue }
    # Request ids are unique inside each peer generation and Remote identity;
    # native handler/completion taps have no wire publication field. Keep the
    # observed packet epoch in the stage record instead of inventing one.
    $Domain = if ($V.kind -eq '5') { 'Character' } elseif ($V.kind -eq '200') { 'Structure' } elseif ($V.kind -in @('103','104')) { 'Rpc' } else { "Remote$($V.kind)" }
    $EpochKey = if ($Domain -in @('Character','Structure')) { $V.epoch } else { '' }
    $Key = "$($V.phase)/$($V.peer)/$($V.peerGen)/$($V.object)/$($V.objectGen)/$($V.sequence)/$EpochKey/$Domain"
    if (!$Groups.ContainsKey($Key)) { $Groups[$Key] = @{ Domain=$Domain; Phase=$V.phase; Peer=$V.peer; Object=$V.object; ObjectGeneration=$V.objectGen; Sequence=$V.sequence } }
    $Stage = if ($Domain -eq 'Rpc' -and $V.kind -eq '103') { 'Request' + $V.stage } elseif ($Domain -eq 'Rpc') { 'Response' + $V.stage } else { $V.stage }
    if ($Groups[$Key].ContainsKey($Stage)) { $Duplicates[$Domain] = 1 + $Duplicates[$Domain] }
    $Groups[$Key][$Stage] = $V
}
if (!$Limits) { throw 'Missing bounded latency capture' }
if ([long]$Limits.dropped -ne 0 -or [long]$Limits.failures -ne 0) { throw 'Incomplete diagnostic capture; do not infer distributions' }
foreach ($Domain in @('Character','Rpc','Structure')) {
    if ($Duplicates[$Domain]) { throw "Ambiguous stage correlation in $Domain; do not overwrite samples" }
}
$Cases = [ordered]@{}
foreach ($Phase in @('baseline','load','evict','reload')) {
    $Selected = @($Groups.Values | Where-Object Phase -eq $Phase)
    $Char = @($Selected | Where-Object Domain -eq 'Character')
    $Rpc = @($Selected | Where-Object Domain -eq 'Rpc')
    $Structure = @($Selected | Where-Object Domain -eq 'Structure')
    $CharacterIntervals = [ordered]@{}
    foreach ($Pair in @(@('CharacterProduced','SchedulerAccepted'),@('SchedulerAccepted','Handoff'),@('Handoff','SimulatedDelivered'),@('SimulatedDelivered','Observer'),@('Observer','ClientCallback'),@('ClientCallback','ClientHandled'))) {
        $Values = @($Char | ForEach-Object { Interval $_ $Pair[0] $Pair[1] } | Where-Object { $null -ne $_ })
        $CharacterIntervals[($Pair -join 'To') + 'Ms'] = Distribution $Values
    }
    $CharacterIntervals.DueTickToProductionTicks = Distribution @($Char | Where-Object { $_.ContainsKey('CharacterProduced') -and $_.CharacterProduced.operations -eq '0' } | ForEach-Object { [long]$_.CharacterProduced.tick - [long]$_.CharacterProduced.due })
    $CharacterIntervals.DueTickEngineCompleteToProductionMs = Distribution @($Char | ForEach-Object {
        if ($_.ContainsKey('CharacterProduced')) {
            $P = $_.CharacterProduced; $Key = "$Phase/$($P.peer)/$($P.due)"
            if ($P.operations -eq '0' -and $Engine.ContainsKey($Key)) { ([long]$P.ns - $Engine[$Key]) / 1e6 }
        }
    })
    $Worst = $null
    foreach ($Relationship in $Char | Where-Object { $_.ContainsKey('Observer') } | Group-Object { "$($_.Peer)/$($_.Object)/$($_.ObjectGeneration)" }) {
        $Ordered = @($Relationship.Group | Sort-Object { [long]$_.Observer.ns })
        for ($Index=1; $Index -lt $Ordered.Count; $Index++) {
            $Previous=$Ordered[$Index-1]; $Current=$Ordered[$Index]
            $Gap=([long]$Current.Observer.ns-[long]$Previous.Observer.ns)/1e6
            if (!$Worst -or $Gap -gt $Worst.GapMs) {
                $P=$Current.CharacterProduced; $DueKey="$Phase/$($Current.Peer)/$($P.due)"
                $Worst=[ordered]@{ GapMs=$Gap; Peer=$Current.Peer; Object=$Current.Object; ObjectGeneration=$Current.ObjectGeneration; RootMotion=$(if ($Roots.Count) {$Roots.ContainsKey("$($Current.Object)/$($Current.ObjectGeneration)")} else {$null}); PreviousObserver=$Previous.Observer; Stages=$Current; DueTickEngineCompleteNs=$(if ($Engine.ContainsKey($DueKey)) {$Engine[$DueKey]} else {$null}) }
            }
        }
    }
    $RpcIntervals = [ordered]@{}
    foreach ($Pair in @(@('RequestSchedulerAccepted','RequestHandoff'),@('RequestHandoff','RequestSimulatedDelivered'),@('RequestSimulatedDelivered','RequestServerCallback'),@('RequestServerCallback','RequestRpcHandler'),@('RequestRpcHandler','ResponseRpcResponseProduced'),@('ResponseRpcResponseProduced','ResponseSchedulerAccepted'),@('ResponseSchedulerAccepted','ResponseHandoff'),@('ResponseHandoff','ResponseSimulatedDelivered'),@('ResponseSimulatedDelivered','ResponseClientCallback'),@('ResponseClientCallback','ResponseRpcCompletion'),@('RequestSchedulerAccepted','ResponseRpcCompletion'))) {
        $RpcIntervals[($Pair -join 'To')+'Ms'] = Distribution @($Rpc | ForEach-Object { Interval $_ $Pair[0] $Pair[1] } | Where-Object { $null -ne $_ })
    }
    $StructuralFrames = @($Structure | Where-Object { $_.ContainsKey('ClientCallback') -and $_.ContainsKey('ClientHandled') } | ForEach-Object {
        [ordered]@{ Peer=$_.Peer; Sequence=$_.Sequence; Epoch=$_.ClientCallback.epoch; Bytes=[long]$_.ClientCallback.bytes; Operations=[long]$_.ClientCallback.operations; CallbackNs=[long]$_.ClientCallback.ns; HandledNs=[long]$_.ClientHandled.ns; HandlingMs=(Interval $_ 'ClientCallback' 'ClientHandled') }
    } | Sort-Object { $_.HandlingMs } -Descending)
    foreach ($Frame in $StructuralFrames) {
        # Aggregate replica counters belong to a tick, not necessarily one frame.
        # Attribute them only when exactly one captured frame encloses that apply interval.
        $Matches=@($ClientTicks | Where-Object {
            $_.phase -eq $Phase -and $_.ClientApplyCalls -eq '1' -and $_.ContainsKey('ClientApplyFirstNs') -and
            ([long]$Limits.originNs+[long]$_.ClientApplyFirstNs) -ge $Frame.CallbackNs -and
            ([long]$Limits.originNs+[long]$_.ClientApplyLastNs) -le $Frame.HandledNs
        })
        if ($Matches.Count -eq 1) {
            $T=$Matches[0]; $Frame.ClientTick=$T
            if ($T.ContainsKey('ReplicaCopyNs')) {
                $Frame.UnscopedApplyResidualMs=([long]$T.ClientApplyNs-[long]$T.ReplicaCopyNs-[long]$T.ReplicaSemanticNs-[long]$T.ReplicaPreflightNs-[long]$T.ReplicaLiveNs)/1e6
            }
        }
    }
    $Window=@($ServerTicks | Where-Object { $Worst -and $_.phase -eq $Phase -and [long]$_.tick -ge [long]$Worst.PreviousObserver.tick -and [long]$_.tick -le [long]$Worst.Stages.Observer.tick } | ForEach-Object {
        $Tick=$_; $Row=[ordered]@{}
        foreach ($Key in @('tick','startMs','frameElapsedMs','observerDrainMs','EngineStepNs','SessionStepNs','CharacterPublicationNs','RelevanceNs','StructuralPlanningNs','StructuralSelectionNs','StructuralFrameBuildNs','StructuralValidationEncodeNs','StructuralSubmissionEncodeNs','StructuralSubmitNs','StructuralAcceptanceNs','KnownCommitNs','AcceptedMetadataCommitNs','SchedulerFlushNs','TransportSendNs','selectedOps','acceptedOps')) { $Row[$Key]=$Tick[$Key] }
        $Row
    })
    $Cases[$Phase]=[ordered]@{ Character=$CharacterIntervals; WorstSampledCharacterGap=$Worst; WorstGapSurroundingTicks=$Window; Rpc=$RpcIntervals; StructuralHandlingMs=(Distribution @($StructuralFrames | ForEach-Object { $_.HandlingMs })); StructuralFrames=$StructuralFrames }
}
[ordered]@{
    Path=(Resolve-Path -LiteralPath $Path).Path; SHA256=(Get-FileHash -LiteralPath $Path).Hash
    Method='One-process monotonic clock; first/last server peer identities only for stage traces. All-recipient histogram reports 1-ms bucket upper bounds, exact maxima, overflow explicitly. Observer is before real GameSession apply; raw observers never apply. ClientHandled is callback return, not rendered visibility. Null means not measured. Stage quantiles are not additive.'
    Limits=$Limits; DuplicateStageKeysByDomain=$Duplicates; Histograms=$Histograms; Cases=$Cases
} | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath $Output -Encoding utf8
Write-Output "[Content:LatencyAnalysis] $Output"
