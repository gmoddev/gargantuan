-- Install this as a server Script next to a ModuleScript named Profile.
local Profiles = require("./Profile")
local Players = game:GetService("Players")
local Sessions = {}

-- A trusted, fixed test account allows the same local profile to be loaded
-- after reconnect and from independent test servers. Replace this mapping
-- with server-owned authenticated identity in a production integration.
local ProfileKey = "DevelopmentExplorer"

local function Enter(Player)
	if Sessions[Player] then
		return
	end
	local Session = { Present = true }
	Sessions[Player] = Session
	task.defer(function()
		local function IsPresent()
			return Session.Present
		end
		local Status, Profile = Profiles.GrantStarterReward(ProfileKey, IsPresent)
		if not Session.Present then
			return
		end
		if Profile then
			print("[Persistence:QuestSample]", Status, Profile.Currency, #Profile.Inventory)
		else
			warn("[Persistence:QuestSample]", Status)
		end
	end)
end

Players.PlayerRemoving:Connect(function(Player)
	local Session = Sessions[Player]
	if Session then
		Session.Present = false
		Sessions[Player] = nil
	end
end)
Players.PlayerAdded:Connect(Enter)
for _, Player in Players:GetPlayers() do
	Enter(Player)
end
