-- Appended to the unmodified Yard Script in a disposable Play session. The real
-- deck, terrain, clock and door functions run normally. Only terminal ticket
-- scheduling is controlled so overlaps, expiry and delayed cancellation are repeatable.
task.spawn(function()
	local Platform = Space.FreightPlatform
	local function Await(Label, Predicate)
		for _ = 1, 180 do
			if Predicate() then return end
			task.wait(.02)
		end
		error("[Spatial:YardInterlock] timeout " .. Label)
	end
	local function Recipient(Deferred)
		local Ticket = {Status="Pending"}
		function Ticket:GetStatus() return self.Status end
		function Ticket:Cancel()
			if not Deferred then self.Status = "Canceled" end
		end
		local Owner = {}
		function Owner:RequestSpatialTransfer(Destination, _, Low, High)
			assert(Destination == Space and Low.X < High.X and Platform.MotionMode == "Static")
			return Ticket
		end
		return Owner, Ticket
	end
	Await("initial dynamic service", function()
		return Platform.MotionMode == "Dynamic" and Players.LocalPlayer and Players.LocalPlayer.Character and
			Players.LocalPlayer.Character.RootPart
	end)
	assert(not Space:GetAttribute("FreightLandingHeld") and not next(PendingDoors))
	local Avatar = Players.LocalPlayer.Character
	local A, First = Recipient(false)
	local B, Second = Recipient(true)
	UseDoor(A, Avatar, true)
	local Old = PendingDoors[A]
	assert(Old and Old.Ticket == First and Platform.MotionMode == "Static" and Space:GetAttribute("FreightLandingHeld"))
	UseDoor(B, Avatar, true)
	assert(PendingDoors[B].Ticket == Second)
	local Parked = Platform.CFrame.Position
	CancelDoor(A)
	assert(not PendingDoors[A] and First.Status == "Canceled" and Space:GetAttribute("FreightLandingHeld"))
	CancelDoor(B)
	assert(PendingDoors[B] and Second.Status == "Pending" and Space:GetAttribute("FreightLandingHeld"))
	task.wait(.35)
	assert(Platform.MotionMode == "Static" and (Platform.CFrame.Position - Parked).Magnitude < .0001)
	local C, Third = Recipient(false)
	UseDoor(C, Avatar, true)
	EndDoor(C, Old)
	assert(PendingDoors[C].Ticket == Third)
	Second.Status = "Expired"
	Await("deferred cancel completes", function() return not PendingDoors[B] end)
	assert(Space:GetAttribute("FreightLandingHeld") and Platform.MotionMode == "Static")
	Third.Status = "Expired"
	Await("last expired hold releases", function()
		return not next(PendingDoors) and not Space:GetAttribute("FreightLandingHeld") and Platform.MotionMode == "Dynamic"
	end)
	Await("rigid service resumes", function() return math.abs(Platform.CFrame.Position.X - Parked.X) > .25 end)
	assert(Platform:GetAssemblyState().Mass == 64 and Platform:GetBlock(V(0,1,0)).Definition == "Stone")
	local Refusing = {}
	function Refusing:RequestSpatialTransfer() return nil, "Denied" end
	UseDoor(Refusing, Avatar, true)
	assert(not next(PendingDoors) and not Space:GetAttribute("FreightLandingHeld"))
	Await("denied request resumes", function() return Platform.MotionMode == "Dynamic" end)
	print("[Spatial:YardInterlock] holders=3 maxConcurrent=2 cancel=1 deferred=1 expired=2 denied=1 resumed=true mass=64")
end)
