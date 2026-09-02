return function(CharacterControl, Workspace)
	local WalkSpeed = 16
	local JumpSpeed = 50

	CharacterControl:SetMovementPolicy(function(Character, DeltaTime, MoveIntent, FacingYawRadians, JumpRequested)
		local Velocity = Character.Velocity
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
