-- RunContext: Server. Creates one anchored Part in the current runtime DataModel.
local Workspace = game:GetService("Workspace")
local Tags = game:GetService("Tags")

local Part = Instance.new("Part")
Part.Name = "DocumentationMarker"
Part.Archivable = true
Part.Anchored = true
Part.Size = Vector3.new(4, 1, 4)
Part.Position = Vector3.new(0, 3, 0)

local Ok, ErrorMessage = pcall(function()
	Part.Parent = Workspace
	Part:SetAttribute("Purpose", "Example")
	Tags:Add(Part, "Documentation")
end)

if not Ok then
	Part:Destroy()
	warn("[Docs:Instance] Could not create marker:", ErrorMessage)
else
	print("[Docs:Instance] Tagged marker count:", #Tags:GetTagged("Documentation"))
end
