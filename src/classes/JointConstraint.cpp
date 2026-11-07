#include "gargantuan/classes/JointConstraint.hpp"
#include "gargantuan/classes/Attachment.hpp"
#include <stdexcept>
#include <utility>

namespace gargantuan {
	std::string JointConstraint::GetJointType() const {
		return JointType;
	}

	void JointConstraint::SetJointType(std::string Value) {
		AssertCanMutate();
		if (Value != "BallSocket" && Value != "Hinge" && Value != "Slider" && Value != "Distance")
			throw std::invalid_argument("[Physics:Constraint] Unsupported joint type");
		ValidatePropertyMutation("JointType", Value);
		if (JointType == Value) return;
		JointType = std::move(Value);
		NotifyPropertyCommitted("JointType");
	}

	PhysicsConstraintDesc JointConstraint::GetPhysicsConstraint(
		PhysicsBodyId BodyA,
		PhysicsBodyId BodyB
	) const {
		PhysicsConstraintKind Kind = static_cast<PhysicsConstraintKind>(255);
		if (GetJointType() == "BallSocket") Kind = PhysicsConstraintKind::BallSocket;
		else if (GetJointType() == "Hinge") Kind = PhysicsConstraintKind::Hinge;
		else if (GetJointType() == "Slider") Kind = PhysicsConstraintKind::Slider;
		else if (GetJointType() == "Distance") Kind = PhysicsConstraintKind::Distance;
		return {
			.Kind = Kind,
			.BodyA = BodyA,
			.BodyB = BodyB,
			.CollideConnected = GetCollideConnected(),
			.LocalFrameA = GetAttachment0() && *GetAttachment0() ? (*GetAttachment0())->GetCFrame() : CFrame{},
			.LocalFrameB = GetAttachment1() && *GetAttachment1() ? (*GetAttachment1())->GetCFrame() : CFrame{},
			.EnableLimit = GetEnableLimit(),
			.LowerLimit = GetLowerLimit(),
			.UpperLimit = GetUpperLimit(),
			.EnableMotor = GetEnableMotor(),
			.MotorSpeed = GetMotorSpeed(),
			.MaximumMotorForce = GetMaximumMotorForce(),
			.RestLength = GetRestLength(),
			.SpringFrequency = GetSpringFrequency(),
			.SpringDamping = GetSpringDamping(),
		};
	}
}
