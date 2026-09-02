return function(Player, Workspace)
	local function AssembleCharacter()
		local Character = Instance.new("KinematicCharacter")
		Character.Name = Player.Name .. "Character"
		Character.CFrame = CFrame.new(0, 6, 0)
		Character.Parent = Workspace

		local RootPart = Instance.new("Part")
		RootPart.Name = "CharacterRoot"
		RootPart.Anchored = true
		RootPart.CanCollide = false
		RootPart.CanTouch = false
		RootPart.Size = Vector3.new(2, 4, 2)
		RootPart.CFrame = Character.CFrame
		RootPart.Parent = Character
		Character.RootPart = RootPart
		Player.Character = Character
	end

	local SpawnConnection = Player.CharacterSpawnRequested:Connect(AssembleCharacter)
	return function()
		SpawnConnection:Disconnect()
		if not Player.Destroyed then
			Player:RemoveCharacter()
		end
	end
end
