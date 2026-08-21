return function(Players, ActionMap, Workspace, RunService)
	local LocalPlayer = Players.LocalPlayer
	if LocalPlayer == nil then
		error("[Player:Controller] Players.LocalPlayer is unavailable")
	end

	local JumpRequested = false
	local WalkSpeed = 16
	local JumpSpeed = 50
	local WalkableFloorY = 0.55
	local StepHeight = 1.25
	local StepForwardInset = 1
	local GroundProbeDistance = 0.2

	local function AssembleCharacter()
		local Character = Instance.new("KinematicCharacter")
		Character.Name = LocalPlayer.Name .. "Character"
		Character.Position = Vector3.new(0, 6, 0)
		Character.Parent = Workspace

		local RootPart = Instance.new("Part")
		RootPart.Name = "CharacterRoot"
		RootPart.Anchored = true
		RootPart.CanCollide = false
		RootPart.CanTouch = false
		RootPart.Size = Vector3.new(2, 4, 2)
		RootPart.Position = Character.Position
		RootPart.Parent = Character
		Character.RootPart = RootPart
		LocalPlayer.Character = Character
	end

	local SpawnConnection = LocalPlayer.CharacterSpawnRequested:Connect(AssembleCharacter)
	local ActionConnection = ActionMap.ActionBegan:Connect(function(ActionName)
		if ActionName == "Jump" then
			JumpRequested = true
		end
	end)

	if Players.CharacterAutoLoads then
		LocalPlayer:LoadCharacter()
	end

	local SimulationConnection = RunService.PreSimulation:Connect(function(DeltaTime)
		local Character = LocalPlayer.Character
		if Character == nil or DeltaTime <= 0 then
			JumpRequested = false
			return
		end

		local Camera = Workspace.CurrentCamera
		local Forward = Camera.CFrame.LookVector
		Forward = Vector3.new(Forward.X, 0, Forward.Z)
		if Forward.Magnitude == 0 then
			Forward = Vector3.new(0, 0, -1)
		else
			Forward = Forward.Unit
		end
		local Right = Camera.CFrame.RightVector
		Right = Vector3.new(Right.X, 0, Right.Z)
		if Right.Magnitude == 0 then
			Right = Vector3.xAxis
		else
			Right = Right.Unit
		end

		local ForwardValue = (if ActionMap:IsDown("MoveForward") then 1 else 0)
			- (if ActionMap:IsDown("MoveBackward") then 1 else 0)
		local RightValue = (if ActionMap:IsDown("MoveRight") then 1 else 0)
			- (if ActionMap:IsDown("MoveLeft") then 1 else 0)
		local Direction = Forward * ForwardValue + Right * RightValue
		if Direction.Magnitude > 1 then
			Direction = Direction.Unit
		end

		local WasGrounded = Character.Grounded
		local Velocity = Character.Velocity
		Velocity = Vector3.new(Direction.X * WalkSpeed, Velocity.Y - Workspace.Gravity * DeltaTime, Direction.Z * WalkSpeed)
		if JumpRequested and Character.Grounded then
			Velocity = Vector3.new(Velocity.X, JumpSpeed, Velocity.Z)
			Character.Grounded = false
		end
		JumpRequested = false

		local Translation = Velocity * DeltaTime
		local Motion = Workspace:MoveKinematicCapsule(
			Character.Position,
			Character.CapsuleRadius,
			Character.CapsuleHeight,
			Translation,
			Velocity
		)

		-- Step policy stays in Luau: the native boundary only supplies bounded
		-- capsule motion and contact data. At most three extra queries are made,
		-- and only when grounded horizontal motion loses tangential progress.
		local DesiredHorizontal = Vector3.new(Translation.X, 0, Translation.Z)
		local AppliedHorizontal = Vector3.new(Motion.AppliedTranslation.X, 0, Motion.AppliedTranslation.Z)
		if WasGrounded
			and DesiredHorizontal.Magnitude > 0
			and Motion.Collided
			and AppliedHorizontal.Magnitude + 0.01 < DesiredHorizontal.Magnitude
		then
			local StepUp = Workspace:MoveKinematicCapsule(
				Character.Position,
				Character.CapsuleRadius,
				Character.CapsuleHeight,
				Vector3.new(0, StepHeight, 0),
				Velocity
			)
			if StepUp.AppliedTranslation.Y >= StepHeight * 0.8 then
				local StepTranslation = DesiredHorizontal + DesiredHorizontal.Unit * StepForwardInset
				local StepForward = Workspace:MoveKinematicCapsule(
					StepUp.Position,
					Character.CapsuleRadius,
					Character.CapsuleHeight,
					StepTranslation,
					Velocity
				)
				local StepDown = Workspace:MoveKinematicCapsule(
					StepForward.Position,
					Character.CapsuleRadius,
					Character.CapsuleHeight,
					Vector3.new(0, -StepHeight - GroundProbeDistance, 0),
					StepForward.Velocity
				)
				local SteppedHorizontal = Vector3.new(
					StepDown.Position.X - Character.Position.X,
					0,
					StepDown.Position.Z - Character.Position.Z
				)
				if StepDown.HasFloor
					and StepDown.FloorNormal.Y >= WalkableFloorY
					and StepDown.Position.Y > Character.Position.Y + 0.1
					and SteppedHorizontal.Magnitude > AppliedHorizontal.Magnitude + 0.01
				then
					Motion = StepDown
				end
			end
		end
		Character.Position = Motion.Position
		Character.Velocity = Motion.Velocity
		Character.FloorNormal = Motion.FloorNormal
		Character.Grounded = Motion.HasFloor and Motion.FloorNormal.Y >= WalkableFloorY and Character.Velocity.Y <= 0
		if Character.Grounded and Character.Velocity.Y < 0 then
			Character.Velocity = Vector3.new(Character.Velocity.X, 0, Character.Velocity.Z)
		end

		local RootPart = Character.RootPart
		if RootPart ~= nil then
			RootPart.CFrame = CFrame.new(Character.Position)
		end
	end)

	return function()
		SimulationConnection:Disconnect()
		ActionConnection:Disconnect()
		SpawnConnection:Disconnect()
		LocalPlayer:RemoveCharacter()
	end
end
