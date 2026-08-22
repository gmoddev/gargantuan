const c, t, p = require("@tools/classes").unpack()

return c({
	name = "Camera",
	description = "Class that defines the 3D view of a world.",
	since = "0.1.0",

	properties = {
		CameraType = {
			description = "Type of camera input",
			type = t.EnumItem("CameraType"),
			unmodified = "Enums::CameraType::Freecam",
			serializable = true,
			replicated = true,
			category = "Behavior",
		},
		CFrame = {
			description = "World transform of this instance",
			type = t.CFrame,
			unmodified = "gargantuan::CFrame()",
			serializable = true,
			replicated = true,
			category = "Transform",
			editorHint = "CFrameComponents",
		},

		ViewportSize = {
			description = "Viewport size in pixels",
			type = t.Vector2,
			unmodified = "gargantuan::Vector2()",
			category = "Camera",
			editable = false,
			writeLevel = "Engine",
		},
		AspectRatio = {
			description = "Viewport aspect ratio",
			type = t.Float,
			unmodified = "1.0f",
			overrideRead = "GetAspectRatio",
			category = "Camera",
		},

		Pitch = {
			description = "Local pitch of this camera",
			type = t.Float,
			unmodified = "0.0f",
			category = "Transform",
			editable = false,
		},
		Yaw = {
			description = "Local yaw of this camera",
			type = t.Float,
			unmodified = "0.0f",
			category = "Transform",
			editable = false,
		},
		Roll = {
			description = "Local pitch of this camera",
			type = t.Float,
			unmodified = "0.0f",
			writeLevel = "Never",
			category = "Transform",
		},

		-- TODO: Calculate unmodified values for fovies
		FieldOfView = {
			description = "Vertical field of view in degrees",
			type = t.Float,
			unmodified = "70.0f",
			serializable = true,
			replicated = true,
			category = "Camera",
			range = { minimum = 1, maximum = 120 },
		},
		HorizontalFieldOfView = {
			description = "Horizontal field of view in degrees",
			type = t.Float,
			unmodified = "70.0f",
			overrideRead = "GetHorizontalFieldOfView",
			overrideWrite = "SetHorizontalFieldOfView",
			category = "Camera",
			editable = false,
		},
		DiagonalFieldOfView = {
			description = "Horizontal field of view in degrees",
			type = t.Float,
			unmodified = "70.0f",
			overrideRead = "GetDiagonalFieldOfView",
			overrideWrite = "SetDiagonalFieldOfView",
			category = "Camera",
			editable = false,
		},
	},
})
