# Four creator workflows

These examples use **proposed** native methods and existing Gargantuan GUI signals. The examples are independent projects. In the first three, set ApplicationEntry to a client Script beside a ScreenGui named MainMenu. Named buttons, panels and labels are authored in the existing GUI tools. The app owns the controls before the first Player exists. Luau code in the application receives a scoped `game` and resolves the same native services through `GetService`. The following snippets are sample project code, separate from the executable research probes.

## Minimal standalone game

The menu contains Play, Resume, Quit and Status. Escape is an app-owned input action that remains eligible while the registered screen is hidden. Entry registration makes the title visible on cold startup. The same entry owns both pause and resume requests.

```luau
local Menu = script.Parent.MainMenu
local Sessions = game:GetService("SessionService")
local RunService = game:GetService("RunService")
local Application = game:GetService("ApplicationService")
assert(game:SetMainMenu(Menu).Ok)

Menu.Play.Activated:Connect(function()
    local Started = Sessions:StartLocal():Await()
    if not Started.Ok then
        Menu.Status.Text = Started.Message
        return
    end
    game:SetMainMenuVisible(false)
    game:SetGameplayInputEnabled(true)
end)

local Context = game:GetService("InputService"):AddContext("Application")
local Pause = Context:AddAction("Pause", { Type = Enum.InputActionType.Bool })
Pause:AddBinding(Enum.KeyCode.Escape, { Consume = true })
Pause.Pressed:Connect(function()
    if Sessions:GetSession().State ~= "Active" then return end
    local Held = RunService:PauseGameplay():Await()
    if Held.Ok then
        game:SetGameplayInputEnabled(false)
        game:SetMainMenuVisible(true)
    end
end)

Menu.Resume.Activated:Connect(function()
    local Released = RunService:ResumeGameplay():Await()
    if Released.Ok and not Released.Value then
        game:SetMainMenuVisible(false)
        game:SetGameplayInputEnabled(true)
    end
end)

Menu.Quit.Activated:Connect(function()
    Application:Quit()
end)
```

## Persistent voxel construction game

The save list displays the first page in reusable buttons cloned from SaveRow. A title with more slots can request the next page using NextCursor. Select a slot to load or host the same world. Seed text stays exact through save selection and reload. For clarity, these examples use a single panel and report failures in Status.

```luau
local Menu = script.Parent.MainMenu
local Saves = game:GetService("WorldSaveService")
local Sessions = game:GetService("SessionService")
assert(game:SetMainMenu(Menu).Ok)
local Selected

local function AddSave(Save)
    local Row = Menu.SaveRow:Clone()
    Row.Text = Save.Name
    Row.Visible = true
    Row.Parent = Menu.Slots
    Row.Activated:Connect(function()
        Selected = Save.SaveId
        Menu.Status.Text = Save.Name
    end)
end

task.defer(function()
    local Page = Saves:List():Await()
    if Page.Ok then
        for _, Save in Page.Value.Items do AddSave(Save) end
    else
        Menu.Status.Text = Page.Message
    end
end)

Menu.Create.Activated:Connect(function()
    local Created = Saves:Create({Name = Menu.WorldName.Text, Seed = Menu.Seed.Text}):Await()
    if not Created.Ok then
        Menu.Status.Text = Created.Message
        return
    end
    Selected = Created.Value.SaveId
    AddSave(Created.Value)
end)

local function Enter(Operation)
    local Result = Operation:Await()
    if Result.Ok then
        game:SetMainMenuVisible(false)
        game:SetGameplayInputEnabled(true)
    else
        Menu.Status.Text = Result.Message
    end
end

Menu.Load.Activated:Connect(function()
    if Selected then Enter(Sessions:StartLocal({SaveId = Selected})) end
end)
Menu.Host.Activated:Connect(function()
    if Selected then Enter(Sessions:Host({SaveId = Selected, MaxPlayers = 4, Discovery = "LAN"})) end
end)
Menu.Join.Activated:Connect(function()
    Enter(Sessions:Join({Kind = "Direct", Address = Menu.Address.Text}))
end)

local Context = game:GetService("InputService"):AddContext("WorldMenu")
local Open = Context:AddAction("OpenWorldMenu", {Type = Enum.InputActionType.Bool})
Open:AddBinding(Enum.KeyCode.Escape, {Consume = true})
Open.Pressed:Connect(function()
    game:SetGameplayInputEnabled(false)
    game:SetMainMenuVisible(true)
end)
Menu.ReturnToTitle.Activated:Connect(function()
    local Left = Sessions:Leave():Await()
    Menu.Status.Text = if Left.Ok then "Choose a world" else Left.Message
end)
```

An authority-side world generator can read the descriptor of the selected slot. The game's generator owns the seed-to-cells algorithm. The native save checkpoint owns the committed grid datasets and migration metadata. Invoke generation once when initializing a new world. Ordinary load restores the committed grid and applies the game's versioned recovery policy.

```luau
local World = game:GetService("WorldSaveService"):GetCurrentWorld()
if World.Ok then
    print("[Construction:World]", World.Value.WorldId, World.Value.Seed)
end
```

## Multiplayer browser, local overlay and reconnect

Author one row template and a Refresh button. A public game may choose Node discovery if its installation advertises that feature. This example browses LAN. The guest disables its own controls while the authoritative game continues. The same screen also handles an involuntary disconnect and offers reconnect. Each listed target is an opaque, expiring row ID.

```luau
local Menu = script.Parent.MainMenu
local Sessions = game:GetService("SessionService")
assert(game:SetMainMenu(Menu).Ok)
local Pending
local RefreshId = 0
local Rows = {}

local function Connect(Operation)
    Pending = Operation
    local Result = Operation:Await()
    if Pending ~= Operation then return end
    Pending = nil
    if Result.Ok then
        game:SetMainMenuVisible(false)
        game:SetGameplayInputEnabled(true)
    else
        Menu.Status.Text = Result.Message
    end
end

Menu.Refresh.Activated:Connect(function()
    RefreshId += 1
    local Id = RefreshId
    local Page = Sessions:FindServers("LAN"):Await()
    if Id ~= RefreshId then return end
    if not Page.Ok then
        Menu.Status.Text = Page.Message
        return
    end
    for _, Row in Rows do Row:Destroy() end
    table.clear(Rows)
    for _, Listing in Page.Value.Items do
        local Row = Menu.ServerRow:Clone()
        Row.Text = Listing.Name
        Row.Visible = true
        Row.Parent = Menu.Servers
        table.insert(Rows, Row)
        Row.Activated:Connect(function()
            if not Pending then Connect(Sessions:Join({Kind = "Listing", Id = Listing.Id})) end
        end)
    end
end)
Menu.Cancel.Activated:Connect(function()
    if Pending then Pending:Cancel() end
end)
Menu.Reconnect.Activated:Connect(function()
    if not Pending then Connect(Sessions:Reconnect()) end
end)

local Context = game:GetService("InputService"):AddContext("SessionMenu")
local Open = Context:AddAction("OpenSessionMenu", {Type = Enum.InputActionType.Bool})
Open:AddBinding(Enum.KeyCode.Escape, {Consume = true})
Open.Pressed:Connect(function()
    game:SetGameplayInputEnabled(false)
    game:SetMainMenuVisible(true)
    Menu.Status.Text = "Online game continues"
end)
Menu.BackToGame.Activated:Connect(function()
    if Sessions:GetSession().State == "Active" then
        game:SetMainMenuVisible(false)
        game:SetGameplayInputEnabled(true)
    end
end)
Menu.Leave.Activated:Connect(function()
    local Result = Sessions:Leave():Await()
    Menu.Status.Text = if Result.Ok then "Select a server" else Result.Message
end)
Sessions.Changed:Connect(function(Session)
    if Session.State == "Idle" and Session.LastError then
        game:SetMainMenuVisible(true)
        Menu.Status.Text = Session.LastError.Message
    end
end)
```

## Existing game with direct startup

Leave ApplicationEntry unset. The creator adds **zero menu code**. An existing gameplay Script can continue to observe normal startup, for example:

```luau
local Players = game:GetService("Players")
local Player = Players.LocalPlayer
if Player then
    print("[Game:Startup]", Player.Name)
end
```

Run the installed offline Player exactly as before. Existing distributed packages retain their endpoint requirement and initiate ordinary client bootstrap. An opt-in successor may use a browser while keeping its package audience. Package validation, Player identity and readiness rules still apply.
