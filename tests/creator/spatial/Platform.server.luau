-- Server Script in an empty Play world.
local Grid = Instance.new("BlockGrid")
Grid.Name = "ReaderPlatform25"
Grid.Parent = workspace

local Minimum = Vector3.new(-2, 0, -2)
local Upper = Vector3.new(3, 1, 3)
Grid:Fill(Minimum, Upper, "Stone")
assert(#Grid:ReadRegion(Minimum, Upper).Cells == 25, "expected a five by five floor")
assert(Grid.Visible and Grid.CanCollide, "the default grid should participate")

local Expected = Vector3.new(-1, 0, 1)
local Hit = assert(Grid:Raycast(Vector3.new(-0.5, 6, 1.5), Vector3.new(0, -8, 0)), "expected a grid hit")
assert(Hit.Cell == Expected and Hit.Block.Definition == "Stone", "wrong cell or definition")
local Edit = Grid:BeginEdit(Hit.Revision)
Edit:RemoveBlock(Hit.Cell)
Edit:Commit()
assert(Grid:GetBlock(Expected) == nil, "the picked cell remained")
assert(Grid:GetBlock(Vector3.new(-2, 0, 1)) ~= nil, "an adjacent cell was lost")
assert(#Grid:ReadRegion(Minimum, Upper).Cells == 24, "expected exactly one removal")
print("[Reader:Platform] PASS placed 25, picked (-1,0,1), and removed exactly one")
