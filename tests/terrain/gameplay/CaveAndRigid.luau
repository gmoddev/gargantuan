local Workspace = game:GetService("Workspace")
local Players = game:GetService("Players")
local Terrain = Workspace.Terrain
local Params = RaycastParams.new()
Params.IncludeTerrain = true
Params.FilterType = Enum.RaycastFilterType.Include
Params.FilterDescendantsInstances = { Terrain }

local function AwaitEdit(Ticket)
	for Step = 1, 1600 do
		local Status = Terrain:GetEditStatus(Ticket)
		if Status.Status ~= "Pending" then
			assert(Status.Status == "Committed" and Status.CollisionReady, Status.Status .. " " .. (Status.Diagnostic or ""))
			return
		end
		task.wait()
	end
	error("[Terrain:Contact] Edit readiness timeout")
end

AwaitEdit(Terrain:FillBlock(Vector3.new(16, 5.4, 16), Vector3.new(18.5, 10, 18.5), 3))
AwaitEdit(Terrain:ClearRegion(Vector3.new(13, 2, 5), Vector3.new(19, 8, 16)))
local Origin = Vector3.new(15, 4.6, 12.1)
local Floor = Workspace:Raycast(Origin, Vector3.new(0, -5, 0), Params)
local Ceiling = Workspace:Raycast(Origin, Vector3.new(0, 5, 0), Params)
assert(Floor and Floor.Instance == Terrain and Floor.Normal.Y > 0.95, "[Terrain:Contact] Cave floor ray")
assert(Ceiling and Ceiling.Instance == Terrain and Ceiling.Normal.Y < -0.95, "[Terrain:Contact] Cave ceiling ray")
local FloorCast = Workspace:SphereCast(Origin, 0.35, Vector3.new(0, -5, 0), Params)
assert(FloorCast and FloorCast.Instance == Terrain, "[Terrain:Contact] Cave floor sphere cast")
local CeilingTouches = Workspace:GetPartsInSphere(Ceiling.Position, 0.6, Params)
assert(CeilingTouches.Terrain == Terrain and #CeilingTouches.Parts == 0 and not CeilingTouches.Truncated,
	"[Terrain:Contact] Cave ceiling overlap")
print("[Terrain:Contact] Cave query floor and ceiling", Floor.Position.Y, Ceiling.Position.Y)

local Character = Players.LocalPlayer.Character
assert(Character and Character:IsA("KinematicCharacter"), "[Terrain:Contact] Stock Character missing")
Character.CFrame = CFrame.new(15, Floor.Position.Y + 2.2, 8)
local Grounded = 0
for Step = 1, 8 do
	local Result = Character:Move(Vector3.new(0, -0.16, 0.7), Vector3.new(0, 0, 1))
	if Result.HasFloor and Result.FloorNormal.Y > 0.95 then Grounded += 1 end
end
assert(Grounded >= 5 and Character.Position.Z > 13, "[Terrain:Contact] Narrow tunnel traversal lost floor")
local HitRoof = Character:Move(Vector3.new(0, 4.7, 0), Vector3.new(0, 4, 0))
assert(HitRoof.Collided and HitRoof.ContactNormal.Y < -0.8 and HitRoof.Position.Y < Ceiling.Position.Y - 1.6,
	"[Terrain:Contact] Character penetrated cave ceiling")
print("[Terrain:Contact] Stock Character in narrow tunnel", Grounded, HitRoof.Position.Z, HitRoof.ContactNormal.Y)

Character.CFrame = CFrame.new(50, 15, 50)
local Rigid = Instance.new("Part")
Rigid.Name = "TerrainQualificationRigid"
Rigid.Size = Vector3.new(1, 1, 1)
Rigid.CFrame = CFrame.new(22, 13, 21)
Rigid.Anchored = false
Rigid.Parent = Workspace
for Step = 1, 1200 do
	task.wait(0.005)
	if Rigid.Position.Y < 11.05 then break end
end
local SupportedY = Rigid.Position.Y
assert(SupportedY > 10.5 and SupportedY <= 11.05, "[Terrain:Contact] Rigid never settled on the terrain shelf")
task.wait(0.1)
assert(math.abs(Rigid.Position.Y - SupportedY) < 0.13, "[Terrain:Contact] Rigid body unstable on terrain")
AwaitEdit(Terrain:RemoveBall(Vector3.new(22, 9.2, 21), 3.6))
for Step = 1, 1200 do
	task.wait(0.005)
	if Rigid.Position.Y < SupportedY - 1 then break end
end
assert(Rigid.Position.Y < SupportedY - 1, "[Terrain:Contact] Dynamic rigid kept stale terrain support")
print("[Terrain:Contact] Dynamic rigid fell after excavation", SupportedY, Rigid.Position.Y)
print("[Terrain:Sample] Landscape collision is ready")
