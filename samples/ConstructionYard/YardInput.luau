local Actions = game:GetService("ActionMap")
local Players = game:GetService("Players")
local Space = game:GetService("Workspace")
local YardRequest = game:GetService("ReplicatedStorage").YardRequest
local Bindings = {
	Actions:BindKey("YardBuild", Enum.KeyCode.B, 1, 101, true),
	Actions:BindKey("YardDig", Enum.KeyCode.E, 1, 101, true),
	Actions:BindKey("YardDoor", Enum.KeyCode.F, 1, 101, true),
}

local Input = Actions.ActionBegan:Connect(function(Name)
	if Name == "YardBuild" or Name == "YardDig" or Name == "YardDoor" then YardRequest:TryFireServer(Name) end
end)
local Reply = YardRequest.OnClientEvent:Connect(function(Result)
	if Result ~= "DoorApplied" then return end
	local Player = Players.LocalPlayer
	local Avatar = Player and Player.Character
	if Avatar and Avatar.RootPart then Space.CurrentSpatialRegion = Avatar.RootPart.SpatialRegion end
end)
script.Destroying:Connect(function()
	Input:Disconnect()
	Reply:Disconnect()
	for _, Binding in Bindings do Actions:Unbind(Binding) end
end)

