-- RunContext: Server. Pre-author a RemoteEvent named Ping in ReplicatedStorage.
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local Ping = ReplicatedStorage:FindFirstChild("Ping")

if not Ping or not Ping:IsA("RemoteEvent") then
	warn("[Docs:Remote] Missing ReplicatedStorage.Ping RemoteEvent")
else
	Ping.OnServerEvent:Connect(function(Peer, Message)
		if Message ~= "hello" then
			return
		end
		local Ok, ErrorMessage = pcall(function()
			Ping:FireClient(Peer.Slot, Peer.Generation, "hello from server")
		end)
		if not Ok then
			warn("[Docs:Remote] Reply rejected:", ErrorMessage)
		end
	end)
end
