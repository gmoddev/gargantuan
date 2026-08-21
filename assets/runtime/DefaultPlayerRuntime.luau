local Players = game:GetService("Players")
local ActionMap = game:GetService("ActionMap")
local RunService = game:GetService("RunService")
local UserInputService = game:GetService("UserInputService")
local Workspace = game:GetService("Workspace")

local Cleanups = {}
if Players.DefaultControllerEnabled or Players.DefaultCameraEnabled then
	table.insert(
		Cleanups,
		require("@game/Players/PlayerRuntimeModules/DefaultActionMap")(ActionMap, Players.DefaultCameraEnabled)
	)
end
if Players.DefaultControllerEnabled then
	table.insert(Cleanups, require("@game/Players/PlayerRuntimeModules/DefaultPlayerController")(Players, ActionMap, Workspace, RunService))
end
if Players.DefaultCameraEnabled then
	table.insert(Cleanups, require("@game/Players/PlayerRuntimeModules/DefaultCamera")(Players, ActionMap, UserInputService, Workspace, RunService))
end

script.Destroying:Connect(function()
	for Index = #Cleanups, 1, -1 do
		Cleanups[Index]()
	end
	table.clear(Cleanups)
end)
