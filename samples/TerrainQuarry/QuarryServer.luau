local Workspace = game:GetService("Workspace")
local Players = game:GetService("Players")
local DataStoreService = game:GetService("DataStoreService")
local Terrain = Workspace.Terrain
local Course = Workspace.QuarryCourse
local WorkRequest = Course.WorkRequest

local Stations = {
	{ Name = "Excavate", Point = Vector3.new(25, 2, 15), Radius = 1.65, Material = 0 },
	{ Name = "Fill", Point = Vector3.new(58, 3, 15), Sample = Vector3.new(58, 4, 15), Radius = 1.55, Material = 4 },
	{ Name = "Paint", Point = Vector3.new(90, 3, 12), Radius = 1.20, Material = 5 },
}
local Params = RaycastParams.new()
Params.IncludeTerrain = true
Params.FilterType = Enum.RaycastFilterType.Include
Params.FilterDescendantsInstances = { Terrain }

local Progress = DataStoreService:GetDocumentStore("QuarryCourseProgress")
local ProgressKey = "QuarryV1"
local Persistence = "durable"
local ProgressRevision = "0"
local Phase = 0
local Proofs = {}
local Ready = false
local Busy = false
local Faulted = false

local function AwaitStore(Operation, Write)
	for _ = 1, 200 do
		local Result = Operation:Poll()
		if Result.Status ~= "Pending" then
			return Result
		end
		task.wait(0.05)
	end
	Operation:Cancel()
	return { Status = if Write then "UnknownOutcome" else "Timeout" }
end

local function ReadProgress()
	local Result = AwaitStore(Progress:GetAsync(ProgressKey), false)
	if Result.Status == "Missing" then
		return "Missing", 0, "0"
	end
	if Result.Status ~= "Ok" then
		return Result.Status
	end
	local Record = Result.Record
	local Value = Record.Value
	if type(Value) ~= "table" or Value.Version ~= 1
		or type(Value.Phase) ~= "number" or Value.Phase % 1 ~= 0
		or Value.Phase < 0 or Value.Phase > #Stations then
		return "Invalid"
	end
	return "Ok", Value.Phase, Record.Revision
end

local function SaveProgress(Step)
	if Persistence == "ephemeral" then
		return
	end
	local Result = AwaitStore(Progress:PutAsync(ProgressKey, { Version = 1, Phase = Step }, ProgressRevision), true)
	if Result.Status == "Ok" and Result.Revision then
		ProgressRevision = Result.Revision
		return
	end
	if Result.Status == "Conflict" or Result.Status == "UnknownOutcome" then
		local Status, StoredPhase, Revision = ReadProgress()
		if Status == "Ok" and StoredPhase == Step then
			ProgressRevision = Revision
			return
		end
	end
	error("could not confirm stored phase " .. Step .. " (" .. Result.Status .. ")")
end

local function ApplyTerrain(Step)
	local Station = Stations[Step]
	local Ticket
	if Step == 1 then
		Ticket = Terrain:RemoveBall(Station.Point, Station.Radius)
	elseif Step == 2 then
		Ticket = Terrain:FillBall(Station.Point, Station.Radius, Station.Material)
	else
		Ticket = Terrain:PaintBall(Station.Point, Station.Radius, Station.Material)
	end

	local Receipt
	for _ = 1, 400 do
		Receipt = Terrain:GetEditStatus(Ticket)
		if Receipt and Receipt.Status == "Committed" and Receipt.CollisionReady then
			break
		end
		if not Receipt or Receipt.Status ~= "Pending" then
			error("terrain edit for " .. Station.Name .. " ended with " .. (Receipt and Receipt.Status or "missing receipt"))
		end
		task.wait(0.05)
	end
	if not Receipt or Receipt.Status ~= "Committed" or not Receipt.CollisionReady then
		Terrain:CancelEdit(Ticket)
		error("terrain edit timed out for " .. Station.Name)
	end

	local Point = Station.Sample or Station.Point
	local Sample, Revision = Terrain:ReadRegion(Point, Point + Vector3.one)
	local Density = buffer.readi16(Sample, 0)
	local Material = buffer.readu32(Sample, 2)
	if Revision ~= Receipt.Revision or Material ~= Station.Material or (Density < 0) ~= (Material ~= 0) then
		error("authoritative sample did not confirm " .. Station.Name)
	end
	local Origin = Vector3.new(Station.Point.X, 16, Station.Point.Z)
	local Hit = Workspace:Raycast(Origin, Vector3.new(0, -20, 0), Params)
	if not Hit or Hit.Instance ~= Terrain then
		error("no Terrain collision at " .. Station.Name)
	end
	return { Density = Density, Material = Material, Height = Hit.Position.Y, Revision = Revision }
end

local function PublishPhase(Step, Proof, Replay)
	Phase = Step
	Proofs[Step] = Proof
	print(string.format(
		"[Terrain:Quarry] server phase=%d density=%d material=%d ray=%.4f revision=%s replay=%s",
		Step, Proof.Density, Proof.Material, Proof.Height, Proof.Revision, tostring(Replay)
	))
end

local function Reply(Player, Code, Step)
	local Proof = Proofs[Step]
	if Player.Parent == Players then
		WorkRequest:TryFireClient(Player, Code, Phase, Step, Proof and Proof.Density or 0,
			Proof and Proof.Material or 0, Proof and Proof.Height or 0, Proof and Proof.Revision or "")
	end
end

local function StopCourse(Reason)
	Faulted = true
	warn("[Terrain:Quarry] course halted " .. tostring(Reason))
end

local function PlacePlayer(Player)
	local function Place(Character)
		Character.CFrame = CFrame.new(6, 6.5, if Player.PlayerId % 2 == 1 then 8 else 10)
	end
	Player.CharacterAdded:Connect(Place)
	if Player.Character then
		Place(Player.Character)
	end
end
Players.PlayerAdded:Connect(PlacePlayer)
for _, Player in Players:GetPlayers() do
	PlacePlayer(Player)
end

-- An ordinary dynamic body shares the same terrain collider as the characters.
local Marker = Instance.new("Part")
Marker.Name = "QuarryLooseStone"
Marker.Size = Vector3.new(1, 1, 1)
Marker.Position = Vector3.new(40, 9, 17)
Marker.Color = Color3.fromRGB(108, 121, 134)
Marker.Anchored = false
Marker.Parent = Workspace

WorkRequest.OnPlayerEvent:Connect(function(Player, Command, Step)
	if Command == "Status" then
		Reply(Player, if Faulted then "Halted" elseif Ready then "Ready" else "Starting", 0)
		return
	end
	if Command ~= "Work" or type(Step) ~= "number" or Step % 1 ~= 0 or not Stations[Step] then
		return
	end
	if Faulted or not Ready then
		Reply(Player, if Faulted then "Halted" else "Starting", Step)
		return
	end
	local Position = Player.Character and Player.Character.Position
	if not Position or math.abs(Position.X - Stations[Step].Point.X) > 3.5
		or Position.Z < 6 or Position.Z > 15 or Position.Y < 2 or Position.Y > 10 then
		Reply(Player, "TooFar", Step)
		return
	end
	if Step <= Phase then
		Reply(Player, "Complete", Step)
		return
	end
	if Step ~= Phase + 1 then
		Reply(Player, "OutOfOrder", Step)
		return
	end
	if Busy then
		Reply(Player, "Working", Step)
		return
	end

	Busy = true
	Reply(Player, "Working", Step)
	task.spawn(function()
		local Ok, Failure = pcall(function()
			local Proof = ApplyTerrain(Step)
			SaveProgress(Step)
			PublishPhase(Step, Proof, false)
		end)
		Busy = false
		if not Ok then
			StopCourse(Failure)
		end
		Reply(Player, if Ok then "Complete" else "Halted", Step)
	end)
end)

local Ok, Failure = pcall(function()
	local Grid = Terrain:GetGridInfo()
	if not Grid.EditingAvailable or Grid.SampleSpacing ~= 1 then
		error("the course requires editable terrain with sample spacing 1")
	end
	local ResumePhase = 0
	if Progress:GetCapabilities().Documents then
		local Status, StoredPhase, Revision = ReadProgress()
		if Status ~= "Ok" and Status ~= "Missing" then
			error("progress could not be loaded (" .. Status .. ")")
		end
		ResumePhase = StoredPhase
		ProgressRevision = Revision
	else
		Persistence = "ephemeral"
	end
	for Step = 1, ResumePhase do
		PublishPhase(Step, ApplyTerrain(Step), true)
	end
	Ready = true
	print(string.format("[Terrain:Quarry] ready phase=%d persistence=%s", Phase, Persistence))
end)
if not Ok then
	StopCourse(Failure)
end
