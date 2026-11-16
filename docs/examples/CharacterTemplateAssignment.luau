-- ModuleScript under ReplicatedStorage. The server chooses the playable Character; clients only observe it.
local Assignment = {}
Assignment.__index = Assignment

local function ValidateTemplate(Template)
	assert(Template and Template:IsA("KinematicCharacter") and Template.Archivable,
		"[Character:Template] An Archivable KinematicCharacter template is required")
	local RootPart = Template.RootPart
	assert(RootPart and RootPart:IsA("MeshPart") and RootPart:FindFirstChildOfClass("Animator"),
		"[Character:Template] RootPart must be a skinned MeshPart with a direct Animator")
	local Current = RootPart
	while Current and Current ~= Template do Current = Current.Parent end
	assert(Current == Template, "[Character:Template] RootPart must belong to the template subtree")
end

function Assignment.New(Players, Workspace, Template)
	assert(Players and Workspace, "[Character:Template] Players and Workspace are required")
	ValidateTemplate(Template)
	local Self = setmetatable({Players = Players, Workspace = Workspace, Template = Template,
		Connections = {}, Owned = {}, Closed = false}, Assignment)
	local function AddPlayer(Player)
		if Self.Closed or Self.Connections[Player] then return end
		local function Replace(Current)
			if Self.Closed or Current == nil or Current == Self.Owned[Player] then return end
			local Replacement = nil
			local PreviousOwned = Self.Owned[Player]
			local Ok, Failure = pcall(function()
				Replacement = Self.Template:Clone()
				local RootPart = Replacement.RootPart
				assert(Replacement:IsA("KinematicCharacter") and RootPart and
					RootPart:IsA("MeshPart") and RootPart ~= Self.Template.RootPart and
					RootPart:FindFirstChildOfClass("Animator"),
					"[Character:Template] Clone lost its internal rig or RootPart reference")
				Replacement.Parent = Self.Workspace
				Self.Owned[Player] = Replacement
				Player.Character = Replacement
			end)
			if not Ok then
				Self.Owned[Player] = PreviousOwned
				if Replacement and Player.Character ~= Replacement then
					pcall(function() Replacement:Destroy() end)
				end
				if warn then warn("[Character:Template] Could not replace Player.Character:", Failure)
				else print("[Character:Template] Could not replace Player.Character:", Failure) end
			end
		end
		Self.Connections[Player] = Player.CharacterAdded:Connect(Replace)
		if Player.Character then Replace(Player.Character) end
	end
	local function RemovePlayer(Player)
		local Connection = Self.Connections[Player]
		if Connection then Connection:Disconnect() end
		Self.Connections[Player] = nil
		Self.Owned[Player] = nil
	end
	Self.AddedConnection = Players.PlayerAdded:Connect(AddPlayer)
	Self.RemovingConnection = Players.PlayerRemoving:Connect(RemovePlayer)
	for _, Player in Players:GetPlayers() do AddPlayer(Player) end
	return Self
end

function Assignment.Close(Self)
	if Self.Closed then return end
	Self.Closed = true
	Self.AddedConnection:Disconnect()
	Self.RemovingConnection:Disconnect()
	for _, Connection in Self.Connections do Connection:Disconnect() end
	Self.Connections = {}
	Self.Owned = {}
	Self.Template = nil
end

return Assignment
