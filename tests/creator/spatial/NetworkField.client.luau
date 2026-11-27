-- Client Script for a real Distributed Player in the server's project.
-- A full offline world cannot satisfy the required unknown-distant-cell check.
local Near = Vector3.new(2, 0, 2)
local Air = Vector3.new(3, 0, 2)
local Far = Vector3.new(8192, 0, 2)
local Unit = Vector3.new(1, 1, 1)
for _ = 1, 400 do
    local Grid = workspace:FindFirstChild("ReaderNetworkField")
    if Grid and Grid:IsA("BlockGrid") and Grid:IsRegionAvailable(Near, Near + Unit)
        and Grid:IsRegionAvailable(Air, Air + Unit) and not Grid:IsRegionAvailable(Far, Far + Unit) then
        local NearBlock = Grid:GetBlock(Near)
        if NearBlock and NearBlock.Definition == "Stone" then
            assert(Grid:GetBlock(Air) == nil, "a covered neighboring cell should be air")
            local ReadFar = pcall(function() return Grid:GetBlock(Far) end)
            assert(not ReadFar, "a valid unseen occupied cell must raise instead of reading as air")
            print("[Reader:NetworkReplica] PASS known Stone, verified air, and unavailable distant Brick")
            return
        end
    end
    task.wait(0.05)
end
error("[Reader:NetworkReplica] partial coverage was not observed for the stationary client")
