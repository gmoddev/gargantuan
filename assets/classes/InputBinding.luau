const c, t = require("@tools/classes").unpack()

return c({
	name = "InputBinding",
	description = "A persistent physical binding with four normalized modifier slots.",
	since = "0.2.0",
	properties = {
		Enabled = { description = "Whether this binding may activate its action.", type = t.Bool, unmodified = "true", serializable = true, replicated = true, category = "Input" },
		Type = { description = "The physical binding kind.", type = t.EnumItem("InputBindingType"), unmodified = "Enums::InputBindingType::Key", overrideRead = "GetType", overrideWrite = "SetType", serializable = true, replicated = true, category = "Input" },
		KeyCode = { description = "The primary key or supported gamepad button; None is incomplete.", type = t.EnumItem("KeyCode"), unmodified = "Enums::KeyCode::None", overrideRead = "GetKeyCode", overrideWrite = "SetKeyCode", serializable = true, replicated = true, category = "Input" },
		InputType = { description = "The mouse input kind for mouse bindings.", type = t.EnumItem("UserInputType"), unmodified = "Enums::UserInputType::None", overrideRead = "GetInputType", overrideWrite = "SetInputType", serializable = true, replicated = true, category = "Input" },
		Modifier1 = { description = "First required left/right modifier, or None.", type = t.EnumItem("KeyCode"), unmodified = "Enums::KeyCode::None", overrideRead = "GetModifier1", overrideWrite = "SetModifier1", serializable = true, replicated = true, category = "Modifiers" },
		Modifier2 = { description = "Second required left/right modifier, or None.", type = t.EnumItem("KeyCode"), unmodified = "Enums::KeyCode::None", overrideRead = "GetModifier2", overrideWrite = "SetModifier2", serializable = true, replicated = true, category = "Modifiers" },
		Modifier3 = { description = "Third required left/right modifier, or None.", type = t.EnumItem("KeyCode"), unmodified = "Enums::KeyCode::None", overrideRead = "GetModifier3", overrideWrite = "SetModifier3", serializable = true, replicated = true, category = "Modifiers" },
		Modifier4 = { description = "Fourth required left/right modifier, or None.", type = t.EnumItem("KeyCode"), unmodified = "Enums::KeyCode::None", overrideRead = "GetModifier4", overrideWrite = "SetModifier4", serializable = true, replicated = true, category = "Modifiers" },
		ExactModifiers = { description = "Reject additional held modifier keys.", type = t.Bool, unmodified = "false", serializable = true, replicated = true, category = "Modifiers" },
		Scale = { description = "Finite scalar contribution.", type = t.Float, unmodified = "1.0f", serializable = true, replicated = true, category = "Input", range = {} },
		VectorScale = { description = "Finite relative-pointer contribution scale.", type = t.Vector2, unmodified = "Vector2(1.0f, 1.0f)", serializable = true, replicated = true, category = "Input", range = {} },
		Priority = { description = "Priority within the context.", type = t.Integer, unmodified = "0", serializable = true, replicated = true, category = "Input" },
		Consume = { description = "Consume this physical event even when the context does not consume.", type = t.Bool, unmodified = "false", serializable = true, replicated = true, category = "Input" },
	},
})
