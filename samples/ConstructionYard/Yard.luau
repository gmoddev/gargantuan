local Space = game:GetService("Workspace")
local Players = game:GetService("Players")
local Actions = game:GetService("ActionMap")
local Workshop = Space.Workshop
local Wall = Space.RetainingWall
local Floor = Workshop.WorkshopBlocks
local V = Vector3.new

local function PlacePart(Parent, Name, Position, Anchored, Color)
	local Part = Instance.new("Part")
	Part.Name = Name
	Part.Anchored = Anchored
	Part.Size = V(1, 1, 1)
	Part.Position = Position
	Part.Color = Color
	Part.Parent = Parent
	return Part
end

-- Both survey markers occupy the same coordinates in separate physics worlds.
PlacePart(Space, "OutdoorMarker", V(4, 3, 3), true, Color3.fromRGB(229, 154, 55))
PlacePart(Workshop, "WorkshopMarker", V(4, 3, 3), true, Color3.fromRGB(59, 142, 190))
PlacePart(Space, "LooseStone", V(5, 6, 1), false, Color3.fromRGB(108, 121, 134))

local function Character()
	local Player = Players.LocalPlayer or Players:GetPlayers()[1]
	return if Player then Player.Character else nil
end

local Width = 6
local BlocksAdded = {[Wall] = 0, [Floor] = 0}
local DigTicket = nil
local function UseTool(Name)
	local Avatar = Character()
	if not Avatar or not Avatar.RootPart then return end
	local Inside = Avatar.RootPart.SpatialRegion == Workshop
	if Name == "YardDoor" then
		local Destination = if Inside then Space else Workshop
		local Params = RaycastParams.new()
		Params.IncludeBlockGrids = true
		Params.IncludeTerrain = true
		local FloorHit = Destination:Raycast(V(0, 8, 2), V(0, -20, 0), Params)
		if not FloorHit then return end
		Avatar.RootPart.SpatialRegion = if Inside then nil else Workshop
		Avatar.CFrame = CFrame.new(0, FloorHit.Position.Y + 3, 2)
		Space.CurrentSpatialRegion = if Inside then nil else Workshop
	elseif Name == "YardBuild" then
		local Grid = if Inside then Floor else Wall
		local Count = BlocksAdded[Grid]
		if Count >= Width * 4 then return end
		local Cell = V(Count % Width - 2, 2 + math.floor(Count / Width), -5)
		Grid:SetBlock(Cell, "Brick")
		BlocksAdded[Grid] = Count + 1
	elseif Name == "YardDig" and not Inside then
		if DigTicket then
			local Receipt = Space.Terrain:GetEditStatus(DigTicket)
			if Receipt and Receipt.Status == "Pending" then return end
		end
		local Point = Avatar.Position + Avatar.CFrame.LookVector * 2.5 - V(0, 1.5, 0)
		DigTicket = Space.Terrain:RemoveBall(Point, 1.65)
	end
end

local Bindings = {
	Actions:BindKey("YardBuild", Enum.KeyCode.B, 1, 101, true),
	Actions:BindKey("YardDig", Enum.KeyCode.E, 1, 101, true),
	Actions:BindKey("YardDoor", Enum.KeyCode.F, 1, 101, true),
}
local Connection = Actions.ActionBegan:Connect(UseTool)
script.Destroying:Connect(function()
	Connection:Disconnect()
	for _, Binding in Bindings do Actions:Unbind(Binding) end
end)
print("[Spatial:ConstructionYard] B builds a course, E excavates ahead, F enters or leaves the workshop")
