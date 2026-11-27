-- Server Script in a project with its regular character and interaction controls.
local Room = Instance.new("SpatialRegion")
Room.Name = "ReaderTransferRoom"
Room.Parent = workspace

local function BuildFloor(Owner, Key)
    local Floor = Instance.new("BlockGrid")
    Floor.Name = "ReaderDoorFloor"
    Floor.Parent = Owner
    Floor:Fill(Vector3.new(-9, 0, -9), Vector3.new(10, 1, 10), Key)
end
BuildFloor(workspace, "Stone")
BuildFloor(Room, "Brick")

local Landing = Vector3.new(0, 5, 0)
local HalfSize = Vector3.new(4, 4, 4)
local Minimum, Maximum = Landing - HalfSize, Landing + HalfSize
local Visits = setmetatable({}, { __mode = "k" })

local function RequestTrip(Player, Destination, StillAllowed)
    local Hold, Reason = Destination:RequestCollisionArea(Minimum, Maximum, 12, "Interactive")
    if not Hold then return Reason or "NoCollisionObservation" end
    local Trip
    local Ok, Disposition = pcall(function()
        local Status = Hold:GetStatus()
        for _ = 1, 100 do
            if Status.Life ~= "Active" or Status.Collision ~= "Pending" then break end
            task.wait(0.05)
            Status = Hold:GetStatus()
        end
        if Status.Life ~= "Active" or Status.Collision ~= "Ready" then
            return Status.Life .. "/" .. Status.Collision
        end
        assert(type(Status.Revision) == "string")
        print("[Reader:Door] collision ready", Destination.Name, Status.Revision)
        if not StillAllowed() then return "NoLongerAtDoor" end
        local Admission, Rejection = Player:RequestSpatialTransfer(Destination, CFrame.new(Landing), Minimum, Maximum)
        if not Admission then return Rejection or "NoAdmission" end
        Trip = Admission
        for _ = 1, 160 do
            local State = Admission:GetStatus()
            if State ~= "Pending" then return State end
            task.wait(0.05)
        end
        return Admission:Cancel()
    end)
    Hold:Release()
    if Ok then return Disposition end
    warn("[Reader:Door]", Disposition)
    if Trip then
        local Canceled, Final = pcall(function() return Trip:Cancel() end)
        return if Canceled then Final else "NeedsReconciliation"
    end
    return "Failed"
end

local function InstallDoor(Source, Destination, Entering)
    local Marker = Instance.new("Part")
    Marker.Name = if Entering then "ReaderEntrance" else "ReaderExit"
    Marker.Anchored = true
    Marker.CanCollide = false
    Marker.Size = Vector3.new(2, 3, 1)
    Marker.Position = Vector3.new(5, 2.5, 0)
    Marker.Color = if Entering then Color3.fromRGB(55, 132, 229) else Color3.fromRGB(231, 142, 56)
    Marker.Parent = Source
    local Prompt = Instance.new("ProximityPrompt")
    Prompt.ActionText = if Entering then "Enter Workshop" else "Return Outside"
    Prompt.ObjectText = "Workshop door"
    Prompt.MaxActivationDistance = 9
    Prompt.Parent = Marker

    Prompt.Triggered:Connect(function(Player)
        local Character = Player.Character
        if not Character or (Character.Position - Marker.Position).Magnitude > Prompt.MaxActivationDistance then return end
        local Visit = Visits[Player]
        if not Visit or Visit.Character ~= Character then
            Visit = { Character = Character, Inside = false, Busy = false, Unconfirmed = false }
            Visits[Player] = Visit
        end
        if Visit.Busy or Visit.Unconfirmed or Visit.Inside == Entering then return end
        Visit.Busy = true
        task.spawn(function()
            local function StillAllowed()
                return Player.Character == Character and Visits[Player] == Visit and Visit.Inside ~= Entering
                    and not Visit.Unconfirmed and (Character.Position - Marker.Position).Magnitude <= Prompt.MaxActivationDistance
            end
            local Ok, State = pcall(RequestTrip, Player, Destination, StillAllowed)
            Visit.Busy = false
            if not Ok then
                Visit.Unconfirmed = true
                warn("[Reader:Door]", State)
            elseif State == "Applied" and Player.Character == Character then
                Visit.Inside = Entering
                Player:SetAttribute("ReaderDoorInside", Entering)
                print("[Reader:Door] PASS Applied", Destination.Name)
            elseif State == "CommittedUnconfirmed" or State == "NeedsReconciliation" then
                Visit.Unconfirmed = true
                warn("[Reader:Door] reconcile this character before another request", State)
            else
                warn("[Reader:Door] transfer ended", State)
            end
        end)
    end)
end
InstallDoor(workspace, Room, true)
InstallDoor(Room, workspace, false)
print("[Reader:Door] installed both prompts; use the blue entrance near the default origin")
