return function(LocalPlayer, ActionMap, Workspace, RunService, CharacterControl)
	local JumpRequested = false
	local ActionConnection = ActionMap.ActionBegan:Connect(function(ActionName)
		if ActionName == "Jump" then
			JumpRequested = true
		end
	end)

	local SimulationConnection = RunService.PreSimulation:Connect(function()
		local Character = LocalPlayer.Character
		if Character == nil or not Character:IsA("KinematicCharacter") then
			JumpRequested = false
			return
		end
		local Camera = Workspace.CurrentCamera
		local Forward = Camera.CFrame.LookVector
		Forward = Vector3.new(Forward.X, 0, Forward.Z)
		Forward = if Forward.Magnitude == 0 then Vector3.new(0, 0, -1) else Forward.Unit
		local Right = Camera.CFrame.RightVector
		Right = Vector3.new(Right.X, 0, Right.Z)
		Right = if Right.Magnitude == 0 then Vector3.xAxis else Right.Unit

		local ForwardValue = (if ActionMap:IsDown("MoveForward") then 1 else 0)
			- (if ActionMap:IsDown("MoveBackward") then 1 else 0)
		local RightValue = (if ActionMap:IsDown("MoveRight") then 1 else 0)
			- (if ActionMap:IsDown("MoveLeft") then 1 else 0)
		local Direction = Forward * ForwardValue + Right * RightValue
		if Direction.Magnitude > 1 then
			Direction = Direction.Unit
		end
		CharacterControl:SubmitMoveIntent(Vector2.new(Direction.X, Direction.Z), 0, JumpRequested)
		JumpRequested = false
	end)

	return function()
		SimulationConnection:Disconnect()
		ActionConnection:Disconnect()
	end
end
