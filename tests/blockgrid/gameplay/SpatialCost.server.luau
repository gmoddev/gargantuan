-- The runner prepends Profile. Each run owns a fresh server Script and grid.
local Grid = Instance.new("BlockGrid")
Grid.CFrame = CFrame.new(64, 0, 0)
Grid.Parent = workspace
local Bounds = {Min = Vector3.new(0, 0, 0), MaxExclusive = Vector3.new(16, 1, 16)}
local Shapes = {"Slab", "Wedge", "Stairs", "Cube"}
local Keys = {}
for I, Shape in Shapes do
    Keys[I] = "project:" .. string.lower(Shape)
    Grid:DefineBlock(Keys[I], {Shape = Shape, Color = Color3.fromRGB(45 * I, 170 - 20 * I, 150)})
end
local Scan = Profile == "scan" or Profile == "indexed"
if Scan or Profile == "empty-channel" then
    Grid:DefineChannel("wear", {Kind = "UInt8", Default = 0, Indexed = true})
end
local Cells = {}
local Positions = Grid:BeginEdit()
for I = 0, 255 do
    local Cell = Vector3.new(I % 16, 0, math.floor(I / 16))
    Cells[I + 1] = Cell
    Positions:SetBlock(Cell, Keys[1 + I % 4])
    if Scan and I % 8 == 0 then Positions:SetValue(Cell, "wear", 1) end
end
Positions:Commit()

local function Edit(Round)
    local Individual = Profile == "individual"
    local Batch = if Individual then nil else Grid:BeginEdit()
    local Destination = Batch or Grid
    local Offset = ((Round - 1) % 8) * 32
    local Pass = math.floor((Round - 1) / 8)
    for I = 0, 31 do
        Destination:SetBlock(Cells[1 + Offset + I], Keys[1 + (I + Pass + 1) % 4])
    end
    if Batch then Batch:Commit() end
end

local function Enumerate()
    local Count, Sum = 0, 0
    if Profile == "scan" then
        for I, Cell in Cells do
            if Grid:GetValue(Cell, "wear") ~= 0 then
                Count += 1
                Sum += I - 1
            end
        end
    else
        local Cursor = Grid:FindActive("wear", Bounds)
        while true do
            local Page = Cursor:Step(32)
            for _, Cell in Page.Cells do
                Count += 1
                Sum += Cell.X + 16 * Cell.Z
            end
            if Page.Status == "Complete" then break end
            assert(Page.Status == "More", Page.Status)
        end
    end
    return Count, Sum
end

local Times = {}
for Round = 1, 136 do
    local Start = os.clock()
    local Count, Sum
    if Scan then Count, Sum = Enumerate() else Edit(Round) end
    local Elapsed = (os.clock() - Start) * 1000000
    if Scan then assert(Count == 32 and Sum == 3968) end
    if Round > 8 then table.insert(Times, Elapsed) end
    task.wait()
end
if not Scan then
    for I, Cell in Cells do assert(Grid:GetBlock(Cell).Definition == Keys[1 + (I - 1 + 17) % 4]) end
end
table.sort(Times)
print(string.format("[BlockGrid:ScriptCost] %s %d %.3f %.3f %.3f %.3f",
    Profile, #Times, Times[64], Times[122], Times[127], Times[#Times]))
