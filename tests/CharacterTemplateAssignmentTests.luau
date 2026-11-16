-- Run with: luau tests/CharacterTemplateAssignmentTests.luau
local Assignment = require("../docs/examples/CharacterTemplateAssignment")

local function NewSignal()
	local Signal = {Connections = {}}
	function Signal.Connect(Self, Callback)
		local Connection = {Connected = true, Callback = Callback}
		function Connection.Disconnect(Current) Current.Connected = false end
		table.insert(Self.Connections, Connection)
		return Connection
	end
	function Signal.Fire(Self, ...)
		for _, Connection in Self.Connections do
			if Connection.Connected then Connection.Callback(...) end
		end
	end
	return Signal
end

local function NewRoot(Owner)
	local Root = {Parent = Owner}
	function Root.IsA(_, Name) return Name == "MeshPart" end
	function Root.FindFirstChildOfClass(_, Name)
		return if Name == "Animator" then {} else nil
	end
	return Root
end

local function NewCharacter(Name)
	local Character = {Name = Name, Archivable = true, Destroyed = false, Parent = nil}
	function Character.IsA(_, ClassName) return ClassName == "KinematicCharacter" or ClassName == "Character" end
	function Character.Destroy(Self) Self.Destroyed = true end
	Character.RootPart = NewRoot(Character)
	return Character
end

local Template = NewCharacter("PlayableCharacterTemplate")
Template.Clones = 0
function Template.Clone(Self)
	Self.Clones += 1
	return NewCharacter("Playable" .. tostring(Self.Clones))
end

local function NewPlayer(Initial)
	local Current = Initial
	local Player = {CharacterAdded = NewSignal(), CharacterRemoving = NewSignal()}
	return setmetatable(Player, {
		__index = function(_, Key)
			if Key == "Character" then return Current end
			return nil
		end,
		__newindex = function(Self, Key, Value)
			if Key ~= "Character" then rawset(Self, Key, Value); return end
			if Current == Value then return end
			local Previous = Current
			if Previous then
				Self.CharacterRemoving:Fire(Previous)
				Previous:Destroy()
			end
			Current = Value
			if Current then Self.CharacterAdded:Fire(Current) end
		end,
	})
end

local FirstDefault = NewCharacter("DefaultPartActor")
local FirstPlayer = NewPlayer(FirstDefault)
local Players = {PlayerAdded = NewSignal(), PlayerRemoving = NewSignal(), Values = {FirstPlayer}}
function Players.GetPlayers(Self) return Self.Values end
local Workspace = {}
local Controller = Assignment.New(Players, Workspace, Template)
local FirstPlayable = FirstPlayer.Character
assert(FirstDefault.Destroyed and FirstPlayable ~= FirstDefault and FirstPlayable.Parent == Workspace and
	FirstPlayable.RootPart ~= Template.RootPart and Template.Clones == 1,
	"Existing player's default actor is replaced by an independent template clone")
FirstPlayer.CharacterAdded:Fire(FirstPlayable)
assert(FirstPlayer.Character == FirstPlayable and Template.Clones == 1,
	"The policy does not recurse when its own replacement is published")

local SecondDefault = NewCharacter("RespawnedPartActor")
FirstPlayer.Character = SecondDefault
local SecondPlayable = FirstPlayer.Character
assert(SecondDefault.Destroyed and FirstPlayable.Destroyed and SecondPlayable ~= FirstPlayable and
	SecondPlayable.Parent == Workspace and Template.Clones == 2,
	"A respawn receives a fresh playable clone and the previous Character is retired")

local LateDefault = NewCharacter("LatePartActor")
local LatePlayer = NewPlayer(LateDefault)
Players.PlayerAdded:Fire(LatePlayer)
assert(LateDefault.Destroyed and LatePlayer.Character.Parent == Workspace and Template.Clones == 3,
	"New players receive the same template policy")
Players.PlayerRemoving:Fire(LatePlayer)
local AfterRemoval = NewCharacter("AfterRemoval")
LatePlayer.Character = AfterRemoval
assert(LatePlayer.Character == AfterRemoval and not AfterRemoval.Destroyed and Template.Clones == 3,
	"Player removal disconnects its replacement listener")
Controller:Close()
Controller:Close()
local AfterClose = NewCharacter("AfterClose")
FirstPlayer.Character = AfterClose
assert(FirstPlayer.Character == AfterClose and not AfterClose.Destroyed and Template.Clones == 3,
	"Closing the policy disconnects existing players and is idempotent")

local BrokenTemplate = NewCharacter("BrokenTemplate")
function BrokenTemplate.Clone() error("clone unavailable") end
local Fallback = NewCharacter("FallbackPartActor")
local FallbackPlayer = NewPlayer(Fallback)
local FallbackPlayers = {PlayerAdded = NewSignal(), PlayerRemoving = NewSignal()}
function FallbackPlayers.GetPlayers() return {FallbackPlayer} end
local FailedPolicy = Assignment.New(FallbackPlayers, Workspace, BrokenTemplate)
assert(FallbackPlayer.Character == Fallback and not Fallback.Destroyed,
	"A clone failure retains the playable default actor")
FailedPolicy:Close()
local BadRemapTemplate = NewCharacter("BadRemapTemplate")
local BadClone = nil
function BadRemapTemplate.Clone(Self)
	BadClone = NewCharacter("BadClone")
	BadClone.RootPart = Self.RootPart
	return BadClone
end
local Unchanged = NewCharacter("UnchangedPartActor")
local UnchangedPlayer = NewPlayer(Unchanged)
local UnchangedPlayers = {PlayerAdded = NewSignal(), PlayerRemoving = NewSignal()}
function UnchangedPlayers.GetPlayers() return {UnchangedPlayer} end
local BadRemapPolicy = Assignment.New(UnchangedPlayers, Workspace, BadRemapTemplate)
assert(UnchangedPlayer.Character == Unchanged and not Unchanged.Destroyed and BadClone.Destroyed,
	"A clone with an unremapped RootPart is discarded without replacing the current Character")
BadRemapPolicy:Close()
assert(not pcall(function()
	local Invalid = NewCharacter("InvalidTemplate")
	Invalid.RootPart = nil
	Assignment.New(FallbackPlayers, Workspace, Invalid)
end), "A template without a bound MeshPart is rejected before listeners are installed")

print("[Character:TemplateTests] PASS")
