local FirstPlatformX
local ObservedPlatform = false
local function ObserveMovingPlatform(World)
	local Grid = World:FindFirstChild("FreightPlatform")
	if not Grid or Grid.MotionMode ~= "Dynamic" then return false end
	local State = Grid:GetAssemblyState()
	local Cell = Vector3.new(0, 1, 0)
	local Tile = Grid:GetBlock(Cell)
	if not Tile or Tile.Definition ~= "Stone" or math.abs(State.Mass - 64) > .01 then return false end
	local Center = Grid:CellToWorld(Cell)
	local Origin = Center + State.CFrame.UpVector * 2
	local Direction = State.CFrame.UpVector * -4
	local Params = RaycastParams.new()
	Params.IncludeBlockGrids = true
	Params.FilterType = Enum.RaycastFilterType.Include
	Params.FilterDescendantsInstances = {Grid}
	local Local = Grid:Raycast(Origin, Direction)
	local Hit = World:Raycast(Origin, Direction, Params)
	if not Local or Local.Cell ~= Cell or not Hit or Hit.Instance ~= Grid or Hit.GridHit.Cell ~= Cell then return false end
	local X = State.CFrame.Position.X
	if FirstPlatformX == nil then FirstPlatformX = X end
	if math.abs(FirstPlatformX-X)<.25 then return false end
	if not ObservedPlatform then
		ObservedPlatform=true
		warn(string.format("[Spatial:FreightPlatform] firstX=%.4f nextX=%.4f mass=%.4f cell=(0,1,0)",FirstPlatformX,X,State.Mass))
	end
	return true
end
