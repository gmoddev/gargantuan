--!strict
-- A server-authored circuit. Grid traversal supplies connectivity. The game decides what powers a gate.
local Grid = Instance.new("BlockGrid")
Grid.Name = "PoweredGate"
Grid.Parent = game.Workspace

Grid:DefineBlock("project:wire", {Shape = "Slab", Ports = {"+X", "-X"}})
Grid:DefineBlock("project:supply", {Color = Color3.new(0.9, 0.65, 0.12), Ports = {"+X"}})
Grid:DefineBlock("project:gate", {Color = Color3.new(0.18, 0.48, 0.29), Ports = {"-X"}})
Grid:DefineChannel("powered", {Kind = "Bool", Default = false, Indexed = true, Audience = "RelevantPeers"})

local Gate = Vector3.new(3, 0, 0)
local Site = {Min = Vector3.zero, MaxExclusive = Vector3.new(4, 1, 1)}
local Install = Grid:BeginEdit()
Install:SetBlock(Vector3.zero, "project:supply")
Install:Fill(Vector3.new(1, 0, 0), Vector3.new(3, 1, 1), "project:wire")
Install:SetBlock(Gate, "project:gate")
Install:Commit()

local function RefreshGate(): boolean
	local Expected = Grid:GetRevision()
	local Search = Grid:FindConnected({Gate}, {
		Bounds = Site, UsePorts = true, StopAt = {"project:supply"}, MaxVisited = 4,
	})
	while true do
		local Page = Search:Step(32)
		if Page.Revision ~= Expected or Grid:GetRevision() ~= Expected then
			Search:Cancel()
			return false
		end
		if Page.Status == "Found" or Page.Status == "Complete" then
			local Update = Grid:BeginEdit(Expected)
			Update:SetValue(Gate, "powered", Page.Status == "Found")
			Update:Commit()
			return true
		end
		if Page.Status ~= "More" then return false end
	end
end

assert(RefreshGate() and Grid:GetValue(Gate, "powered") == true)
Grid:RemoveBlock(Vector3.new(2, 0, 0))
assert(RefreshGate() and Grid:GetValue(Gate, "powered") == false)
