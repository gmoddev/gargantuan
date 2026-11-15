-- RunContext: Server in a real network session. Integrate this admission rule with your game's one server action policy.
local Control = game:GetService("CharacterControlService")
local Reference = script:GetAttribute("AnimationReference")
if type(Reference) ~= "string" then
	warn("[Animation:Action] Set AnimationReference to an imported ready Animation asset")
	return
end

local Ok, Registered = pcall(function()
	-- Translation is a requested 0.9-stud action delta over 0.5 seconds, not a direct Character transform write.
	return Control:RegisterAction("CreatorLunge", Reference, 0.5, Vector3.new(0.9, 0, 0), 0, false)
end)
if not Ok or not Registered then
	warn("[Animation:Action] Server action registration failed:", Registered)
	return
end

local PolicyOk, PolicyError = pcall(function()
	Control:SetActionPolicy(function(Player, Character, ActionName)
		return ActionName == "CreatorLunge" and Player.Character == Character
	end)
end)
if not PolicyOk then warn("[Animation:Action] Server admission policy failed:", PolicyError) end
