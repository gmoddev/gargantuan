const c, t, p = require("@tools/classes").unpack()

return c({
	name = "GuiInputEvent",
	description = "Runtime-only pointer routing data with explicit propagation consumption",
	abstract = true,
	since = "0.2.0",

	properties = {
		Target = {
			description = "Original routed target",
			type = t.Instance(),
			unmodified = "std::shared_ptr<gargantuan::Instance>{}",
			writeLevel = "Never",
		},
		CurrentTarget = {
			description = "Object currently receiving this route phase",
			type = t.Instance(),
			unmodified = "std::shared_ptr<gargantuan::Instance>{}",
			writeLevel = "Never",
		},
		PointerId = {
			description = "Stable pointer contact identity for the event",
			type = t.Integer,
			unmodified = "0",
			writeLevel = "Never",
		},
		Position = {
			description = "Logical viewport position",
			type = t.Vector2,
			unmodified = "gargantuan::Vector2(0.0f, 0.0f)",
			writeLevel = "Never",
		},
		PointerType = {
			description = "Kind of pointer producing the event",
			type = t.EnumItem("GuiPointerType"),
			unmodified = "Enums::GuiPointerType::Mouse",
			writeLevel = "Never",
		},
		Button = {
			description = "Pointer button semantic",
			type = t.EnumItem("GuiPointerButton"),
			unmodified = "Enums::GuiPointerButton::Primary",
			writeLevel = "Never",
		},
		Phase = {
			description = "Current propagation phase",
			type = t.EnumItem("GuiEventPhase"),
			unmodified = "Enums::GuiEventPhase::Target",
			writeLevel = "Never",
		},
		Handled = {
			description = "Whether propagation was consumed",
			type = t.Bool,
			unmodified = "false",
			writeLevel = "Never",
		},
	},

	methods = {
		Consume = {
			description = "Consumes this route and prevents later propagation phases",
		},
	},
})
