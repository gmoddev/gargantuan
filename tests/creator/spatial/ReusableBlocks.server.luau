-- Server Script. The grids are displaced so they can also be inspected in Play.
local function NewGrid(Name, X)
    local Grid = Instance.new("BlockGrid")
    Grid.Name = Name
    Grid.CFrame = CFrame.new(X, 0, 0)
    Grid.Parent = workspace
    return Grid
end

local Source = NewGrid("ReaderStepAssembly", 48)
local Destination = NewGrid("ReaderStepCopy", 56)
local Key = "project:keystone"
local Spec = { Shape = "Stairs", Color = Color3.fromRGB(175, 122, 62), Ports = { "+X", "-X" } }
local Rotation = { Up = "+Y", Front = "+X" }
for _, Grid in { Source, Destination } do
    Grid:DefineBlock(Key, Spec)
    Grid:DefineChannel("wear", { Kind = "UInt8", Default = 0, OnCopy = "Value" })
    Grid:DefineChannel("armed", { Kind = "Bool", Default = false, OnCopy = "Default" })
end
assert(Source:GetDefinitionDigest(Key) == Destination:GetDefinitionDigest(Key), "different shared definitions")

local Start, Unit = Vector3.new(0, 0, 0), Vector3.new(1, 1, 1)
local CopyCell = Vector3.new(2, 0, 0)
local Edit = Source:BeginEdit(Source:GetRevision())
Edit:SetBlock(Start, Key, Rotation)
Edit:SetValue(Start, "wear", 7)
Edit:SetValue(Start, "armed", true)
Edit:Commit()
assert(Source:GetValue(Start, "wear") == 7 and Source:GetValue(Start, "armed") == true)

local SameGrid = Source:BeginEdit(Source:GetRevision())
SameGrid:CopyRegion(Start, Unit, CopyCell)
SameGrid:Commit()
local Copy = assert(Source:GetBlock(CopyCell))
assert(Copy.Definition == Key and Copy.Orientation.Front == "+X" and Copy.Orientation.Up == "+Y")
assert(Source:GetValue(CopyCell, "wear") == 7 and Source:GetValue(CopyCell, "armed") == false)

local Image = Source:ReadPacked(Start, Unit)
local Paste = Destination:BeginEdit(Destination:GetRevision())
Paste:WritePacked(Start, Unit, Image)
Paste:Commit()
local Pasted = assert(Destination:GetBlock(Start))
assert(Pasted.Definition == Key and Pasted.Orientation.Front == "+X" and Pasted.Orientation.Up == "+Y")
assert(Destination:GetValue(Start, "wear") == 0, "a packed image unexpectedly copied per-cell state")

local BadCell = Vector3.new(3, 0, 0)
local Invalid = Source:BeginEdit()
local Accepted = pcall(function()
    Invalid:SetBlock(BadCell, Key)
    Invalid:SetValue(BadCell, "wear", 256)
    Invalid:Commit()
end)
assert(not Accepted and Source:GetBlock(BadCell) == nil, "out-of-range UInt8 did not reject the edit atomically")
pcall(function() Invalid:Cancel() end)

local Pending = Source:BeginEdit(Source:GetRevision())
Pending:SetBlock(BadCell, Key, Rotation)
Source:SetBlock(Vector3.new(4, 0, 0), "Brick")
local Committed = pcall(function() Pending:Commit() end)
assert(not Committed and Source:GetBlock(BadCell) == nil, "a stale edit published a staged cell")
pcall(function() Pending:Cancel() end)
print("[Reader:Reuse] PASS oriented steps, typed transaction, native copy and packed cross-grid reuse")
