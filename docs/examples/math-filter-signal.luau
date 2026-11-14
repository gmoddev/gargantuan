-- RunContext: Server. A VM-local filter and user signal; no geometry is required.
local Workspace = game:GetService("Workspace")
local Origin: Vector3 = Vector3.new(0, 5, 0)
local Direction: Vector3 = Vector3.new(0, -20, 0)
local Params = RaycastParams.new()
Params.FilterType = Enum.RaycastFilterType.Exclude
Params.FilterDescendantsInstances = {}

local QueryFinished = Signal.new()
local Connection = QueryFinished:Connect(function(HitName)
	print("[Docs:Query] Hit:", HitName)
end)

local Ok, Hit = pcall(function()
	return Workspace:Raycast(Origin, Direction, Params)
end)
if not Ok then
	warn("[Docs:Query] Raycast failed:", Hit)
elseif Hit then
	QueryFinished:Fire(Hit.Instance:GetFullName())
else
	QueryFinished:Fire("none")
end

Connection:Disconnect()
