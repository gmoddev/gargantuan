local V = Vector3.new
local Room = Instance.new("SpatialRegion")
Room.Parent = game.Workspace
Room.Gravity = 32
local function Steps(Count)
	for _ = 1, Count do task.wait() end
end
local function Deck(X, Y, Side)
	local G = Instance.new("BlockGrid")
	G.CellSize = .5
	G.CFrame = CFrame.new(X, Y, 0)
	G.Parent = Room
	G:Fill(V(-Side/2,0,-Side/2), V(Side/2,2,Side/2), "Stone")
	G:SetMotionMode("Dynamic", G:GetRevision())
	return G
end
local Terrain = Room.Terrain
local Ticket = Terrain:FillBlock(V(-24,-2,0), V(14,4,14), 3)
print("[Spatial:ContactStage] waiting for terrain")
for _ = 1, 240 do
	local Record = Terrain:GetEditStatus(Ticket)
	if Record.Status == "Committed" then assert(Record.CollisionReady) break end
	assert(Record.Status == "Pending",Record.Status)
	Steps(1)
end
local TerrainQuery = RaycastParams.new()
TerrainQuery.IncludeTerrain = true
TerrainQuery.FilterType = Enum.RaycastFilterType.Include
TerrainQuery.FilterDescendantsInstances = {Terrain}
local Surface = Room:Raycast(V(-24,8,0),V(0,-16,0),TerrainQuery)
assert(Surface and Surface.Instance==Terrain,"missing committed terrain surface")
local Fixed = Instance.new("BlockGrid")
Fixed.Parent = Room
Fixed:Fill(V(-8,-1,-8),V(8,0,8),"Brick")
local Footing = Instance.new("Part")
Footing.Size = V(14,1,14)
Footing.Anchored = true
Footing.Position = V(24,-.5,0)
Footing.Parent = Room
local OnTerrain, OnGrid = Deck(-24,4,Side), Deck(0,4,Side)
local Lower, Upper = Deck(24,2,Side), Deck(24,5,Side)
print("[Spatial:ContactStage] settling four bodies")
Steps(130)
local TerrainY=OnTerrain.CFrame.Position.Y
local GridY=OnGrid.CFrame.Position.Y
local LowerY, UpperY=Lower.CFrame.Position.Y,Upper.CFrame.Position.Y
assert(math.abs(TerrainY-Surface.Position.Y)<.16,"moving deck missed terrain")
assert(math.abs(GridY)<.16,"moving deck missed static grid")
assert(math.abs(LowerY)<.16 and math.abs(UpperY-LowerY-1)<.16,"two dynamic decks failed to stack")
local Avatar = Instance.new("KinematicCharacter")
Avatar.Parent = Room
local Root = Instance.new("Part")
Root.Anchored = true
Root.CanCollide = false
Root.Parent = Avatar
Avatar.RootPart = Root
Avatar.Position = V(0,8,0)
local Move = Avatar:Move(V(0,-12,0),V(0,-4,0))
assert(Move.HasFloor and math.abs(Avatar.Position.Y-(GridY+1+Avatar.CapsuleHeight*.5))<.12,
	"character did not land on the actual moving compound")
local Before = OnGrid:GetRevision()
local Other=Instance.new("SpatialRegion")
Other.Name="OtherDock"
Other.Parent=game.Workspace
assert(not pcall(function() OnGrid.SpatialRegion=Other end) and OnGrid:GetRevision()==Before)
assert(not pcall(function() OnGrid.Parent=Other end) and OnGrid.Parent==Room and OnGrid:GetRevision()==Before)
Lower:Destroy()
print("[Spatial:ContactStage] removed lower deck")
Steps(90)
assert(math.abs(Upper.CFrame.Position.Y)<.16,"upper deck retained a removed contact")
local Params=RaycastParams.new()
Params.IncludeBlockGrids=true
Params.FilterType=Enum.RaycastFilterType.Include
Params.FilterDescendantsInstances={OnGrid}
local Start=V(.25,7,.25)
local LocalOnly=RaycastParams.new()
LocalOnly.IncludeBlockGrids=true
local ShortStart, ShortRay=V(.25,1.75,.25),V(0,-1.2,0)
assert(Room:Raycast(Start,V(0,-10,0),Params).Instance==OnGrid and Other:Raycast(ShortStart,ShortRay,LocalOnly)==nil)
assert(not pcall(function() Other:Raycast(Start,V(0,-10,0),Params) end))
OnGrid:SetMotionMode("Static",OnGrid:GetRevision())
OnGrid.Parent=Other
OnGrid:SetMotionMode("Dynamic",OnGrid:GetRevision())
assert(Other:Raycast(Start,V(0,-10,0),Params).Instance==OnGrid and Room:Raycast(ShortStart,ShortRay,LocalOnly)==nil)
assert(not pcall(function() Room:Raycast(Start,V(0,-10,0),Params) end))
OnGrid:SetAssemblyVelocity(V(2,.5,-1),V(.25,1,-.5),OnGrid:GetRevision())
local Whole=OnGrid:GetAssemblyState()
local SplitMin,SplitMax=V(0,0,-Side/2),V(Side/2,2,Side/2)
local Piece=OnGrid:SplitRegion(SplitMin,SplitMax,OnGrid:GetRevision())
assert(Piece.Parent==Other and Piece.SpatialRegion==OnGrid.SpatialRegion and OnGrid:GetBlock(SplitMin)==nil)
assert(Piece:GetBlock(Vector3.zero).Definition=="Stone")
local SplitError=0
for _,G in {OnGrid,Piece} do
	local S=G:GetAssemblyState()
	assert(S.Mode=="Dynamic" and math.abs(S.Mass*2-Whole.Mass)<.01)
	assert((S.AngularVelocity-Whole.AngularVelocity).Magnitude<.002)
	local Expected=Whole.LinearVelocity+Whole.AngularVelocity:Cross(S.CenterOfMass-Whole.CenterOfMass)
	SplitError=math.max(SplitError,(S.LinearVelocity-Expected).Magnitude)
end
assert(SplitError<.002)
local A,B=OnGrid:GetRevision(),Piece:GetRevision()
assert(not pcall(function() OnGrid:MergeFrom(Piece,A,A) end) and OnGrid:GetRevision()==A and Piece:GetRevision()==B)
OnGrid:MergeFrom(Piece,A,B)
local Joined=OnGrid:GetAssemblyState()
local JoinError=(Whole.LinearVelocity-Joined.LinearVelocity).Magnitude+(Whole.AngularVelocity-Joined.AngularVelocity).Magnitude
assert(JoinError<.002 and math.abs(Joined.Mass-Whole.Mass)<.01 and (Joined.CenterOfMass-Whole.CenterOfMass).Magnitude<.002)
assert(OnGrid:GetBlock(SplitMin).Definition=="Stone" and Piece.MotionMode=="Static" and Piece:GetBlock(Vector3.zero)==nil)
print(string.format("[Spatial:CompoundContacts] cells=%d terrain=%.4f surface=%.4f grid=%.4f lower=%.4f upper=%.4f after=%.4f character=%.4f splitHalfMass=%.4f splitError=%.7f joinError=%.7f",
	Side*Side*2,TerrainY,Surface.Position.Y,GridY,LowerY,UpperY,Upper.CFrame.Position.Y,Avatar.Position.Y,Joined.Mass*.5,SplitError,JoinError))
Room:Destroy()
Other:Destroy()
