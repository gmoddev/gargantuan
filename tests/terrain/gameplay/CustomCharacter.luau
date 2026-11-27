local Workspace = game:GetService("Workspace")
local Player = game:GetService("Players").LocalPlayer
local Terrain = Workspace.Terrain
local Params = RaycastParams.new()
Params.IncludeTerrain = true

local function AwaitEdit(Ticket)
	for Step = 1, 1200 do
		local Status = Terrain:GetEditStatus(Ticket)
		if Status.Status ~= "Pending" then
			assert(Status.Status == "Committed" and Status.CollisionReady, Status.Status .. " " .. (Status.Diagnostic or ""))
			return
		end
		task.wait()
	end
	error("[Terrain:Contact] Edit readiness timeout")
end

AwaitEdit(Terrain:FillBall(Vector3.new(17, 5.6, 16), 7.3, 7))
local Drive = Instance.new("KinematicCharacter")
Drive.Name = "TerrainQualificationPossessed"
Drive.CapsuleRadius = 0.65
Drive.CapsuleHeight = 2.4
Drive.Parent = Workspace
Player:SetPossession(Drive)
assert(Player.PossessedEntity == Drive, "[Terrain:Contact] Custom possession did not commit")
local Hit = Workspace:Raycast(Vector3.new(12, 20, 16.1), Vector3.new(0, -20, 0), Params)
assert(Hit and Hit.Instance == Terrain and Hit.Normal.Y > 0.65 and Hit.Normal.Y < 0.9,
	"[Terrain:Contact] Rolling surface starting ray is not on a slope")
Drive.CFrame = CFrame.new(12, Hit.Position.Y + 1.65, 16.1)
local Initial = Drive:Move(Vector3.new(0, -0.8, 0))
local PeakY, Grounded, Steepest = Initial.Position.Y, 0, 1
for Step = 1, 14 do
	local Motion = Drive:Move(Vector3.new(0.7, -0.22, 0), Vector3.new(1, 0, 0))
	if Motion.HasFloor then
		Grounded += 1
		Steepest = math.min(Steepest, Motion.FloorNormal.Y)
	end
	PeakY = math.max(PeakY, Motion.Position.Y)
end
print("[Terrain:Contact] Possessed rolling surface", Initial.Position.Y, PeakY, Drive.Position.X, Drive.Position.Y,
	Grounded, Steepest)
assert(Drive.Position.X > 19.5 and Grounded >= 7 and PeakY > Initial.Position.Y + 1 and Drive.Position.Y < PeakY - 0.35,
	"[Terrain:Contact] Custom possessed Character did not climb and descend the rolling surface")

Drive.CFrame = CFrame.new(39.8, 6, 14.3)
local Start = Drive.Position
AwaitEdit(Terrain:FillBlock(Vector3.new(47.4, 6.3, 14.3), Vector3.new(10.8, 12.6, 12.6), 5))
assert((Drive.Position - Start).Magnitude < 0.05, "[Terrain:Contact] A nearby fill teleported the possessed Character")
local Cliff = Drive:Move(Vector3.new(6, 0, 0), Vector3.new(1, 0, 0))
assert(Cliff.Collided and Cliff.ContactNormal.X < -0.9 and Cliff.Position.X < 41.9,
	"[Terrain:Contact] Possessed Character penetrated filled cliff")
print("[Terrain:Contact] Possessed filled cliff", Cliff.Position.X, Cliff.ContactNormal.X)
Player:SetPossession(nil)
assert(Player.PossessedEntity == nil and not Drive.Destroyed, "[Terrain:Contact] Possession release retired the entity")
print("[Terrain:Sample] Landscape collision is ready")
