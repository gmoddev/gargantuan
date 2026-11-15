-- RunContext: Client in the same network session as CharacterActionServer.server.luau.
local Control = game:GetService("CharacterControlService")
local ActionMap = game:GetService("ActionMap")
local Players = game:GetService("Players")
local Reference = script:GetAttribute("AnimationReference")
if type(Reference) ~= "string" then
	warn("[Animation:Action] Set AnimationReference to the same imported Animation asset as the server")
	return
end

local Ok, Registered = pcall(function()
	return Control:RegisterAction("CreatorLunge", Reference, 0.5, Vector3.new(0.9, 0, 0), 0, false)
end)
if not Ok or not Registered then
	warn("[Animation:Action] Client action registration failed:", Registered)
	return
end

local ResolutionConnection = Control.ActionResolved:Connect(function(Character, ActionName, Accepted)
	if ActionName ~= "CreatorLunge" or Players.LocalPlayer == nil or Character ~= Players.LocalPlayer.Character then return end
	if Accepted then
		print("[Animation:Action] Server accepted CreatorLunge")
	else
		print("[Animation:Action] Server denied CreatorLunge")
	end
end)
local BindingId = ActionMap:BindKey("CreatorLunge", Enum.KeyCode.F, 1, 0, false)
local InputConnection = ActionMap.ActionBegan:Connect(function(ActionName)
	if ActionName ~= "CreatorLunge" then return end
	local Submitted, Result = pcall(function() return Control:RequestAction("CreatorLunge") end)
	if not Submitted or not Result then
		warn("[Animation:Action] Action request was not submitted:", Result)
	end
	-- A true result means submitted, not accepted; ActionResolved is the authority result.
end)
script.Destroying:Connect(function()
	InputConnection:Disconnect()
	ResolutionConnection:Disconnect()
	ActionMap:Unbind(BindingId)
end)
