local function ReadSpeed(Character, Name, Default)
	local Speed = Character:GetAttribute(Name)
	if type(Speed) == "number" and Speed >= 0 and Speed <= 100 then
		return Speed
	end
	return Default
end

return function(CharacterControl, Workspace, UseMovementAttributes)
	CharacterControl:SetMovementPolicy(function(Character, DeltaTime, MoveIntent, FacingYawRadians, JumpRequested)
		local Velocity = Character.Velocity
		local Speed = 16
		if UseMovementAttributes then
			Speed = if Character:GetAttribute("Sprinting") == true then ReadSpeed(Character, "RunSpeed", 24)
				else ReadSpeed(Character, "WalkSpeed", 16)
		end
		-- A grounded contact must not carry accumulated downward speed into the
		-- next command. Retain a small gravity step for floor contact and retain
		-- positive jump velocity until the Character actually leaves the floor.
		if Character.Grounded and Velocity.Y < 0 then
			Velocity = Vector3.new(Velocity.X, 0, Velocity.Z)
		end
		Velocity = Vector3.new(
			MoveIntent.X * Speed,
			Velocity.Y - Workspace.Gravity * DeltaTime,
			MoveIntent.Y * Speed
		)
		if JumpRequested and Character.Grounded then
			local JumpSpeed = if UseMovementAttributes then ReadSpeed(Character, "JumpSpeed", 50) else 50
			Velocity = Vector3.new(Velocity.X, JumpSpeed, Velocity.Z)
		end
		return Velocity * DeltaTime, Velocity, FacingYawRadians
	end)
end
