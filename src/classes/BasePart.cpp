#define GLM_ENABLE_EXPERIMENTAL

#include "gargantuan/classes/BasePart.hpp"
#include "gargantuan/datatypes/CFrame.hpp"
#include "gargantuan/render/RenderDirtyAccumulator.hpp"

#include <glm/gtx/euler_angles.hpp>
#include <cmath>

namespace gargantuan {
	namespace {
		[[nodiscard]] bool ValidPhysicsVector(glm::vec3 Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z) &&
				glm::dot(Value, Value) <= 1.0e12f;
		}
	}
	glm::vec3 BasePart::GetPosition() const {
		return GetCFrame().Position;
	}

	void BasePart::SetPosition(glm::vec3 value) {
		ValidatePropertyMutation("Position", value);
		SetCFrame(gargantuan::CFrame(value, GetCFrame().Rotation));
	}

	glm::vec3 BasePart::GetRotation() const {
		float rx, ry, rz;
		glm::extractEulerAngleYXZ(glm::mat4(GetCFrame().Rotation), ry, rx, rz);
		return glm::vec3(rx, ry, rz);
	}

	void BasePart::SetRotation(glm::vec3 value) {
		ValidatePropertyMutation("Rotation", value);
		glm::mat3 rotMatrix = glm::mat3(glm::eulerAngleYXZ(value.y, value.x, value.z));
		SetCFrame(gargantuan::CFrame(GetCFrame().Position, rotMatrix));
	}

	void BasePart::ApplyImpulse(glm::vec3 Impulse) {
		AssertCanMutate();
		const auto Candidate = AccumulatedImpulse + Impulse;
		if (ValidPhysicsVector(Impulse) && ValidPhysicsVector(Candidate)) AccumulatedImpulse = Candidate;
	}

	void BasePart::ApplyAngularImpulse(glm::vec3 Impulse) {
		AssertCanMutate();
		const auto Candidate = AccumulatedAngularImpulse + Impulse;
		if (ValidPhysicsVector(Impulse) && ValidPhysicsVector(Candidate)) AccumulatedAngularImpulse = Candidate;
	}

	void BasePart::ApplyImpulseAtPosition(glm::vec3 Impulse, glm::vec3 Position) {
		AssertCanMutate();
		if (!ValidPhysicsVector(Impulse) || !ValidPhysicsVector(Position) ||
			AccumulatedPointImpulses.size() >= MaximumPointImpulsesPerStep) return;
		AccumulatedPointImpulses.emplace_back(Impulse, Position);
	}

	void BasePart::ApplyForce(glm::vec3 Force) {
		AssertCanMutate();
		const auto Candidate = AccumulatedForce + Force;
		if (ValidPhysicsVector(Force) && ValidPhysicsVector(Candidate)) AccumulatedForce = Candidate;
	}

	void BasePart::ApplyTorque(glm::vec3 Torque) {
		AssertCanMutate();
		const auto Candidate = AccumulatedTorque + Torque;
		if (ValidPhysicsVector(Torque) && ValidPhysicsVector(Candidate)) AccumulatedTorque = Candidate;
	}

	void BasePart::SetLinearVelocity(glm::vec3 Velocity) {
		AssertCanMutate();
		if (ValidPhysicsVector(Velocity)) PendingLinearVelocity = Velocity;
	}

	void BasePart::SetAngularVelocity(glm::vec3 Velocity) {
		AssertCanMutate();
		if (ValidPhysicsVector(Velocity)) PendingAngularVelocity = Velocity;
	}

	void BasePart::SetAwake(bool Awake) {
		AssertCanMutate();
		PendingAwake = Awake;
	}

	glm::vec3 BasePart::GetLinearVelocity() {
		return PendingLinearVelocity.value_or(PublishedPhysicsState.LinearVelocity);
	}
	glm::vec3 BasePart::GetAngularVelocity() {
		return PendingAngularVelocity.value_or(PublishedPhysicsState.AngularVelocity);
	}
	float BasePart::GetMass() { return PublishedPhysicsState.Mass; }
	glm::vec3 BasePart::GetLocalInertia() { return PublishedPhysicsState.LocalInertia; }
	bool BasePart::GetAwake() { return PendingAwake.value_or(PublishedPhysicsState.Awake); }

	void BasePart::SetCharacterSimulationCFrame(const gargantuan::CFrame &Value) {
		if (CFrame.FuzzyEq(Value)) return;
		CFrame = Value;
		RenderDirtyAccumulator::Get().Mark(
			GetReplicationScopeId(), GetObjectId(), RenderUpdateDomain::Transform);
		if (auto Found = PropertyChangedSignals.find("CFrame"); Found != PropertyChangedSignals.end() && Found->second)
			Found->second->Fire({});
	}

	gargantuan::CFrame BasePart::GetRenderCFrame() const {
		return CharacterPresentationOffset ? GetCFrame() * *CharacterPresentationOffset : GetCFrame();
	}

	void BasePart::SetCharacterPresentationOffset(std::optional<gargantuan::CFrame> Value) {
		if ((!CharacterPresentationOffset && !Value) ||
			(CharacterPresentationOffset && Value && CharacterPresentationOffset->FuzzyEq(*Value)))
			return;
		CharacterPresentationOffset = std::move(Value);
		RenderDirtyAccumulator::Get().Mark(
			GetReplicationScopeId(), GetObjectId(), RenderUpdateDomain::Transform);
	}

}
