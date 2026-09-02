local Players = game:GetService("Players")
local CharacterControl = game:GetService("CharacterControlService")
local Workspace = game:GetService("Workspace")

require("@game/Players/PlayerRuntimeModules/DefaultNetworkLocomotion")(CharacterControl, Workspace)

local CharacterCleanups = {}
local function AddPlayer(Player)
	if CharacterCleanups[Player] ~= nil then
		return
	end
	CharacterCleanups[Player] = require("@game/Players/PlayerRuntimeModules/DefaultCharacterAssembly")(
		Player,
		Workspace
	)
	if Players.CharacterAutoLoads then
		Player:LoadCharacter()
	end
end

local function RemovePlayer(Player)
	local Cleanup = CharacterCleanups[Player]
	if Cleanup ~= nil then
		CharacterCleanups[Player] = nil
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
	for Player, Cleanup in CharacterCleanups do
		CharacterCleanups[Player] = nil
		Cleanup()
	end
end)
