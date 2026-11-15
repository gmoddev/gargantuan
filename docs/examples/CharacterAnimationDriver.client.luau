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
if type(IdleReference) ~= "string" or type(MoveReference) ~= "string" then
	warn("[Animation:Policy] Set IdleReference and MoveReference on this Script")
	return
end

local Controller = nil
local function Detach()
	if Controller then Controller:Close() end
	Controller = nil
end

local function IsMoving()
	return ActionMap:IsDown("MoveForward") or ActionMap:IsDown("MoveBackward")
		or ActionMap:IsDown("MoveLeft") or ActionMap:IsDown("MoveRight")
end

local function ChooseVisualState()
	if Controller then Controller:Request(if IsMoving() then "Move" else "Idle", 0.2) end
end

local function Attach(Character)
	Detach()
	local RootPart = Character.RootPart
	if RootPart == nil or not RootPart:IsA("MeshPart") then
		warn("[Animation:Policy] Character.RootPart must be an imported skinned MeshPart")
		return
	end
	local Animator = RootPart:FindFirstChildOfClass("Animator")
	if Animator == nil then
		warn("[Animation:Policy] RootPart needs a direct Animator child")
		return
	end
	local Ok, Result = pcall(function()
		return Policy.New(Animator, RunService, {
			{Name = "Idle", Reference = IdleReference, Looped = true},
			{Name = "Move", Reference = MoveReference, Looped = true},
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
if LocalPlayer.Character then Attach(LocalPlayer.Character) end
script.Destroying:Connect(function()
	BeganConnection:Disconnect()
	EndedConnection:Disconnect()
	AddedConnection:Disconnect()
	RemovingConnection:Disconnect()
	Detach()
end)
