#requires -Version 7.0
# Execute the actual embedded client recovery callback, including all coroutine
# formatting and its ten RPC/event completion gates, using bounded fake services.
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
$Client = $Source.IndexOf('auto ClientScript')
$Begin = $Source.IndexOf('R"(', $Client)
$End = $Source.IndexOf(')"', $Begin + 3)
if ($Client -lt 0 -or $Begin -lt 0 -or $End -lt 0) { throw 'embedded client Luau source delimiters are missing' }
$Prelude = @'
local Qualified, ProducerPlayerId, PhysicalFarm = true, 1, true
local NativePrint = print
local Records, Acknowledgements, Callbacks = {}, {}, {}
local function print(Value) table.insert(Records, Value) end
local function Signal()
    return {Connect = function(Self, Callback) Self.Callback = Callback end}
end
local ControlMock = {Attributes = {}, ActionResolved = Signal(), ActionEnded = Signal()}
function ControlMock:RegisterAction(...) return true end
function ControlMock:SetAttribute(Key, Value) self.Attributes[Key] = Value end
function ControlMock:GetAttribute(Key) return self.Attributes[Key] end
local PostSimulation = {}
function PostSimulation:Connect(Callback) table.insert(Callbacks, Callback) end
local PlayersMock = {LocalPlayer = {PlayerId = 9}}
local PhaseMock = {OnClientEvent = Signal()}
function PhaseMock:FireServer(Kind, Case)
    local Key = Kind .. ':' .. Case
    Acknowledgements[Key] = (Acknowledgements[Key] or 0) + 1
end
local EventMock = {OnClientEvent = Signal()}
local OverloadEventMock = {OnClientEvent = Signal()}
function OverloadEventMock:FireServer(Message, Sequence, Case)
    self.OnClientEvent.Callback(Message, Sequence, Case)
end
local RpcCount = 0
local FunctionMock = {}
function FunctionMock:InvokeServerWithTimeout(Timeout, Payload)
    RpcCount += 1
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
local task = {spawn = function(Callback) Callback() end, wait = function() end}
'@
$Assertions = @'
assert(#Callbacks == 2, 'actual client must install both content and recovery callbacks')
-- Invoke recovery independently; content-producer ticks must not be required.
local Recovery = Callbacks[2]
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
    assert(RpcCount - RpcBefore == 10, 'each recovery must execute exactly ten real callback RPC invocations')
    assert(Acknowledgements['recovery_done:' .. Case] == 1, 'ten RPC/event ACK gates must complete')
    if CaseIndex > 1 then assert(Acknowledgements['name_converged:' .. Case] == 1) end
    local Probes, Summary = 0, 0
    for _, Record in Records do
        if string.find(Record, 'event=rpc_probe case=' .. Case .. ' ', 1, true) then
            Probes += 1
            local ClientTick = tonumber(string.match(Record, 'client_tick=(%d+)'))
            assert(ClientTick and ClientTick > PreviousTick, 'client_tick must be local, numeric and advance across cases')
            assert(string.match(Record, 'ok=1$'), 'bounded echo must pass')
        elseif string.find(Record, 'event=client_probes case=' .. Case .. ' ', 1, true) then Summary += 1 end
    end
    assert(Probes == 10 and Summary == 1, 'all formatted probe records and terminal summary must exist')
    PreviousTick = RecoveryCallbackTick
    Recovery()
    assert(RpcCount - RpcBefore == 10 and Acknowledgements['recovery_done:' .. Case] == 1,
        'completed callback must not repeat probes or acknowledgement')
end
NativePrint('[Qualification:RecoveryScript] cases=3 rpc_probes=30 completed_cases=3 result=PASS')
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
