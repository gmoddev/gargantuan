-- Place beside a ModuleScript named Masonry. Run as a server Script.
local Masonry = require("./Masonry")
local Grid = Instance.new("BlockGrid")
Grid.Name = "MasonryWorkshop"
Grid.Parent = workspace
Masonry.DefineWear(Grid)

local Min = Vector3.new(-5, 0, -4)
local Max = Vector3.new(6, 5, 5)
local LeftDoorpost = Vector3.new(-1, 1, -4)
local RightDoorpost = Vector3.new(1, 1, -4)
local Lintel = Vector3.new(0, 4, -4)
local Changes = 0
local Subscription = Grid:GetPropertyChangedSignal("BlockRevision"):Connect(function()
	Changes += 1
end)
local Build = Grid:BeginEdit()
Build:Fill(Min, Max, "Brick")
Build:Clear(Min + Vector3.one, Max - Vector3.one)
Build:Clear(Vector3.new(0, 1, -4), Vector3.new(1, 4, -3))
Build:Commit()
assert(Changes == 1 and Grid:GetBlock(Vector3.new(0, 2, -4)) == nil)
Subscription:Disconnect()

assert(Masonry.Damage(Grid, LeftDoorpost, 70))
assert(Masonry.Damage(Grid, RightDoorpost, 50))
assert(Masonry.Damage(Grid, LeftDoorpost, 30) and Grid:GetBlock(LeftDoorpost) == nil)
assert(not Masonry.Damage(Grid, LeftDoorpost, 1))
assert(Grid:GetValue(Lintel, "wear") == 0)
local Count, Status = Masonry.Repair(Grid, Min, Max)
assert(Count == 1 and Status == "Complete" and Grid:GetValue(RightDoorpost, "wear") == 0)
assert(Masonry.Repair(Grid, Min, Max) == 0)

-- A larger repair spans scheduler ticks. Existing repairs stay committed if another builder changes the grid.
local Weather = Grid:BeginEdit()
for X = -5, 5 do
	for Z = -4, 4 do Weather:SetValue(Vector3.new(X, 4, Z), "wear", 5) end
end
Weather:Commit()
local RoofCount, RoofStatus = Masonry.Repair(Grid, Min, Max)
assert(RoofCount == 99 and RoofStatus == "Complete")
print("[BlockGrid:Masonry] Built the workshop in one edit, removed a failed post and repaired its neighbor")
