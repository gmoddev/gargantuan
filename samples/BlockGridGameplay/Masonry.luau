-- Optional server ModuleScript. Each construction opts into persistent, indexed wear once.
local Masonry = {}

function Masonry.DefineWear(Grid)
	Grid:DefineChannel("wear", {Kind = "UInt8", Default = 0, Indexed = true, Persistent = true,
		Audience = "RelevantPeers", OnCopy = "Default"})
end

function Masonry.Damage(Grid, Cell, Amount)
	assert(Amount >= 1 and Amount <= 100 and Amount % 1 == 0, "Damage must be an integer from 1 to 100")
	local Revision = Grid:GetRevision()
	if Grid:GetBlock(Cell) == nil then return false end
	local Total = Grid:GetValue(Cell, "wear") + Amount
	local Edit = Grid:BeginEdit(Revision)
	if Total >= 100 then
		Edit:RemoveBlock(Cell)
	else
		Edit:SetValue(Cell, "wear", Total)
	end
	Edit:Commit()
	return true
end

function Masonry.Repair(Grid, Min, MaxExclusive)
	local Expected = Grid:GetRevision()
	local Scan = Grid:FindActive("wear", {Min = Min, MaxExclusive = MaxExclusive})
	local Repaired = 0
	while true do
		if Grid:GetRevision() ~= Expected then
			Scan:Cancel()
			return Repaired, "Changed"
		end
		local Page = Scan:Step(32)
		if Page.Status ~= "More" and Page.Status ~= "Complete" then return Repaired, Page.Status end
		if #Page.Cells > 0 then
			local Edit = Grid:BeginEdit(Expected)
			for _, Cell in Page.Cells do Edit:SetValue(Cell, "wear", 0) end
			Expected = Edit:Commit()
			Repaired += #Page.Cells
		end
		if Page.Status == "Complete" then return Repaired, Page.Status end
		task.wait()
	end
end

return Masonry
