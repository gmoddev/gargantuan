-- Low-level regression fixture for games that disable Players defaults and own
-- controller policy directly through UserInputService, RunService, and Camera.
local RunService = game:GetService("RunService")
local UserInputService = game:GetService("UserInputService")
local Workspace = game:GetService("Workspace")

local Baseplate = Instance.new("Part")
Baseplate.Name = "Baseplate"
Baseplate.Anchored = true
Baseplate.Color = Color3.fromHSV(0, 0, 0.5)
Baseplate.Position = Vector3.new(0, -10, 0)
Baseplate.Size = Vector3.new(2000, 20, 2000)
Baseplate.Parent = Workspace

local RootPart = Instance.new("Part")
RootPart.Name = "CustomControllerRoot"
RootPart.Anchored = true
RootPart.Size = Vector3.new(2, 4, 1)
RootPart.Position = Vector3.new(0, 2, 0)
RootPart.Parent = Workspace

Workspace.CurrentCamera.CameraType = Enum.CameraType.Scriptable
Workspace.CurrentCamera.CFrame = CFrame.new(
	(RootPart.CFrame * CFrame.new(0, 20, 0)).Position,
	RootPart.Position
)

RunService.PreRender:Connect(function(DeltaTime: number)
	local WalkDirection = Vector3.zero
	if UserInputService:IsKeyDown(Enum.KeyCode.W) then
		WalkDirection += Vector3.new(0, 0, 0.5)
	end
	if UserInputService:IsKeyDown(Enum.KeyCode.S) then
		WalkDirection -= Vector3.new(0, 0, 0.5)
	end
	if UserInputService:IsKeyDown(Enum.KeyCode.A) then
		WalkDirection += Vector3.new(0.5, 0, 0)
	end
	if UserInputService:IsKeyDown(Enum.KeyCode.D) then
		WalkDirection -= Vector3.new(0.5, 0, 0)
	end

	WalkDirection *= 16 * DeltaTime
	RootPart.CFrame = CFrame.new(RootPart.Position + WalkDirection, RootPart.Position)
	local LookCFrame = CFrame.new(
		(RootPart.CFrame * CFrame.new(0, 20, 0)).Position,
		RootPart.Position
	)
	Workspace.CurrentCamera.CFrame = Workspace.CurrentCamera.CFrame:Lerp(LookCFrame, DeltaTime * 2)
end)
