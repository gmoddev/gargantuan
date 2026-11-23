local Players = game:GetService("Players")
local ActionMap = game:GetService("ActionMap")
local CharacterControl = game:GetService("CharacterControlService")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")
local Player = Players.LocalPlayer
if not Player then
	warn("[Gameplay:Relay] Local player is unavailable")
	return
end

local Hud = Instance.new("ScreenGui")
Hud.Name = "RelayHud"
Hud.ResetOnSpawn = false
Hud.Parent = game
local Status = Instance.new("TextLabel")
Status.Name = "RelayStatus"
Status.Position = UDim2.new(0, 16, 0, 16)
Status.Size = UDim2.new(1, -32, 0, 56)
Status.BackgroundColor3 = Color3.new(0.03, 0.08, 0.13)
Status.BackgroundTransparency = 0.15
Status.TextColor3 = Color3.new(0.84, 1, 0.94)
Status.TextSize = 17
Status.TextWrapped = true
Status.Parent = Hud

local PulseReference = "asset://ca34ffb8742655186db2cfb54124a5ff"
local BeaconAnimator
local TriedAnimator
local Pulse
local PreviousRelayEnabled
local LandRequested = false
local LastFacing = 0
local LastDrone

local function GetDrone()
	local Entity = Player.PossessedEntity
	if Entity and Entity:IsA("BasePart") and Entity:GetAttribute("RelayDrone") == true then
		return Entity
	end
	return nil
end

local ActionConnection = ActionMap.ActionBegan:Connect(function(ActionName)
	if ActionName == "Jump" and GetDrone() then
		LandRequested = true
	end
end)

local SimulationConnection = RunService.PreSimulation:Connect(function()
	if not GetDrone() then
		LandRequested = false
		return
	end
	local Horizontal = ActionMap:GetValue("MoveRight") - ActionMap:GetValue("MoveLeft")
	local Forward = ActionMap:GetValue("MoveForward") - ActionMap:GetValue("MoveBackward")
	local Direction = Vector2.new(Horizontal, -Forward)
	if Direction.Magnitude > 1 then
		Direction = Direction.Unit
	end
	if Direction.Magnitude > 0.001 then
		LastFacing = math.atan2(-Direction.X, -Direction.Y)
	end
	if CharacterControl:SubmitPossessionIntent(Direction, LastFacing, LandRequested) then
		LandRequested = false
	end
end)

local RenderConnection = RunService.PreRender:Connect(function(DeltaTime)
	local Drone = GetDrone()
	if Drone then
		local Camera = Workspace.CurrentCamera
		local Focus = Drone.Position
		local View = CFrame.new(Focus + Vector3.new(0, 6, 12), Focus)
		Camera.CFrame = if Drone == LastDrone then Camera.CFrame:Lerp(View, math.clamp(DeltaTime * 14, 0, 1)) else View
	end
	LastDrone = Drone

	local Course = Workspace:FindFirstChild("RelayCourse")
	local Beacon = Course and Course:FindFirstChild("RelayBeacon")
	BeaconAnimator = Beacon and Beacon:FindFirstChildOfClass("Animator")
	if BeaconAnimator ~= TriedAnimator then
		if Pulse then
			Pulse:Stop()
			Pulse = nil
		end
		TriedAnimator = BeaconAnimator
		PreviousRelayEnabled = nil
		if BeaconAnimator then
			local Loaded, Track = pcall(function()
				return BeaconAnimator:LoadAnimation(PulseReference)
			end)
			if Loaded then
				Pulse = Track
				Pulse.Looped = true
				Pulse:Play()
			else
				warn("[Gameplay:Relay] Relay beacon animation failed to load")
			end
		end
	end

	local Enabled = Course and Course:GetAttribute("RelayEnabled") == true
	if Pulse and Enabled ~= PreviousRelayEnabled then
		Pulse.Speed = if Enabled then 2 else 0.35
		PreviousRelayEnabled = Enabled
	end

	local Health = Player:GetAttribute("Health") or 100
	local Charges = if Course then Course:GetAttribute("RelayCharges") or 0 else 0
	local Mode = Player:GetAttribute("RelayMode")
	local Help
	if Charges == 3 then
		Help = "Relay restored"
	elseif Mode == "Respawning" then
		Help = "Respawning at the start"
	elseif Drone then
		Help = if Enabled then "Fly to the elevated relay. Space to land." else "Ask a teammate to hold the green switch. Space to land."
	elseif Course and Course:GetAttribute("RelayMultiplayer") == false then
		Help = "Local preview. Walk with WASD and jump with Space."
	else
		Help = "Walk onto the cyan launch pad or hold the green switch."
	end
	local Text = string.format("Health %d   |   Relay %d / 3\n%s", Health, Charges, Help)
	if Status.Text ~= Text then
		Status.Text = Text
	end
end)

script.Destroying:Connect(function()
	RenderConnection:Disconnect()
	SimulationConnection:Disconnect()
	ActionConnection:Disconnect()
	if Pulse then
		Pulse:Stop()
	end
	if not Hud.Destroyed then
		Hud:Destroy()
	end
end)
