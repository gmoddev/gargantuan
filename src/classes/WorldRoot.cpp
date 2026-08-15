#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/Log.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace gargantuan {
	namespace {
		[[nodiscard]] bool HasImpulse(const glm::vec3 &Impulse) {
			return Impulse.x != 0.0f || Impulse.y != 0.0f || Impulse.z != 0.0f;
		}

		[[nodiscard]] std::vector<ObjectId> SortedIds(const std::unordered_set<ObjectId> &Ids) {
			std::vector<ObjectId> Result(Ids.begin(), Ids.end());
			std::sort(Result.begin(), Result.end());
			return Result;
		}
	}

	PhysicsBodyDesc WorldRoot::DescribePart(const BasePart &Part) const {
		return {
			.Transform = Part.GetCFrame(),
			.Shape = Part.GetPhysicsShape(),
			.Anchored = Part.GetAnchored(),
			.CanCollide = Part.GetCanCollide(),
			.CanTouch = Part.GetCanTouch(),
			.Density = 0.7f,
		};
	}

	PhysicsBodyId WorldRoot::CreatePartBody(const std::shared_ptr<BasePart> &Part) {
		if (!Part || Part->GetDestroyed() || Part->IsDestroying() || !Physics.IsValid()) return {};
		const auto Object = Part->GetObjectId();
		if (auto Existing = PartBodies.find(Object); Existing != PartBodies.end()) {
			if (Physics.IsBodyValid(Existing->second)) return Existing->second;
			BodyParts.erase(Existing->second);
			PartBodies.erase(Existing);
		}
		const auto Body = Physics.CreateBody(DescribePart(*Part));
		if (!Body.IsValid()) {
			LOG_ERROR(App, "[Physics:Backend] Failed to create body for %s", Part->GetFullName().c_str());
			return {};
		}
		if (std::find(Parts.begin(), Parts.end(), Part) == Parts.end()) Parts.push_back(Part);
		PartBodies[Object] = Body;
		BodyParts[Body] = Part;
		return Body;
	}

	PhysicsConstraintId WorldRoot::CreateConstraintJoint(const std::shared_ptr<Constraint> &Constraint) {
		if (!Constraint || Constraint->GetDestroyed() || Constraint->IsDestroying() || !Constraint->GetEnabled() ||
			!Physics.IsValid()) return {};
		if (auto Existing = ConstraintJoints.find(Constraint->GetObjectId()); Existing != ConstraintJoints.end())
			return Physics.IsConstraintValid(Existing->second) ? Existing->second : PhysicsConstraintId{};
		auto [PartA, PartB] = Constraint->GetActiveParts();
		if (!PartA || !PartB || PartA == PartB || PartA->GetDestroyed() || PartB->GetDestroyed() ||
			PartA->IsDestroying() || PartB->IsDestroying()) return {};

		const auto ConstraintWorld = Constraint->FindFirstAncestorWhichIsA("WorldRoot");
		const auto PartAWorld = PartA->FindFirstAncestorWhichIsA("WorldRoot");
		const auto PartBWorld = PartB->FindFirstAncestorWhichIsA("WorldRoot");
		if (ConstraintWorld.get() != this || PartAWorld.get() != this || PartBWorld.get() != this) return {};

		const auto BodyA = CreatePartBody(PartA);
		const auto BodyB = CreatePartBody(PartB);
		if (!BodyA.IsValid() || !BodyB.IsValid()) return {};
		const auto Joint = Physics.CreateConstraint(Constraint->GetPhysicsConstraint(BodyA, BodyB));
		if (!Joint.IsValid()) {
			LOG_ERROR(App, "[Physics:Backend] Failed to create constraint for %s", Constraint->GetFullName().c_str());
			return {};
		}
		ConstraintJoints[Constraint->GetObjectId()] = Joint;
		return Joint;
	}

	void WorldRoot::TrackPart(const std::shared_ptr<BasePart> &Part) {
		if (!Part || Part->GetDestroyed() || Part->IsDestroying()) return;
		const auto Id = Part->GetObjectId();
		TrackedParts[Id] = Part;
		if (PhysicsConnections.contains(Id)) return;
		auto MarkDirty = [this, Id](std::monostate) {
			if (!ShuttingDownPhysics && !PublishingPhysicsState && TrackedParts.contains(Id)) DirtyBodies.insert(Id);
		};
		auto &Connections = PhysicsConnections[Id];
		for (const std::string_view Name : {"CFrame", "Size", "Anchored", "CanCollide", "CanTouch"})
			Connections.push_back(Part->GetPropertyChangedSignal(std::string(Name))->Connect(MarkDirty));
		if (Part->FindProperty("Shape"))
			Connections.push_back(Part->GetPropertyChangedSignal("Shape")->Connect(MarkDirty));
	}

	void WorldRoot::TrackConstraint(const std::shared_ptr<Constraint> &Constraint) {
		if (!Constraint || Constraint->GetDestroyed() || Constraint->IsDestroying()) return;
		const auto Id = Constraint->GetObjectId();
		TrackedConstraints[Id] = Constraint;
		if (PhysicsConnections.contains(Id)) return;
		auto MarkDirty = [this, Id](std::monostate) {
			if (!ShuttingDownPhysics && TrackedConstraints.contains(Id)) DirtyConstraints.insert(Id);
		};
		auto &Connections = PhysicsConnections[Id];
		for (const std::string_view Name : {"Enabled", "Attachment0", "Attachment1"})
			Connections.push_back(Constraint->GetPropertyChangedSignal(std::string(Name))->Connect(MarkDirty));
		for (const std::string_view Name : {"Part0", "Part1"})
			if (Constraint->FindProperty(std::string(Name)))
				Connections.push_back(Constraint->GetPropertyChangedSignal(std::string(Name))->Connect(MarkDirty));
	}

	void WorldRoot::RemovePart(const std::shared_ptr<BasePart> &Part) {
		if (!Part) return;
		const auto Object = Part->GetObjectId();
		erase(Parts, Part);
		DirtyBodies.erase(Object);
		TrackedParts.erase(Object);
		if (auto Connections = PhysicsConnections.find(Object); Connections != PhysicsConnections.end()) {
			for (const auto &Connection : Connections->second) Connection->Disconnect();
			PhysicsConnections.erase(Connections);
		}
		if (auto Found = PartBodies.find(Object); Found != PartBodies.end()) {
			BodyParts.erase(Found->second);
			if (Physics.IsBodyValid(Found->second)) {
				auto Result = Physics.DestroyBody(Found->second);
				if (!Result.Succeeded()) LOG_ERROR(App, "[Physics:Backend] Body destruction failed: %s", Result.Message.c_str());
			}
			PartBodies.erase(Found);
		}
		RemoveInvalidConstraintMappings();
		for (const auto &[Id, _] : TrackedConstraints) DirtyConstraints.insert(Id);
	}

	void WorldRoot::RemoveConstraint(const std::shared_ptr<Constraint> &Constraint) {
		if (!Constraint) return;
		const auto Object = Constraint->GetObjectId();
		DirtyConstraints.erase(Object);
		TrackedConstraints.erase(Object);
		if (auto Connections = PhysicsConnections.find(Object); Connections != PhysicsConnections.end()) {
			for (const auto &Connection : Connections->second) Connection->Disconnect();
			PhysicsConnections.erase(Connections);
		}
		if (auto Found = ConstraintJoints.find(Object); Found != ConstraintJoints.end()) {
			if (Physics.IsConstraintValid(Found->second)) {
				auto Result = Physics.DestroyConstraint(Found->second);
				if (!Result.Succeeded()) LOG_ERROR(App, "[Physics:Backend] Constraint destruction failed: %s", Result.Message.c_str());
			}
			ConstraintJoints.erase(Found);
		}
	}

	void WorldRoot::ReconcileConstraint(const std::shared_ptr<Constraint> &Constraint) {
		if (!Constraint || Constraint->GetDestroyed() || Constraint->IsDestroying()) return;
		const auto Object = Constraint->GetObjectId();
		auto [PartA, PartB] = Constraint->GetActiveParts();
		const bool ShouldExist = Constraint->GetEnabled() && PartA && PartB && PartA != PartB &&
			Constraint->FindFirstAncestorWhichIsA("WorldRoot").get() == this &&
			PartA->FindFirstAncestorWhichIsA("WorldRoot").get() == this &&
			PartB->FindFirstAncestorWhichIsA("WorldRoot").get() == this;
		auto Existing = ConstraintJoints.find(Object);
		if (!ShouldExist) {
			if (Existing != ConstraintJoints.end()) {
				if (Physics.IsConstraintValid(Existing->second)) Physics.DestroyConstraint(Existing->second);
				ConstraintJoints.erase(Existing);
			}
			return;
		}

		const auto BodyA = CreatePartBody(PartA);
		const auto BodyB = CreatePartBody(PartB);
		if (!BodyA.IsValid() || !BodyB.IsValid()) {
			DirtyConstraints.insert(Object);
			return;
		}
		const auto Replacement = Physics.CreateConstraint(Constraint->GetPhysicsConstraint(BodyA, BodyB));
		if (!Replacement.IsValid()) {
			LOG_ERROR(App, "[Physics:Backend] Constraint update failed for %s", Constraint->GetFullName().c_str());
			DirtyConstraints.insert(Object);
			return;
		}
		if (Existing != ConstraintJoints.end() && Physics.IsConstraintValid(Existing->second))
			Physics.DestroyConstraint(Existing->second);
		ConstraintJoints[Object] = Replacement;
	}

	void WorldRoot::ApplyPendingPhysicsChanges() {
		if (ShuttingDownPhysics || !Physics.IsValid()) return;
		if (GravityDirty) {
			auto Result = Physics.SetGravity({0.0f, -GetGravity(), 0.0f});
			if (Result.Succeeded()) GravityDirty = false;
			else LOG_ERROR(App, "[Physics:Backend] Gravity update failed: %s", Result.Message.c_str());
		}

		const auto BodyUpdates = SortedIds(DirtyBodies);
		DirtyBodies.clear();
		for (const auto Object : BodyUpdates) {
			auto Tracked = TrackedParts.find(Object);
			auto Part = Tracked == TrackedParts.end() ? nullptr : Tracked->second.lock();
			if (!Part || Part->GetDestroyed() || Part->IsDestroying() ||
				Part->FindFirstAncestorWhichIsA("WorldRoot").get() != this) continue;
			auto Found = PartBodies.find(Object);
			if (Found == PartBodies.end() || !Physics.IsBodyValid(Found->second)) {
				if (!CreatePartBody(Part).IsValid()) DirtyBodies.insert(Object);
				continue;
			}
			auto Result = Physics.UpdateBody(Found->second, DescribePart(*Part));
			if (!Result.Succeeded()) {
				LOG_ERROR(App, "[Physics:Backend] Body update failed for %s: %s", Part->GetFullName().c_str(), Result.Message.c_str());
				DirtyBodies.insert(Object);
			}
		}

		const auto ConstraintUpdates = SortedIds(DirtyConstraints);
		DirtyConstraints.clear();
		for (const auto Object : ConstraintUpdates) {
			auto Tracked = TrackedConstraints.find(Object);
			auto Constraint = Tracked == TrackedConstraints.end() ? nullptr : Tracked->second.lock();
			if (Constraint) ReconcileConstraint(Constraint);
		}
	}

	void WorldRoot::ApplyPendingImpulses() {
		for (const auto &Part : Parts) {
			if (!Part || Part->GetDestroyed() || Part->IsDestroying() || !HasImpulse(Part->AccumulatedImpulse)) continue;
			auto Found = PartBodies.find(Part->GetObjectId());
			if (Found == PartBodies.end()) continue;
			auto Result = Physics.ApplyLinearImpulse(Found->second, Part->AccumulatedImpulse);
			if (Result.Succeeded()) Part->AccumulatedImpulse = {};
			else LOG_ERROR(App, "[Physics:Backend] Impulse application failed: %s", Result.Message.c_str());
		}
	}

	void WorldRoot::RemoveInvalidConstraintMappings() {
		std::erase_if(ConstraintJoints, [this](const auto &Entry) {
			return !Physics.IsConstraintValid(Entry.second);
		});
	}

	void WorldRoot::ShutdownPhysics() {
		if (ShuttingDownPhysics) return;
		ShuttingDownPhysics = true;
		if (UnbindDescendants) UnbindDescendants();
		if (DescendantRemovedConnection) DescendantRemovedConnection->Disconnect();
		if (DestroyingConnection) DestroyingConnection->Disconnect();
		if (GravityConnection) GravityConnection->Disconnect();
		for (auto &[_, Connections] : PhysicsConnections)
			for (const auto &Connection : Connections) Connection->Disconnect();
		PhysicsConnections.clear();
		DirtyBodies.clear();
		DirtyConstraints.clear();
		TrackedParts.clear();
		TrackedConstraints.clear();
		ConstraintJoints.clear();
		PartBodies.clear();
		BodyParts.clear();
		Parts.clear();
	}

	std::shared_ptr<BasePart> WorldRoot::ResolvePart(PhysicsBodyId Body) const {
		auto Found = BodyParts.find(Body);
		if (Found == BodyParts.end()) return nullptr;
		auto Part = Found->second.lock();
		if (!Part || Part->GetDestroyed() || Part->IsDestroying()) return nullptr;
		auto Current = PartBodies.find(Part->GetObjectId());
		return Current != PartBodies.end() && Current->second == Body ? Part : nullptr;
	}

	WorldRoot::WorldRoot() : Physics(PhysicsWorldConfig{.Gravity = {0.0f, -Gravity, 0.0f}}) {
		GravityConnection = GetPropertyChangedSignal("Gravity")->Connect([this](std::monostate) {
			if (!ShuttingDownPhysics) GravityDirty = true;
		});

		DestroyingConnection = Destroying->Once([this](std::monostate) { ShutdownPhysics(); });

		UnbindDescendants = BindDescendants([this](std::shared_ptr<Instance> Instance) {
			if (auto Part = std::dynamic_pointer_cast<BasePart>(Instance)) {
				TrackPart(Part);
				(void)CreatePartBody(Part);
				for (const auto &[Id, _] : TrackedConstraints) DirtyConstraints.insert(Id);
			} else if (auto Constraint = std::dynamic_pointer_cast<gargantuan::Constraint>(Instance)) {
				TrackConstraint(Constraint);
				(void)CreateConstraintJoint(Constraint);
			}
		});

		DescendantRemovedConnection = DescendantRemoved->Connect([this](std::shared_ptr<Instance> Instance) {
			if (ShuttingDownPhysics) return;
			if (auto Part = std::dynamic_pointer_cast<BasePart>(Instance)) RemovePart(Part);
			else if (auto Constraint = std::dynamic_pointer_cast<gargantuan::Constraint>(Instance))
				RemoveConstraint(Constraint);
		});
	}

	WorldRoot::~WorldRoot() {
		ShutdownPhysics();
	}

	void WorldRoot::StepPhysics(
		double DeltaTime,
		std::optional<std::vector<std::shared_ptr<Instance>>> Instances
	) {
		(void)Instances;
		ApplyPendingPhysicsChanges();
		StepAccumulator += static_cast<float>(DeltaTime);
		int Steps = 0;
		while (StepAccumulator >= STEP_INTERVAL && Steps < MAX_STEPS_PER_FRAME) {
			ApplyPendingImpulses();
			auto Result = Physics.Step({.DeltaTime = STEP_INTERVAL, .SubStepCount = SUB_STEP_COUNT});
			if (Result.EventsTruncated)
				LOG_ERROR(App, "[Physics:Backend] Step event limit reached; excess events were rejected");
			for (const auto &Motion : Result.Motions) {
				auto Part = ResolvePart(Motion.Body);
				if (!Part || Part->GetCFrame().FuzzyEq(Motion.Transform)) continue;
				PublishingPhysicsState = true;
				try {
					Part->SetCFrame(Motion.Transform);
				} catch (...) {
					PublishingPhysicsState = false;
					throw;
				}
				PublishingPhysicsState = false;
			}

			for (const auto &Contact : Result.Contacts) {
				auto PartA = ResolvePart(Contact.BodyA);
				auto PartB = ResolvePart(Contact.BodyB);
				if (!PartA || !PartB) continue;
				if (Contact.Phase == PhysicsContactPhase::Began) {
					if (PartA->GetCanTouch()) PartA->Touched->Fire(PartB);
					if (PartB->GetCanTouch()) PartB->Touched->Fire(PartA);
				} else {
					if (PartA->GetCanTouch()) PartA->TouchEnded->Fire(PartB);
					if (PartB->GetCanTouch()) PartB->TouchEnded->Fire(PartA);
				}
			}
			StepAccumulator -= STEP_INTERVAL;
			++Steps;
		}
		if (Steps == MAX_STEPS_PER_FRAME) StepAccumulator = 0.0f;
	}
}
