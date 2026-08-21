return function(ActionMap, ConsumeNativeCamera)
	local BindingIds = {
		ActionMap:BindKey("MoveForward", Enum.KeyCode.W, 1, 0, ConsumeNativeCamera),
		ActionMap:BindKey("MoveBackward", Enum.KeyCode.S, 1, 0, ConsumeNativeCamera),
		ActionMap:BindKey("MoveLeft", Enum.KeyCode.A, 1, 0, ConsumeNativeCamera),
		ActionMap:BindKey("MoveRight", Enum.KeyCode.D, 1, 0, ConsumeNativeCamera),
		ActionMap:BindKey("Jump", Enum.KeyCode.Space, 1, 0, ConsumeNativeCamera),
		ActionMap:BindMouseButton("CameraOrbit", Enum.UserInputType.MouseButton2, 1, 0, ConsumeNativeCamera),
		ActionMap:BindPointerDelta("Look", Vector2.one, 0, ConsumeNativeCamera),
	}

	return function()
		for _, BindingId in BindingIds do
			ActionMap:Unbind(BindingId)
		end
	end
end
