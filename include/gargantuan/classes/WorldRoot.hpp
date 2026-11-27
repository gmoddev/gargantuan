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
#include "gargantuan/runtime/RuntimeMode.hpp"
#include "gargantuan/runtime/SpatialDestination.hpp"
#include <span>

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gargantuan {
	class AssetService;
	class BlockGrid;
	struct BlockGridWorldState;
	class LocalLight;
	class ReflectionProbe;
	class Terrain;
	struct RenderCameraInput;
	struct WorldRootTestAccess;

	namespace network {
		class GameSession;
	}

	namespace terrain {
		enum class EditStatus;
		class Field;
		class PhysicsStage;
		class TerrainMaterialPalette;
		class TerrainRuntime;
		struct ChunkPublicationCandidate;
		struct TerrainMutation;
		struct RenderState;
	}

	struct WorldRaycastResult {
		PhysicsOperationStatus Status = PhysicsOperationStatus::Success;
		std::string Message;
		std::shared_ptr<BasePart> Instance;
		glm::vec3 Position{0.0f};
		glm::vec3 Normal{0.0f};
		float Distance = 0.0f;
		std::shared_ptr<Terrain> TerrainInstance;
		std::shared_ptr<BlockGrid> GridInstance;
		std::optional<glm::ivec3> Cell;

		[[nodiscard]] bool Succeeded() const {
			return Status == PhysicsOperationStatus::Success;
		}
		[[nodiscard]] bool HasHit() const {
			return Instance != nullptr || TerrainInstance != nullptr || GridInstance != nullptr;
		}

		[[nodiscard]] std::shared_ptr<gargantuan::Instance> GetHitInstance() const;
	};

	class WorldRoot : public Instance {
		I_WorldRoot;
		friend class Character;
		friend class SpatialDomainMember;

		static constexpr int MAX_STEPS_PER_FRAME = 4;
		static constexpr float STEP_INTERVAL = 1.0f / 60.0f;
		static constexpr int SUB_STEP_COUNT = 4;
		static constexpr std::size_t MaximumRaycastFilterTraversal = 16'384;

		WorldRoot();
		~WorldRoot() override;
		[[nodiscard]] static std::shared_ptr<WorldRoot> GetEffectiveWorld(const Instance &Member);
		[[nodiscard]] std::expected<SpatialDestinationReceipt, PhysicsOperationStatus> PrepareSpatialDestination(
			const SpatialDestinationBounds &Bounds, std::span<const ObjectId> RequiredOwners = {});
		[[nodiscard]] bool ValidateSpatialDestination(const SpatialDestinationReceipt &Receipt);
		[[nodiscard]] const std::map<ObjectId, std::weak_ptr<Instance>> &GetExplicitSpatialRoots() const { return ExplicitSpatialRoots; }

		PhysicsWorld Physics;
		SoftBodyWorld Deformables;
		float StepAccumulator = 0.0f;
		bool SteppingPhysics = false;
		std::vector<std::shared_ptr<BasePart>> Parts;
		std::unordered_set<ObjectId> PartMembership;
		struct DynamicReadbackRecord {
			ObjectId Object;
			PhysicsBodyId Body;
			std::weak_ptr<BasePart> Part;
		};
		std::vector<DynamicReadbackRecord> DynamicReadback;
		std::unordered_map<ObjectId, std::size_t> DynamicReadbackPositions;
		bool FullDynamicReadbackNeeded = false;
		bool IndexedDynamicReadbackActive = false;
		bool AcceptedDynamicBodyCountKnown = true;
		std::size_t AcceptedDynamicBodyCount = 0;
		struct DynamicReadbackPolicy {
			std::size_t ActivatePartCount = 8192;
			std::size_t RetirePartCount = 4096;
			std::size_t ActivateDynamicDivisor = 8;
			std::size_t RetireDynamicDivisor = 4;
		} ReadbackPolicy;
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
		bool FullPartScanNeeded = false;
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
		void ForEachBlockGrid(const std::function<void(const BlockGrid &)> &Visit) const;
		[[nodiscard]] bool HasBlockGrids() const { return BlockGridState != nullptr; }

		void StepPhysics(double DeltaTime, std::optional<std::vector<std::shared_ptr<Instance>>> Instances);
		[[nodiscard]] PhysicsKinematicMotionResult ResolveKinematicMotion(const PhysicsKinematicMotionRequest &Request);
		[[nodiscard]] WorldRaycastResult
		ResolveRaycast(glm::vec3 Origin, glm::vec3 Direction, const RaycastParams &Params = {});
		[[nodiscard]] WorldRaycastResult ResolveShapeCast(const PhysicsQueryShape &Shape, glm::vec3 Direction, const RaycastParams &Params = {});
		struct OverlapResult {
			struct GridCell { std::shared_ptr<BlockGrid> Grid; glm::ivec3 Cell; std::string Revision; };
			PhysicsOperationStatus Status = PhysicsOperationStatus::Success;
			std::string Message;
			std::vector<std::shared_ptr<BasePart>> Parts;
			bool Truncated = false;
			std::shared_ptr<Terrain> TerrainInstance;
			std::vector<std::shared_ptr<BlockGrid>> BlockGrids;
			std::vector<GridCell> GridCells;
			[[nodiscard]] bool Succeeded() const { return Status == PhysicsOperationStatus::Success; }
		};
		[[nodiscard]] OverlapResult ResolveOverlap(const PhysicsQueryShape &Shape, const RaycastParams &Params = {}, std::size_t MaxResults = MaximumPhysicsOverlapResults);
		[[nodiscard]] std::optional<SoftBodyState> GetDeformableState(ObjectId Object) const;
		[[nodiscard]] std::optional<SoftBodyInspection> GetDeformableInspection(ObjectId Object) const;
		[[nodiscard]] std::optional<glm::vec3> GetDeformableVertex(ObjectId Object, std::size_t VertexIndex) const;
		[[nodiscard]] const SoftBodyStepProfile &GetLastSoftBodyProfile() const { return LastSoftBodyProfile; }
		[[nodiscard]] const std::map<ObjectId, std::weak_ptr<LocalLight>> &GetLocalLights() const { return LocalLights; }
		[[nodiscard]] const std::map<ObjectId, std::weak_ptr<ReflectionProbe>> &GetReflectionProbes() const { return ReflectionProbes; }
		[[nodiscard]] std::shared_ptr<Terrain> GetOwnedTerrain() const;
		void RestoreTerrainField(std::unique_ptr<terrain::Field> Restored);
		[[nodiscard]] std::shared_ptr<const terrain::RenderState> GetTerrainRenderState() const noexcept;
		[[nodiscard]] bool IsTerrainCollisionReady() const noexcept { return HasTerrainRuntime(); }

	  private:
		friend class Engine;
		std::unique_ptr<BlockGridWorldState> BlockGridState;
		void AddBlockGrid(const std::shared_ptr<Instance> &Value);
		void RemoveBlockGrid(const std::shared_ptr<Instance> &Value);
		bool SyncBlockGridCollision();
		bool AppendBlockGridFilter(ObjectId Object, std::vector<PhysicsBodyId> &Bodies) const;
		std::shared_ptr<BlockGrid> ResolveBlockGrid(PhysicsBodyId Body) const;
		std::optional<glm::ivec3> ResolveBlockCell(PhysicsBodyId Body, glm::vec3 Position) const;
		PhysicsOperationStatus VisitBlockOverlap(PhysicsBodyId Body,const PhysicsQueryShape& Query,std::size_t& Work,
			const std::function<bool(glm::ivec3)>& Emit) const;
		friend class EditorHost;
		friend class MutationGateway;
		friend class Terrain;
		friend class Workspace;
		friend class SpatialRegion;
		friend class Instance;
		void InitializeTerrain();
		std::weak_ptr<Terrain> TerrainChild;
		bool TerrainRestored = false;
		friend class network::GameSession;
		// A Main-owned session may bound motion to its installed collision coverage.
		using TerrainMotionGuard = std::function<bool(ObjectId, glm::vec3, glm::vec3)>;
		std::weak_ptr<const TerrainMotionGuard> TerrainMotionAdmission;
		bool TerrainReplicaCoverageRequired{};
		[[nodiscard]] bool HasTerrainQueryCoverage(glm::vec3 Origin, glm::vec3 Destination, glm::vec3 Extents) const;
		[[nodiscard]] bool InstallTerrainReplica(std::unique_ptr<terrain::Field> Field,
			std::vector<terrain::ChunkPublicationCandidate> Chunks, std::uint64_t Publication);
		void UpdateTerrainVisualDensity(const RenderCameraInput &Camera, std::uint32_t Width, std::uint32_t Height);
		[[nodiscard]] WorldRaycastResult ResolveQueryHit(PhysicsRaycastResult Result, bool IncludeTerrain);
		static int RunShapeQuery(lua_State *L, Instance *Value, PhysicsQueryShapeKind Kind, bool IsOverlap);
		friend struct WorldRootTestAccess;
		[[nodiscard]] PhysicsBodyDesc DescribePart(const BasePart &Part);
		[[nodiscard]] SoftBodyDefinition DescribeDeformable(DeformableBody &Body) const;
		[[nodiscard]] PhysicsBodyId CreatePartBody(const std::shared_ptr<BasePart> &Part);
		void SyncDynamicReadback(const std::shared_ptr<BasePart> &Part, PhysicsBodyId Body);
		void RemoveDynamicReadback(ObjectId Object);
		void DeactivateDynamicReadback();
		[[nodiscard]] bool ShouldIndexDynamicReadback() const;
		void ReadbackOrderedParts(bool RecountAcceptedDynamics);
		void ReadbackDynamicBodies();
		[[nodiscard]] SoftBodyId CreateDeformableBody(const std::shared_ptr<DeformableBody> &Body);
		[[nodiscard]] PhysicsConstraintId CreateConstraintJoint(const std::shared_ptr<Constraint> &Constraint);
		std::map<ObjectId, std::weak_ptr<Instance>> ExplicitSpatialRoots;
		static void MoveSpatialMembers(const std::shared_ptr<Instance> &Root,
			const std::shared_ptr<WorldRoot> &Before, const std::shared_ptr<WorldRoot> &After);
		void AdmitSpatialMember(const std::shared_ptr<Instance> &Member);
		void RemoveSpatialMember(const std::shared_ptr<Instance> &Member);
		void TrackPart(const std::shared_ptr<BasePart> &Part);
		bool MarkPartDirty(ObjectId Object);
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
		[[nodiscard]] std::shared_ptr<Terrain> GetTerrainQueryOwner() const;
		[[nodiscard]] PhysicsOperationResult
		BuildRaycastFilter(const RaycastParams &Params, PhysicsQueryFilter &Filter);

		void InitializeTerrainRuntime(RuntimeMode Mode, bool AdmitNetworkTerrain = false,
			bool ExperimentalVisualDensity = false, std::shared_ptr<const terrain::TerrainMaterialPalette> Palette = {});
		void PollTerrainWork();
		void RetireDomainTerrainRuntime();
		[[nodiscard]] bool IsSpatiallyAvailable() const;
		std::uint64_t SpatialActivation{1};
		std::uint64_t TerrainTicketBase{};
		std::uint64_t LastTerrainTicket{};
		[[nodiscard]] std::expected<std::uint64_t, terrain::EditStatus>
		SubmitTerrainEdit(const Terrain &Owner, const terrain::TerrainMutation &Edit);
		[[nodiscard]] bool CancelTerrainEdit(const Terrain &Owner, std::uint64_t Ticket) noexcept;
		struct TerrainTicketInfo {
			std::string_view Status;
			std::optional<std::uint64_t> Revision;
			std::string_view Diagnostic;
		};
		[[nodiscard]] std::optional<TerrainTicketInfo> GetTerrainTicket(std::uint64_t Ticket) const noexcept;
		[[nodiscard]] bool HasTerrainRuntime() const noexcept;

		[[nodiscard]] const terrain::PhysicsStage *GetTerrainPhysics() const noexcept;
		std::unique_ptr<terrain::TerrainRuntime> TerrainRuntimeValue;
		struct TerrainTickets;
		std::unique_ptr<TerrainTickets> TerrainEditTickets;
	};
}
