-- Server Script in the spatial Development Dedicated Server project.
-- Use a stationary, authorized player focus near the origin.
local Grid = Instance.new("BlockGrid")
Grid.Name = "ReaderNetworkField"
Grid.Parent = workspace
local Near = Vector3.new(2, 0, 2)
local Air = Vector3.new(3, 0, 2)
local Far = Vector3.new(8192, 0, 2)
local Unit = Vector3.new(1, 1, 1)
Grid:SetBlock(Near, "Stone")
Grid:SetBlock(Far, "Brick")
assert(Grid:IsRegionAvailable(Near, Near + Unit) and Grid:GetBlock(Near).Definition == "Stone")
assert(Grid:IsRegionAvailable(Air, Air + Unit) and Grid:GetBlock(Air) == nil)
assert(Grid:IsRegionAvailable(Far, Far + Unit) and Grid:GetBlock(Far).Definition == "Brick")
print("[Reader:NetworkAuthority] PASS two occupied cells and verified air on the authority")
