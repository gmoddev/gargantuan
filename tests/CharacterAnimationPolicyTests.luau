-- Run with: luau tests/CharacterAnimationPolicyTests.luau
local Policy = require("../docs/examples/CharacterAnimationPolicy")

local function Near(Actual, Expected)
	assert(math.abs(Actual - Expected) < 0.00001, tostring(Actual) .. " ~= " .. tostring(Expected))
end

local function NewSignal()
	local Signal = {Connection = nil}
	function Signal.Connect(Self, Callback)
		local Connection = {Connected = true, Callback = Callback}
		function Connection.Disconnect(Current)
			Current.Connected = false
		end
		Self.Connection = Connection
		return Connection
	end
	function Signal.Fire(Self, DeltaTime)
		local Connection = Self.Connection
		if Connection and Connection.Connected then Connection.Callback(DeltaTime) end
	end
	return Signal
end

local function NewAnimator()
	local Animator = {Tracks = {}}
	function Animator.LoadAnimation(Self, Reference)
		if Reference == "missing" then error("missing clip") end
		local Track = {Reference = Reference, Weight = 1, Speed = 1, Looped = false,
			RootMotionEnabled = false, Plays = 0, Stops = 0, PlaybackState = "Stopped"}
		function Track.Play(Current)
			Current.Plays += 1
			Current.PlaybackState = "Playing"
		end
		function Track.Stop(Current)
			Current.Stops += 1
			Current.PlaybackState = "Stopped"
		end
		table.insert(Self.Tracks, Track)
		return Track
	end
	return Animator
end

local Signal = NewSignal()
local Animator = NewAnimator()
local Controller = Policy.New(Animator, {PreSimulation = Signal}, {
	{Name = "Idle", Reference = "idle", Looped = true},
	{Name = "Walk", Reference = "walk", Speed = 1.25, Looped = true},
	{Name = "Attack", Reference = "attack", Looped = false},
})
local Idle, Walk, Attack = Animator.Tracks[1], Animator.Tracks[2], Animator.Tracks[3]
assert(Signal.Connection.Connected and Walk.Speed == 1.25 and Walk.Looped)
assert(Controller:Request("Idle") and Controller:GetState() == "Idle")
Near(Idle.Weight, 1)
assert(Idle.Plays == 1 and Walk.Plays == 0)
assert(Controller:Request("Walk", 1))
Signal:Fire(0.25)
Near(Idle.Weight, 0.75)
Near(Walk.Weight, 0.25)
assert(Controller:Request("Attack", 0.5))
Signal:Fire(0.25)
Near(Idle.Weight, 0.375)
Near(Walk.Weight, 0.125)
Near(Attack.Weight, 0.5)
assert(Controller:Request("Walk", 0.5))
assert(Walk.Plays == 1, "An interrupted fading track keeps its timeline")
Signal:Fire(0.5)
Near(Idle.Weight, 0)
Near(Walk.Weight, 1)
Near(Attack.Weight, 0)
assert(Idle.PlaybackState == "Stopped" and Attack.PlaybackState == "Stopped")
assert(Controller:GetState() == "Walk")
assert(Controller:Request("Walk") and Walk.Plays == 1, "Repeated target choice is idempotent")
assert(not Controller:Request("Unknown", 0) and not Controller:Request("Idle", -1) and
	not Controller:Request("Idle", 6) and not Controller:Request("Idle", 0 / 0) and
	not Controller:Request("Idle", math.huge), "Invalid and unknown transitions are denied")
assert(Controller:GetState() == "Walk" and Walk.Weight == 1)
assert(not Controller:Step(-1) and not Controller:Step(0 / 0) and Walk.Weight == 1)
assert(Controller:Request("Idle", 0) and Idle.Plays == 2 and Walk.PlaybackState == "Stopped")
Controller:Close()
Controller:Close()
assert(not Signal.Connection.Connected and Controller:GetState() == nil and not Controller:Request("Walk"))
assert(Idle.PlaybackState == "Stopped" and Idle.Weight == 0 and not Idle.RootMotionEnabled)
Signal:Fire(1)
assert(not Controller:Step(1), "Closed policy cannot advance")

local FailedSignal = NewSignal()
local FailedAnimator = NewAnimator()
local Ok = pcall(function()
	Policy.New(FailedAnimator, {PreSimulation = FailedSignal}, {
		{Name = "First", Reference = "first"},
		{Name = "Missing", Reference = "missing"},
	})
end)
assert(not Ok and FailedAnimator.Tracks[1].PlaybackState == "Stopped" and
	FailedAnimator.Tracks[1].Weight == 0 and FailedSignal.Connection == nil,
	"Failed preload stops earlier tracks and never binds a tick")
assert(not pcall(function()
	Policy.New(NewAnimator(), {PreSimulation = NewSignal()}, {{Name = "TooFast", Reference = "fast", Speed = math.huge}})
end), "Non-finite configured speed is rejected")
assert(not pcall(function()
	local TooMany = {}
	for Index = 1, 9 do table.insert(TooMany, {Name = tostring(Index), Reference = "clip"}) end
	Policy.New(NewAnimator(), {PreSimulation = NewSignal()}, TooMany)
end), "Policy state count is bounded")

print("[Animation:PolicyTests] PASS")
