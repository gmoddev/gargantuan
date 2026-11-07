-- Three authored mechanisms using the same WorldRoot constraint path as gameplay scripts.
local Workspace = game:GetService("Workspace")
Workspace.Gravity = 18

local function MakePart(Name, Position, Size, Anchored)
	local Part = Instance.new("Part")
	Part.Name = Name
	Part.Position = Position
	Part.Size = Size
	Part.Anchored = Anchored
	Part.Parent = Workspace
	return Part
end

local function MakeJoint(Name, Kind, PartA, PartB, FrameA, FrameB)
	local AttachmentA = Instance.new("Attachment")
	AttachmentA.CFrame = FrameA
	AttachmentA.Parent = PartA
	local AttachmentB = Instance.new("Attachment")
	AttachmentB.CFrame = FrameB
	AttachmentB.Parent = PartB
	local Joint = Instance.new("JointConstraint")
	Joint.Name = Name
	Joint.JointType = Kind
	Joint.Attachment0 = AttachmentA
	Joint.Attachment1 = AttachmentB
	Joint.CollideConnected = false
	Joint.Parent = Workspace
	return Joint
end

local DoorFrame = MakePart("DoorFrame", Vector3.new(-8, 8, 0), Vector3.new(1, 8, 1), true)
local Door = MakePart("Door", Vector3.new(-6, 8, 0), Vector3.new(3, 7, 0.4), false)
local Hinge = MakeJoint("DoorHinge", "Hinge", DoorFrame, Door,
	CFrame.new(0.5, 0, 0), CFrame.new(-1.5, 0, 0))
Hinge.EnableLimit = true
Hinge.LowerLimit = -1.2
Hinge.UpperLimit = 1.2

local Rail = MakePart("Rail", Vector3.new(4, 8, 0), Vector3.new(5, 0.5, 1), true)
local SliderBody = MakePart("SliderBody", Vector3.new(4, 8, 0), Vector3.new(1, 1, 1), false)
local Slider = MakeJoint("DrivenSlider", "Slider", Rail, SliderBody, CFrame.new(), CFrame.new())
Slider.EnableLimit = true
Slider.LowerLimit = -2
Slider.UpperLimit = 2
Slider.EnableMotor = true
Slider.MotorSpeed = 1
Slider.MaximumMotorForce = 1000

local SpringMount = MakePart("SpringMount", Vector3.new(14, 12, 0), Vector3.new(1, 1, 1), true)
local SpringWeight = MakePart("SpringWeight", Vector3.new(14, 8, 0), Vector3.new(1, 1, 1), false)
local Spring = MakeJoint("SuspendedWeight", "Distance", SpringMount, SpringWeight,
	CFrame.new(), CFrame.new())
Spring.RestLength = 2
Spring.SpringFrequency = 8
Spring.SpringDamping = 0.8
