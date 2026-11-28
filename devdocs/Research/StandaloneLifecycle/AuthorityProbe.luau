--!strict
-- Executable contract model. The caller classification in this fixture represents
-- verified native input. This file has no transport, credential or Engine binding.
type Owner = "MainMenu" | "System" | "RemoteClient"
type World = { Generation: number, Holds: {[Owner]: boolean}, SimTime: number, ServiceTime: number }
local Generation = 0
local Active: World? = nil
local Attempt: number? = nil
local MenuVisible = true
local function Begin(): number
    assert(not Active and not Attempt)
    Generation += 1
    Attempt = Generation
    return Generation
end
local function Retire()
    Generation += 1
    Attempt = nil
    Active = nil
end
local function Ready(Ticket: number): boolean
    if Attempt ~= Ticket then return false end
    Attempt = nil
    Active = { Generation = Ticket, Holds = {}, SimTime = 0, ServiceTime = 0 }
    return true
end
local function SetHold(World: World, Ticket: number, Owner: Owner, Held: boolean): boolean
    if World ~= Active or Ticket ~= World.Generation or Owner == "RemoteClient" then return false end
    World.Holds[Owner] = if Held then true else nil
    return true
end
local function Advance(World: World, Delta: number)
    World.ServiceTime += Delta
    if not next(World.Holds) then World.SimTime += Delta end
end

local Abandoned = Begin()
Retire()
local First = Begin()
assert(not Ready(Abandoned) and Ready(First))
local World = assert(Active :: World?)
assert(not SetHold(World, First, "RemoteClient", true))
Advance(World, 1)
assert(MenuVisible and World.SimTime == 1)
assert(SetHold(World, First, "MainMenu", true))
assert(SetHold(World, First, "MainMenu", true))
assert(SetHold(World, First, "System", true))
assert(SetHold(World, First, "MainMenu", false))
Advance(World, 60)
assert(World.SimTime == 1 and World.ServiceTime == 61)
assert(not SetHold(World, First, "RemoteClient", false))
assert(SetHold(World, First, "System", false))
Advance(World, 0.016)
assert(math.abs(World.SimTime - 1.016) < 0.000001)
Retire()
local Second = Begin()
assert(Ready(Second))
local Replacement = assert(Active :: World?)
assert(not SetHold(World, First, "System", true))
assert(not SetHold(Replacement, First, "System", true))
assert(Replacement.SimTime == 0 and MenuVisible)
print("[Lifecycle:Authority] remote_denied=true control_alive=true nested_release=true stale_ready_and_pause_rejected=true")
