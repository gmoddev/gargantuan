local Players = game:GetService("Players")
local CharacterControl = game:GetService("CharacterControlService")
local Workspace = game:GetService("Workspace")

require("@game/Players/PlayerRuntimeModules/DefaultNetworkLocomotion")(
	CharacterControl, Workspace, Players:GetAttribute("DefaultMovementTuning") == true
)

local CharacterCleanups = {}
local function AddPlayer(Player)
	local PlayerId = Player.PlayerId
	if CharacterCleanups[PlayerId] ~= nil then
		return
	end
	CharacterCleanups[PlayerId] = require("@game/Players/PlayerRuntimeModules/DefaultCharacterAssembly")(
		Player,
		Workspace
	)
	if Players.CharacterAutoLoads then
		Player:LoadCharacter()
	end
end

local function RemovePlayer(Player)
	local PlayerId = Player.PlayerId
	local Cleanup = CharacterCleanups[PlayerId]
	if Cleanup ~= nil then
		CharacterCleanups[PlayerId] = nil
		Cleanup()
	end
end

for _, Player in Players:GetPlayers() do
	AddPlayer(Player)
end
local AddedConnection = Players.PlayerAdded:Connect(AddPlayer)
local RemovingConnection = Players.PlayerRemoving:Connect(RemovePlayer)

script.Destroying:Connect(function()
	RemovingConnection:Disconnect()
	AddedConnection:Disconnect()
	for PlayerId, Cleanup in CharacterCleanups do
		CharacterCleanups[PlayerId] = nil
		Cleanup()
	end
end)
