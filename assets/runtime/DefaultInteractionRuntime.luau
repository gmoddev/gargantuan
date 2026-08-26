local ActionMap = game:GetService("ActionMap")
local InteractionService = game:GetService("InteractionService")

local BindingIds = {
	ActionMap:BindKey("Interact", Enum.KeyCode.E, 1, 100, true),
	ActionMap:BindKey("Interact", Enum.KeyCode.ButtonA, 1, 100, true),
}

local Connections = {}
local Root = nil

if InteractionService.DefaultPresentationEnabled then
	Root = Instance.new("ScreenGui")
	Root.Name = "DefaultInteractionGui"
	Root.Archivable = false
	Root.DisplayOrder = 1000
	Root.Parent = game

	local Panel = Instance.new("TextButton")
	Panel.Name = "Prompt"
	Panel.Archivable = false
	Panel.AnchorPoint = Vector2.new(0.5, 1)
	Panel.Position = UDim2.new(0.5, 0, 1, -40)
	Panel.Size = UDim2.new(0, 300, 0, 88)
	Panel.BackgroundColor3 = Color3.new(0.055, 0.067, 0.09)
	Panel.BackgroundTransparency = 0.08
	Panel.TextColor3 = Color3.new(0.96, 0.97, 1)
	Panel.TextSize = 20
	Panel.TextWrapped = true
	Panel.Interactable = true
	Panel.Selectable = true
	Panel.InputSink = Enum.InputSink.Activate
	Panel.Visible = false
	Panel.Parent = Root

	local ProgressTrack = Instance.new("Frame")
	ProgressTrack.Name = "HoldTrack"
	ProgressTrack.Archivable = false
	ProgressTrack.Position = UDim2.new(0, 16, 1, -12)
	ProgressTrack.Size = UDim2.new(1, -32, 0, 5)
	ProgressTrack.BackgroundColor3 = Color3.new(0.18, 0.2, 0.25)
	ProgressTrack.BackgroundTransparency = 0.1
	ProgressTrack.Visible = false
	ProgressTrack.Parent = Panel

	local ProgressFill = Instance.new("Frame")
	ProgressFill.Name = "Fill"
	ProgressFill.Archivable = false
	ProgressFill.Size = UDim2.new(0, 0, 1, 0)
	ProgressFill.BackgroundColor3 = Color3.new(0.27, 0.72, 1)
	ProgressFill.Parent = ProgressTrack

	local function Update()
		local Available = InteractionService.Available
		Panel.Visible = Available
		if not Available then
			ProgressTrack.Visible = false
			ProgressFill.Size = UDim2.new(0, 0, 1, 0)
			return
		end

		local ObjectText = InteractionService.ObjectText
		local Suffix = if #ObjectText > 0 then "\n" .. ObjectText else ""
		Panel.Text = "[ " .. InteractionService.InputHint .. " ] " .. InteractionService.ActionText .. Suffix
		Panel.AccessibleName = InteractionService.ActionText .. (if #ObjectText > 0 then " " .. ObjectText else "")
		ProgressTrack.Visible = InteractionService.HoldDuration > 0
		ProgressFill.Size = UDim2.new(InteractionService.HoldProgress, 0, 1, 0)
	end

	table.insert(Connections, InteractionService.PresentationChanged:Connect(Update))
	table.insert(
		Connections,
		Panel.PointerDown:Connect(function()
			InteractionService:BeginActivation()
		end)
	)
	table.insert(
		Connections,
		Panel.PointerUp:Connect(function()
			InteractionService:EndActivation()
		end)
	)
	Update()
end

script.Destroying:Connect(function()
	InteractionService:EndActivation()
	for _, Connection in Connections do
		Connection:Disconnect()
	end
	table.clear(Connections)
	for _, BindingId in BindingIds do
		ActionMap:Unbind(BindingId)
	end
	if Root then
		Root:Destroy()
		Root = nil
	end
end)
