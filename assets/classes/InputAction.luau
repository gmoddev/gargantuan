const c, t = require("@tools/classes").unpack()

return c({
	name = "InputAction",
	description = "A persistent semantic action whose local runtime state is inspectable.",
	since = "0.2.0",
	properties = {
		Enabled = { description = "Whether this action may activate.", type = t.Bool, unmodified = "true", serializable = true, replicated = true, category = "Input" },
		Type = { description = "The semantic action value type.", type = t.EnumItem("InputActionType"), unmodified = "Enums::InputActionType::Bool", overrideRead = "GetType", overrideWrite = "SetType", serializable = true, replicated = true, category = "Input" },
	},
	signals = {
		Pressed = { description = "Fired once when the action becomes active." },
		Released = { description = "Fired once when an active action ends." },
		Changed = { description = "Fired after the current action value changes.", args = { t.Vector2 } },
		Canceled = { description = "Fired with a bounded reason when activation is canceled.", args = { t.String } },
	},
	methods = {
		AddBinding = { description = "Creates a canonical binding with up to four modifier keys.", asLuau = true, luauSignature = "(KeyCode: Enum.KeyCode, Options: { Modifiers: { Enum.KeyCode }?, Enabled: boolean?, ExactModifiers: boolean?, Scale: number?, Priority: number?, Consume: boolean? }?) -> InputBinding" },
		IsDown = { description = "Returns the local active state.", returns = t.Bool },
		GetValue = { description = "Returns the local scalar value.", returns = t.Float },
		GetVector = { description = "Returns the local two-component value.", returns = t.Vector2 },
	},
})
