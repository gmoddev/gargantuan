local Terrain = game.Workspace.Terrain
local OversizeOk, Oversize = pcall(function()
	Terrain:ReadRegion(Vector3.zero, Vector3.new(17, 17, 17))
end)
assert(not OversizeOk and tostring(Oversize):find("4913 samples", 1, true))
local BrushOk, BrushError = pcall(function()
	Terrain:FillBlock(Vector3.new(12, 12, 12), Vector3.new(200, 200, 200), 3)
end)
assert(not BrushOk and tostring(BrushError):find("LimitExceeded", 1, true))

local Tickets = {}
local Started = os.clock()
for Index = 1, 4 do
	Tickets[Index] = Terrain:FillBall(Vector3.new(12 + (Index - 1) * 128, 12.3, 12), 2.4, Index)
	assert(Terrain:GetEditStatus(Tickets[Index]).Status == "Pending")
end
local QueueOk, QueueError = pcall(function()
	Terrain:FillBall(Vector3.new(524, 12.3, 12), 2.4, 5)
end)
assert(not QueueOk and tostring(QueueError):find("Wait for pending edits", 1, true))
assert(Terrain:CancelEdit(Tickets[4]))
local Finished = {}
for Step = 1, 1000 do
	task.wait()
	for Index, Ticket in Tickets do
		local Status = Terrain:GetEditStatus(Ticket)
		if Status.Status ~= "Pending" then
			assert(Status.Status == (Index == 4 and "Canceled" or "Committed"), Status.Diagnostic or Status.Status)
			assert(Status.CollisionReady == (Index ~= 4))
			Finished[Index] = Finished[Index] or (os.clock() - Started)
		end
	end
	if Finished[1] and Finished[2] and Finished[3] and Finished[4] then
		local Missing = Terrain:ReadRegion(Vector3.new(524, 12, 12), Vector3.new(525, 13, 13))
		assert(buffer.readi16(Missing, 0) == 32767 and buffer.readu32(Missing, 2) == 0)
		print("[Terrain:Admission] Four slots bounded, three commits and one canceled", table.unpack(Finished))
		print("[Terrain:Sample] Landscape collision is ready")
		return
	end
end
error("[Terrain:Admission] The bounded cohort did not retire")
