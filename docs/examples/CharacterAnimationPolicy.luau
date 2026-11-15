-- Place in a project ModuleScript named CharacterAnimationPolicy.
-- This is local visual policy. Character movement and action admission remain authoritative elsewhere.
local Policy = {}
Policy.__index = Policy

local MaximumStates = 8
local MaximumFadeSeconds = 5

local function IsFinite(Value)
	return type(Value) == "number" and Value == Value and Value > -math.huge and Value < math.huge
end

local function StopTracks(Entries)
	for _, Entry in Entries do
		Entry.Track.RootMotionEnabled = false
		Entry.Track:Stop()
		Entry.Track.Weight = 0
	end
end

function Policy.New(Animator, RunService, Clips)
	assert(Animator ~= nil and RunService ~= nil, "[Animation:Policy] Animator and RunService are required")
	assert(type(Clips) == "table" and #Clips >= 1 and #Clips <= MaximumStates,
		"[Animation:Policy] Provide one to eight clip entries")
	local EntryCount = 0
	for Index in Clips do
		EntryCount += 1
		assert(type(Index) == "number" and Index % 1 == 0 and Index >= 1 and Index <= #Clips,
			"[Animation:Policy] Clips must be a dense array")
	end
	assert(EntryCount == #Clips, "[Animation:Policy] Clips must be a dense array")

	local Self = setmetatable({Entries = {}, ByName = {}, TargetName = nil, Transition = nil,
		Connection = nil, Closed = false}, Policy)
	local Ok, Failure = pcall(function()
		for _, Clip in Clips do
			assert(type(Clip) == "table" and type(Clip.Name) == "string" and #Clip.Name > 0 and #Clip.Name <= 32,
				"[Animation:Policy] Each clip needs a bounded state name")
			assert(Self.ByName[Clip.Name] == nil, "[Animation:Policy] State names must be unique")
			assert(type(Clip.Reference) == "string", "[Animation:Policy] Each state needs an Animation reference")
			local Speed = if Clip.Speed == nil then 1 else Clip.Speed
			local Looped = if Clip.Looped == nil then false else Clip.Looped
			assert(IsFinite(Speed) and Speed >= 0 and Speed <= 16,
				"[Animation:Policy] Speed must be finite and within 0..16")
			assert(type(Looped) == "boolean", "[Animation:Policy] Looped must be a boolean")
			local Track = Animator:LoadAnimation(Clip.Reference)
			local Entry = {Name = Clip.Name, Track = Track, StoppedState = Track.PlaybackState}
			table.insert(Self.Entries, Entry)
			Self.ByName[Clip.Name] = Entry
			Track.RootMotionEnabled = false
			Track.Speed = Speed
			Track.Looped = Looped
			Track.Weight = 0
		end
		Self.Connection = RunService.PreSimulation:Connect(function(DeltaTime)
			Self:Step(DeltaTime)
		end)
	end)
	if not Ok then
		if Self.Connection then Self.Connection:Disconnect() end
		StopTracks(Self.Entries)
		error("[Animation:Policy] Initialization failed: " .. tostring(Failure), 2)
	end
	return Self
end

function Policy.Request(Self, StateName, FadeSeconds)
	if Self.Closed then return false end
	local Target = Self.ByName[StateName]
	if Target == nil then return false end
	local Duration = if FadeSeconds == nil then 0 else FadeSeconds
	if not IsFinite(Duration) or Duration < 0 or Duration > MaximumFadeSeconds then return false end
	if Self.TargetName == StateName then return true end

	-- A currently fading track keeps its time position when a transition is redirected.
	if Target.Track.Weight == 0 or Target.Track.PlaybackState == Target.StoppedState then
		Target.Track:Play()
	end
	if Duration == 0 then
		for _, Entry in Self.Entries do
			Entry.Track.Weight = if Entry == Target then 1 else 0
			if Entry ~= Target then Entry.Track:Stop() end
		end
		Self.Transition = nil
	else
		local StartWeights = {}
		for Index, Entry in Self.Entries do StartWeights[Index] = Entry.Track.Weight end
		Self.Transition = {Duration = Duration, Elapsed = 0, StartWeights = StartWeights, Target = Target}
	end
	Self.TargetName = StateName
	return true
end

function Policy.Step(Self, DeltaTime)
	if Self.Closed or not IsFinite(DeltaTime) or DeltaTime < 0 then return false end
	local Transition = Self.Transition
	if Transition == nil then return true end
	Transition.Elapsed = math.min(Transition.Duration, Transition.Elapsed + DeltaTime)
	local Fraction = Transition.Elapsed / Transition.Duration
	for Index, Entry in Self.Entries do
		local TargetWeight = if Entry == Transition.Target then 1 else 0
		Entry.Track.Weight = Transition.StartWeights[Index] * (1 - Fraction) + TargetWeight * Fraction
	end
	if Fraction == 1 then
		for _, Entry in Self.Entries do
			if Entry ~= Transition.Target then Entry.Track:Stop() end
		end
		Self.Transition = nil
	end
	return true
end

function Policy.GetState(Self)
	return if Self.Closed then nil else Self.TargetName
end

function Policy.Close(Self)
	if Self.Closed then return end
	Self.Closed = true
	if Self.Connection then Self.Connection:Disconnect() end
	StopTracks(Self.Entries)
	Self.Entries = {}
	Self.ByName = {}
	Self.Connection = nil
	Self.Transition = nil
	Self.TargetName = nil
end

return Policy
