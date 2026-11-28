#include "gargantuan/classes/Character.hpp"

#include "gargantuan/classes/BasePart.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/network/SpatialTransferProtection.hpp"
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
		if (RootPartValue && !RootPartValue->GetDestroyed() && !RootPartValue->IsDestroying())
			RootPartValue->SetCharacterPresentationOffset(std::nullopt);
		RootPartValue = std::move(Replacement);
		NotifyPropertyCommitted("RootPart");
		SynchronizeRootPart();
	}

	void Character::SynchronizeRootPart(bool Simulation) {
		if (!RootPartValue || RootPartValue->GetDestroyed() || RootPartValue->IsDestroying()) return;
		if (!RootPartValue->GetCFrame().FuzzyEq(Transform)) {
			if (Simulation)
				RootPartValue->SetSimulationCFrame(Transform);
			else
				RootPartValue->SetCFrame(Transform);
		}
		RootPartValue->SetCharacterPresentationOffset(PresentationOffset);
	}

	void Character::CommitSimulationTransform(const CFrame &Value) {
		if (Transform.FuzzyEq(Value)) return;
		if (RootPartValue && !RootPartValue->GetCFrame().FuzzyEq(Value)) if (const auto Realm=GetDataModel())
			if (const auto Guard=Realm->GetSpatialTransferProtection(); Guard && !Guard->AllowsSimulationPose(*RootPartValue,Value)) return;
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

	bool Character::ApplyRuntimeSourceCheckpoint(const CFrame &Value, glm::vec3 Velocity, glm::vec3 FloorNormal, bool Grounded) {
		const auto Root = RootPartValue;
		auto *Controller = dynamic_cast<KinematicCharacter *>(this);
		if (!Controller || !Root || GetDestroyed() || IsDestroying() || Root->GetDestroyed() || Root->IsDestroying() ||
			!IsFinite(Value) || !IsFinite(Velocity) || !IsFinite(FloorNormal)) return false;
		const auto Realm = GetDataModel();
		const auto Guard = Realm ? Realm->GetSpatialTransferProtection() : nullptr;
		const auto Source = WorldRoot::GetEffectiveWorld(*Root);
		if (!Source || !Guard || !Guard->Controls(*Root) || !Guard->AllowsSimulationPose(*Root, Value) ||
			!Source->CanHoldSpatialSubject(Root->GetObjectId())) return false;
		const glm::vec3 Capsule(Controller->GetCapsuleRadius(), Controller->GetCapsuleHeight() * .5f, Controller->GetCapsuleRadius());
		if (!Guard->AllowsMotion(*Root, *Source, {glm::min(Transform.Position, Value.Position) - Capsule,
			glm::max(Transform.Position, Value.Position) + Capsule})) return false;
		const auto Body = Source->PartBodies.at(Root->GetObjectId());
		const auto State = Source->Physics.GetBodyState(Body);
		if (!State) return false;
		const auto &OldPose = State->Transform;
		if (OldPose.Position != Value.Position || OldPose.Rotation != Value.Rotation) {
			auto Description = State->Description;
			Description.Transform = Value;
			if (!Source->Physics.UpdateBody(Body, Description).Succeeded()) return false;
		}
		const bool Changed = Transform.Position != Value.Position || Transform.Rotation != Value.Rotation;
		// A source-world raycast from the first callback must already see the corrected native body.
		Controller->ApplyRuntimeControllerFacts(Velocity, FloorNormal, Grounded);
		Transform = Value;
		Root->SetSimulationCFrame(Value, true);
		const auto Current = [&] {
			return !GetDestroyed() && !IsDestroying() && GetRootPart().value_or(nullptr) == Root &&
				WorldRoot::GetEffectiveWorld(*Root) == Source && Transform.Position == Value.Position &&
				Transform.Rotation == Value.Rotation && Root->GetCFrame().Position == Value.Position &&
				Root->GetCFrame().Rotation == Value.Rotation && Controller->GetVelocity() == Velocity &&
				Controller->GetFloorNormal() == FloorNormal && Controller->GetGrounded() == Grounded;
		};
		if (Changed) {
			for (const char *Property : {"CFrame", "Position"}) {
				if (!Current()) return false;
				const auto It = PropertyChangedSignals.find(Property);
				if (It != PropertyChangedSignals.end() && It->second) {
					const auto Signal = It->second;
					Signal->Fire({});
				}
			}
		}
		return Current();
	}

	void Character::ApplyRuntimePresentationOffset(std::optional<CFrame> Value) {
		if (GetDestroyed() || IsDestroying() || (Value && !IsFinite(*Value)))
			throw std::invalid_argument(
				"[Character:Presentation] runtime presentation offset must be finite and target a live Character"
			);
		if ((!PresentationOffset && !Value) ||
			(PresentationOffset && Value && PresentationOffset->FuzzyEq(*Value)))
			return;
		PresentationOffset = std::move(Value);
		if (RootPartValue && !RootPartValue->GetDestroyed() && !RootPartValue->IsDestroying())
			RootPartValue->SetCharacterPresentationOffset(PresentationOffset);
	}

	CFrame Character::GetPresentationCFrame() const {
		return PresentationOffset ? Transform * *PresentationOffset : Transform;
	}

	bool Character::ResolveMotionWorld(std::shared_ptr<WorldRoot> &World) const {
		World.reset();
		if (RootPartValue && (RootPartValue->GetDestroyed() || RootPartValue->IsDestroying())) return false;
		const Instance &Member = RootPartValue ? static_cast<const Instance &>(*RootPartValue) : *this;
		return !WorldRoot::FindEffectiveWorld(Member, World, RootPartValue ? this : nullptr) || World;
	}

	CharacterMotionResult Character::AdmitMotion(WorldRoot &CallerWorld, const CharacterMotionRequest &Request) {
		return AdmitRoutedMotion(std::ref(CallerWorld), Request);
	}

	CharacterMotionResult Character::AdmitRoutedMotion(
		MotionCaller Caller, const CharacterMotionRequest &Request
	) {
		std::shared_ptr<WorldRoot> SpatialRoot;
		if (!ResolveMotionWorld(SpatialRoot))
			return {.Status = PhysicsOperationStatus::ResourceLimit};
		WorldRoot *SelectedWorld = SpatialRoot.get();
		if (const auto *Script = std::get_if<std::reference_wrapper<DataModel>>(&Caller)) {
			auto &Realm = Script->get();
			// Hierarchical and explicit routes already identify the physical world.
			// Instances outside every world retain the script API's canonical Workspace fallback.
			if (!SpatialRoot) SpatialRoot = std::dynamic_pointer_cast<WorldRoot>(Realm.GetService("Workspace"));
			if (!SpatialRoot || SpatialRoot->GetDataModel().get() != &Realm)
				return {.Status = PhysicsOperationStatus::InvalidDescription};
			SelectedWorld = SpatialRoot.get();
		} else {
			auto &Host = std::get<std::reference_wrapper<WorldRoot>>(Caller).get();
			if (SpatialRoot && SpatialRoot.get() != &Host && SpatialRoot->GetDataModel() != Host.GetDataModel())
				return {.Status = PhysicsOperationStatus::InvalidDescription};
			if (!SelectedWorld) SelectedWorld = &Host;
		}
		WorldRoot &World = *SelectedWorld;
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
		const auto Admission = World.TerrainMotionAdmission.lock();
		const glm::vec3 Extents(Kinematic->GetCapsuleRadius(), Kinematic->GetCapsuleHeight() * 0.5f,
			Kinematic->GetCapsuleRadius());
		if (World.RequiresSpatialMotionProof) {
			const auto Proof = World.SpatialMotionAdmission.lock();
			if (!Proof || !(*Proof)(World,GetObjectId(),Transform.Position,Transform.Position + WorldTranslation,Extents)) {
				Result.Status = PhysicsOperationStatus::ResourceLimit;
				return Result;
			}
		}
		if ((!Admission && World.TerrainReplicaCoverageRequired) ||
			(Admission && (!(*Admission)(GetObjectId(), Transform.Position, Extents) ||
				!(*Admission)(GetObjectId(), Transform.Position + WorldTranslation, Extents)))) {
			Result.Status = PhysicsOperationStatus::ResourceLimit;
			return Result;
		}
		if (const auto Realm=World.GetDataModel()) if (const auto Guard=Realm->GetSpatialTransferProtection()) {
			const auto End=Transform.Position+WorldTranslation;
			const SpatialDestinationBounds Swept{glm::min(Transform.Position,End)-Extents,glm::max(Transform.Position,End)+Extents};
			if (!Guard->AllowsMotion(RootPartValue?static_cast<const Instance&>(*RootPartValue):*this,World,Swept)) {
				Result.Status=PhysicsOperationStatus::ResourceLimit; return Result;
			}
		}
		const auto PhysicsStarted = std::chrono::steady_clock::now();
		auto PhysicsResult = World.ResolveKinematicMotion({
			.Position = Transform.Position,
			.Radius = Kinematic->GetCapsuleRadius(),
			.Height = Kinematic->GetCapsuleHeight(),
			.Translation = WorldTranslation,
			.Velocity = Request.Velocity,
			.IsCenterCovered = Admission ? std::function<bool(glm::vec3)>([&](glm::vec3 Position) {
				return (*Admission)(GetObjectId(), Position, Extents);
			}) : std::function<bool(glm::vec3)>{},
		});
		Result.PhysicsCpuNanoseconds = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - PhysicsStarted
			).count()
		);
		if (PhysicsResult.Succeeded() && Admission && !(*Admission)(GetObjectId(), PhysicsResult.Position, Extents)) {
			Result.Status = PhysicsOperationStatus::ResourceLimit;
			return Result;
		}
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

	int Character::GetMotionWorld(lua_State *L, Instance *InstanceValue) {
		if (!GetCurrentScriptSecurityContext().HasCapability(ScriptCapability::ReadDataModel))
			throw std::runtime_error("[Character:Query] Motion world lookup requires ReadDataModel");
		const auto *Actor = dynamic_cast<const Character *>(InstanceValue);
		if (Actor && !Actor->GetDestroyed() && !Actor->IsDestroying()) {
			const auto Realm = Actor->GetDataModel();
			std::shared_ptr<WorldRoot> World;
			if (Realm && Actor->ResolveMotionWorld(World)) {
				if (!World)
					World = std::dynamic_pointer_cast<WorldRoot>(Realm->FindService("Workspace").value_or(nullptr));
				if (World && !World->GetDestroyed() && !World->IsDestroying() &&
					World->IsSpatiallyAvailable() && World->GetDataModel() == Realm)
					return StackValue<std::shared_ptr<Instance>>::Push(L, World);
			}
		}
		lua_pushnil(L);
		return 1;
	}

	int Character::Move(lua_State *L, Instance *InstanceValue) {
		if (!GetCurrentScriptSecurityContext().HasCapability(ScriptCapability::MutateDataModel))
			throw std::runtime_error("Character movement requires MutateDataModel");
		auto *CharacterValue = dynamic_cast<Character *>(InstanceValue);
		if (!CharacterValue || CharacterValue->GetDestroyed() || CharacterValue->IsDestroying())
			throw std::runtime_error("[Character:Authority] Move requires a live Character");
		auto DataModelValue = CharacterValue->GetDataModel();
		if (!DataModelValue) throw std::runtime_error("[Character:Authority] Move requires a live Workspace");
		const auto Translation = CheckStackValue<glm::vec3>(L, 2);
		const auto Velocity = lua_isnoneornil(L, 3) ? glm::vec3(0.0f) : CheckStackValue<glm::vec3>(L, 3);
		auto Result = CharacterValue->AdmitRoutedMotion(std::ref(*DataModelValue), {
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
