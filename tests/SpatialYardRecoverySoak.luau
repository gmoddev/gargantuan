-- This server-side observer writes the Yard's public persistence format in 16-event batches.
-- A fresh process exercises the unmodified Yard script's replay before the next batch starts.
local Space = game.Workspace
local Store = game:GetService("DataStoreService"):GetDocumentStore("ConstructionYard")
local V = Vector3.new
local Total, Batch = 64, 16
local function Event(Index)
	local Group = math.floor((Index - 1) / 4)
	local Slot = (Index - 1) % 4
	if Slot == 2 then
		local Positions = {-7, -3, 3, 8}
		return {Kind = "dig", X = 16 * Positions[Group % 4 + 1], Y = -12, Z = 16 * Positions[math.floor(Group / 4) + 1]}
	end
	return {Kind = if Slot == 0 or (Slot == 3 and Group % 2 == 0) then "wall" else "floor"}
end

local function Await(Operation)
	for _ = 1, 400 do
		local Result = Operation:Poll()
		if Result.Status ~= "Pending" then return Result end
		task.wait(.01)
	end
	error("persistence operation did not finish")
end

local function Validate(Document, Count)
	assert(Document.Version == 1 and #Document.Events == Count)
	for Index, Actual in ipairs(Document.Events) do
		local Expected = Event(Index)
		for Key, Value in Expected do assert(Actual[Key] == Value) end
		for Key in Actual do assert(Expected[Key] ~= nil) end
	end
end

local function Hash(Text)
	local Value = 0
	for Index = 1, #Text do Value = (Value * 131 + string.byte(Text, Index)) % 4294967291 end
	return string.format("%.0f", Value)
end

local function EmitBytes(Name, Data)
	for Offset=1,#Data,192 do
		local Hex = {}
		for Index=Offset,math.min(Offset+191,#Data) do
			table.insert(Hex, string.format("%02x", string.byte(Data,Index)))
		end
		warn(string.format("[Spatial:YardBytes] %s %d %s", Name, Offset-1, table.concat(Hex)))
	end
end

local function MatchGrid(World, Grid, Count)
	local Params = RaycastParams.new()
	Params.FilterType = Enum.RaycastFilterType.Include
	Params.FilterDescendantsInstances = {Grid}
	Params.IncludeBlockGrids = true
	for Index = 0, 23 do
		local Cell = V(Index % 6 - 2, 2 + math.floor(Index / 6), -5)
		local Block = Grid:GetBlock(Cell)
		local Hit = World:Raycast(Grid:CellToWorld(Cell) + V(0,0,.85), V(0,0,-1.5), Params)
		if Index < Count then
			if not Block or Block.Definition ~= "Brick" or not Hit or Hit.Instance ~= Grid or not Hit.GridHit or Hit.GridHit.Cell ~= Cell then return false end
		elseif Block or Hit then
			return false
		end
	end
	return true
end

local function Replay(Count)
	local Wall, Floor = 0, 0
	for Index = 1, Count do
		local Kind = Event(Index).Kind
		if Kind == "wall" then Wall += 1 elseif Kind == "floor" then Floor += 1 end
	end
	local Params = RaycastParams.new()
	Params.IncludeTerrain = true
	Params.FilterType = Enum.RaycastFilterType.Include
	Params.FilterDescendantsInstances = {Space.Terrain}
	for _ = 1, 400 do
		if MatchGrid(Space, Space.RetainingWall, Wall) and MatchGrid(Space.Workshop, Space.Workshop.WorkshopBlocks, Floor) then
			local Surface = {}
			local Rays = buffer.create(100*5)
			local Hits = 0
			for Z = -8, 10, 2 do
				for X = -8, 10, 2 do
					local Hit = Space:Raycast(V(X,10,Z), V(0,-18,0), Params)
					if Hit then
						Hits += 1
						buffer.writeu8(Rays, #Surface*5, 1)
						buffer.writef32(Rays, #Surface*5+1, Hit.Position.Y)
					end
					table.insert(Surface, if Hit then tostring(math.round(Hit.Position.Y * 128)) else ".")
				end
			end
			if Hits > 0 then
				local Field = Space.Terrain:ReadRegion(V(-9,-5,-9), V(11,5,11))
				EmitBytes("field", buffer.tostring(Field))
				EmitBytes("rays", buffer.tostring(Rays))
				warn(string.format("[Spatial:YardSoak] replay start=%d wall=%d floor=%d digs=%d field=%s surface=%s hits=%d",
					Count, Wall, Floor, Count - Wall - Floor, Hash(buffer.tostring(Field)), Hash(table.concat(Surface,",")), Hits))
				return
			end
		end
		task.wait(.04)
	end
	error("authored Yard replay did not publish the expected two grids and terrain")
end

local function Run()
	assert(Store:GetCapabilities().Documents)
	local Read = Await(Store:GetAsync("YardV1"))
	local Revision, History = "0", {}
	if Read.Status == "Ok" then
		Validate(Read.Record.Value, #Read.Record.Value.Events)
		for Index, Entry in ipairs(Read.Record.Value.Events) do History[Index] = Entry end
		Revision = Read.Record.Revision
	else
		assert(Read.Status == "Missing")
	end
	local Start = #History
	assert(Start <= Total and Start % Batch == 0)
	assert(Await(Store:GetAsync("RecoveryScratch")).Status == "Missing")
	Replay(Start)
	local Last = math.min(Start + Batch, Total)
	for Index = Start + 1, Last do
		local Next = table.clone(History)
		table.insert(Next, Event(Index))
		local Value = {Version=1, Events=Next}
		local Submitted = os.clock()
		local First = Store:PutAsync("YardV1", Value, Revision)
		local Second = if Index % 8 == 0 then Store:PutAsync("YardV1", Value, Revision) else nil
		local Result = Await(First)
		local Elapsed = (os.clock() - Submitted) * 1000
		if Second then
			local Other = Await(Second)
			assert((Result.Status == "Ok" and Other.Status == "Conflict") or (Result.Status == "Conflict" and Other.Status == "Ok"))
			if Other.Status == "Ok" then Result = Other end
			Elapsed = (os.clock() - Submitted) * 1000
		end
		assert(Result.Status == "Ok" and Result.Revision ~= Revision)
		if Index % 4 == 0 then assert(Await(Store:PutAsync("YardV1", Value, Revision)).Status == "Conflict") end
		Revision = Result.Revision
		local Observed = Await(Store:GetAsync("YardV1"))
		assert(Observed.Status == "Ok" and Observed.Record.Revision == Revision)
		Validate(Observed.Record.Value, Index)
		History = Next
		warn(string.format("[Spatial:YardSoak] write index=%d revision=%s ms=%.3f race=%d stale=%d", Index, Revision, Elapsed,
			if Second then 1 else 0, if Index % 4 == 0 then 1 else 0))
	end
	if Start < Total then
		local Scratch = Await(Store:PutAsync("RecoveryScratch", {Batch=Last}, "0"))
		assert(Scratch.Status == "Ok")
		assert(Await(Store:DeleteAsync("RecoveryScratch", Revision)).Status == "Conflict")
		assert(Await(Store:DeleteAsync("RecoveryScratch", Scratch.Revision)).Status == "Ok")
		assert(Await(Store:GetAsync("RecoveryScratch")).Status == "Missing")
	end
	warn(string.format("[Spatial:YardSoak] complete start=%d end=%d revision=%s", Start, Last, Revision))
end
task.spawn(function()
	local Ok, Failure = pcall(Run)
	if not Ok then warn("[Spatial:YardSoak] failure "..tostring(Failure)) end
end)
