-- RunContext: Server. Pre-author a RemoteEvent named Ping in ReplicatedStorage.
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local Ping = ReplicatedStorage:FindFirstChild("Ping")

if not Ping or not Ping:IsA("RemoteEvent") then
	warn("[Docs:Remote] Missing ReplicatedStorage.Ping RemoteEvent")
else
	Ping.OnPlayerEvent:Connect(function(Player, Message)
		if Message ~= "hello" then
			return
		end
		local Ok, Code = Ping:TryFireClient(Player, "hello from server")
		if not Ok then
			warn("[Docs:Remote] Reply rejected:", Code)
		end
	end)
end
