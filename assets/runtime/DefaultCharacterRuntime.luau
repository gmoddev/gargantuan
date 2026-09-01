return function(Players, ActionMap, Workspace, RunService)
	local LocalPlayer = Players.LocalPlayer
	if LocalPlayer == nil then
		error("[Character:Runtime] Players.LocalPlayer is unavailable")
	end

	local function AssembleCharacter()
		local Character = Instance.new("KinematicCharacter")
		Character.Name = LocalPlayer.Name .. "Character"
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
		LocalPlayer.Character = Character
	end

	local SpawnConnection = LocalPlayer.CharacterSpawnRequested:Connect(AssembleCharacter)
	local LocomotionCleanup = require("@game/Players/PlayerRuntimeModules/DefaultLocomotion")(
		LocalPlayer,
		ActionMap,
		Workspace,
		RunService
	)

	if Players.CharacterAutoLoads then
		LocalPlayer:LoadCharacter()
	end

	return function()
		LocomotionCleanup()
		SpawnConnection:Disconnect()
		LocalPlayer:RemoveCharacter()
	end
end
