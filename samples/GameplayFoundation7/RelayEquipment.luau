local Hud = script.Parent
local Toggle
local Preview
local Animator
for Attempt = 1, 300 do
	Toggle = Hud:FindFirstChild("EquipmentToggle")
	Preview = Hud:FindFirstChild("EquipmentPreview")
	local Crystal = Preview and Preview:FindFirstChild("Crystal")
	Animator = Crystal and Crystal:FindFirstChild("PreviewAnimator")
	if Toggle and Preview and Animator then
		break
	end
	task.wait()
end
if not (Toggle and Preview and Animator) then
	warn("[Gameplay:Relay] Equipment preview did not receive its complete scene")
	return
end

local Loaded, Track = pcall(function()
	return Animator:LoadAnimation("asset://ca34ffb8742655186db2cfb54124a5ff")
end)
if Loaded then
	Track.Looped = true
	Track:Play()
else
	warn("[Gameplay:Relay] Equipment preview animation failed to load")
end

Toggle.AccessibilitySelected = Preview.Visible
local Connection = Toggle.Activated:Connect(function()
	Preview.Visible = not Preview.Visible
	Toggle.AccessibilitySelected = Preview.Visible
	Toggle:ReleaseFocus()
	if Loaded then
		if Preview.Visible then
			Track:Play()
		else
			Track:Stop()
		end
	end
end)
script.Destroying:Connect(function()
	Connection:Disconnect()
	if Loaded then
		Track:Stop()
	end
end)
