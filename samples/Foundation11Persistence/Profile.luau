local DataStoreService = game:GetService("DataStoreService")
local Profiles = DataStoreService:GetDocumentStore("PersistentQuestProfiles")

local function Wait(Operation, IsPresent, IsMutation)
	local Result = Operation:Poll()
	while Result.Status == "Pending" do
		if IsPresent and not IsPresent() then
			Operation:Cancel()
			return { Status = if IsMutation then "UnknownOutcome" else "Cancelled" }
		end
		task.wait()
		Result = Operation:Poll()
	end
	return Result
end

local function NewProfile()
	return {
		Version = 1,
		Currency = 0,
		Inventory = DataStoreService:CreateArray(),
		Rewards = {},
		Settings = { Subtitles = true },
	}
end

local function IsProfile(Value)
	return type(Value) == "table"
		and Value.Version == 1
		and type(Value.Currency) == "number"
		and Value.Currency >= 0
		and Value.Currency <= 1_000_000_000
		and Value.Currency == math.floor(Value.Currency)
		and type(Value.Inventory) == "table"
		and type(Value.Rewards) == "table"
		and type(Value.Settings) == "table"
end

local function Load(ProfileKey, IsPresent)
	local Result = Wait(Profiles:GetAsync(ProfileKey), IsPresent)
	if Result.Status == "Missing" then
		return "Missing", NewProfile(), "0"
	end
	if Result.Status ~= "Ok" then
		return Result.Status
	end
	if not IsProfile(Result.Record.Value) then
		return "Invalid"
	end
	return "Ok", Result.Record.Value, Result.Record.Revision
end

local function GrantStarterReward(ProfileKey, IsPresent)
	for Attempt = 1, 3 do
		if IsPresent and not IsPresent() then
			return "Cancelled"
		end
		local Status, Profile, Revision = Load(ProfileKey, IsPresent)
		if Status ~= "Ok" and Status ~= "Missing" then
			return Status
		end
		if Profile.Rewards.StarterQuestV1 == true then
			return "AlreadyGranted", Profile
		end
		if #Profile.Inventory >= 32 or Profile.Currency > 999_999_975 then
			return "Invalid"
		end
		Profile.Currency += 25
		table.insert(Profile.Inventory, "ExplorerBadge")
		Profile.Rewards.StarterQuestV1 = true
		local Mutation = Wait(Profiles:PutAsync(ProfileKey, Profile, Revision), IsPresent, true)
		if Mutation.Status == "Ok" then
			return "Granted", Profile
		end
		if Mutation.Status == "UnknownOutcome" then
			local ConfirmedStatus, Confirmed = Load(ProfileKey, IsPresent)
			if ConfirmedStatus == "Ok" and Confirmed.Rewards.StarterQuestV1 == true then
				return "Reconciled", Confirmed
			end
			-- A negative or unavailable read does not settle the original mutation.
			return "UnknownOutcome"
		end
		if Mutation.Status ~= "Conflict" then
			return Mutation.Status
		end
		if Attempt < 3 then
			task.wait()
		end
	end
	return "Conflict"
end

return { Load = Load, GrantStarterReward = GrantStarterReward }
