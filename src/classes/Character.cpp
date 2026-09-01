#include "gargantuan/classes/Character.hpp"

#include "gargantuan/classes/BasePart.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"
#include "gargantuan/scripting/StackValue.hpp"

#include <cmath>
#include <chrono>
#include <glm/gtc/quaternion.hpp>
#include <lua.h>
#include <lualib.h>
#include <stdexcept>

namespace gargantuan {
	namespace {
		bool IsFinite(const glm::vec3 &Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		bool IsFinite(const CFrame &Value) {
			if (!IsFinite(Value.Position)) return false;
			for (int Column = 0; Column < 3; ++Column)
				for (int Row = 0; Row < 3; ++Row)
					if (!std::isfinite(Value.Rotation[Column][Row])) return false;
			return true;
		}
	}

	CFrame Character::GetCFrame() const {
		return Transform;
	}

	void Character::SetCFrame(CFrame Value) {
		AssertCanMutate();
		ValidatePropertyMutation("CFrame", Value);
		if (!IsFinite(Value)) throw std::invalid_argument("[Character:Authority] CFrame must be finite");
		if (Transform.FuzzyEq(Value)) return;
		Transform = Value;
		NotifyPropertyCommitted("CFrame");
		NotifyPropertyCommitted("Position");
		SynchronizeRootPart();
	}

	glm::vec3 Character::GetPosition() const {
		return Transform.Position;
	}

	void Character::SetPosition(glm::vec3 Value) {
		if (!IsFinite(Value)) throw std::invalid_argument("[Character:Authority] Position must be finite");
		SetCFrame(CFrame(Value, Transform.Rotation));
	}

	std::optional<std::shared_ptr<BasePart>> Character::GetRootPart() const {
		return RootPartValue && !RootPartValue->GetDestroyed() && !RootPartValue->IsDestroying()
			? std::optional(RootPartValue)
			: std::nullopt;
	}

	void Character::SetRootPart(std::optional<std::shared_ptr<BasePart>> Value) {
		AssertCanMutate();
		ValidatePropertyMutation("RootPart", Value);
		auto Replacement = Value ? *Value : nullptr;
		if (Replacement) {
			if (Replacement->GetDestroyed() || Replacement->IsDestroying())
				throw std::invalid_argument("[Character:Authority] RootPart must be live");
			bool Descendant = false;
			auto Current = Replacement->GetParent();
			while (Current && *Current) {
				if (Current->get() == this) {
					Descendant = true;
					break;
				}
				Current = (*Current)->GetParent();
			}
			if (!Descendant)
				throw std::invalid_argument("[Character:Authority] RootPart must be a Character descendant");
		}
		if (RootPartValue == Replacement) return;
		RootPartValue = std::move(Replacement);
		NotifyPropertyCommitted("RootPart");
		SynchronizeRootPart();
	}

	void Character::SynchronizeRootPart(bool Simulation) {
		if (!RootPartValue || RootPartValue->GetDestroyed() || RootPartValue->IsDestroying()) return;
		if (RootPartValue->GetCFrame().FuzzyEq(Transform)) return;
		if (Simulation)
			RootPartValue->SetCharacterSimulationCFrame(Transform);
		else
			RootPartValue->SetCFrame(Transform);
	}

	void Character::CommitSimulationTransform(const CFrame &Value) {
		if (Transform.FuzzyEq(Value)) return;
		Transform = Value;
		if (auto Found = PropertyChangedSignals.find("CFrame");
			Found != PropertyChangedSignals.end() && Found->second)
			Found->second->Fire({});
		if (auto Found = PropertyChangedSignals.find("Position");
			Found != PropertyChangedSignals.end() && Found->second)
			Found->second->Fire({});
		SynchronizeRootPart(true);
	}

	void Character::ApplyRuntimeTransform(const CFrame &Value) {
		if (GetDestroyed() || IsDestroying() || !IsFinite(Value))
			throw std::invalid_argument("[Character:Authority] runtime transform must be finite and target a live Character");
		CommitSimulationTransform(Value);
	}

	CharacterMotionResult Character::AdmitMotion(WorldRoot &World, const CharacterMotionRequest &Request) {
		CharacterMotionResult Result{
			.RequestedTranslation = Request.Translation,
			.Position = Transform.Position,
			.Velocity = Request.Velocity,
			.RequestedYawRadians = Request.YawRadians,
		};
		if (GetDestroyed() || IsDestroying() || !IsFinite(Request.Translation) || !IsFinite(Request.Velocity) ||
			!std::isfinite(Request.YawRadians) || glm::length(Request.Translation) > MaximumMotionTranslation ||
			std::abs(Request.YawRadians) > MaximumMotionYawRadians) {
			Result.Status = PhysicsOperationStatus::InvalidDescription;
			return Result;
		}
		auto Kinematic = dynamic_cast<KinematicCharacter *>(this);
		if (!Kinematic || !std::isfinite(Kinematic->GetCapsuleRadius()) ||
			!std::isfinite(Kinematic->GetCapsuleHeight())) {
			Result.Status = PhysicsOperationStatus::InvalidDescription;
			return Result;
		}
		const auto WorldTranslation = Request.LocalSpace ? Transform.Rotation * Request.Translation : Request.Translation;
		const auto PhysicsStarted = std::chrono::steady_clock::now();
		auto PhysicsResult = World.ResolveKinematicMotion({
			.Position = Transform.Position,
			.Radius = Kinematic->GetCapsuleRadius(),
			.Height = Kinematic->GetCapsuleHeight(),
			.Translation = WorldTranslation,
			.Velocity = Request.Velocity,
		});
		Result.PhysicsCpuNanoseconds = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - PhysicsStarted
			).count()
		);
		Result.Status = PhysicsResult.Status;
		Result.AppliedTranslation = PhysicsResult.AppliedTranslation;
		Result.Position = PhysicsResult.Position;
		Result.Velocity = PhysicsResult.Velocity;
		Result.ContactNormal = PhysicsResult.ContactNormal;
		Result.FloorNormal = PhysicsResult.FloorNormal;
		Result.Collided = PhysicsResult.Collided;
		Result.HasFloor = PhysicsResult.HasFloor;
		Result.PlanesTruncated = PhysicsResult.PlanesTruncated;
		if (!PhysicsResult.Succeeded()) return Result;

		const auto Yaw = glm::mat3_cast(glm::angleAxis(Request.YawRadians, glm::vec3(0.0f, 1.0f, 0.0f)));
		CommitSimulationTransform(CFrame(PhysicsResult.Position, Yaw * Transform.Rotation));
		Result.AppliedYawRadians = Request.YawRadians;
		return Result;
	}

	int Character::Move(lua_State *L, Instance *InstanceValue) {
		if (!GetCurrentScriptSecurityContext().HasCapability(ScriptCapability::MutateDataModel))
			throw std::runtime_error("Character movement requires MutateDataModel");
		auto *CharacterValue = dynamic_cast<Character *>(InstanceValue);
		if (!CharacterValue || CharacterValue->GetDestroyed() || CharacterValue->IsDestroying())
			throw std::runtime_error("[Character:Authority] Move requires a live Character");
		auto DataModelValue = CharacterValue->GetDataModel();
		auto World = DataModelValue ? std::dynamic_pointer_cast<WorldRoot>(DataModelValue->GetService("Workspace")) : nullptr;
		if (!World) throw std::runtime_error("[Character:Authority] Move requires a live Workspace");
		const auto Translation = CheckStackValue<glm::vec3>(L, 2);
		const auto Velocity = lua_isnoneornil(L, 3) ? glm::vec3(0.0f) : CheckStackValue<glm::vec3>(L, 3);
		auto Result = CharacterValue->AdmitMotion(*World, {
			.Translation = Translation,
			.Velocity = Velocity,
			.Source = CharacterMotionSource::Script,
		});
		if (!Result.Succeeded()) throw std::runtime_error("[Character:Authority] movement request was rejected");

		lua_createtable(L, 0, 8);
		auto SetVector = [L](const char *Name, const glm::vec3 &Value) {
			StackValue<glm::vec3>::Push(L, Value);
			lua_setfield(L, -2, Name);
		};
		auto SetBoolean = [L](const char *Name, bool Value) {
			lua_pushboolean(L, Value);
			lua_setfield(L, -2, Name);
		};
		SetVector("Position", Result.Position);
		SetVector("AppliedTranslation", Result.AppliedTranslation);
		SetVector("Velocity", Result.Velocity);
		SetVector("ContactNormal", Result.ContactNormal);
		SetVector("FloorNormal", Result.FloorNormal);
		SetBoolean("Collided", Result.Collided);
		SetBoolean("HasFloor", Result.HasFloor);
		SetBoolean("PlanesTruncated", Result.PlanesTruncated);
		lua_setreadonly(L, -1, true);
		return 1;
	}
}
