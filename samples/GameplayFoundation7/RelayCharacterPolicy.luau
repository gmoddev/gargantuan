local Players = game:GetService("Players")
local Workspace = game:GetService("Workspace")
local Template = game:GetService("ReplicatedStorage"):FindFirstChild("RelayCharacterTemplate")
assert(Template and Template:IsA("KinematicCharacter") and Template.Archivable and Template.RootPart,
	"[Gameplay:RelayCharacter] Author an archivable KinematicCharacter template with a root")

local Spawns = {}

local function ReplaceCharacter(Player)
	local Current = Player.Character
	if not Current then
		return
	end
	local Custom = Template:Clone()
	Custom.Name = Current.Name
	Custom.CFrame = Current.CFrame
	Custom.Parent = Workspace
	Player.Character = Custom
end

local function AddPlayer(Player)
	if Spawns[Player] then
		return
	end
	-- The default runtime supplies the initial pose, movement policy and camera.
	-- The server bootstrap registers the default spawn listener first.
	Spawns[Player] = Player.CharacterSpawnRequested:Connect(function()
		ReplaceCharacter(Player)
	end)
	ReplaceCharacter(Player)
end

local function RemovePlayer(Player)
	local Spawn = Spawns[Player]
	if Spawn then
		Spawn:Disconnect()
		Spawns[Player] = nil
	end
end

local Added = Players.PlayerAdded:Connect(AddPlayer)
local Removing = Players.PlayerRemoving:Connect(RemovePlayer)
for _, Player in Players:GetPlayers() do
	AddPlayer(Player)
end

script.Destroying:Connect(function()
	Added:Disconnect()
	Removing:Disconnect()
	for Player in Spawns do
		RemovePlayer(Player)
	end
end)
