local V = Vector3.new
local function Region(Name)
	local Chart = Instance.new("SpatialRegion")
	Chart.Name = Name
	Chart.Parent = game.Workspace
	Chart.Gravity = 0
	return Chart
end
local function Grid(World, Index)
	local G = Instance.new("BlockGrid")
	G.CFrame = CFrame.new(Index*3, 4, 0)
	G.Parent = World
	return G
end
local function Dynamic(G)
	G:SetMotionMode("Dynamic", G:GetRevision())
end

local First, Other = Region("FreightA"), Region("FreightB")
local EdgeGrid = Grid(First, 0)
local Axes = {V(1,0,0), V(-1,0,0), V(0,1,0), V(0,-1,0), V(0,0,1), V(0,0,-1)}
for _, Axis in Axes do EdgeGrid:SetBlock(Axis*64, "Brick") end
Dynamic(EdgeGrid)
assert(EdgeGrid:GetAssemblyState().Mass == 6)
for _, Axis in Axes do
	local Revision = EdgeGrid:GetRevision()
	local Ok, Reason = pcall(function() EdgeGrid:SetBlock(Axis*65, "Stone") end)
	assert(not Ok and string.find(tostring(Reason), "within 64 cells", 1, true))
	assert(EdgeGrid:GetRevision() == Revision and EdgeGrid:GetBlock(Axis*65) == nil)
	assert(EdgeGrid.MotionMode == "Dynamic" and EdgeGrid:GetBlock(Axis*64).Definition == "Brick")
end
EdgeGrid:SetMotionMode("Static", EdgeGrid:GetRevision())
for _, Axis in Axes do
	EdgeGrid:SetBlock(Axis*65, "Stone")
	local Revision = EdgeGrid:GetRevision()
	local Ok, Reason = pcall(Dynamic, EdgeGrid)
	assert(not Ok and string.find(tostring(Reason), "within 64 cells", 1, true))
	assert(EdgeGrid.MotionMode == "Static" and EdgeGrid:GetRevision() == Revision and
		EdgeGrid:GetBlock(Axis*65).Definition == "Stone")
	EdgeGrid:RemoveBlock(Axis*65)
end
EdgeGrid:Destroy()

local Active = {}
for I = 1, 64 do
	local G = Grid(First, I)
	G:SetBlock(Vector3.zero, "Stone")
	Dynamic(G)
	assert(G:GetAssemblyState().Mass == 1)
	Active[I] = G
end
local Waiting = Grid(First, 65)
Waiting:SetBlock(Vector3.zero, "Brick")
local Revision = Waiting:GetRevision()
local Ok, Reason = pcall(Dynamic, Waiting)
assert(not Ok and string.find(tostring(Reason), "World assembly limit", 1, true))
assert(Waiting.MotionMode == "Static" and Waiting:GetRevision() == Revision and
	Waiting:GetBlock(Vector3.zero).Definition == "Brick")
local Foreign = Grid(Other, 65)
Foreign:SetBlock(Vector3.zero, "Stone")
Dynamic(Foreign)
assert(Foreign.MotionMode == "Dynamic" and Foreign:GetAssemblyState().Mass == 1)
Active[17]:Destroy()
Dynamic(Waiting)
assert(Waiting:GetAssemblyState().Mass == 1 and Foreign:GetAssemblyState().Mass == 1)
First:Destroy()
Other:Destroy()
print("[Spatial:CompoundBounds] axesAccepted=6 liveExtentDenied=6 staticExtentDenied=6 chartA=64 chartB=1 worldExhausted=1 freedReused=1")
