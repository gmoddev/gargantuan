local Players = game:GetService("Players")
local ActionMap = game:GetService("ActionMap")
local CharacterControl = game:GetService("CharacterControlService")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")
local Mix = game:GetService("AudioMixService")
local Player = Players.LocalPlayer
if not Player then
	warn("[Gameplay:Relay] Local player is unavailable")
	return
end

local Status
local MixToggle
for Attempt = 1, 300 do
	local PlayerGui = Player.PlayerGui
	local Hud = PlayerGui and PlayerGui:FindFirstChild("RelayHud")
	if Hud then
		Status = Hud:FindFirstChild("RelayStatus")
		MixToggle = Hud:FindFirstChild("RelayMixToggle")
		if Status and MixToggle then
			break
		end
	end
	task.wait()
end
if not (Status and MixToggle) then
	warn("[Gameplay:Relay] PlayerGui did not receive the complete HUD")
	return
end
MixToggle.Interactable = Mix ~= nil

local MixFeedback
local function ShowMixState(Muted)
	MixToggle.AccessibilitySelected = Muted
	MixToggle.AccessibleName = if Muted then "Unmute game audio" else "Mute game audio"
	MixToggle.ImageColor3 = if Muted then Color3.new(0.50, 0.58, 0.62) else Color3.new(1, 1, 1)
end
ShowMixState(Mix ~= nil and Mix:GetMasterMuted())

local MixConnection = MixToggle.Activated:Connect(function()
	if not Mix then
		return
	end
	local Muted = not Mix:GetMasterMuted()
	Mix:SetMasterMuted(Muted)
	ShowMixState(Muted)
	MixToggle:ReleaseFocus()
	if MixFeedback then
		task.cancel(MixFeedback)
	end
	MixToggle.BackgroundColor3 = Color3.new(0.25, 0.49, 0.53)
	MixFeedback = task.delay(0.25, function()
		MixFeedback = nil
		MixToggle.BackgroundColor3 = Color3.new(0.12, 0.22, 0.26)
	end)
end)

local PulseReference = "asset://ca34ffb8742655186db2cfb54124a5ff"
local ToneReference = "asset://a0d10f78c368437daac002a4e59fdd64"
local BeaconAnimator
local TriedAnimator
local Pulse
local CueBeacon
local ChargeTone
local PreviousCharges
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
	if CueBeacon ~= Beacon then
		CueBeacon = Beacon
		PreviousCharges = nil
		if ChargeTone and not ChargeTone.Destroyed then
			ChargeTone:Destroy()
		end
		ChargeTone = nil
		if Beacon then
			local Cue = Instance.new("Sound")
			Cue.Name = "RelayChargeTone"
			Cue.SoundId = ToneReference
			Cue.MixCategory = Enum.SoundMixCategory.Effects
			Cue.Volume = 0.4
			Cue.Parent = Beacon
			ChargeTone = Cue
		end
	end
	if ChargeTone and PreviousCharges and Charges > PreviousCharges then
		ChargeTone:Play()
	end
	PreviousCharges = Charges

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
	MixConnection:Disconnect()
	if MixFeedback then
		task.cancel(MixFeedback)
	end
	if ChargeTone and not ChargeTone.Destroyed then
		ChargeTone:Destroy()
	end
	if Pulse then
		Pulse:Stop()
	end
end)
