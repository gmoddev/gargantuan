-- RunContext: Client. See README for the imported skinned RootPart and ModuleScript setup.
local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
local ActionMap = game:GetService("ActionMap")
local Policy = require("@game/ReplicatedStorage/CharacterAnimationPolicy")
local LocalPlayer = Players.LocalPlayer
if LocalPlayer == nil then
	warn("[Animation:Policy] LocalPlayer is unavailable")
	return
end

local IdleReference = script:GetAttribute("IdleReference")
local MoveReference = script:GetAttribute("MoveReference")
local ActionReference = script:GetAttribute("ActionReference")
if type(IdleReference) ~= "string" or type(MoveReference) ~= "string" or type(ActionReference) ~= "string" then
	warn("[Animation:Policy] Set IdleReference, MoveReference, and ActionReference on this Script")
	return
end

local Controller = nil
local ActionActive = false
local function Detach()
	if Controller then Controller:Close() end
	Controller = nil
	ActionActive = false
end

local function IsMoving()
	return ActionMap:IsDown("MoveForward") or ActionMap:IsDown("MoveBackward")
		or ActionMap:IsDown("MoveLeft") or ActionMap:IsDown("MoveRight")
end

local function ChooseVisualState()
	if Controller and not ActionActive then
		Controller:Request(if IsMoving() then "Move" else "Idle", 0.2)
	end
end

local function Attach(Character)
	Detach()
	local RootPart = Character.RootPart
	-- The shipped assembly may briefly publish its Part root before the server substitutes the template.
	if RootPart == nil or not RootPart:IsA("MeshPart") then return end
	local Animator = RootPart:FindFirstChildOfClass("Animator")
	if Animator == nil then
		warn("[Animation:Policy] RootPart needs a direct Animator child")
		return
	end
	local Ok, Result = pcall(function()
		return Policy.New(Animator, RunService, {
			{Name = "Idle", Reference = IdleReference, Looped = true},
			{Name = "Move", Reference = MoveReference, Looped = true},
			{Name = "Action", Reference = ActionReference, Looped = false},
		})
	end)
	if not Ok then
		warn("[Animation:Policy] Could not load compatible clips:", Result)
		return
	end
	Controller = Result
	Controller:Request("Idle")
	ChooseVisualState()
end

local AddedConnection = LocalPlayer.CharacterAdded:Connect(Attach)
local RemovingConnection = LocalPlayer.CharacterRemoving:Connect(Detach)
local BeganConnection = ActionMap.ActionBegan:Connect(ChooseVisualState)
local EndedConnection = ActionMap.ActionEnded:Connect(ChooseVisualState)
local Control = game:GetService("CharacterControlService")
local ResolvedConnection = Control.ActionResolved:Connect(function(Character, ActionName, Accepted)
	if ActionName ~= "CreatorLunge" or Character ~= LocalPlayer.Character then return end
	if Accepted and Controller then
		ActionActive = true
		Controller:Request("Action", 0.08)
	end
end)
local ActionEndedConnection = Control.ActionEnded:Connect(function(Character, ActionName)
	if ActionName ~= "CreatorLunge" or Character ~= LocalPlayer.Character then return end
	ActionActive = false
	ChooseVisualState()
end)
if LocalPlayer.Character then Attach(LocalPlayer.Character) end
script.Destroying:Connect(function()
	ActionEndedConnection:Disconnect()
	ResolvedConnection:Disconnect()
	BeganConnection:Disconnect()
	EndedConnection:Disconnect()
	AddedConnection:Disconnect()
	RemovingConnection:Disconnect()
	Detach()
end)
