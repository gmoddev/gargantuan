local Workspace = game:GetService("Workspace")
local Player = game:GetService("Players").LocalPlayer
local Terrain = Workspace.Terrain
local function Await(Ticket)
	for Step = 1, 600 do
		task.wait()
		local Receipt = Terrain:GetEditStatus(Ticket)
		if Receipt.Status ~= "Pending" then
			assert(Receipt.Status == "Committed" and Receipt.CollisionReady, Receipt.Diagnostic or Receipt.Status)
			return
		end
	end
	error("Rolling terrain edit timed out")
end

Await(Terrain:FillBall(Vector3.new(47, 5.31, 17), 6.02, 1))
local Params = RaycastParams.new()
Params.IncludeTerrain = true
Params.FilterType = Enum.RaycastFilterType.Include
Params.FilterDescendantsInstances = { Terrain }

local InitialHit = Workspace:Raycast(Vector3.new(43.5, 14, 17), Vector3.new(0, -12, 0), Params)
assert(InitialHit and InitialHit.Instance == Terrain and InitialHit.Normal.Y > 0.65 and InitialHit.Normal.Y < 0.95)
local Character = Player.Character
Character.CFrame = CFrame.new(43.5, InitialHit.Position.Y + 2.25, 17)
local BeforeY = Character.Position.Y
local Grounded = 0
local MinNormalY = 1
for Step = 1, 11 do
	local Movement = Character:Move(Vector3.new(0.38, -0.21, 0), Vector3.new(1, 0, 0))
	if Movement.HasFloor then
		Grounded += 1
		MinNormalY = math.min(MinNormalY, Movement.FloorNormal.Y)
	end
end
assert(Grounded >= 6 and Character.Position.X > 46.2 and Character.Position.Y > BeforeY + 0.3 and MinNormalY < 0.96,
	tostring(Grounded) .. " ground contacts, final " .. tostring(Character.Position) .. " minimum normal " .. tostring(MinNormalY))
print("[Terrain:Contact] Rolling terrain climbed", Grounded, MinNormalY, BeforeY, Character.Position.Y)

local Drone = Instance.new("Part")
Drone.Name = "TerrainQualificationDrone"
Drone.Shape = Enum.PartType.Ball
Drone.Size = Vector3.new(0.6, 0.6, 0.6)
Drone.Anchored = true
Drone.CanCollide = false
Drone.Position = Vector3.new(38, 7, 17)
Drone.Parent = Workspace
Player:SetPossession(Drone)
assert(Player.PossessedEntity == Drone)
local Requested = Vector3.new(15, 0, 0)
local Hit = Workspace:SphereCast(Drone.Position, 0.3, Requested, Params)
assert(Hit and Hit.Instance == Terrain and Hit.Normal.X < -0.8 and Hit.Distance > 0)
local Move = Requested.Unit * math.max(0, Hit.Distance - 0.07)
Drone.CFrame = CFrame.new(Drone.Position + Move)
local Overlap = Workspace:GetPartsInSphere(Drone.Position, 0.3, Params)
assert(Drone.Position.X > 39 and Drone.Position.X < 42 and not Overlap.Terrain)
Player:SetPossession(nil)
assert(Player.PossessedEntity == nil)
print("[Terrain:Contact] Custom possessed Part stopped at steep terrain", Drone.Position.X, Hit.Normal.X)
print("[Terrain:Sample] Landscape collision is ready")
