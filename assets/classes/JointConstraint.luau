const c, t = require("@tools/classes").unpack()

return c({
	name = "JointConstraint",
	extends = "Constraint",
	description = "Connects two attachment frames as a ball socket, hinge, slider, or distance joint.",
	since = "0.1.0",
	properties = {
		JointType = { description = "BallSocket, Hinge, Slider, or Distance.", type = t.String, unmodified = '"BallSocket"', overrideRead = "GetJointType", overrideWrite = "SetJointType", serializable = true, replicated = true, category = "Constraint" },
		CollideConnected = { description = "Whether attached parts may collide.", type = t.Bool, unmodified = "false", serializable = true, replicated = true, category = "Constraint" },
		EnableLimit = { description = "Enable hinge angle, slider translation, or distance limits.", type = t.Bool, unmodified = "false", serializable = true, replicated = true, category = "Constraint" },
		LowerLimit = { description = "Lower limit in radians for a hinge, or world units for a slider or distance joint.", type = t.Float, unmodified = "0.0f", serializable = true, replicated = true, category = "Constraint" },
		UpperLimit = { description = "Upper limit in radians for a hinge, or world units for a slider or distance joint.", type = t.Float, unmodified = "0.0f", serializable = true, replicated = true, category = "Constraint" },
		EnableMotor = { description = "Enable the hinge or slider motor.", type = t.Bool, unmodified = "false", serializable = true, replicated = true, category = "Constraint" },
		MotorSpeed = { description = "Hinge motor radians per second or slider motor world units per second.", type = t.Float, unmodified = "0.0f", serializable = true, replicated = true, category = "Constraint" },
		MaximumMotorForce = { description = "Maximum hinge torque or slider force.", type = t.Float, unmodified = "0.0f", serializable = true, replicated = true, category = "Constraint", range = { minimum = 0 } },
		RestLength = { description = "Unstretched distance between attachment points.", type = t.Float, unmodified = "1.0f", serializable = true, replicated = true, category = "Constraint", range = { minimum = 0.001 } },
		SpringFrequency = { description = "Distance spring frequency in hertz; zero means a rigid distance joint.", type = t.Float, unmodified = "0.0f", serializable = true, replicated = true, category = "Constraint", range = { minimum = 0, maximum = 15 } },
		SpringDamping = { description = "Distance spring damping ratio.", type = t.Float, unmodified = "0.7f", serializable = true, replicated = true, category = "Constraint", range = { minimum = 0, maximum = 2 } },
	},
})
