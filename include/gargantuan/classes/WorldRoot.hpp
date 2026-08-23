#pragma once

#include "gargantuan/classes/BasePart.hpp"
#include "gargantuan/classes/Constraint.hpp"
#include "gargantuan/classes/DeformableBody.hpp"
#include "gargantuan/classes/SoftBodyAttachment.hpp"
#include "gargantuan/classes/SoftBodyMaterial.hpp"
#include "gargantuan/classes/generated/WorldRoot.hpp"
#include "gargantuan/physics/PhysicsBackend.hpp"
#include "gargantuan/physics/SoftBodyBackend.hpp"

#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gargantuan {
	struct WorldRootTestAccess;

	class WorldRoot : public Instance {
		I_WorldRoot;

		static constexpr int MAX_STEPS_PER_FRAME = 4;
		static constexpr float STEP_INTERVAL = 1.0f / 60.0f;
		static constexpr int SUB_STEP_COUNT = 4;

		WorldRoot();
		~WorldRoot() override;

		PhysicsWorld Physics;
		SoftBodyWorld Deformables;
		float StepAccumulator = 0.0f;
		std::vector<std::shared_ptr<BasePart>> Parts;
		std::vector<std::shared_ptr<DeformableBody>> SoftBodies;
		std::unordered_map<ObjectId, PhysicsBodyId> PartBodies;
		std::unordered_map<PhysicsBodyId, std::weak_ptr<BasePart>> BodyParts;
		std::unordered_map<ObjectId, SoftBodyId> DeformableIds;
		std::unordered_map<SoftBodyId, std::weak_ptr<DeformableBody>> IdDeformables;
		std::unordered_map<ObjectId, SoftBodyState> DeformableStates;
		SoftBodyStepProfile LastSoftBodyProfile;
		std::unordered_map<ObjectId, PhysicsConstraintId> ConstraintJoints;
		std::unordered_map<ObjectId, std::weak_ptr<BasePart>> TrackedParts;
		std::unordered_map<ObjectId, std::weak_ptr<DeformableBody>> TrackedDeformables;
		std::unordered_map<ObjectId, std::weak_ptr<SoftBodyMaterial>> TrackedSoftBodyMaterials;
		std::unordered_map<ObjectId, ObjectId> SoftBodyAttachmentOwners;
		std::unordered_map<ObjectId, std::weak_ptr<Constraint>> TrackedConstraints;
		std::unordered_map<ObjectId, std::vector<SignalConnection::Pointer>> PhysicsConnections;
		std::unordered_set<ObjectId> DirtyBodies;
		std::unordered_set<ObjectId> DirtyDeformables;
		std::unordered_set<ObjectId> DirtyConstraints;
		SignalConnection::Pointer GravityConnection;
		SignalConnection::Pointer DestroyingConnection;
		SignalConnection::Pointer DescendantRemovedConnection;
		std::function<void()> UnbindDescendants;
		bool GravityDirty = false;
		bool PublishingPhysicsState = false;
		bool ShuttingDownPhysics = false;

		[[nodiscard]] PhysicsKinematicMotionResult ResolveKinematicMotion(const PhysicsKinematicMotionRequest &Request);
		[[nodiscard]] std::optional<SoftBodyState> GetDeformableState(ObjectId Object) const;
		[[nodiscard]] const SoftBodyStepProfile &GetLastSoftBodyProfile() const { return LastSoftBodyProfile; }

	  private:
		friend struct WorldRootTestAccess;
		[[nodiscard]] PhysicsBodyDesc DescribePart(const BasePart &Part) const;
		[[nodiscard]] SoftBodyDefinition DescribeDeformable(DeformableBody &Body) const;
		[[nodiscard]] PhysicsBodyId CreatePartBody(const std::shared_ptr<BasePart> &Part);
		[[nodiscard]] SoftBodyId CreateDeformableBody(const std::shared_ptr<DeformableBody> &Body);
		[[nodiscard]] PhysicsConstraintId CreateConstraintJoint(const std::shared_ptr<Constraint> &Constraint);
		void TrackPart(const std::shared_ptr<BasePart> &Part);
		void TrackDeformable(const std::shared_ptr<DeformableBody> &Body);
		void TrackSoftBodyAttachment(const std::shared_ptr<SoftBodyAttachment> &Attachment);
		void TrackSoftBodyMaterial(const std::shared_ptr<SoftBodyMaterial> &Material);
		void TrackConstraint(const std::shared_ptr<Constraint> &Constraint);
		void RemovePart(const std::shared_ptr<BasePart> &Part);
		void RemoveDeformable(const std::shared_ptr<DeformableBody> &Body);
		void RemoveSoftBodyAttachment(const std::shared_ptr<SoftBodyAttachment> &Attachment);
		void RemoveSoftBodyMaterial(const std::shared_ptr<SoftBodyMaterial> &Material);
		void RemoveConstraint(const std::shared_ptr<Constraint> &Constraint);
		void ApplyPendingPhysicsChanges();
		void ApplyPendingImpulses();
		void ApplyPendingDeformableForces();
		[[nodiscard]] std::vector<SoftBodyCollider> BuildSoftBodyColliders() const;
		void ReconcileConstraint(const std::shared_ptr<Constraint> &Constraint);
		void RemoveInvalidConstraintMappings();
		void ShutdownPhysics();
		[[nodiscard]] std::shared_ptr<BasePart> ResolvePart(PhysicsBodyId Body) const;
	};
}
