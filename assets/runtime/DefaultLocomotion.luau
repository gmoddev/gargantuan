return function(LocalPlayer, ActionMap, Workspace, RunService)
	local JumpRequested = false
	local WalkSpeed = 16
	local JumpSpeed = 50
	local WalkableFloorY = 0.55
	local StepHeight = 1.25
	local StepForwardInset = 1
	local GroundProbeDistance = 0.2

	local ActionConnection = ActionMap.ActionBegan:Connect(function(ActionName)
		if ActionName == "Jump" then
			JumpRequested = true
		end
	end)

	local SimulationConnection = RunService.PreSimulation:Connect(function(DeltaTime)
		local Character = LocalPlayer.Character
		if Character == nil or not Character:IsA("KinematicCharacter") or DeltaTime <= 0 then
			JumpRequested = false
			return
		end
		DeltaTime = math.min(DeltaTime, 0.1)

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
		local Motion = Character:Move(Translation, Velocity)

		-- Preview-only capsule queries keep step selection in Luau. Accepted
		-- segments still commit through Character:Move, so Character remains the
		-- sole transform owner and the native primitive remains game-neutral.
		local DesiredHorizontal = Vector3.new(Translation.X, 0, Translation.Z)
		local AppliedHorizontal = Vector3.new(Motion.AppliedTranslation.X, 0, Motion.AppliedTranslation.Z)
		if WasGrounded
			and DesiredHorizontal.Magnitude > 0
			and Motion.Collided
			and AppliedHorizontal.Magnitude + 0.01 < DesiredHorizontal.Magnitude
		then
			local StepOrigin = Character.Position
			local StepUp = Workspace:MoveKinematicCapsule(
				StepOrigin,
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
					StepDown.Position.X - StepOrigin.X,
					0,
					StepDown.Position.Z - StepOrigin.Z
				)
				if StepDown.HasFloor
					and StepDown.FloorNormal.Y >= WalkableFloorY
					and StepDown.Position.Y > StepOrigin.Y + 0.1
					and SteppedHorizontal.Magnitude > 0.01
				then
					Character:Move(StepUp.AppliedTranslation, Velocity)
					Character:Move(StepForward.AppliedTranslation, StepForward.Velocity)
					Motion = Character:Move(StepDown.AppliedTranslation, StepDown.Velocity)
				end
			end
		end

		Character.Velocity = Motion.Velocity
		Character.FloorNormal = Motion.FloorNormal
		Character.Grounded = Motion.HasFloor and Motion.FloorNormal.Y >= WalkableFloorY and Character.Velocity.Y <= 0
		if Character.Grounded and Character.Velocity.Y < 0 then
			Character.Velocity = Vector3.new(Character.Velocity.X, 0, Character.Velocity.Z)
		end
	end)

	return function()
		SimulationConnection:Disconnect()
		ActionConnection:Disconnect()
	end
end
