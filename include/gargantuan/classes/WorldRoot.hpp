#pragma once

#include "gargantuan/classes/BasePart.hpp"
#include "gargantuan/classes/Constraint.hpp"
#include "gargantuan/classes/DeformableBody.hpp"
#include "gargantuan/classes/SoftBodyAttachment.hpp"
#include "gargantuan/classes/SoftBodyMaterial.hpp"
#include "gargantuan/classes/generated/WorldRoot.hpp"
#include "gargantuan/datatypes/RaycastParams.hpp"
#include "gargantuan/physics/PhysicsBackend.hpp"
#include "gargantuan/physics/SoftBodyBackend.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gargantuan {
	class AssetService;
	class LocalLight;
	class ReflectionProbe;
	struct WorldRootTestAccess;

	struct WorldRaycastResult {
		PhysicsOperationStatus Status = PhysicsOperationStatus::Success;
		std::string Message;
		std::shared_ptr<BasePart> Instance;
		glm::vec3 Position{0.0f};
		glm::vec3 Normal{0.0f};
		float Distance = 0.0f;

		[[nodiscard]] bool Succeeded() const {
			return Status == PhysicsOperationStatus::Success;
		}
		[[nodiscard]] bool HasHit() const {
			return Instance != nullptr;
		}
	};

	class WorldRoot : public Instance {
		I_WorldRoot;
		friend class Character;

		static constexpr int MAX_STEPS_PER_FRAME = 4;
		static constexpr float STEP_INTERVAL = 1.0f / 60.0f;
		static constexpr int SUB_STEP_COUNT = 4;
		static constexpr std::size_t MaximumRaycastFilterTraversal = 16'384;

		WorldRoot();
		~WorldRoot() override;

		PhysicsWorld Physics;
		SoftBodyWorld Deformables;
		float StepAccumulator = 0.0f;
		std::vector<std::shared_ptr<BasePart>> Parts;
		std::unordered_set<ObjectId> PartMembership;
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
		std::unordered_map<ObjectId, std::vector<SignalConnection::Pointer>> JointAttachmentConnections;
		std::unordered_set<ObjectId> DirtyBodies;
		// Test-only, Main-owned attribution. Normal listener behavior is unchanged when disabled.
		struct CFrameDirtyCallbackProfile {
			std::uint64_t Calls = 0;
			std::uint64_t NewDirty = 0;
			std::uint64_t Coalesced = 0;
			std::uint64_t Suppressed = 0;
			std::uint64_t Nanoseconds = 0;
		};
		bool ProfileCFrameDirtyCallback = false;
		CFrameDirtyCallbackProfile CFrameDirtyProfile;
		std::unordered_set<ObjectId> FailedMeshUpdatesLogged;
		std::unordered_set<ObjectId> DirtyDeformables;
		std::unordered_set<ObjectId> DirtyConstraints;
		std::unordered_set<ObjectId> FailedConstraintUpdatesLogged;
		SignalConnection::Pointer GravityConnection;
		SignalConnection::Pointer DestroyingConnection;
		SignalConnection::Pointer DescendantRemovedConnection;
		std::function<void()> UnbindDescendants;
		bool GravityDirty = false;
		ObjectId PublishingPhysicsPart;
		bool ShuttingDownPhysics = false;
		bool RaycastFailureLogged = false;
		bool DynamicMeshFallbackLogged = false;
		bool MeshScaleFallbackLogged = false;
		std::weak_ptr<AssetService> CollisionAssets;
		std::uint64_t CollisionAssetChangeSequence = 0;
		std::map<ObjectId, std::weak_ptr<LocalLight>> LocalLights;
		std::map<ObjectId, std::weak_ptr<ReflectionProbe>> ReflectionProbes;

		[[nodiscard]] PhysicsKinematicMotionResult ResolveKinematicMotion(const PhysicsKinematicMotionRequest &Request);
		[[nodiscard]] WorldRaycastResult
		ResolveRaycast(glm::vec3 Origin, glm::vec3 Direction, const RaycastParams &Params = {});
		[[nodiscard]] WorldRaycastResult ResolveShapeCast(const PhysicsQueryShape &Shape, glm::vec3 Direction, const RaycastParams &Params = {});
		struct OverlapResult {
			PhysicsOperationStatus Status = PhysicsOperationStatus::Success;
			std::string Message;
			std::vector<std::shared_ptr<BasePart>> Parts;
			bool Truncated = false;
			[[nodiscard]] bool Succeeded() const { return Status == PhysicsOperationStatus::Success; }
		};
		[[nodiscard]] OverlapResult ResolveOverlap(const PhysicsQueryShape &Shape, const RaycastParams &Params = {}, std::size_t MaxResults = MaximumPhysicsOverlapResults);
		[[nodiscard]] std::optional<SoftBodyState> GetDeformableState(ObjectId Object) const;
		[[nodiscard]] std::optional<SoftBodyInspection> GetDeformableInspection(ObjectId Object) const;
		[[nodiscard]] std::optional<glm::vec3> GetDeformableVertex(ObjectId Object, std::size_t VertexIndex) const;
		[[nodiscard]] const SoftBodyStepProfile &GetLastSoftBodyProfile() const { return LastSoftBodyProfile; }
		[[nodiscard]] const std::map<ObjectId, std::weak_ptr<LocalLight>> &GetLocalLights() const { return LocalLights; }
		[[nodiscard]] const std::map<ObjectId, std::weak_ptr<ReflectionProbe>> &GetReflectionProbes() const { return ReflectionProbes; }

	  private:
		[[nodiscard]] WorldRaycastResult ResolveQueryHit(PhysicsRaycastResult Result);
		static int RunShapeQuery(lua_State *L, Instance *Value, PhysicsQueryShapeKind Kind, bool IsOverlap);
		friend struct WorldRootTestAccess;
		[[nodiscard]] PhysicsBodyDesc DescribePart(const BasePart &Part);
		[[nodiscard]] SoftBodyDefinition DescribeDeformable(DeformableBody &Body) const;
		[[nodiscard]] PhysicsBodyId CreatePartBody(const std::shared_ptr<BasePart> &Part);
		[[nodiscard]] SoftBodyId CreateDeformableBody(const std::shared_ptr<DeformableBody> &Body);
		[[nodiscard]] PhysicsConstraintId CreateConstraintJoint(const std::shared_ptr<Constraint> &Constraint);
		void TrackPart(const std::shared_ptr<BasePart> &Part);
		void TrackDeformable(const std::shared_ptr<DeformableBody> &Body);
		void TrackSoftBodyAttachment(const std::shared_ptr<SoftBodyAttachment> &Attachment);
		void TrackSoftBodyMaterial(const std::shared_ptr<SoftBodyMaterial> &Material);
		void TrackConstraint(const std::shared_ptr<Constraint> &Constraint);
		void RefreshJointAttachmentConnections(const std::shared_ptr<Constraint> &Constraint);
		void RemovePart(const std::shared_ptr<BasePart> &Part);
		void RemoveDeformable(const std::shared_ptr<DeformableBody> &Body);
		void RemoveSoftBodyAttachment(const std::shared_ptr<SoftBodyAttachment> &Attachment);
		void RemoveSoftBodyMaterial(const std::shared_ptr<SoftBodyMaterial> &Material);
		void RemoveConstraint(const std::shared_ptr<Constraint> &Constraint);
		void ApplyPendingPhysicsChanges();
		void ObserveCollisionAssetChanges();
		void ApplyPendingImpulses();
		void ApplyPendingDeformableForces();
		[[nodiscard]] std::vector<SoftBodyCollider> BuildSoftBodyColliders() const;
		void ReconcileConstraint(const std::shared_ptr<Constraint> &Constraint);
		void RemoveInvalidConstraintMappings();
		void ShutdownPhysics();
		[[nodiscard]] std::shared_ptr<BasePart> ResolvePart(PhysicsBodyId Body) const;
		[[nodiscard]] PhysicsOperationResult
		BuildRaycastFilter(const RaycastParams &Params, PhysicsQueryFilter &Filter) const;
	};
}
