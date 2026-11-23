local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")
local CharacterControl = game:GetService("CharacterControlService")
local Course = Workspace:FindFirstChild("RelayCourse")
if not Course then
	warn("[Gameplay:Relay] RelayCourse is missing")
	return
end

local LaunchPad = Course:FindFirstChild("LaunchPad")
local GroundSwitch = Course:FindFirstChild("GroundSwitch")
local Hazard = Course:FindFirstChild("Hazard")
local Relay = Course:FindFirstChild("RelayBeacon")
if not LaunchPad or not GroundSwitch or not Hazard or not Relay then
	warn("[Gameplay:Relay] A course station is missing")
	return
end

local IsMultiplayer = Players.LocalPlayer == nil
local TargetCharges = 3
local DroneRadius = 0.8
local DroneSpeed = 12
local RespawnSeconds = 1.5
local HealthMaximum = 100
local UpdateInterval = 1 / 20
local Elapsed = 0
local Charges = 0
local States = {}

Course:SetAttribute("RelayMultiplayer", IsMultiplayer)
Course:SetAttribute("RelayEnabled", false)
Course:SetAttribute("RelayCharges", Charges)

local function IsStandingOn(Position, Pad)
	local Offset = Position - Pad.Position
	return math.abs(Offset.X) <= Pad.Size.X * 0.5
		and math.abs(Offset.Z) <= Pad.Size.Z * 0.5
		and Offset.Y > 0
		and Offset.Y <= 3.5
end

local function SpawnPosition(Player)
	-- Stagger the first few joins without binding a spawn to transport identity.
	return Vector3.new(((Player.PlayerId - 1) % 5 - 2) * 3, 4, 8)
end

local function SetHealth(Player, State, Health)
	State.Health = math.clamp(Health, 0, HealthMaximum)
	Player:SetAttribute("Health", State.Health)
end

local function AddPlayer(Player)
	if States[Player] then
		return
	end
	local Inventory = Instance.new("Folder")
	Inventory.Name = "RelayInventory"
	Inventory.Parent = Player
	local State = {
		Inventory = Inventory,
		Health = HealthMaximum,
		NextDamage = 0,
		NextLaunch = 0,
		NextDroneStep = 0,
		HasCharged = false,
	}
	States[Player] = State
	Player:SetAttribute("Team", "RelayCrew")
	Player:SetAttribute("RelayCharges", 0)
	SetHealth(Player, State, HealthMaximum)
	Player:SetAttribute("RelayMode", "OnFoot")

	local function PlaceCharacter(Character)
		Character.CFrame = CFrame.new(SpawnPosition(Player))
	end
	State.CharacterConnection = Player.CharacterAdded:Connect(PlaceCharacter)
	if Player.Character then
		PlaceCharacter(Player.Character)
	end
end

local function RetireDrone(State)
	local Drone = State.Drone
	State.Drone = nil
	State.HasCharged = false
	if Drone and not Drone.Destroyed then
		Drone:Destroy()
	end
end

local function RemovePlayer(Player)
	local State = States[Player]
	if State then
		States[Player] = nil
		State.CharacterConnection:Disconnect()
		RetireDrone(State)
	end
end

local function LaunchDrone(Player, State)
	local Drone = Instance.new("Part")
	Drone.Name = "SurveyDrone_" .. Player.PlayerId
	Drone.Shape = Enum.PartType.Ball
	Drone.Size = Vector3.one * (DroneRadius * 2)
	Drone.Color = Color3.new(0.08, 0.96, 0.76)
	Drone.Anchored = true
	Drone.CanCollide = false
	Drone.CanTouch = false
	Drone.CFrame = CFrame.new(LaunchPad.Position + Vector3.new(0, 7, 0))
	Drone:SetAttribute("RelayDrone", true)
	Drone.Parent = Course

	local Ok = pcall(function()
		Player:SetPossession(Drone)
	end)
	if not Ok then
		Drone:Destroy()
		State.NextLaunch = Elapsed + 1
		return
	end
	State.Drone = Drone
	State.NextDroneStep = Elapsed
	Player:SetAttribute("RelayMode", "Drone")
	Player:RemoveCharacter()
end

local function LandDrone(Player, State)
	if Player.PossessedEntity == State.Drone then
		Player:SetPossession(nil)
	end
	RetireDrone(State)
	State.NextLaunch = Elapsed + 1
	Player:SetAttribute("RelayMode", "OnFoot")
	Player:LoadCharacter()
end

local function StepDrone(Player, Drone, Input)
	local Origin = Drone.Position
	local Requested = Vector3.new(Input.Movement.X, 0, Input.Movement.Y) * (DroneSpeed * UpdateInterval)
	local Destination = Origin + Requested
	local Translation = Vector3.new(
		math.clamp(Destination.X, -29, 29),
		Destination.Y,
		math.clamp(Destination.Z, -22, 22)
	) - Origin
	if Translation.Magnitude < 0.001 then
		return
	end
	local Hit = Workspace:SphereCast(Origin, DroneRadius, Translation)
	if Hit then
		Translation = Translation.Unit * math.max(0, Hit.Distance - 0.05)
	end
	local Transform = CFrame.new(Origin + Translation) * CFrame.Angles(0, Input.FacingYawRadians, 0)
	CharacterControl:MovePossessedPart(Player, Transform)
end

for _, Player in Players:GetPlayers() do
	AddPlayer(Player)
end
local AddedConnection = Players.PlayerAdded:Connect(AddPlayer)
local RemovingConnection = Players.PlayerRemoving:Connect(RemovePlayer)
local StepConnection = RunService.PreSimulation:Connect(function(DeltaTime)
	Elapsed += math.clamp(DeltaTime, 0, 0.1)
	local CurrentPlayers = Players:GetPlayers()
	local SwitchHeld = false
	for _, Player in CurrentPlayers do
		local State = States[Player]
		local Character = Player.Character
		if State and State.Health > 0 and Character and not State.Drone
			and IsStandingOn(Character.Position, GroundSwitch) then
			SwitchHeld = true
			break
		end
	end
	if SwitchHeld ~= Course:GetAttribute("RelayEnabled") then
		Course:SetAttribute("RelayEnabled", SwitchHeld)
		GroundSwitch.Color = if SwitchHeld then Color3.new(0.55, 1, 0.24) else Color3.new(0.34, 0.52, 0.12)
	end

	for _, Player in CurrentPlayers do
		local State = States[Player]
		if not State then
			continue
		end
		local Character = Player.Character
		local Drone = State.Drone
		if Drone then
			if Drone.Destroyed or Player.PossessedEntity ~= Drone then
				LandDrone(Player, State)
				continue
			end
			local Input = CharacterControl:GetPossessionInput(Player)
			if Input and Input.Fresh and Input.JumpRequested then
				LandDrone(Player, State)
				continue
			end
			if Elapsed >= State.NextDroneStep then
				State.NextDroneStep = Elapsed + UpdateInterval
				if Input and Input.Fresh then
					StepDrone(Player, Drone, Input)
				end
			end
			local AtRelay = (Drone.Position - Relay.Position).Magnitude <= 2.2
			if SwitchHeld and AtRelay and not State.HasCharged and Charges < TargetCharges then
				State.HasCharged = true
				Charges += 1
				local Cell = Instance.new("Folder")
				Cell.Name = "RelayCell" .. Charges
				Cell:SetAttribute("Kind", "RelayCell")
				Cell.Parent = State.Inventory
				Course:SetAttribute("RelayCharges", Charges)
				Player:SetAttribute("RelayCharges", (Player:GetAttribute("RelayCharges") or 0) + 1)
			elseif not AtRelay then
				State.HasCharged = false
			end
		elseif State.RespawnAt then
			if Elapsed >= State.RespawnAt then
				State.RespawnAt = nil
				SetHealth(Player, State, HealthMaximum)
				Player:SetAttribute("RelayMode", "OnFoot")
				Player:LoadCharacter()
			end
		elseif Character then
			if Character.Position.Y < -12 then
				SetHealth(Player, State, 0)
			elseif Elapsed >= State.NextDamage and IsStandingOn(Character.Position, Hazard) then
				State.NextDamage = Elapsed + 0.25
				SetHealth(Player, State, State.Health - 25)
			end
			if State.Health == 0 then
				State.RespawnAt = Elapsed + RespawnSeconds
				Player:SetAttribute("RelayMode", "Respawning")
				Player:RemoveCharacter()
			elseif IsMultiplayer and Elapsed >= State.NextLaunch and IsStandingOn(Character.Position, LaunchPad) then
				LaunchDrone(Player, State)
			end
		end
	end
end)

script.Destroying:Connect(function()
	StepConnection:Disconnect()
	RemovingConnection:Disconnect()
	AddedConnection:Disconnect()
	for Player in States do
		RemovePlayer(Player)
	end
end)
