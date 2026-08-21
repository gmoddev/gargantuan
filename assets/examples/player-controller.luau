-- Productized player-runtime fixture. The engine-owned Luau modules provide
-- input mapping, character assembly, movement, and camera policy; this game
-- script only supplies world geometry and observes the public contracts.
local ActionMap = game:GetService("ActionMap")
local Players = game:GetService("Players")
local Workspace = game:GetService("Workspace")

local function MakePart(Name: string, Position: Vector3, Size: Vector3, Color: Color3)
	local Part = Instance.new("Part")
	Part.Name = Name
	Part.Anchored = true
	Part.Color = Color
	Part.Position = Position
	Part.Size = Size
	Part.Parent = Workspace
	return Part
end

MakePart("Baseplate", Vector3.new(0, -1, 0), Vector3.new(200, 2, 200), Color3.fromHSV(0, 0, 0.5))
MakePart("CollisionWall", Vector3.new(0, 3, -12), Vector3.new(16, 6, 2), Color3.fromHSV(1, 0.5, 1))
MakePart("ControllerStep", Vector3.new(5, 0.5, 0), Vector3.new(4, 1, 6), Color3.fromHSV(0.12, 0.6, 1))

local Slope = MakePart("ControllerSlope", Vector3.new(-7, 1, -4), Vector3.new(8, 2, 6), Color3.fromHSV(0.3, 0.5, 1))
Slope.Shape = Enum.PartType.Wedge

task.defer(function()
	local LocalPlayer = Players.LocalPlayer
	assert(LocalPlayer ~= nil, "[Player:Fixture] Players.LocalPlayer must exist during Play")
	assert(ActionMap:GetBindingCount() > 0, "[Player:Fixture] default semantic bindings must start automatically")

	local function ReportCharacter(Character)
		print("[Player:Fixture] Character ready", Character.Name)
	end

	if LocalPlayer.Character ~= nil then
		ReportCharacter(LocalPlayer.Character)
	else
		LocalPlayer.CharacterAdded:Connect(ReportCharacter)
	end
end)
