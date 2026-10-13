#requires -Version 7.0
# Execute the actual embedded client scope handoff and recovery callbacks,
# including coroutine formatting and completion gates, using bounded fake services.
param(
    [string]$SourceHeader = (Join-Path $PSScriptRoot 'ContentScaleGameplay.hpp'),
    [string]$LutePath = 'lute',
    [switch]$ReproduceMissingClientTick
)
$ErrorActionPreference = 'Stop'
# Lute is already pinned by rokit.toml and installed on hosted CI. The engine's
# separate Luau dependency intentionally builds without standalone CLI tools.
$Interpreter = (Get-Command $LutePath -ErrorAction Stop).Source
$Source = Get-Content -LiteralPath $SourceHeader -Raw
if ($ReproduceMissingClientTick) {
    # Negative control restores precisely the historical out-of-scope lookup.
    $Source = $Source.Replace('local StartedTick = RecoveryCallbackTick', 'local StartedTick = PhaseTick')
}
# The native predicate must see this step's final replicated names before a
# completion marker can evaluate it. This guards actual host call order; native
# DataModel ownership/type checks also execute in gargantuan_replication_tests.
$HostSource = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../src/host/player/PlayerHost.cpp') -Raw
$Observe = $HostSource.IndexOf('host::detail::FarmRecoveryCaseIndex(*Runtime->DataModel)')
$Complete = $HostSource.IndexOf('if (*PhaseName == "complete" && !FarmScaleCompleted)')
if ($Observe -lt 0 -or $Complete -lt 0 -or $Observe -ge $Complete) {
    throw 'native recovery observation must precede same-step completion evaluation'
}
$Enroll = $HostSource.IndexOf('SetOrdinarySourceNormal(*Session, Normal)')
$TracePhase = $HostSource.IndexOf('PublicationEvidence->SetTrafficPhase(PhaseName, Overload)', $Enroll)
$Handoff = $HostSource.IndexOf('ApplyAttributeMutation("ScaleOrdinarySourcePhase", SourcePhase', $Enroll)
if ($Enroll -lt 0 -or $TracePhase -lt $Enroll -or $Handoff -lt $TracePhase) {
    throw 'native prospective enrollment and trace phase must precede the local scope handoff'
}
# The supported server never advances calibration while a peer's four-probe
# coroutine is still running. This is the actual production barrier, rather
# than assuming a phase cannot change during a yielding client callback.
$ServerSource = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../src/host/server/PhysicalScaleQualification.hpp') -Raw
$Calibrating = $ServerSource.IndexOf('if (State == Stage::Calibrating)')
$ClockAcks = $ServerSource.IndexOf('if (HasAllPeerAcks("ScaleClockAcks"))', $Calibrating)
$NextPhase = $ServerSource.IndexOf('BeginPhase(NextPhase, Tick)', $Calibrating)
if ($Calibrating -lt 0 -or $ClockAcks -lt $Calibrating -or $NextPhase -lt $ClockAcks) {
    throw 'all peer clock completions must precede the next measured phase'
}
$Client = $Source.IndexOf('auto ClientScript')
$Begin = $Source.IndexOf('R"(', $Client)
$End = $Source.IndexOf(')"', $Begin + 3)
if ($Client -lt 0 -or $Begin -lt 0 -or $End -lt 0) { throw 'embedded client Luau source delimiters are missing' }
$Prelude = @'
local Qualified, ProducerPlayerId, PhysicalFarm = true, 1, true
local NativePrint = print
local Records, Acknowledgements, Callbacks = {}, {}, {}
local AcknowledgementAttempts = {}
local TestClock = 10
local RecoveryTesting, RecoveryMode, RecoveryRpcCalls = false, 'Healthy', 0
local HeldRecoveryEvents = {}
local os = {clock = function() return TestClock end}
local VerifyScope, HostScopePhase, HostScopeNormal = false, nil, false
local function CheckScope(Phase, Normal)
    if VerifyScope then
        assert(HostScopePhase == Phase and HostScopeNormal == Normal,
            'actual send must follow matching prospective host enrollment')
    end
end
local function print(Value) table.insert(Records, Value) end
local function Signal()
    return {Connect = function(Self, Callback) Self.Callback = Callback end}
end
local ControlMock = {Attributes = {}, ActionResolved = Signal(), ActionEnded = Signal()}
function ControlMock:RegisterAction(...) return true end
function ControlMock:RequestAction(...) CheckScope('baseline', true) return false end
function ControlMock:SetAttribute(Key, Value) self.Attributes[Key] = Value end
function ControlMock:GetAttribute(Key) return self.Attributes[Key] end
local PostSimulation = {}
function PostSimulation:Connect(Callback) table.insert(Callbacks, Callback) end
local PlayersMock = {LocalPlayer = {PlayerId = 9}}
local PhaseMock = {OnClientEvent = Signal()}
function PhaseMock:FireServer(Kind, Case, Attempt)
    if Kind == 'phase_ack' or Kind == 'content_observed' then
        CheckScope(Case, Case ~= 'complete' and Case ~= 'calibrating')
    elseif Kind == 'clock_quiesced' or Kind == 'clock_done' then
        CheckScope('calibrating', false)
    elseif Kind == 'recovery_done' then
        assert(type(Attempt) == 'number' and Attempt >= 1 and Attempt <= 20 and Attempt == math.floor(Attempt),
            'actual recovery acknowledgement must carry its qualified attempt')
        AcknowledgementAttempts[Case] = Attempt
    end
    local Key = Kind .. ':' .. Case
    Acknowledgements[Key] = (Acknowledgements[Key] or 0) + 1
end
local EventMock = {OnClientEvent = Signal()}
function EventMock:FireServer(Message, Sequence)
    CheckScope(Message, true)
    self.OnClientEvent.Callback(Message, Sequence)
end
local OverloadEventMock = {OnClientEvent = Signal()}
function OverloadEventMock:FireServer(Message, Sequence, Case)
    if RecoveryTesting and Message == 'recovery' then
        if RecoveryMode == 'EventError' then error('accepted Event fixture failure') end
        if RecoveryMode == 'PendingEvents' then
            table.insert(HeldRecoveryEvents, {Message, Sequence, Case})
            return
        end
    end
    self.OnClientEvent.Callback(Message, Sequence, Case)
end
local RpcCount = 0
local FunctionMock = {}
function FunctionMock:InvokeServerWithTimeout(Timeout, Payload)
    if VerifyScope then
        CheckScope(if string.sub(Payload, 1, 6) == 'clock:' then 'calibrating' else Payload,
            string.sub(Payload, 1, 6) ~= 'clock:')
    end
    RpcCount += 1
    if RecoveryTesting and Payload == 'recovery' then
        RecoveryRpcCalls += 1
        TestClock += if RecoveryMode == 'AlwaysSlow' or
            (RecoveryMode == 'FirstSlow' and RecoveryRpcCalls == 1) then 0.618219 else 0.03
        if RecoveryMode == 'PendingRpc' then coroutine.yield() end
        if RecoveryMode == 'RpcError' then return nil end
    end
    return Payload
end
local Children, Holders = {}, {}
for Index = 0, 31 do
    Children[Index + 1] = {Name = 'initial'}
    Holders['part' .. Index] = {GetChildren = function() return {Children[Index + 1]} end}
end
local Root = {FindFirstChild = function(_, Name) return Holders[Name] end}
local WorkspaceMock = {FindFirstChild = function(_, Name)
    return if Name == 'ScaleOverloadRegion' then Root else nil
end}
local Services = {CharacterControlService = ControlMock, Players = PlayersMock,
    RunService = {PostSimulation = PostSimulation}, Workspace = WorkspaceMock}
local Objects = {ScaleFunction = FunctionMock, ScaleEvent = EventMock,
    ScalePhaseControl = PhaseMock, ScaleOverloadFunction = FunctionMock, ScaleOverloadEvent = OverloadEventMock}
local Attributes = {}
local game = {
    GetService = function(_, Name) return assert(Services[Name]) end,
    FindFirstChild = function(_, Name) return Objects[Name] end,
    GetAttribute = function(_, Name) return Attributes[Name] end,
}
local Vector3 = {new = function(...) return {...} end}
local DeferredTasks, TaskStarts, PendingTasks = false, 0, {}
local task = {
    spawn = function(Callback)
        TaskStarts += 1
        if not DeferredTasks then Callback() return end
        local Thread = coroutine.create(Callback)
        table.insert(PendingTasks, Thread)
        assert(coroutine.resume(Thread))
    end,
    wait = function(Seconds)
        if RecoveryTesting then TestClock += Seconds or 0 end
        if DeferredTasks then coroutine.yield() end
    end,
}
local function DrainTasks()
    for Opportunity = 1, 110 do
        local Pending = false
        for _, Thread in PendingTasks do
            if coroutine.status(Thread) ~= 'dead' then
                Pending = true
                assert(coroutine.resume(Thread))
            end
        end
        if not Pending then table.clear(PendingTasks) return end
    end
    error('bounded actual client coroutines did not complete')
end
'@
$Assertions = @'
assert(#Callbacks == 2, 'actual client must install both content and recovery callbacks')
local Content = Callbacks[1]
VerifyScope, DeferredTasks = true, true
-- Ready must work before any phase or marker exists, including nonproducers.
Content()
assert(Acknowledgements['ready:'] == 1 and RpcCount == 0 and TaskStarts == 0,
    'initial ready must not deadlock on host enrollment')
PhaseMock.OnClientEvent.Callback('phase', 'baseline')
Content()
assert(not Acknowledgements['phase_ack:baseline'] and not Acknowledgements['content_observed:baseline'] and
    RpcCount == 0 and TaskStarts == 0, 'nonproducer controls must also wait for prospective enrollment')
PlayersMock.LocalPlayer = {PlayerId = 1, Character = {}}
Content()
local ReceivedPhaseClock = PhaseStartedAt
assert(ReceivedPhaseClock == 10 and RpcCount == 0 and TaskStarts == 0 and
    not Acknowledgements['phase_ack:baseline'] and not Acknowledgements['content_observed:baseline'],
    'same-step phase event must defer both control messages and producer RPCs')
TestClock = 10.025
HostScopePhase, HostScopeNormal = 'baseline', true
ControlMock:SetAttribute('ScaleOrdinarySourcePhase', 'baseline')
Content()
assert(Acknowledgements['phase_ack:baseline'] == 1 and Acknowledgements['content_observed:baseline'] == 1 and
    RpcCount == 1 and TaskStarts == 1 and PhaseStartedAt == ReceivedPhaseClock,
    'next host enrollment must release both control messages and exactly one producer coroutine')
Content()
assert(RpcCount == 1 and TaskStarts == 1 and PhaseStartedAt == ReceivedPhaseClock,
    'repeated callback must not respawn or retimestamp the phase')
DrainTasks()
assert(RpcCount == 100 and #RpcTrace == 100 and RpcComplete,
    'actual normal coroutine must retain all one hundred RPC samples')
assert(tonumber(string.match(RpcTrace[1], '^1:(%d+):')) >= 24999999,
    'first RPC must retain the original received-phase clock across handoff')
-- Closing the class also waits one host opportunity; old normal membership
-- cannot enroll calibration messages merely because the phase event arrived.
PhaseMock.OnClientEvent.Callback('phase', 'calibrating')
Attributes.ScaleClockEpoch, Attributes.ScaleClockActive = 1, false
Content()
assert(not Acknowledgements['clock_quiesced:1'] and RpcCount == 100 and TaskStarts == 1,
    'same-step calibration close must not send under preceding normal membership')
HostScopePhase, HostScopeNormal = 'calibrating', false
ControlMock:SetAttribute('ScaleOrdinarySourcePhase', 'calibrating')
Content()
assert(Acknowledgements['clock_quiesced:1'] == 1 and RpcCount == 100 and TaskStarts == 1,
    'closing enrollment must permit quiescence without enabling clock probes')
Attributes.ScaleClockActive = true
Content()
assert(RpcCount == 101 and TaskStarts == 2 and
    not Acknowledgements['clock_done:1'], 'clock completion must wait for all four real probes')
DrainTasks()
assert(RpcCount == 104 and Acknowledgements['clock_done:1'] == 1,
    'actual clock coroutine must finish four probes before the server can advance phase')
Content()
assert(RpcCount == 104 and TaskStarts == 2 and Acknowledgements['clock_done:1'] == 1,
    'completed calibration must not restart its coroutine')
PhaseMock.OnClientEvent.Callback('phase', 'complete')
Content()
assert(not Acknowledgements['phase_ack:complete'], 'complete must wait for its own closing handoff')
HostScopePhase, HostScopeNormal = 'complete', false
ControlMock:SetAttribute('ScaleOrdinarySourcePhase', 'complete')
Content()
assert(Acknowledgements['phase_ack:complete'] == 1 and RpcCount == 104 and TaskStarts == 2,
    'complete acknowledgement must remain outside normal membership')
VerifyScope, DeferredTasks = false, false
PlayersMock.LocalPlayer = {PlayerId = 9}
NativePrint('[Qualification:ScopeHandoffScript] opening=PASS closing=PASS rpc_samples=100 clock_probes=4 phase_clock=IMMUTABLE result=PASS')
-- Invoke recovery independently; content-producer ticks must not be required.
local Recovery = Callbacks[2]
RecoveryTesting = true
local PreviousTick = 0
for CaseIndex, Case in {'gameplay', 'structural', 'mixed'} do
    Attributes.ScaleOverloadCase = Case
    Recovery()
    assert(Acknowledgements['overload_ready:' .. Case] == 1)
    for Index = 0, 31 do
        local Opportunity = if Index < 16 then 479 else 480
        Children[Index + 1].Name = string.rep(string.char(string.byte('a') +
            (((CaseIndex - 1) * 7 + Opportunity + Index) % 26)), 24576)
    end
    Attributes.ScaleOverloadCase = 'recover_' .. Case
    local RpcBefore = RpcCount
    Recovery()
    Recovery() -- Only a settled cohort may publish its complete receipt.
    assert(RpcCount - RpcBefore == 10, 'each recovery must execute exactly ten real callback RPC invocations')
    assert(Acknowledgements['recovery_done:' .. Case] == 1, 'ten RPC/event ACK gates must complete')
    assert(AcknowledgementAttempts[Case] == RecoveryAttempt, 'server handoff must identify the qualified cohort')
    if CaseIndex > 1 then assert(Acknowledgements['name_converged:' .. Case] == 1) end
    local Probes, Summary = 0, 0
    for _, Record in Records do
        if string.find(Record, 'event=rpc_probe case=' .. Case .. ' ', 1, true) then
            Probes += 1
            local ClientTick = tonumber(string.match(Record, 'client_tick=(%d+)'))
            assert(ClientTick and ClientTick > PreviousTick, 'client_tick must be local, numeric and advance across cases')
            assert(string.match(Record, 'ok=1 contract=bounded_recovery_cohort_v1 attempt=1$'), 'bounded echo must pass')
        elseif string.find(Record, 'event=client_probes case=' .. Case .. ' ', 1, true) then Summary += 1 end
    end
    assert(Probes == 10 and Summary == 1, 'all formatted probe records and terminal summary must exist')
    PreviousTick = RecoveryCallbackTick
    Recovery()
    assert(RpcCount - RpcBefore == 10 and Acknowledgements['recovery_done:' .. Case] == 1,
        'completed callback must not repeat probes or acknowledgement')
end
NativePrint('[Qualification:RecoveryScript] cases=3 rpc_probes=30 completed_cases=3 result=PASS')
-- Fresh bounded fixture occurrences use the actual case-reset path. They do
-- not replay any transport operation or reuse an accepted request/Event.
local function BeginFixture(Mode)
    RecoveryMode, RecoveryRpcCalls = Mode, 0
    table.clear(HeldRecoveryEvents)
    CurrentOverloadCase = nil
    Acknowledgements['recovery_done:gameplay'] = nil
    Attributes.ScaleOverloadCase = 'gameplay'
    Recovery()
    Attributes.ScaleOverloadCase = 'recover_gameplay'
    return #Records
end
local function CountRecords(After, Fragment)
    local Count = 0
    for Index = After + 1, #Records do
        if string.find(Records[Index], Fragment, 1, true) then Count += 1 end
    end
    return Count
end
local First = BeginFixture('FirstSlow')
Recovery()
local OriginalDeadline = RecoveryDeadlineNs
Recovery() -- Retains UNPROVEN first cohort and starts fresh second cohort.
assert(RecoveryAttempt == 2 and RecoveryRpcCalls == 20 and not RecoveryResultSent,
    'slow successful first cohort is unproven and cannot publish recovery')
Recovery()
assert(RecoveryResultSent and Acknowledgements['recovery_done:gameplay'] == 1 and AcknowledgementAttempts.gameplay == 2 and
    RecoveryDeadlineNs == OriginalDeadline and CountRecords(First, 'status=UNPROVEN') == 1 and
    CountRecords(First, 'status=QUALIFIED') == 1 and CountRecords(First, 'event=rpc_probe') == 20 and
    CountRecords(First, 'event=event_probe') == 20,
    'healthy fresh cohort qualifies without discarding first-cohort evidence or restarting deadline')
local Starts = {}
for Index = First + 1, #Records do
    if string.find(Records[Index], 'event=cohort_start ', 1, true) then
        table.insert(Starts, tonumber(string.match(Records[Index], ' started_clock_ns=(%d+)')))
    end
end
assert(#Starts == 2 and Starts[2] - Starts[1] >= 1000000000, 'fresh cohort starts must remain at most one per second')
First = BeginFixture('AlwaysSlow')
for Opportunity = 1, 30 do Recovery() if RecoveryFailed then break end end
assert(RecoveryFailed and not RecoveryResultSent and not Acknowledgements['recovery_done:gameplay'] and
    RecoveryDeadlineNs - RecoveryStartedNs == 20000000000 and RecoveryAttempt <= 20 and
    CountRecords(First, 'status=UNPROVEN') > 0 and CountRecords(First, 'status=QUALIFIED') == 0,
    'always unsuitable cohorts fail at the unchanged deadline instead of retrying indefinitely')
for _, Mode in {'RpcError', 'EventError'} do
    First = BeginFixture(Mode)
    Recovery()
    local Attempts, Calls = RecoveryAttempt, RecoveryRpcCalls
    RecoveryMode = 'Healthy'
    for Opportunity = 1, 3 do Recovery() end
    assert(RecoveryFailed and not RecoveryResultSent and RecoveryAttempt == Attempts and RecoveryRpcCalls == Calls and
        CountRecords(First, 'status=FAILED') == 1 and not Acknowledgements['recovery_done:gameplay'],
        'accepted RPC/Event errors remain fatal and cannot be hidden by a later healthy cohort')
end
First = BeginFixture('PendingEvents')
Recovery()
local PendingDeadline = RecoveryDeadlineNs
TestClock += 1
Recovery()
assert(RecoveryAttempt == 1 and #HeldRecoveryEvents == 10 and RecoveryEventAcks == 0 and
    next(RecoveryEventStarted) ~= nil, 'pending accepted Events must prevent a fresh cohort')
TestClock = PendingDeadline / 1000000000
Recovery()
assert(RecoveryFailed and next(RecoveryEventStarted) ~= nil and RecoveryDeadlineNs == PendingDeadline,
    'deadline failure must retain pending accepted Events')
for _, Held in HeldRecoveryEvents do OverloadEventMock.OnClientEvent.Callback(table.unpack(Held)) end
Recovery()
assert(not RecoveryResultSent and CountRecords(First, 'event=event_probe') == 10 and
    not Acknowledgements['recovery_done:gameplay'], 'late Events remain raw evidence without retrospective qualification')
First = BeginFixture('PendingRpc')
DeferredTasks = true
Recovery()
local RpcDeadline = RecoveryDeadlineNs
local StructuralReadyBefore = Acknowledgements['overload_ready:structural']
Attributes.ScaleOverloadCase = 'structural'
Recovery()
assert(RecoveryFailed and RecoveryFailureReason == 'case_transition' and CurrentOverloadCase == 'gameplay' and
    RecoveryDeadlineNs == RpcDeadline and not RecoveryProbesDone and
    Acknowledgements['overload_ready:structural'] == StructuralReadyBefore,
    'case transition must retain an in-flight RPC and refuse next-case offers')
DrainTasks()
DeferredTasks = false
assert(RecoveryProbesDone and RecoveryRpcCalls == 1 and not RecoveryResultSent and
    CountRecords(First, 'event=rpc_probe case=gameplay ') == 1 and
    CountRecords(First, 'event=rpc_probe case=structural ') == 0,
    'late RPC must preserve its captured old case/attempt without retrospective qualification')
First = BeginFixture('PendingEvents')
Recovery()
local EventDeadline = RecoveryDeadlineNs
StructuralReadyBefore = Acknowledgements['overload_ready:structural']
Attributes.ScaleOverloadCase = 'structural'
Recovery()
assert(RecoveryFailed and CurrentOverloadCase == 'gameplay' and RecoveryDeadlineNs == EventDeadline and
    next(RecoveryEventStarted) ~= nil and #HeldRecoveryEvents == 10 and
    Acknowledgements['overload_ready:structural'] == StructuralReadyBefore,
    'case transition must preserve pending Events and their old context')
for _, Held in HeldRecoveryEvents do OverloadEventMock.OnClientEvent.Callback(table.unpack(Held)) end
Recovery()
assert(not RecoveryResultSent and CountRecords(First, 'event=event_probe') == 10 and
    CountRecords(First, 'status=QUALIFIED') == 0, 'late old-case Events must not qualify or start a new case')
First = BeginFixture('Healthy')
RecoveryAttempt = 20 -- Exercise the defensive counter guard independently.
Recovery()
Recovery()
assert(RecoveryFailed and RecoveryAttempt == 20 and RecoveryRpcCalls == 0 and
    CountRecords(First, 'event=cohort_start') == 0, 'attempt cap must reject a twenty-first cohort')
NativePrint('[Qualification:RecoveryCohortScript] first_slow=PASS deadline=PASS errors=PASS pending=PASS transition=PASS attempt_cap=PASS result=PASS')
'@
$Path = Join-Path ([IO.Path]::GetTempPath()) ('gargantuan-recovery-client-' + [Guid]::NewGuid().ToString('N') + '.luau')
try {
    [IO.File]::WriteAllText($Path, $Prelude + "`n" + $Source.Substring($Begin + 3, $End - $Begin - 3) + "`n" + $Assertions)
    $Output = & $Interpreter run $Path 2>&1
    $Result = $LASTEXITCODE
    if ($ReproduceMissingClientTick) {
        if ($Result -eq 0 -or ($Output -join "`n") -notmatch "invalid argument #5 to 'format'.*number expected, got nil") {
            throw 'historical missing-tick negative control did not reproduce the physical formatting error'
        }
        Write-Output '[Qualification:RecoveryScript] missing_client_tick_negative=PASS exact_format_argument=5'
    } else {
        $Output | Write-Output
        if ($Result -ne 0) { throw "embedded client recovery execution failed: $Result" }
    }
} finally {
    if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Force }
}
# A deliberately failing negative-control child is a successful test only
# after its exact failure was checked above; do not leak its exit code to CI.
exit 0
