local function Chart(Name)
    local Region = Instance.new("SpatialRegion")
    Region.Name = Name
    Region.Gravity = 0
    Region.Parent = workspace
    return Region
end

local function Part(Region, Position, Anchored, Collidable)
    local Body = Instance.new("Part")
    Body.Size = Vector3.new(2, 2, 2)
    Body.Anchored = Anchored
    Body.CanCollide = Collidable
    Body.CanTouch = false
    Body.CFrame = CFrame.new(Position)
    Body.Parent = Region
    return Body
end

local A, B = Chart("JointRoomA"), Chart("JointRoomB")
local MarkA = Part(A, Vector3.new(70, 1, 8), true, true)
local MarkB = Part(B, Vector3.new(70, 4, 8), true, true)
local function Cast(Region)
    return Region:SphereCast(Vector3.new(70, 10, 8), .5, Vector3.new(0, -15, 0))
end
local HitA, HitB = assert(Cast(A)), assert(Cast(B))
assert(HitA.Instance == MarkA and HitB.Instance == MarkB and HitA.Distance > HitB.Distance + 2,
    "coincident charts did not select independent native shapes")

local Start = Vector3.new(40, 0, 12)
local Anchor = Part(A, Start + Vector3.new(0, 6, 0), true, false)
local Moving = Part(B, Start, false, false)
local FixedEnd, FreeEnd = Instance.new("Attachment"), Instance.new("Attachment")
FixedEnd.Parent, FreeEnd.Parent = Anchor, Moving
local Spring = Instance.new("JointConstraint")
Spring.JointType = "Distance"
Spring.RestLength, Spring.SpringFrequency, Spring.SpringDamping = 1, 8, .8
Spring.Parent = A
Spring.Attachment0, Spring.Attachment1 = FixedEnd, FreeEnd

local function Reset()
    Moving.CFrame = CFrame.new(Start)
    Moving:SetLinearVelocity(Vector3.zero)
    Moving:SetAngularVelocity(Vector3.zero)
end
local function Still()
    assert(not Spring.Active, "a split or retired joint reported active")
    Reset()
    task.wait(.22)
    assert(math.abs(Moving.CFrame.Position.Y) < .03, "a nonparticipating native spring moved its endpoint")
end
local function Pull()
    assert(Spring.Active, "the reunited joint reported inactive")
    Reset()
    for _ = 1, 40 do
        task.wait(.025)
        if Moving.CFrame.Position.Y > 2 then return Moving.CFrame.Position.Y end
    end
    error("the reunited native spring failed to move its endpoint")
end

Still()
Moving.SpatialRegion = A
local FirstPull = Pull()
local Permitted, Reason = pcall(function() Moving.SpatialRegion = B end)
assert(not Permitted and string.find(tostring(Reason), "constrained assembly", 1, true) and
    Moving.SpatialRegion == A and Spring.Active, "one endpoint of a live assembly moved independently")

Spring.Enabled = false
Moving.SpatialRegion = B
Spring.Enabled = true
Still()
Moving.SpatialRegion = A
Spring.Parent = B
Still()
Spring.Parent = A
local Rejoined = Pull()

FreeEnd:Destroy()
Still()
local Fresh = Instance.new("Attachment")
Fresh.Parent = Moving
Spring.Attachment1 = Fresh
local FreshPull = Pull()
A.Parent = nil
assert(not Spring.Active and not pcall(function() Cast(A) end) and Cast(B).Instance == MarkB,
    "retired chart retained a live cross-chart endpoint or affected its neighbor")
A.Parent = workspace
local Restored = Pull()
assert(Cast(A).Instance == MarkA, "rejoined chart lost its native shape")
Moving.Parent = A
Moving.SpatialRegion = nil
assert(Spring.Active)
A.Parent = nil
local ImplicitActive = Spring.Active
assert(not ImplicitActive, "the enabled joint in a detached inherited chart reported active")
assert(not pcall(function() Cast(A) end) and Cast(B).Instance == MarkB)
local RetiredPose = Moving.CFrame.Position
task.wait(.15)
assert((Moving.CFrame.Position - RetiredPose).Magnitude < .001, "the detached chart continued moving its endpoint")
A.Parent = workspace
local ImplicitRestored = Pull()
print(string.format("[Reader:Joints] PASS casts=%.3f/%.3f pulls=%.3f/%.3f/%.3f/%.3f/%.3f retiredImplicitActive=%s",
    HitA.Distance, HitB.Distance, FirstPull, Rejoined, FreshPull, Restored, ImplicitRestored, tostring(ImplicitActive)))
