-- RunContext: Server. Creates a point light attached to an anchored Part.
local Workspace = game:GetService("Workspace")
local Marker = Instance.new("Part")

local Ok, ErrorMessage = pcall(function()
	Marker.Name = "DocumentationLightMarker"
	Marker.Anchored = true
	Marker.CanCollide = false
	Marker.Transparency = 1
	Marker.Position = Vector3.new(0, 5, 0)

	local Light = Instance.new("PointLight")
	Light.LightColor = Color3.new(1, 0.8, 0.5)
	Light.LightIntensity = 32
	Light.LightRange = 20
	Light.LightCFrame = CFrame.new(0, 2, 0)
	Light.LightShadows = false
	Light.Parent = Marker
	Marker.Parent = Workspace
end)

if not Ok then
	pcall(function()
		Marker:Destroy()
	end)
	warn("[Docs:LocalLight] Could not create light:", ErrorMessage)
end
