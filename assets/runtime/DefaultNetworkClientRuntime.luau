local Players = game:GetService("Players")
local ActionMap = game:GetService("ActionMap")
local CharacterControl = game:GetService("CharacterControlService")
local RunService = game:GetService("RunService")
local UserInputService = game:GetService("UserInputService")
local Workspace = game:GetService("Workspace")

local LocalPlayer = Players.LocalPlayer
if LocalPlayer == nil then
	error("[Character:Runtime] trusted LocalPlayer is unavailable")
end

local Cleanups = {}
if Players.DefaultControllerEnabled or Players.DefaultCameraEnabled then
	table.insert(
		Cleanups,
		require("@game/Players/PlayerRuntimeModules/DefaultActionMap")(ActionMap, Players.DefaultCameraEnabled)
	)
end
if Players.DefaultControllerEnabled then
	require("@game/Players/PlayerRuntimeModules/DefaultNetworkLocomotion")(CharacterControl, Workspace)
	table.insert(
		Cleanups,
		require("@game/Players/PlayerRuntimeModules/DefaultNetworkCharacterRuntime")(
			LocalPlayer,
			ActionMap,
			Workspace,
			RunService,
			CharacterControl
		)
	)
end
if Players.DefaultCameraEnabled then
	table.insert(
		Cleanups,
		require("@game/Players/PlayerRuntimeModules/DefaultCamera")(
			Players,
			ActionMap,
			UserInputService,
			Workspace,
			RunService
		)
	)
end

script.Destroying:Connect(function()
	for Index = #Cleanups, 1, -1 do
		Cleanups[Index]()
	end
	table.clear(Cleanups)
end)
