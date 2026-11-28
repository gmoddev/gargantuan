-- Server Script in Workspace, offline Play or an authoritative spatial session.
-- Run once in a fresh world. The terrain field must have editing enabled.
local World = game:GetService("Workspace")
local RandomSource = Random.new(271828)
local Grid = Instance.new("BlockGrid")
Grid.Name = "GeneratedFoundation"
Grid.Parent = World

-- Integer cells, exclusive upper bound. One native bulk mutation.
Grid:Fill(Vector3.new(-8, 0, -8), Vector3.new(8, 1, 8), "Stone")
for Index = 1, 8 do
	local X = RandomSource:NextInteger(-7, 7)
	local Z = RandomSource:NextInteger(-7, 7)
	Grid:Fill(Vector3.new(X, 1, Z), Vector3.new(X + 1, 4, Z + 1), "Brick")
end

local Terrain = World.Terrain
local Info = Terrain:GetGridInfo()
if Info.EditingAvailable then
	-- Select the numeric material identity used by this world's terrain palette.
	local Ticket = Terrain:FillBall(Vector3.new(24, -3, 0), 8, 1)
	print("[Generation:Terrain] Submitted", Ticket)
end
