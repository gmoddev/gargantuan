-- RunContext: Client. Use with the server script and pre-authored Ping RemoteEvent.
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local Ping = ReplicatedStorage:FindFirstChild("Ping")

if not Ping or not Ping:IsA("RemoteEvent") then
	warn("[Docs:Remote] Missing replicated Ping RemoteEvent")
else
	Ping.OnClientEvent:Connect(function(Message)
		print("[Docs:Remote] Server replied:", Message)
	end)
	local Ok, ErrorMessage = pcall(function()
		Ping:FireServer("hello")
	end)
	if not Ok then
		warn("[Docs:Remote] Send rejected:", ErrorMessage)
	end
end
