local Workspace = game:GetService("Workspace")
local Players = game:GetService("Players")
local Terrain = Workspace.Terrain
local Course = Workspace.QuarryCourse
local WorkRequest = Course.WorkRequest
local LocalPlayer = Players.LocalPlayer

local Stations = {
	{ Name = "Excavate", X = 25, Z = 15, Material = 0 },
	{ Name = "Fill", X = 58, Z = 15, Material = 4 },
	{ Name = "Paint", X = 90, Z = 12, Material = 5 },
}
local Params = RaycastParams.new()
Params.IncludeTerrain = true
Params.FilterType = Enum.RaycastFilterType.Include
Params.FilterDescendantsInstances = { Terrain }

local Phase = 0
local State = "Starting"
local Proofs = {}
local Qualification = Course:GetAttribute("QualificationRoute") == true

local ReplyConnection = WorkRequest.OnClientEvent:Connect(function(Code, ServerPhase, Step, Density, Material, Height, Revision)
	Phase = ServerPhase
	if Code == "Starting" or Code == "Ready" or Code == "Halted" then
		State = Code
	end
	if Code == "Complete" then
		Proofs[Step] = { Density = Density, Material = Material, Height = Height, Revision = Revision }
	end
	if not Qualification then
		if Code == "TooFar" then
			print("[Terrain:Quarry] approach " .. Stations[Step].Name .. " at X=" .. Stations[Step].X)
		elseif Code == "Complete" then
			print("[Terrain:Quarry] completed " .. Stations[Step].Name .. " at terrain revision " .. Revision)
		elseif Code == "Halted" then
			warn("[Terrain:Quarry] the server halted the course")
		end
	end
end)

local function Send(Command, Step)
	return WorkRequest:TryFireServer(Command, Step)
end

if not Qualification then
	local ActionMap = game:GetService("ActionMap")
	local Binding = ActionMap:BindKey("QuarryWork", Enum.KeyCode.E, 1, 101, true)
	local ActionConnection = ActionMap.ActionBegan:Connect(function(Name)
		if Name ~= "QuarryWork" then
			return
		end
		if State ~= "Ready" then
			Send("Status", 0)
			print("[Terrain:Quarry] checking course readiness")
		elseif Phase == #Stations then
			print("[Terrain:Quarry] all quarry work is complete")
		else
			Send("Work", Phase + 1)
		end
	end)
	task.spawn(function()
		for _ = 1, 80 do
			if State ~= "Starting" then
				break
			end
			Send("Status", 0)
			task.wait(0.5)
		end
		if State == "Ready" and Phase < #Stations then
			local Next = Stations[Phase + 1]
			print("[Terrain:Quarry] approach " .. Next.Name .. " at X=" .. Next.X .. " and press E")
		end
	end)
	script.Destroying:Connect(function()
		ActionMap:Unbind(Binding)
		ActionConnection:Disconnect()
		ReplyConnection:Disconnect()
	end)
	return
end

local CharacterControl = game:GetService("CharacterControlService")
local RunService = game:GetService("RunService")
local Lane = if LocalPlayer.PlayerId % 2 == 1 then 8 else 10
local TargetX

-- Submit on the real client simulation tick used by the stock network controller.
local Movement = RunService.PreSimulation:Connect(function()
	local Character = LocalPlayer.Character
	if not Character or not Character:IsA("KinematicCharacter") then
		return
	end
	local Direction = Vector2.zero
	if TargetX then
		local Position = Character.Position
		Direction = Vector2.new(
			math.clamp((TargetX - Position.X) / 2, -1, 1),
			math.clamp((Lane - Position.Z) / 2, -0.35, 0.35)
		)
		if Direction.Magnitude > 1 then
			Direction = Direction.Unit
		end
	end
	CharacterControl:SubmitMoveIntent(Direction, 0, false)
end)

local function WaitForReady()
	for Attempt = 1, 800 do
		if State == "Halted" then
			error("server halted before the route")
		end
		if State == "Ready" and LocalPlayer.Character then
			return
		end
		if Attempt % 6 == 1 then
			Send("Status", 0)
		end
		task.wait(0.05)
	end
	error("course or character readiness timed out")
end

local function MatchCollision(Station, Proof)
	local Origin = Vector3.new(Station.X, 16, Station.Z)
	local Hit = Workspace:Raycast(Origin, Vector3.new(0, -20, 0), Params)
	return Hit and Hit.Instance == Terrain and math.abs(Hit.Position.Y - Proof.Height) <= 0.035
end

local function Traverse(Step, Station)
	TargetX = Station.X
	for Attempt = 1, 700 do
		if State == "Halted" then
			error("server halted at " .. Station.Name)
		end
		local Character = LocalPlayer.Character
		local Position = Character and Character.Position
		if Position and math.abs(Position.X - Station.X) <= 1.2 and math.abs(Position.Z - Lane) <= 1.25 then
			local Proof = Proofs[Step]
			if Phase >= Step and Proof then
				if Proof.Material ~= Station.Material or (Proof.Density < 0) ~= (Station.Material ~= 0) then
					error("server sample did not confirm " .. Station.Name)
				end
				if MatchCollision(Station, Proof) then
					print(string.format(
						"[Terrain:Quarry] client phase=%d server_material=%d collision=ready player=%d revision=%s ray=%.4f",
						Step, Proof.Material, LocalPlayer.PlayerId, Proof.Revision, Proof.Height
					))
					return
				end
			end
			if Attempt % 6 == 1 then
				Send("Work", Step)
			end
		end
		task.wait(0.05)
	end
	local Character = LocalPlayer.Character
	error(Station.Name .. " timed out at " .. tostring(Character and Character.Position) .. " with server phase " .. Phase)
end

local Ok, Failure = pcall(function()
	WaitForReady()
	for Step, Station in Stations do
		Traverse(Step, Station)
	end
end)
TargetX = nil
-- Send several neutral simulation ticks before yielding control.
task.wait(0.2)
Movement:Disconnect()
ReplyConnection:Disconnect()
if Ok then
	print("[Terrain:Quarry] route complete player=" .. LocalPlayer.PlayerId)
else
	warn("[Terrain:Quarry] route failed player=" .. LocalPlayer.PlayerId .. " " .. tostring(Failure))
end
