local Workspace = game:GetService("Workspace")
local Players = game:GetService("Players")
local Terrain = Workspace.Terrain

local function AwaitEdit(Ticket)
	for _ = 1, 600 do
		local Receipt = Terrain:GetEditStatus(Ticket)
		if not Receipt then
			error("[Terrain:Sample] No receipt for the terrain edit")
		end
		if Receipt.Status == "Committed" and Receipt.CollisionReady then
			return Receipt.Revision
		end
		if Receipt.Status ~= "Pending" then
			error("[Terrain:Sample] Edit ended with " .. Receipt.Status)
		end
		task.wait()
	end
	error("[Terrain:Sample] Terrain edit exceeded the wait bound")
end

local function PlacePlayer(Player)
	local function PositionCharacter(Character)
		Character.CFrame = CFrame.new(14, 15, 10)
	end
	Player.CharacterAdded:Connect(PositionCharacter)
	if Player.Character then
		PositionCharacter(Player.Character)
	end
end

if not Terrain:GetGridInfo().EditingAvailable then
	error("[Terrain:Sample] Start the sample in an offline Engine runtime")
end

AwaitEdit(Terrain:FillBlock(Vector3.new(16, 5.4, 16), Vector3.new(18.5, 10, 18.5), 3))
AwaitEdit(Terrain:FillBall(Vector3.new(20, 9.2, 18), 7.4, 2))
AwaitEdit(Terrain:FillBall(Vector3.new(23.5, 14.8, 19), 6.7, 5))
AwaitEdit(Terrain:FillBall(Vector3.new(38.5, 17.2, 18.5), 4.8, 1))
AwaitEdit(Terrain:PaintBall(Vector3.new(14, 10.1, 9), 5.4, 1))

-- Overlapping brushes connect a chamber to an opening at the low-Z edge.
AwaitEdit(Terrain:RemoveBall(Vector3.new(15, 4.4, 6), 3.2))
AwaitEdit(Terrain:RemoveBall(Vector3.new(15, 4.4, 9.7), 3.2))
AwaitEdit(Terrain:RemoveBall(Vector3.new(16, 4.8, 13.7), 4.1))
AwaitEdit(Terrain:RemoveBall(Vector3.new(19.3, 5.3, 16.3), 3.6))

Players.PlayerAdded:Connect(PlacePlayer)
for _, Player in Players:GetPlayers() do
	PlacePlayer(Player)
end

local Marker = Instance.new("Part")
Marker.Name = "DigMarker"
Marker.Anchored = true
Marker.CanCollide = false
Marker.Color = Color3.fromRGB(233, 146, 31)
Marker.Position = Vector3.new(15, 12.3, 9)
Marker.Size = Vector3.new(1.5, 2.7, 1.5)
Marker.Parent = Workspace

local Prompt = Instance.new("ProximityPrompt")
Prompt.ActionText = "Excavate"
Prompt.ObjectText = "Open cave skylight"
Prompt.MaxActivationDistance = 7
Prompt.Parent = Marker
Prompt.Triggered:Once(function()
	Prompt.Enabled = false
	task.spawn(function()
		local Result, Error = pcall(function()
			AwaitEdit(Terrain:RemoveBall(Vector3.new(15.3, 9, 12), 3.5))
		end)
		if Result then
			print("[Terrain:Sample] Skylight collision is ready")
		else
			warn(Error)
		end
	end)
end)

print("[Terrain:Sample] Landscape collision is ready")
