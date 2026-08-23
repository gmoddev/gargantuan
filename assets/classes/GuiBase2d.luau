const c, t, p = require("@tools/classes").unpack()

return c({
	name = "GuiBase2d",
	extends = "GuiBase",
	description = "Intermediate abstract class for instances that compose 2D graphical user interface",
	abstract = true,
	since = "0.1.0",

	properties = {
		AbsolutePosition = {
			description = "Screen position of this instance in logical pixels",
			type = t.Vector2,
			unmodified = "gargantuan::Vector2(0.0f, 0.0f)",
			writeLevel = "Never",
		},
		AbsoluteRotation = {
			description = "Screen rotation of this instance in degrees",
			type = t.Float,
			unmodified = "0.0f",
			writeLevel = "Never",
		},
		AbsoluteSize = {
			description = "Screen size of this instance in logical pixels",
			type = t.Vector2,
			unmodified = "gargantuan::Vector2(0.0f, 0.0f)",
			writeLevel = "Never",
		},
		AccessibleName = {
			description = "Renderer-independent accessible label for this object",
			type = t.String,
			unmodified = '""',
			serializable = true,
			replicated = true,
		},
		AccessibilityRole = {
			description = "Explicit semantic accessibility role, or Automatic for class semantics",
			type = t.EnumItem("AccessibilityRole"),
			unmodified = "Enums::AccessibilityRole::Automatic",
			serializable = true,
			replicated = true,
		},
		AccessibilitySelected = {
			description = "Whether this semantic accessibility node is selected",
			type = t.Bool,
			unmodified = "false",
			serializable = true,
			replicated = true,
		},
		Opacity = {
			description = "Local opacity multiplied through descendants",
			type = t.Float,
			unmodified = "1.0f",
			serializable = true,
			replicated = true,
			range = { minimum = 0, maximum = 1 },
		},
	},

	signals = {
		PointerEnter = {
			description = "Fired when a pointer begins hovering this object",
			args = { t.Instance("GuiInputEvent") },
		},
		PointerLeave = {
			description = "Fired when a pointer stops hovering this object",
			args = { t.Instance("GuiInputEvent") },
		},
		PointerMoved = {
			description = "Fired during routed pointer movement",
			args = { t.Instance("GuiInputEvent") },
		},
		PointerDown = {
			description = "Fired during capture, target, and bubble pointer-down routing",
			args = { t.Instance("GuiInputEvent") },
		},
		PointerUp = {
			description = "Fired during capture, target, and bubble pointer-up routing",
			args = { t.Instance("GuiInputEvent") },
		},
		Activated = {
			description = "Fired when this semantic control is activated",
			args = {},
		},
		Focused = {
			description = "Fired when this object receives focus",
			args = {},
		},
		FocusLost = {
			description = "Fired when this object loses focus",
			args = {},
		},
	},
})
