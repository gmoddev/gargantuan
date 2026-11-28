-- A server-approved inspection body gives each recipient a real, private possession focus near the freight line.
local Tours = {}
Probe.OnPlayerEvent:Connect(function(Player, Request)
	if not Roles[Player] or (Request~="freight-visit" and Request~="freight-return") then return end
	local Tour=Tours[Player]
	if Request=="freight-visit" then
		if not Tour then
			local CameraCart=Instance.new("Part")
			CameraCart.Name="FreightInspector"..tostring(Player.PlayerId)
			CameraCart.Size=Vector3.new(.5,.5,.5)
			CameraCart.Anchored=true
			CameraCart.CanCollide=false
			CameraCart.CFrame=CFrame.new(22,3,1)
			CameraCart.Parent=Space
			Tour={Previous=Player.PossessedEntity, Cart=CameraCart}
			Tours[Player]=Tour
			Player:SetPossession(CameraCart)
		end
		if Tour.Cart and Player.PossessedEntity==Tour.Cart then Probe:TryFireClient(Player,"near-freight") end
	elseif Tour then
		if Tour.Cart then
			assert(Player.PossessedEntity==Tour.Cart,"Freight visitor changed possession before return")
			Player:SetPossession(Tour.Previous)
			Tour.Cart:Destroy()
			Tour.Cart=nil
		end
		Probe:TryFireClient(Player,"original-yard")
	end
end)
Players.PlayerRemoving:Connect(function(Player)
	local Tour=Tours[Player]
	if Tour and Tour.Cart then Tour.Cart:Destroy() end
	Tours[Player]=nil
end)
