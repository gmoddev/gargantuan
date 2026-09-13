return function(CharacterControl, Workspace)
	local WalkSpeed = 16
	local JumpSpeed = 50

	CharacterControl:SetMovementPolicy(function(Character, DeltaTime, MoveIntent, FacingYawRadians, JumpRequested)
		local Velocity = Character.Velocity
		-- A grounded contact must not carry accumulated downward speed into the
		-- next command. Retain a small gravity step for floor contact and retain
		-- positive jump velocity until the Character actually leaves the floor.
		if Character.Grounded and Velocity.Y < 0 then
			Velocity = Vector3.new(Velocity.X, 0, Velocity.Z)
		end
		Velocity = Vector3.new(
			MoveIntent.X * WalkSpeed,
			Velocity.Y - Workspace.Gravity * DeltaTime,
			MoveIntent.Y * WalkSpeed
		)
		if JumpRequested and Character.Grounded then
			Velocity = Vector3.new(Velocity.X, JumpSpeed, Velocity.Z)
		end
		return Velocity * DeltaTime, Velocity, FacingYawRadians
	end)
end
