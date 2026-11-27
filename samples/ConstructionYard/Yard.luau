local Space = game:GetService("Workspace")
local Players = game:GetService("Players")
local Actions = game:GetService("ActionMap")
local Workshop = Space.Workshop
local Wall = Space.RetainingWall
local Floor = Workshop.WorkshopBlocks
local V = Vector3.new

local function PlacePart(Parent, Name, Position, Anchored, Color)
	local Part = Instance.new("Part")
	Part.Name = Name
	Part.Anchored = Anchored
	Part.Size = V(1, 1, 1)
	Part.Position = Position
	Part.Color = Color
	Part.Parent = Parent
	return Part
end

-- Both survey markers occupy the same coordinates in separate physics worlds.
PlacePart(Space, "OutdoorMarker", V(4, 3, 3), true, Color3.fromRGB(229, 154, 55))
PlacePart(Workshop, "WorkshopMarker", V(4, 3, 3), true, Color3.fromRGB(59, 142, 190))
PlacePart(Space, "LooseStone", V(5, 6, 1), false, Color3.fromRGB(108, 121, 134))

local function Character()
	local Player = Players.LocalPlayer or Players:GetPlayers()[1]
	return if Player then Player.Character else nil
end

local Width = 6
local BlocksAdded = {[Wall] = 0, [Floor] = 0}
local Progress = game:GetService("DataStoreService"):GetDocumentStore("ConstructionYard")
local Durable = Progress:GetCapabilities().Documents
local ProgressKey = "YardV1"
local Revision = "0"
local History = {}
local Busy = false
local Faulted = false
local MaxEvents = 128

local function Await(Operation)
	for _ = 1, 200 do
		local Result = Operation:Poll()
		if Result.Status ~= "Pending" then return Result end
		task.wait(0.05)
	end
	Operation:Cancel()
	return {Status = "UnknownOutcome"}
end

local function ValidEvent(Event)
	if type(Event) ~= "table" then return false end
	if Event.Kind == "wall" or Event.Kind == "floor" then return true end
	if Event.Kind ~= "dig" then return false end
	for _, Axis in {"X", "Y", "Z"} do
		local N = Event[Axis]
		if type(N) ~= "number" or N % 1 ~= 0 or math.abs(N) > 4096 then return false end
	end
	return true
end

local function ApplyEvent(Event)
	if Event.Kind == "dig" then
		local Point = V(Event.X / 16, Event.Y / 16, Event.Z / 16)
		local Ticket = Space.Terrain:RemoveBall(Point, 1.65)
		for _ = 1, 400 do
			local Result = Space.Terrain:GetEditStatus(Ticket)
			if Result and Result.Status == "Committed" and Result.CollisionReady then return end
			if not Result or Result.Status ~= "Pending" then error("excavation failed") end
			task.wait(0.05)
		end
		Space.Terrain:CancelEdit(Ticket)
		error("excavation timed out")
	end
	local Grid = if Event.Kind == "wall" then Wall else Floor
	local Count = BlocksAdded[Grid]
	if Count >= Width * 4 then error("saved construction exceeds its four courses") end
	Grid:SetBlock(V(Count % Width - 2, 2 + math.floor(Count / Width), -5), "Brick")
	BlocksAdded[Grid] = Count + 1
end

local function LoadProgress()
	local Read = Await(Progress:GetAsync(ProgressKey))
	if Read.Status == "Missing" and #History == 0 then return end
	if Read.Status ~= "Ok" then error("could not read yard history (" .. Read.Status .. ")") end
	local Document = Read.Record.Value
	if type(Document) ~= "table" or Document.Version ~= 1 or type(Document.Events) ~= "table" then
		error("invalid yard history")
	end
	local Events = Document.Events
	if #Events < #History or #Events > MaxEvents then error("yard history length changed unexpectedly") end
	local Counts = {wall = 0, floor = 0}
	for Index, Event in ipairs(Events) do
		if not ValidEvent(Event) then error("invalid saved yard event") end
		if Event.Kind ~= "dig" then
			Counts[Event.Kind] += 1
			if Counts[Event.Kind] > Width * 4 then error("invalid saved course count") end
		end
		local Existing = History[Index]
		if Existing and (Event.Kind ~= Existing.Kind or Event.X ~= Existing.X or Event.Y ~= Existing.Y or Event.Z ~= Existing.Z) then
			error("saved yard history diverged")
		end
	end
	for Index = #History + 1, #Events do
		ApplyEvent(Events[Index])
		table.insert(History, Events[Index])
	end
	Revision = Read.Record.Revision
end

local function Record(Event)
	if not ValidEvent(Event) or #History >= MaxEvents then return end
	if Durable then
		local Next = table.clone(History)
		table.insert(Next, Event)
		local Write = Await(Progress:PutAsync(ProgressKey, {Version = 1, Events = Next}, Revision))
		if Write.Status == "Conflict" or Write.Status == "UnknownOutcome" then
			LoadProgress()
			return
		end
		if Write.Status ~= "Ok" or not Write.Revision then error("could not save yard change (" .. Write.Status .. ")") end
		Revision = Write.Revision
	end
	ApplyEvent(Event)
	table.insert(History, Event)
end

local function Change(Event)
	if Busy or Faulted then return end
	Busy = true
	local Ok, Failure = pcall(Record, Event)
	Busy = false
	if not Ok then
		Faulted = true
		warn("[Spatial:ConstructionYard] Editing paused: " .. tostring(Failure))
	end
end

if Durable then
	local Ok, Failure = pcall(LoadProgress)
	if not Ok then
		Faulted = true
		warn("[Spatial:ConstructionYard] Could not restore edits: " .. tostring(Failure))
	end
end
local function UseTool(Name)
	local Avatar = Character()
	if not Avatar or not Avatar.RootPart then return end
	local Inside = Avatar.RootPart.SpatialRegion == Workshop
	if Name == "YardDoor" then
		local Destination = if Inside then Space else Workshop
		local Params = RaycastParams.new()
		Params.IncludeBlockGrids = true
		Params.IncludeTerrain = true
		local FloorHit = Destination:Raycast(V(0, 8, 2), V(0, -20, 0), Params)
		if not FloorHit then return end
		Avatar.RootPart.SpatialRegion = if Inside then nil else Workshop
		Avatar.CFrame = CFrame.new(0, FloorHit.Position.Y + 3, 2)
		Space.CurrentSpatialRegion = if Inside then nil else Workshop
	elseif Name == "YardBuild" then
		local Grid = if Inside then Floor else Wall
		if BlocksAdded[Grid] < Width * 4 then Change({Kind = if Inside then "floor" else "wall"}) end
	elseif Name == "YardDig" and not Inside then
		local Point = Avatar.Position + Avatar.CFrame.LookVector * 2.5 - V(0, 1.5, 0)
		Change({Kind = "dig", X = math.round(Point.X * 16), Y = math.round(Point.Y * 16), Z = math.round(Point.Z * 16)})
	end
end

local Bindings = {
	Actions:BindKey("YardBuild", Enum.KeyCode.B, 1, 101, true),
	Actions:BindKey("YardDig", Enum.KeyCode.E, 1, 101, true),
	Actions:BindKey("YardDoor", Enum.KeyCode.F, 1, 101, true),
}
local Connection = Actions.ActionBegan:Connect(UseTool)
script.Destroying:Connect(function()
	Connection:Disconnect()
	for _, Binding in Bindings do Actions:Unbind(Binding) end
end)
if not Faulted then
	print(string.format("[Spatial:ConstructionYard] ready edits=%d persistence=%s", #History, if Durable then "documents" else "session"))
end
print("[Spatial:ConstructionYard] B builds a course, E excavates ahead, F enters or leaves the workshop")
