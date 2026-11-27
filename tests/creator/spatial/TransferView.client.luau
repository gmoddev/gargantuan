-- Client Script paired with TransferDoor. Run the normal controller locally.
local Players = game:GetService("Players")
local Player = Players.LocalPlayer
for _ = 1, 200 do
    if Player then break end
    task.wait(0.05)
    Player = Players.LocalPlayer
end
assert(Player, "requires a local controlled player")

local function Refresh()
    local Inside = Player:GetAttribute("ReaderDoorInside")
    if Inside == nil then return end
    if Inside == false then
        workspace.CurrentSpatialRegion = nil
        assert(workspace.CurrentSpatialRegion == nil)
        print("[Reader:View] PASS confirmed outside view")
        return
    end
    for _ = 1, 200 do
        if Player:GetAttribute("ReaderDoorInside") ~= true then return end
        local Room = workspace:FindFirstChild("ReaderTransferRoom")
        if Room and Room:IsA("SpatialRegion") then
            workspace.CurrentSpatialRegion = Room
            assert(workspace.CurrentSpatialRegion == Room)
            print("[Reader:View] PASS confirmed interior view")
            return
        end
        task.wait(0.05)
    end
    warn("[Reader:View] confirmed room has not been published to this view")
end

Player:GetAttributeChangedSignal("ReaderDoorInside"):Connect(function() task.spawn(Refresh) end)
Refresh()
