-- RunContext: Server. Query the live Workspace after collidable Parts are present.
local Workspace = game:GetService("Workspace")

local Ok, Result = pcall(function()
	return Workspace:GetPartsInSphere(Vector3.new(0, 3, 0), 8, nil, 32)
end)

if not Ok then
	warn("[Docs:World] Query failed:", Result)
else
	for _, Part in Result.Parts do
		print("[Docs:World] Nearby part:", Part:GetFullName())
	end
	if Result.Truncated then
		warn("[Docs:World] More than 32 parts overlap; result is incomplete")
	end
end
