local Sessions = game:GetService("SessionService")
local Saves = game:GetService("WorldSaveService")
local Run = game:GetService("RunService")
local Menu = Instance.new("ScreenGui")
Menu.Name = "YardMenu"
Menu.Parent = script.Parent

local function Control(Class, Name, Text, X, Y, Width)
	local Item = Instance.new(Class)
	Item.Name = Name
	Item.Text = Text
	Item.Size = UDim2.fromOffset(Width, 36)
	Item.Position = UDim2.fromOffset(X, Y)
	Item.Parent = Menu
	return Item
end

local Status = Control("TextLabel", "Status", "Construction Yard", 16, 16, 320)
local WorldName = Control("TextBox", "WorldName", "Hillside", 16, 60, 156)
local Seed = Control("TextBox", "Seed", "Hillside", 180, 60, 156)
local Create = Control("TextButton", "Create", "Create world", 16, 104, 156)
local Next = Control("TextButton", "Next", "Next world", 180, 104, 156)
local Play = Control("TextButton", "Play", "Play selected world", 16, 148, 320)
local Resume = Control("TextButton", "Resume", "Resume", 16, 60, 156)
local Leave = Control("TextButton", "Leave", "Return to title", 180, 60, 156)
local Quit = Control("TextButton", "Quit", "Quit", 16, 192, 320)
local Slot, Page, Index
local Busy = false

local function SetPlaying(Playing)
	for _, Item in {WorldName, Seed, Create, Next, Play} do Item.Visible = not Playing end
	Resume.Visible = Playing
	Leave.Visible = Playing
end

local function ShowSlot()
	Status.Text = if Slot then Slot.Name else "Create a world to play"
end

local function NextWorld()
	if not Page or Index == #Page.Items then
		local Listed = Saves:List(if Page then Page.NextCursor else nil):Await()
		if not Listed.Ok then Status.Text = Listed.Message return end
		Page = Listed.Value
		Index = 0
	end
	if #Page.Items > 0 then Index += 1 end
	Slot = Page.Items[Index]
	ShowSlot()
end

local function Bind(Event, Callback)
	Event:Connect(function()
		if Busy then return end
		Busy = true
		local Ok, Failure = pcall(Callback)
		if not Ok then Status.Text = tostring(Failure) end
		Busy = false
	end)
end

SetPlaying(false)
assert(game:SetMainMenu(Menu).Ok)
Sessions.Changed:Connect(function(Session)
	if Session.State == "Idle" and Session.LastError then
		SetPlaying(false)
		Status.Text = Session.LastError.Message
	end
end)
Bind(Next.Activated, NextWorld)
Bind(Create.Activated, function()
	local Created = Saves:Create({Name = WorldName.Text, Seed = Seed.Text}):Await()
	if not Created.Ok then Status.Text = Created.Message return end
	Slot = Created.Value
	Page = nil
	ShowSlot()
end)
Bind(Play.Activated, function()
	if not Slot then ShowSlot() return end
	Status.Text = "Opening " .. Slot.Name
	local Opened = Sessions:StartLocal({SaveId = Slot.SaveId}):Await()
	if not Opened.Ok then Status.Text = Opened.Message return end
	SetPlaying(true)
	game:SetMainMenuVisible(false)
	game:SetGameplayInputEnabled(true)
end)
local PauseKey = game:GetService("InputService"):AddContext("Application"):AddAction("Pause")
PauseKey:AddBinding(Enum.KeyCode.Escape, {Consume = true})
Bind(PauseKey.Pressed, function()
	if Sessions:GetSession().State ~= "Active" or Run:IsGameplayPaused() then return end
	local Held = Run:PauseGameplay():Await()
	if Held.Ok then
		game:SetGameplayInputEnabled(false)
		game:SetMainMenuVisible(true)
		Status.Text = Slot.Name
	end
end)
Bind(Resume.Activated, function()
	local Released = Run:ResumeGameplay():Await()
	if Released.Ok and not Released.Value then
		game:SetMainMenuVisible(false)
		game:SetGameplayInputEnabled(true)
	elseif not Released.Ok then Status.Text = Released.Message end
end)
Bind(Leave.Activated, function()
	local Left = Sessions:Leave():Await()
	if not Left.Ok then Status.Text = Left.Message return end
	SetPlaying(false)
	ShowSlot()
end)
Quit.Activated:Connect(function() game:GetService("ApplicationService"):Quit() end)
task.defer(function()
	if Busy then return end
	Busy = true
	NextWorld()
	Busy = false
end)
