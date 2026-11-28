local Space = game.Workspace
local Platform = Space.FreightPlatform
local Run = game:GetService("RunService")

-- The authored 512-cell freight deck is static in the document. Gameplay owns each activation.
if not Space:GetAttribute("FreightLandingHeld") then
	Platform:SetMotionMode("Dynamic", Platform:GetRevision())
end
local Direction = 1
local Elapsed = 0
local Drive = Run.PreSimulation:Connect(function(DeltaTime)
	Elapsed += DeltaTime
	if Elapsed < 0.15 then return end
	Elapsed = 0
	if Space:GetAttribute("FreightLandingHeld") then return end
	if Platform.MotionMode == "Static" then
		Platform:SetMotionMode("Dynamic", Platform:GetRevision())
	end
	local State = Platform:GetAssemblyState()
	if State.CFrame.Position.X >= 25 then Direction = -1
	elseif State.CFrame.Position.X <= 19 then Direction = 1 end
	local DesiredX = Direction * 3
	if math.abs(State.LinearVelocity.X - DesiredX) > 0.1 then
		Platform:SetAssemblyVelocity(Vector3.new(DesiredX, State.LinearVelocity.Y, 0), Vector3.zero, Platform:GetRevision())
	end
end)
script.Destroying:Connect(function() Drive:Disconnect() end)
