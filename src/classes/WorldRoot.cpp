#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/Log.hpp"
#include "gargantuan/render/RenderDirtyAccumulator.hpp"

#include "gargantuan/scripting/ScriptSecurity.hpp"
#include "gargantuan/scripting/StackValue.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <lua.h>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace gargantuan {
	namespace {
		[[nodiscard]] bool HasImpulse(const glm::vec3 &Impulse) {
			return Impulse.x != 0.0f || Impulse.y != 0.0f || Impulse.z != 0.0f;
		}

		[[nodiscard]] SoftBodyQuality ToSoftBodyQuality(Enums::DeformableQuality Quality) {
			switch (Quality) {
				case Enums::DeformableQuality::Low: return SoftBodyQuality::Low;
				case Enums::DeformableQuality::Medium: return SoftBodyQuality::Medium;
				case Enums::DeformableQuality::High: return SoftBodyQuality::High;
				case Enums::DeformableQuality::Automatic: return SoftBodyQuality::Automatic;
			}
			return SoftBodyQuality::Automatic;
		}

		[[nodiscard]] SoftBodyCollisionMode ToSoftBodyCollisionMode(Enums::DeformableCollisionMode Mode) {
			return Mode == Enums::DeformableCollisionMode::None ? SoftBodyCollisionMode::None :
				SoftBodyCollisionMode::RigidPrimitives;
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

	SoftBodyDefinition WorldRoot::DescribeDeformable(DeformableBody &Body) const {
		SoftBodyMaterialDesc Material;
		if (const auto MaterialValue = Body.GetMaterial(); MaterialValue && *MaterialValue &&
			!(*MaterialValue)->GetDestroyed() && !(*MaterialValue)->IsDestroying()) {
			Material = {
				.ParticleMass = (*MaterialValue)->GetParticleMass(),
				.Damping = (*MaterialValue)->GetDamping(),
				.StretchCompliance = (*MaterialValue)->GetStretchCompliance(),
				.BendCompliance = (*MaterialValue)->GetBendCompliance(),
				.ShapeCompliance = (*MaterialValue)->GetShapeCompliance(),
				.VolumeCompliance = (*MaterialValue)->GetVolumeCompliance(),
				.Friction = (*MaterialValue)->GetFriction(),
				.Thickness = (*MaterialValue)->GetThickness(),
			};
		}

		std::map<std::uint32_t, std::pair<ObjectId, glm::vec3>> AttachmentPositions;
		for (const auto &Candidate : Body.GetDescendants()) {
			auto Attachment = std::dynamic_pointer_cast<SoftBodyAttachment>(Candidate);
			if (!Attachment || !Attachment->GetEnabled() || Attachment->GetVertexIndex() < 0 ||
				Attachment->GetDestroyed() || Attachment->IsDestroying()) continue;
			const auto Vertex = static_cast<std::uint32_t>(Attachment->GetVertexIndex());
			const auto Position = AttachmentPositions.find(Vertex);
			if (Position == AttachmentPositions.end() || Attachment->GetObjectId() < Position->second.first)
				AttachmentPositions[Vertex] = {Attachment->GetObjectId(), Attachment->GetPosition()};
		}
		std::vector<SoftBodyAttachmentDesc> Attachments;
		Attachments.reserve(AttachmentPositions.size());
		for (const auto &[Vertex, Entry] : AttachmentPositions)
			Attachments.push_back({Vertex, Entry.second});

		const auto Resolution = Body.GetSoftBodyResolution();
		return {
			.Kind = Body.GetSoftBodyKind(),
			.Position = Body.GetPosition(),
			.Size = Body.GetSize(),
			.ResolutionX = Resolution.x,
			.ResolutionY = Resolution.y,
			.ResolutionZ = Resolution.z,
			.Material = Material,
			.Quality = ToSoftBodyQuality(Body.GetQuality()),
			.CollisionMode = ToSoftBodyCollisionMode(Body.GetCollisionMode()),
			.Enabled = Body.GetEnabled(),
			.Attachments = std::move(Attachments),
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

	SoftBodyId WorldRoot::CreateDeformableBody(const std::shared_ptr<DeformableBody> &Body) {
		if (!Body || Body->GetDestroyed() || Body->IsDestroying() || !Deformables.IsValid()) return {};
		const auto Object = Body->GetObjectId();
		if (auto Existing = DeformableIds.find(Object); Existing != DeformableIds.end()) {
			if (Deformables.IsBodyValid(Existing->second)) return Existing->second;
			IdDeformables.erase(Existing->second);
			DeformableIds.erase(Existing);
		}
		const auto Id = Deformables.CreateBody(DescribeDeformable(*Body));
		if (!Id.IsValid()) {
			LOG_ERROR(App, "[Physics:SoftBody] Failed to create deformable body for %s", Body->GetFullName().c_str());
			return {};
		}
		if (std::find(SoftBodies.begin(), SoftBodies.end(), Body) == SoftBodies.end()) SoftBodies.push_back(Body);
		DeformableIds[Object] = Id;
		IdDeformables[Id] = Body;
		if (auto State = Deformables.GetBodyState(Id)) DeformableStates[Object] = std::move(*State);
		RenderDirtyAccumulator::Get().Mark(
			GetReplicationScopeId(), Object,
			RenderUpdateDomain::Transform | RenderUpdateDomain::Material | RenderUpdateDomain::Visibility |
				RenderUpdateDomain::Geometry | RenderUpdateDomain::DeformableVertices | RenderUpdateDomain::Hierarchy,
			DeformableStates.contains(Object) && DeformableStates.at(Object).Positions ?
				DeformableStates.at(Object).Positions->size() * sizeof(glm::vec3) * 4 : 0
		);
		return Id;
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

	void WorldRoot::TrackDeformable(const std::shared_ptr<DeformableBody> &Body) {
		if (!Body || Body->GetDestroyed() || Body->IsDestroying()) return;
		const auto Id = Body->GetObjectId();
		TrackedDeformables[Id] = Body;
		if (PhysicsConnections.contains(Id)) return;
		auto MarkDirty = [this, Id](std::monostate) {
			if (!ShuttingDownPhysics && TrackedDeformables.contains(Id)) DirtyDeformables.insert(Id);
		};
		auto &Connections = PhysicsConnections[Id];
		for (const std::string_view Name : {
				 "Enabled", "Position", "Size", "Quality", "CollisionMode", "Material", "ResolutionX",
				 "ResolutionY", "ResolutionZ"
			})
			if (Body->FindProperty(std::string(Name)))
				Connections.push_back(Body->GetPropertyChangedSignal(std::string(Name))->Connect(MarkDirty));
		Connections.push_back(Body->DescendantAdded->Connect([MarkDirty](std::shared_ptr<Instance>) {
			MarkDirty(std::monostate{});
		}));
		Connections.push_back(Body->DescendantRemoved->Connect([MarkDirty](std::shared_ptr<Instance>) {
			MarkDirty(std::monostate{});
		}));
	}

	void WorldRoot::TrackSoftBodyAttachment(const std::shared_ptr<SoftBodyAttachment> &Attachment) {
		if (!Attachment || Attachment->GetDestroyed() || Attachment->IsDestroying()) return;
		auto Owner = std::dynamic_pointer_cast<DeformableBody>(Attachment->FindFirstAncestorWhichIsA("DeformableBody"));
		if (!Owner) return;
		const auto Id = Attachment->GetObjectId();
		const auto OwnerId = Owner->GetObjectId();
		SoftBodyAttachmentOwners[Id] = OwnerId;
		DirtyDeformables.insert(OwnerId);
		if (PhysicsConnections.contains(Id)) return;
		auto MarkDirty = [this, Id](std::monostate) {
			auto Found = SoftBodyAttachmentOwners.find(Id);
			if (!ShuttingDownPhysics && Found != SoftBodyAttachmentOwners.end())
				DirtyDeformables.insert(Found->second);
		};
		auto &Connections = PhysicsConnections[Id];
		for (const std::string_view Name : {"Enabled", "VertexIndex", "Position"})
			Connections.push_back(Attachment->GetPropertyChangedSignal(std::string(Name))->Connect(MarkDirty));
	}

	void WorldRoot::TrackSoftBodyMaterial(const std::shared_ptr<SoftBodyMaterial> &Material) {
		if (!Material || Material->GetDestroyed() || Material->IsDestroying()) return;
		const auto Id = Material->GetObjectId();
		TrackedSoftBodyMaterials[Id] = Material;
		if (PhysicsConnections.contains(Id)) return;
		auto MarkUsersDirty = [this, Id](std::monostate) {
			if (ShuttingDownPhysics) return;
			for (const auto &[BodyId, WeakBody] : TrackedDeformables) {
				auto Body = WeakBody.lock();
				const auto BodyMaterial = Body ? Body->GetMaterial() : std::nullopt;
				if (BodyMaterial && *BodyMaterial && (*BodyMaterial)->GetObjectId() == Id)
					DirtyDeformables.insert(BodyId);
			}
		};
		auto &Connections = PhysicsConnections[Id];
		for (const std::string_view Name : {
				 "ParticleMass", "Damping", "StretchCompliance", "BendCompliance", "ShapeCompliance",
				 "VolumeCompliance", "Friction", "Thickness"
			})
			Connections.push_back(Material->GetPropertyChangedSignal(std::string(Name))->Connect(MarkUsersDirty));
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

	void WorldRoot::RemoveDeformable(const std::shared_ptr<DeformableBody> &Body) {
		if (!Body) return;
		const auto Object = Body->GetObjectId();
		std::erase(SoftBodies, Body);
		DirtyDeformables.erase(Object);
		TrackedDeformables.erase(Object);
		if (auto Connections = PhysicsConnections.find(Object); Connections != PhysicsConnections.end()) {
			for (const auto &Connection : Connections->second) Connection->Disconnect();
			PhysicsConnections.erase(Connections);
		}
		if (auto Found = DeformableIds.find(Object); Found != DeformableIds.end()) {
			IdDeformables.erase(Found->second);
			if (Deformables.IsBodyValid(Found->second)) {
				auto Result = Deformables.DestroyBody(Found->second);
				if (!Result.Succeeded())
					LOG_ERROR(App, "[Physics:SoftBody] Body destruction failed: %s", Result.Message.c_str());
			}
			DeformableIds.erase(Found);
		}
		DeformableStates.erase(Object);
		std::erase_if(SoftBodyAttachmentOwners, [Object](const auto &Entry) { return Entry.second == Object; });
	}

	void WorldRoot::RemoveSoftBodyAttachment(const std::shared_ptr<SoftBodyAttachment> &Attachment) {
		if (!Attachment) return;
		const auto Id = Attachment->GetObjectId();
		if (auto Found = SoftBodyAttachmentOwners.find(Id); Found != SoftBodyAttachmentOwners.end()) {
			DirtyDeformables.insert(Found->second);
			SoftBodyAttachmentOwners.erase(Found);
		}
		if (auto Connections = PhysicsConnections.find(Id); Connections != PhysicsConnections.end()) {
			for (const auto &Connection : Connections->second) Connection->Disconnect();
			PhysicsConnections.erase(Connections);
		}
	}

	void WorldRoot::RemoveSoftBodyMaterial(const std::shared_ptr<SoftBodyMaterial> &Material) {
		if (!Material) return;
		const auto Id = Material->GetObjectId();
		for (const auto &[BodyId, WeakBody] : TrackedDeformables) {
			auto Body = WeakBody.lock();
			const auto BodyMaterial = Body ? Body->GetMaterial() : std::nullopt;
			if (BodyMaterial && *BodyMaterial && (*BodyMaterial)->GetObjectId() == Id)
				DirtyDeformables.insert(BodyId);
		}
		TrackedSoftBodyMaterials.erase(Id);
		if (auto Connections = PhysicsConnections.find(Id); Connections != PhysicsConnections.end()) {
			for (const auto &Connection : Connections->second) Connection->Disconnect();
			PhysicsConnections.erase(Connections);
		}
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

		const auto DeformableUpdates = SortedIds(DirtyDeformables);
		DirtyDeformables.clear();
		for (const auto Object : DeformableUpdates) {
			auto Tracked = TrackedDeformables.find(Object);
			auto Body = Tracked == TrackedDeformables.end() ? nullptr : Tracked->second.lock();
			if (!Body || Body->GetDestroyed() || Body->IsDestroying() ||
				Body->FindFirstAncestorWhichIsA("WorldRoot").get() != this) continue;
			auto Found = DeformableIds.find(Object);
			if (Found == DeformableIds.end() || !Deformables.IsBodyValid(Found->second)) {
				if (!CreateDeformableBody(Body).IsValid()) DirtyDeformables.insert(Object);
				continue;
			}
			const auto Previous = Deformables.GetBodyState(Found->second);
			auto Result = Deformables.UpdateBody(Found->second, DescribeDeformable(*Body));
			if (!Result.Succeeded()) {
				LOG_ERROR(
					App, "[Physics:SoftBody] Body update failed for %s: %s", Body->GetFullName().c_str(),
					Result.Message.c_str()
				);
				DirtyDeformables.insert(Object);
				continue;
			}
			if (auto State = Deformables.GetBodyState(Found->second)) {
				const auto TopologyChanged = Previous && Previous->TopologyRevision != State->TopologyRevision;
				DeformableStates[Object] = *State;
				if (TopologyChanged)
					RenderDirtyAccumulator::Get().RequestFullResync(
						GetReplicationScopeId(), "Deformable topology changed; stable mesh residency was rebuilt"
					);
				RenderDirtyAccumulator::Get().Mark(
					GetReplicationScopeId(), Object,
					RenderUpdateDomain::Transform | RenderUpdateDomain::Material | RenderUpdateDomain::Visibility |
						RenderUpdateDomain::Geometry | RenderUpdateDomain::DeformableVertices,
					State->Positions ? State->Positions->size() * sizeof(glm::vec3) * 4 : 0
				);
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

	void WorldRoot::ApplyPendingDeformableForces() {
		for (const auto &Body : SoftBodies) {
			if (!Body || Body->GetDestroyed() || Body->IsDestroying()) continue;
			auto Found = DeformableIds.find(Body->GetObjectId());
			if (Found == DeformableIds.end()) continue;
			if (HasImpulse(Body->AccumulatedForce)) {
				auto Result = Deformables.ApplyForce(Found->second, Body->AccumulatedForce);
				if (Result.Succeeded()) Body->AccumulatedForce = {};
				else LOG_ERROR(App, "[Physics:SoftBody] Force application failed: %s", Result.Message.c_str());
			}
			if (HasImpulse(Body->AccumulatedImpulse)) {
				auto Result = Deformables.ApplyImpulse(Found->second, Body->AccumulatedImpulse);
				if (Result.Succeeded()) Body->AccumulatedImpulse = {};
				else LOG_ERROR(App, "[Physics:SoftBody] Impulse application failed: %s", Result.Message.c_str());
			}
			std::size_t AppliedPointImpulses = 0;
			for (const auto &PointImpulse : Body->AccumulatedPointImpulses) {
				auto Result = Deformables.ApplyImpulseAtPosition(
					Found->second, PointImpulse.Impulse, PointImpulse.Position
				);
				if (!Result.Succeeded()) {
					LOG_ERROR(App, "[Physics:SoftBody] Point impulse application failed: %s", Result.Message.c_str());
					break;
				}
				++AppliedPointImpulses;
			}
			if (AppliedPointImpulses != 0)
				Body->AccumulatedPointImpulses.erase(
					Body->AccumulatedPointImpulses.begin(),
					Body->AccumulatedPointImpulses.begin() + static_cast<std::ptrdiff_t>(AppliedPointImpulses)
				);
		}
	}

	std::vector<SoftBodyCollider> WorldRoot::BuildSoftBodyColliders() const {
		std::vector<std::shared_ptr<BasePart>> Ordered;
		Ordered.reserve(Parts.size());
		for (const auto &Part : Parts)
			if (Part && !Part->GetDestroyed() && !Part->IsDestroying() && Part->GetCanCollide())
				Ordered.push_back(Part);
		std::ranges::sort(Ordered, {}, [](const std::shared_ptr<BasePart> &Part) { return Part->GetObjectId(); });
		std::vector<SoftBodyCollider> Result;
		Result.reserve(Ordered.size());
		for (const auto &Part : Ordered)
			Result.push_back({Part->GetPhysicsShape(), Part->GetCFrame()});
		return Result;
	}

	void WorldRoot::RemoveInvalidConstraintMappings() {
		std::erase_if(ConstraintJoints, [this](const auto &Entry) {
			return !Physics.IsConstraintValid(Entry.second);
		});
	}

	void WorldRoot::ShutdownPhysics() {
		if (ShuttingDownPhysics) return;
		ShuttingDownPhysics = true;
		Deformables.Shutdown();
		if (UnbindDescendants) UnbindDescendants();
		if (DescendantRemovedConnection) DescendantRemovedConnection->Disconnect();
		if (DestroyingConnection) DestroyingConnection->Disconnect();
		if (GravityConnection) GravityConnection->Disconnect();
		for (auto &[_, Connections] : PhysicsConnections)
			for (const auto &Connection : Connections) Connection->Disconnect();
		PhysicsConnections.clear();
		DirtyBodies.clear();
		DirtyDeformables.clear();
		DirtyConstraints.clear();
		TrackedParts.clear();
		TrackedDeformables.clear();
		TrackedSoftBodyMaterials.clear();
		SoftBodyAttachmentOwners.clear();
		TrackedConstraints.clear();
		ConstraintJoints.clear();
		PartBodies.clear();
		BodyParts.clear();
		DeformableIds.clear();
		IdDeformables.clear();
		DeformableStates.clear();
		LastSoftBodyProfile = {};
		Parts.clear();
		SoftBodies.clear();
	}

	std::shared_ptr<BasePart> WorldRoot::ResolvePart(PhysicsBodyId Body) const {
		auto Found = BodyParts.find(Body);
		if (Found == BodyParts.end()) return nullptr;
		auto Part = Found->second.lock();
		if (!Part || Part->GetDestroyed() || Part->IsDestroying()) return nullptr;
		auto Current = PartBodies.find(Part->GetObjectId());
		return Current != PartBodies.end() && Current->second == Body ? Part : nullptr;
	}

	std::optional<SoftBodyState> WorldRoot::GetDeformableState(ObjectId Object) const {
		auto Found = DeformableStates.find(Object);
		return Found == DeformableStates.end() ? std::nullopt : std::optional(Found->second);
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
			} else if (auto Body = std::dynamic_pointer_cast<DeformableBody>(Instance)) {
				TrackDeformable(Body);
				(void)CreateDeformableBody(Body);
			} else if (auto Attachment = std::dynamic_pointer_cast<SoftBodyAttachment>(Instance)) {
				TrackSoftBodyAttachment(Attachment);
			} else if (auto Material = std::dynamic_pointer_cast<SoftBodyMaterial>(Instance)) {
				TrackSoftBodyMaterial(Material);
			} else if (auto Constraint = std::dynamic_pointer_cast<gargantuan::Constraint>(Instance)) {
				TrackConstraint(Constraint);
				(void)CreateConstraintJoint(Constraint);
			}
		});

		DescendantRemovedConnection = DescendantRemoved->Connect([this](std::shared_ptr<Instance> Instance) {
			if (ShuttingDownPhysics) return;
			if (auto Part = std::dynamic_pointer_cast<BasePart>(Instance)) RemovePart(Part);
			else if (auto Body = std::dynamic_pointer_cast<DeformableBody>(Instance)) RemoveDeformable(Body);
			else if (auto Attachment = std::dynamic_pointer_cast<SoftBodyAttachment>(Instance))
				RemoveSoftBodyAttachment(Attachment);
			else if (auto Material = std::dynamic_pointer_cast<SoftBodyMaterial>(Instance))
				RemoveSoftBodyMaterial(Material);
			else if (auto Constraint = std::dynamic_pointer_cast<gargantuan::Constraint>(Instance))
				RemoveConstraint(Constraint);
		});
	}

	WorldRoot::~WorldRoot() {
		ShutdownPhysics();
	}

	PhysicsKinematicMotionResult WorldRoot::ResolveKinematicMotion(const PhysicsKinematicMotionRequest &Request) {
		ApplyPendingPhysicsChanges();
		return Physics.MoveKinematicCapsule(Request);
	}

	PhysicsOperationResult
	WorldRoot::BuildRaycastFilter(const RaycastParams &Params, PhysicsQueryFilter &Filter) const {
		Filter.Type = Params.FilterType == Enums::RaycastFilterType::Include ? PhysicsQueryFilterType::Include
																			 : PhysicsQueryFilterType::Exclude;
		if (Params.FilterDescendantsInstances.Values.size() > RaycastParams::MaximumFilterRoots)
			return {PhysicsOperationStatus::InvalidDescription, "RaycastParams exceeds the 128-root limit"};

		std::vector<std::shared_ptr<Instance>> Pending;
		Pending.reserve(Params.FilterDescendantsInstances.Values.size());
		std::unordered_set<ObjectId> Seen;
		for (const auto &WeakRoot : Params.FilterDescendantsInstances.Values) {
			auto Root = WeakRoot.lock();
			if (!Root || Root->GetDestroyed() || Root->IsDestroying())
				return {PhysicsOperationStatus::InvalidDescription, "RaycastParams contains a destroyed filter root"};
			bool InThisWorld = false;
			auto Current = std::optional<std::shared_ptr<Instance>>(Root);
			while (Current && *Current) {
				if (Current->get() == this) {
					InThisWorld = true;
					break;
				}
				Current = (*Current)->GetParent();
			}
			if (!InThisWorld)
				return {
					PhysicsOperationStatus::InvalidDescription, "RaycastParams filter root belongs to another world"
				};
			if (Seen.insert(Root->GetObjectId()).second) Pending.push_back(std::move(Root));
		}

		std::size_t Traversed = 0;
		while (!Pending.empty()) {
			auto Value = std::move(Pending.back());
			Pending.pop_back();
			if (!Value || Value->GetDestroyed() || Value->IsDestroying())
				return {
					PhysicsOperationStatus::InvalidDescription,
					"RaycastParams filter hierarchy changed during query preparation"
				};
			if (++Traversed > MaximumRaycastFilterTraversal)
				return {PhysicsOperationStatus::InvalidDescription, "RaycastParams descendant traversal limit reached"};
			if (auto Part = std::dynamic_pointer_cast<BasePart>(Value)) {
				auto Found = PartBodies.find(Part->GetObjectId());
				if (Found != PartBodies.end() && Physics.IsBodyValid(Found->second)) {
					Filter.Bodies.push_back(Found->second);
					if (Filter.Bodies.size() > MaximumRaycastFilterBodies)
						return {PhysicsOperationStatus::InvalidDescription, "RaycastParams collider limit reached"};
				}
			}
			for (const auto &Child : Value->Children) {
				if (!Child || Child->GetDestroyed() || Child->IsDestroying()) continue;
				if (!Seen.insert(Child->GetObjectId()).second) continue;
				if (Seen.size() > MaximumRaycastFilterTraversal)
					return {
						PhysicsOperationStatus::InvalidDescription, "RaycastParams descendant traversal limit reached"
					};
				Pending.push_back(Child);
			}
		}
		std::ranges::sort(Filter.Bodies);
		Filter.Bodies.erase(std::unique(Filter.Bodies.begin(), Filter.Bodies.end()), Filter.Bodies.end());
		return {};
	}

	WorldRaycastResult WorldRoot::ResolveRaycast(glm::vec3 Origin, glm::vec3 Direction, const RaycastParams &Params) {
		ApplyPendingPhysicsChanges();
		PhysicsQueryFilter Filter;
		if (auto Prepared = BuildRaycastFilter(Params, Filter); !Prepared.Succeeded())
			return {.Status = Prepared.Status, .Message = std::move(Prepared.Message)};
		auto Fail = [this](PhysicsOperationStatus Status, std::string Message) {
			if (Status == PhysicsOperationStatus::BackendFailure && !RaycastFailureLogged) {
				RaycastFailureLogged = true;
				LOG_ERROR(App, "[Physics:Query] Raycast failed: %s", Message.c_str());
			}
			return WorldRaycastResult{.Status = Status, .Message = std::move(Message)};
		};
		auto BackendResult = Physics.Raycast({
			.Origin = Origin,
			.Direction = Direction,
			.Filter = std::move(Filter),
		});
		if (!BackendResult.Succeeded()) return Fail(BackendResult.Status, std::move(BackendResult.Message));
		if (!BackendResult.HasHit()) return {};
		const float NearestDistance = BackendResult.Candidates.front().Distance;
		const float TieEpsilon = std::max(
			0.00001f, 8.0f * std::numeric_limits<float>::epsilon() * std::max(1.0f, std::abs(NearestDistance))
		);
		const PhysicsRaycastHit *SelectedHit = nullptr;
		std::shared_ptr<BasePart> SelectedPart;
		for (const auto &Candidate : BackendResult.Candidates) {
			if (Candidate.Distance - NearestDistance > TieEpsilon) break;
			auto CandidatePart = ResolvePart(Candidate.Body);
			if (!CandidatePart)
				return Fail(PhysicsOperationStatus::BackendFailure, "Physics raycast hit has no live semantic owner");
			if (!SelectedPart || CandidatePart->GetObjectId() < SelectedPart->GetObjectId()) {
				SelectedHit = &Candidate;
				SelectedPart = std::move(CandidatePart);
			}
		}
		if (!SelectedPart || !SelectedHit)
			return Fail(PhysicsOperationStatus::BackendFailure, "Physics raycast hit has no live semantic owner");
		return {
			.Instance = std::move(SelectedPart),
			.Position = SelectedHit->Position,
			.Normal = SelectedHit->Normal,
			.Distance = SelectedHit->Distance,
		};
	}

	int WorldRoot::Raycast(lua_State *L, Instance *InstanceValue) {
		if (!GetCurrentScriptSecurityContext().HasCapability(ScriptCapability::ReadDataModel))
			throw std::runtime_error("Physics world queries require ReadDataModel");
		auto *World = dynamic_cast<WorldRoot *>(InstanceValue);
		if (!World || World->GetDestroyed() || World->IsDestroying())
			throw std::runtime_error("Raycast requires a live WorldRoot");
		RaycastParams Params;
		if (!lua_isnoneornil(L, 4)) Params = CheckStackValue<RaycastParams>(L, 4);
		auto Result = World->ResolveRaycast(CheckStackValue<glm::vec3>(L, 2), CheckStackValue<glm::vec3>(L, 3), Params);
		if (!Result.Succeeded())
			throw std::runtime_error(Result.Message.empty() ? "Physics raycast failed" : Result.Message);
		if (!Result.HasHit()) {
			lua_pushnil(L);
			return 1;
		}
		lua_createtable(L, 0, 4);
		StackValue<std::shared_ptr<Instance>>::Push(L, Result.Instance);
		lua_setfield(L, -2, "Instance");
		StackValue<glm::vec3>::Push(L, Result.Position);
		lua_setfield(L, -2, "Position");
		StackValue<glm::vec3>::Push(L, Result.Normal);
		lua_setfield(L, -2, "Normal");
		lua_pushnumber(L, Result.Distance);
		lua_setfield(L, -2, "Distance");
		lua_setreadonly(L, -1, true);
		return 1;
	}

	int WorldRoot::MoveKinematicCapsule(lua_State *L, Instance *InstanceValue) {
		if (!GetCurrentScriptSecurityContext().HasCapability(ScriptCapability::ReadDataModel))
			throw std::runtime_error("Kinematic world queries require ReadDataModel");
		auto *World = dynamic_cast<WorldRoot *>(InstanceValue);
		if (!World || World->GetDestroyed() || World->IsDestroying())
			throw std::runtime_error("MoveKinematicCapsule requires a live WorldRoot");
		PhysicsKinematicMotionRequest Request{
			.Position = CheckStackValue<glm::vec3>(L, 2),
			.Radius = static_cast<float>(luaL_checknumber(L, 3)),
			.Height = static_cast<float>(luaL_checknumber(L, 4)),
			.Translation = CheckStackValue<glm::vec3>(L, 5),
			.Velocity = CheckStackValue<glm::vec3>(L, 6),
		};
		auto Result = World->ResolveKinematicMotion(Request);
		if (!Result.Succeeded())
			throw std::runtime_error(Result.Message.empty() ? "Kinematic capsule query failed" : Result.Message);

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
		return 1;
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
			ApplyPendingDeformableForces();
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

			auto SoftResult = Deformables.Step({
				.DeltaTime = STEP_INTERVAL,
				.Gravity = {0.0f, -GetGravity(), 0.0f},
				.Colliders = BuildSoftBodyColliders(),
			});
			LastSoftBodyProfile = SoftResult.Profile;
			if (SoftResult.CollidersTruncated)
				LOG_ERROR(App, "[Physics:SoftBody] Collider limit reached; excess colliders were rejected");
			for (auto &State : SoftResult.States) {
				auto Found = IdDeformables.find(State.Body);
				auto Body = Found == IdDeformables.end() ? nullptr : Found->second.lock();
				if (!Body || Body->GetDestroyed() || Body->IsDestroying()) continue;
				const auto Object = Body->GetObjectId();
				DeformableStates[Object] = State;
				if (!State.Simulated) continue;
				RenderDirtyAccumulator::Get().Mark(
					GetReplicationScopeId(), Object, RenderUpdateDomain::DeformableVertices,
					State.Positions ? State.Positions->size() * sizeof(glm::vec3) * 4 : 0
				);
			}
			StepAccumulator -= STEP_INTERVAL;
			++Steps;
		}
		if (Steps == MAX_STEPS_PER_FRAME) StepAccumulator = 0.0f;
	}
}
