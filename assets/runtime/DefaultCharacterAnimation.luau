local States = { "Idle", "Move", "Jump", "Fall" }

local function Stop(Playback)
	if Playback.Active then
		Playback.Active:Stop()
		Playback.Active = nil
	end
end

local function Load(Animator)
	local Tracks = {}
	for _, Name in States do
		local Reference = Animator:GetAttribute(Name .. "Animation")
		if type(Reference) == "string" and Reference ~= "" then
			local Ok, Track = pcall(function()
				return Animator:LoadAnimation(Reference)
			end)
			if Ok and Track then
				Track.Looped = Name ~= "Jump"
				Tracks[Name] = Track
			else
				warn("[Animation:Locomotion] Could not load stock clip", Name)
			end
		end
	end
	return Tracks
end

local function Evaluate(Character, Tracks)
	local Velocity = Character.Velocity
	if not Character.Grounded then
		return (if Velocity.Y > 0.5 then Tracks.Jump or Tracks.Fall else Tracks.Fall) or Tracks.Idle
	end
	if Velocity.X * Velocity.X + Velocity.Z * Velocity.Z > 0.25 then
		return Tracks.Move or Tracks.Idle
	end
	return Tracks.Idle
end

return function(Players, RunService)
	local PlaybackByPlayerId = {}
	local function Add(Player)
		if Player.Destroyed or not Player:IsA("Player") then
			return
		end
		local PlayerId = Player.PlayerId
		local Previous = PlaybackByPlayerId[PlayerId]
		if Previous and Previous.Player == Player then
			return
		end
		if Previous then
			Stop(Previous)
		end
		PlaybackByPlayerId[PlayerId] = { Player = Player }
	end
	local function Remove(Player)
		for PlayerId, Playback in PlaybackByPlayerId do
			if Playback.Player == Player then
				Stop(Playback)
				PlaybackByPlayerId[PlayerId] = nil
				return
			end
		end
	end
	for _, Player in Players:GetPlayers() do
		Add(Player)
	end
	local AddedConnection = Players.ChildAdded:Connect(Add)
	local RemovedConnection = Players.ChildRemoved:Connect(Remove)
	local RenderConnection = RunService.PreRender:Connect(function()
		for PlayerId, Playback in PlaybackByPlayerId do
			if Playback.Player.Destroyed then
				Stop(Playback)
				PlaybackByPlayerId[PlayerId] = nil
				continue
			end
			local Character = Playback.Player.Character
			local Root = Character and Character:IsA("KinematicCharacter") and Character.RootPart
			local Animator = Root and Root:IsA("MeshPart") and Root:FindFirstChildOfClass("Animator") or nil
			if Animator ~= Playback.Animator then
				Stop(Playback)
				Playback.Animator = Animator
				Playback.Tracks = if Animator then Load(Animator) else nil
			end
			if Playback.Tracks then
				local Track = Evaluate(Character, Playback.Tracks)
				if Track ~= Playback.Active then
					Stop(Playback)
					Playback.Active = Track
					if Track then
						Track:Play()
					end
				end
			end
		end
	end)

	return function()
		RenderConnection:Disconnect()
		RemovedConnection:Disconnect()
		AddedConnection:Disconnect()
		for _, Playback in PlaybackByPlayerId do
			Stop(Playback)
		end
		table.clear(PlaybackByPlayerId)
	end
end
