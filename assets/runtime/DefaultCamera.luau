return function(Players, ActionMap, UserInputService, Workspace, RunService)
	local LocalPlayer = Players.LocalPlayer
	if LocalPlayer == nil then
		error("[Player:Camera] Players.LocalPlayer is unavailable")
	end

	local Camera = Workspace.CurrentCamera
	local PreviousCameraType = Camera.CameraType
	local PreviousCFrame = Camera.CFrame
	local Distance = 12
	local FocusHeight = 1.5
	local Sensitivity = 0.2
	Camera.CameraType = Enum.CameraType.Scriptable
	Camera.Yaw = 0
	Camera.Pitch = 15

	local function ReleasePointer()
		UserInputService.MouseBehavior = Enum.MouseBehavior.Default
	end

	local BeganConnection = ActionMap.ActionBegan:Connect(function(ActionName)
		if ActionName == "CameraOrbit" then
			UserInputService.MouseBehavior = Enum.MouseBehavior.LockCenter
		end
	end)
	local EndedConnection = ActionMap.ActionEnded:Connect(function(ActionName)
		if ActionName == "CameraOrbit" then
			ReleasePointer()
		end
	end)
	local FocusConnection = UserInputService.WindowFocusReleased:Connect(ReleasePointer)

	local RenderConnection = RunService.PreRender:Connect(function()
		local Character = LocalPlayer.Character
		if Character == nil then
			return
		end
		if ActionMap:IsDown("CameraOrbit") then
			local Look = ActionMap:GetVector("Look")
			Camera.Yaw -= Look.X * Sensitivity
			Camera.Pitch = math.clamp(Camera.Pitch - Look.Y * Sensitivity, -85, 85)
		end

		local Yaw = math.rad(Camera.Yaw)
		local Pitch = math.rad(Camera.Pitch)
		local Horizontal = math.cos(Pitch) * Distance
		local Focus = Character.Position + Vector3.new(0, FocusHeight, 0)
		local Offset = Vector3.new(math.sin(Yaw) * Horizontal, math.sin(Pitch) * Distance, math.cos(Yaw) * Horizontal)
		Camera.CFrame = CFrame.new(Focus + Offset, Focus)
	end)

	return function()
		RenderConnection:Disconnect()
		FocusConnection:Disconnect()
		EndedConnection:Disconnect()
		BeganConnection:Disconnect()
		ReleasePointer()
		Camera.CameraType = PreviousCameraType
		Camera.CFrame = PreviousCFrame
	end
end
