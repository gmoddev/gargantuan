local Space = game:GetService("Workspace")
local Players = game:GetService("Players")
local Actions = game:GetService("ActionMap")
local YardRequest = game:GetService("ReplicatedStorage").YardRequest
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
-- The offline stone demonstrates gravity. Online the stationary stone is safe to reserve for a returning player.
local Loose = Players.LocalPlayer ~= nil
PlacePart(Space, "LooseStone", if Loose then V(5, 6, 1) else V(5, 1, 1), not Loose, Color3.fromRGB(108, 121, 134))

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
	local Read
	for Attempt = 1, 3 do
		Read = Await(Progress:GetAsync(ProgressKey, 2000))
		if Read.Status ~= "Busy" and Read.Status ~= "Deadline" and Read.Status ~= "UnknownOutcome" then break end
		if Attempt < 3 then task.wait(0.1 * Attempt) end
	end
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
		local Write = Await(Progress:PutAsync(ProgressKey, {Version = 1, Events = Next}, Revision, 2000))
		if Write.Status == "Conflict" or Write.Status == "UnknownOutcome" or Write.Status == "Deadline" then
			LoadProgress()
			return
		end
		if Write.Status ~= "Ok" or not Write.Revision then error("could not save yard change (" .. Write.Status .. ")") end
		Revision = Write.Revision
	end
	ApplyEvent(Event)
	table.insert(History, Event)
end

local Queue = {}
local function Change(Event)
	if Faulted or #Queue >= 16 then return end
	table.insert(Queue, Event)
	if Busy then return end
	Busy = true
	while #Queue > 0 and not Faulted do
		local Next = table.remove(Queue, 1)
		local Grid = if Next.Kind == "wall" then Wall else Floor
		if Next.Kind == "dig" or BlocksAdded[Grid] < Width * 4 then
			local Ok, Failure = pcall(Record, Next)
			if not Ok then
				Faulted = true
				warn("[Spatial:ConstructionYard] Editing paused: " .. tostring(Failure))
			end
		end
	end
	Busy = false
end

if Durable then
	local Ok, Failure = pcall(LoadProgress)
	if not Ok then
		Faulted = true
		warn("[Spatial:ConstructionYard] Could not restore edits: " .. tostring(Failure))
	end
end
local PendingDoors = {}

local function UseDoor(Player, Avatar, Inside)
	if PendingDoors[Player] then return end
	local LocalPosition = Avatar.RootPart.Position
	if math.abs(LocalPosition.X) > 10 or math.abs(LocalPosition.Z - 2) > 10 then return end
	local Destination = if Inside then Space else Workshop
	local Params = RaycastParams.new()
	Params.IncludeBlockGrids = true
	Params.IncludeTerrain = true
	Params.FilterType = Enum.RaycastFilterType.Include
	Params.FilterDescendantsInstances = {if Inside then Space.Terrain else Floor}
	local FloorHit = Destination:Raycast(V(0, 10, 2), V(0, -24, 0), Params)
	if not FloorHit then return end
	local Landing = FloorHit.Position + V(0, math.max(Avatar.CapsuleHeight, Avatar.RootPart.Size.Y) * 0.5 + 1, 0)
	local Margin = V(4, math.max(5, Avatar.CapsuleHeight + 1), 4)
	local Ticket, Failure = Player:RequestSpatialTransfer(Destination, CFrame.new(Landing), Landing - Margin, Landing + Margin)
	if not Ticket then
		warn("[Spatial:ConstructionYard] Door request " .. tostring(Failure))
		return
	end
	PendingDoors[Player] = Ticket
	task.spawn(function()
		local Status = Ticket:GetStatus()
		while Status == "Pending" do
			task.wait(0.03)
			Status = Ticket:GetStatus()
		end
		if PendingDoors[Player] ~= Ticket then return end
		PendingDoors[Player] = nil
		if Status == "Applied" then
			if Player == Players.LocalPlayer then
				Space.CurrentSpatialRegion = Avatar.RootPart.SpatialRegion
			else
				YardRequest:TryFireClient(Player, "DoorApplied")
			end
		else
			warn("[Spatial:ConstructionYard] Door request ended " .. Status)
		end
	end)
end

local function UseTool(Player, Name)
	if typeof(Name) ~= "string" or (Name ~= "YardDoor" and Name ~= "YardBuild" and Name ~= "YardDig") then return end
	local Avatar = Player and Player.Character
	if not Avatar or not Avatar.RootPart or PendingDoors[Player] then return end
	local Inside = Avatar.RootPart.SpatialRegion == Workshop
	if Name == "YardDoor" then
		UseDoor(Player, Avatar, Inside)
	elseif Name == "YardBuild" then
		local Grid = if Inside then Floor else Wall
		if BlocksAdded[Grid] < Width * 4 then Change({Kind = if Inside then "floor" else "wall"}) end
	elseif Name == "YardDig" and not Inside then
		local Params = RaycastParams.new()
		Params.IncludeTerrain = true
		Params.FilterType = Enum.RaycastFilterType.Include
		Params.FilterDescendantsInstances = {Space.Terrain}
		local Aim = Avatar.Position + Avatar.CFrame.LookVector * 2.5
		local Hit = Space:Raycast(Aim + V(0, 2, 0), V(0, -12, 0), Params)
		if not Hit then return end
		local Point = Hit.Position - V(0, 0.35, 0)
		Change({Kind = "dig", X = math.round(Point.X * 16), Y = math.round(Point.Y * 16), Z = math.round(Point.Z * 16)})
	end
end

local Remote = YardRequest.OnPlayerEvent:Connect(UseTool)
local LocalInput = Actions.ActionBegan:Connect(function(Name)
	local LocalPlayer = Players.LocalPlayer
	if LocalPlayer then UseTool(LocalPlayer, Name) end
end)
local Removing = Players.PlayerRemoving:Connect(function(Player)
	local Ticket = PendingDoors[Player]
	if Ticket then
		PendingDoors[Player] = nil
		Ticket:Cancel()
	end
end)
script.Destroying:Connect(function()
	Remote:Disconnect()
	LocalInput:Disconnect()
	Removing:Disconnect()
end)
if not Faulted then
	print(string.format("[Spatial:ConstructionYard] ready edits=%d persistence=%s", #History, if Durable then "documents" else "session"))
end
print("[Spatial:ConstructionYard] B builds a course, E excavates ahead, F enters or leaves the workshop")
