-- RunContext: Server. Runs in offline Play and on a network server, never as client authority.
local Players = game:GetService("Players")
local Workspace = game:GetService("Workspace")
local ReplicatedStorage = game:GetService("ReplicatedStorage")
if not Players.DefaultControllerEnabled or not Players.CharacterAutoLoads then
	warn("[Character:Template] Enable the saved default controller and CharacterAutoLoads for this template")
	return
end

local Template = ReplicatedStorage:FindFirstChild("PlayableCharacterTemplate")
if Template == nil then
	warn("[Character:Template] Author PlayableCharacterTemplate under ReplicatedStorage first")
	return
end
local Ok, Result = pcall(function()
	return require("@game/ReplicatedStorage/CharacterTemplateAssignment").New(Players, Workspace, Template)
end)
if not Ok then
	warn("[Character:Template] Could not start template assignment:", Result)
	return
end
script.Destroying:Connect(function() Result:Close() end)
