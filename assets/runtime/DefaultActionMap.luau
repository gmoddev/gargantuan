return function(ActionMap, ConsumeNativeCamera)
	local Input = game:GetService("InputService")
	local BindingIds = {}
	local Existing = {}
	for _, Context in Input:GetChildren() do
		if Context:IsA("InputContext") then
			for _, Action in Context:GetChildren() do
				if Action:IsA("InputAction") then Existing[Action.Name] = true end
			end
		end
	end
	local function Key(Name, Code)
		if not Existing[Name] then
			table.insert(BindingIds, ActionMap:BindKey(Name, Code, 1, 0, ConsumeNativeCamera))
		end
	end
	Key("MoveForward", Enum.KeyCode.W)
	Key("MoveBackward", Enum.KeyCode.S)
	Key("MoveLeft", Enum.KeyCode.A)
	Key("MoveRight", Enum.KeyCode.D)
	Key("Jump", Enum.KeyCode.Space)
	if not Existing.CameraOrbit then
		table.insert(BindingIds, ActionMap:BindMouseButton("CameraOrbit", Enum.UserInputType.MouseButton2, 1, 0, ConsumeNativeCamera))
	end
	if not Existing.Look then
		table.insert(BindingIds, ActionMap:BindPointerDelta("Look", Vector2.one, 0, ConsumeNativeCamera))
	end

	return function()
		for _, BindingId in BindingIds do
			ActionMap:Unbind(BindingId)
		end
	end
end
