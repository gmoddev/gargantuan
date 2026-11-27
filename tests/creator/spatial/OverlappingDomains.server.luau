-- Server Script. Use a clear sightline at this coordinate in both domains.
local Interior = Instance.new("SpatialRegion")
Interior.Name = "ReaderRayRoom"
Interior.Parent = workspace

local function PutFloor(Owner, Name, Definition)
    local Grid = Instance.new("BlockGrid")
    Grid.Name = Name
    Grid.CFrame = CFrame.new(80, 5, 20)
    Grid.Parent = Owner
    Grid:SetBlock(Vector3.new(0, 0, 0), Definition)
    return Grid
end

local Outside = PutFloor(workspace, "ReaderOutsideFloor", "Stone")
local Inside = PutFloor(Interior, "ReaderInsideFloor", "Brick")
local Cell = Vector3.new(0, 0, 0)
local Origin = Vector3.new(80.5, 8, 20.5)
local Direction = Vector3.new(0, -5, 0)
local Params = RaycastParams.new()
Params.IncludeBlockGrids = true

local function CheckHit(Owner, Grid, Definition)
    local Hit = assert(Owner:Raycast(Origin, Direction, Params), "missing domain hit")
    local GridHit = assert(Hit.GridHit, "expected a block-grid target")
    assert(Hit.Instance == Grid and GridHit.Cell == Cell and GridHit.Block.Definition == Definition)
end

CheckHit(workspace, Outside, "Stone")
CheckHit(Interior, Inside, "Brick")
Inside:RemoveBlock(Cell)
assert(Interior:Raycast(Origin, Direction, Params) == nil, "the empty interior leaked the outdoor hit")
CheckHit(workspace, Outside, "Stone")
print("[Reader:Domains] PASS overlapping local coordinates stayed isolated")
