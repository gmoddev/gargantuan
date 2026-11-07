#pragma once

#include "gargantuan/datatypes/CFrame.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gargantuan {
	inline constexpr std::size_t MaximumPhysicsEventsPerStep = 65536;

	inline constexpr std::size_t MaximumKinematicCollisionPlanes = 32;
	inline constexpr std::size_t MaximumKinematicMotionIterations = 5;
	inline constexpr std::size_t MaximumRaycastFilterBodies = 4096;
	inline constexpr std::size_t MaximumRaycastCandidates = 4096;
	inline constexpr float MaximumRaycastDistance = 100'000.0f;
	inline constexpr float MinimumRaycastDistance = 0.0001f;
	inline constexpr std::size_t MaximumPhysicsCollisionResources = 1024;
	inline constexpr std::size_t MaximumPhysicsCollisionConsumers = 65536;
	inline constexpr std::size_t MaximumPhysicsCollisionMeshShapes = 65536;
	inline constexpr std::size_t MaximumPhysicsCollisionCanonicalBytes = 64 * 1024 * 1024;
	inline constexpr std::size_t MaximumPhysicsCollisionNativeBytes = 64 * 1024 * 1024;
	inline constexpr std::size_t MaximumPhysicsCollisionResourceIdBytes = 128;

	struct PhysicsBodyId {
		std::uint32_t Slot = 0;
		std::uint32_t Generation = 0;

		[[nodiscard]] bool IsValid() const { return Slot != 0 && Generation != 0; }
		auto operator<=>(const PhysicsBodyId &) const = default;
	};

	struct PhysicsConstraintId {
		std::uint32_t Slot = 0;
		std::uint32_t Generation = 0;

		[[nodiscard]] bool IsValid() const { return Slot != 0 && Generation != 0; }
		auto operator<=>(const PhysicsConstraintId &) const = default;
	};

	enum class PhysicsShapeKind : std::uint8_t {
		Box,
		Ball,
		Cylinder,
		Wedge,
		CornerWedge,
		StaticTriangleMesh,
	};

	// Immutable, validated collision geometry. AssetService owns canonical content;
	// a physics body retains these buffers while its backend shape is live.
	struct PhysicsTriangleMesh {
		std::string ResourceId;
		std::uint64_t ContentRevision = 0;
		std::shared_ptr<const std::vector<glm::vec3>> Positions;
		std::shared_ptr<const std::vector<std::uint32_t>> Indices;
	};

	struct PhysicsShapeDesc {
		PhysicsShapeKind Kind = PhysicsShapeKind::Box;
		glm::vec3 Size{1.0f};
		std::shared_ptr<const PhysicsTriangleMesh> Mesh;
	};

	struct PhysicsBodyDesc {
		CFrame Transform{};
		PhysicsShapeDesc Shape{};
		bool Anchored = false;
		bool CanCollide = true;
		bool CanTouch = true;
		float Density = 0.7f;
	};

	enum class PhysicsConstraintKind : std::uint8_t {
		Weld,
		BallSocket,
		Hinge,
		Slider,
		Distance,
	};

	struct PhysicsConstraintDesc {
		PhysicsConstraintKind Kind = PhysicsConstraintKind::Weld;
		PhysicsBodyId BodyA{};
		PhysicsBodyId BodyB{};
		bool CollideConnected = true;
		CFrame LocalFrameA{};
		CFrame LocalFrameB{};
		bool EnableLimit = false;
		float LowerLimit = 0.0f;
		float UpperLimit = 0.0f;
		bool EnableMotor = false;
		float MotorSpeed = 0.0f;
		float MaximumMotorForce = 0.0f;
		float RestLength = 1.0f;
		float SpringFrequency = 0.0f;
		float SpringDamping = 0.7f;
	};

	struct PhysicsWorldConfig {
		glm::vec3 Gravity{0.0f, -196.2f, 0.0f};
		bool EnableSleep = true;
		std::size_t CollisionResourceLimit = MaximumPhysicsCollisionResources;
		std::size_t CollisionConsumerLimit = MaximumPhysicsCollisionConsumers;
		std::size_t CollisionCanonicalByteLimit = MaximumPhysicsCollisionCanonicalBytes;
		std::size_t CollisionNativeByteLimit = MaximumPhysicsCollisionNativeBytes;
		std::size_t CollisionMeshShapeLimit = MaximumPhysicsCollisionMeshShapes;
	};

	struct PhysicsStepConfig {
		float DeltaTime = 1.0f / 60.0f;
		int SubStepCount = 4;
	};

	struct PhysicsBodyState {
		CFrame Transform{};
		glm::vec3 LinearVelocity{0.0f};
		PhysicsBodyDesc Description{};
	};

	struct PhysicsCollisionResidency {
		std::size_t ResourceLimit = 0;
		std::size_t ConsumerLimit = 0;
		std::size_t CanonicalByteLimit = 0;
		std::size_t NativeByteLimit = 0;
		std::size_t ActiveResources = 0;
		std::size_t ActiveConsumers = 0;
		std::size_t CanonicalBytes = 0;
		std::size_t NativeMeshBytes = 0;
		std::size_t FailedAdmissions = 0;
		std::size_t CreatedResources = 0;
		std::size_t RetiredResources = 0;
		std::size_t CreatedConsumers = 0;
		std::size_t RetiredConsumers = 0;
		std::size_t MeshShapeLimit = 0;
		std::size_t ActiveMeshShapes = 0;
		std::size_t CreatedMeshShapes = 0;
		std::size_t RetiredMeshShapes = 0;
		std::size_t NativeChunks = 0;
		std::size_t ChunkTriangleLimit = 0;
		// Peak charged native mesh blobs during building; excludes builder scratch.
		std::size_t PeakBuildNativeBytes = 0;
		std::string LastAdmissionFailure;
		// Box3D does not expose per-shape storage size.
		bool NativeShapeBytesKnown = false;
	};

	struct PhysicsBodyMotion {
		PhysicsBodyId Body{};
		CFrame Transform{};
	};

	struct PhysicsKinematicMotionRequest {
		glm::vec3 Position{0.0f};
		float Radius = 1.0f;
		float Height = 4.0f;
		glm::vec3 Translation{0.0f};
		glm::vec3 Velocity{0.0f};
	};

	enum class PhysicsQueryFilterType : std::uint8_t {
		Exclude,
		Include,
	};

	struct PhysicsQueryFilter {
		PhysicsQueryFilterType Type = PhysicsQueryFilterType::Exclude;
		std::vector<PhysicsBodyId> Bodies;
	};

	struct PhysicsRaycastRequest {
		glm::vec3 Origin{0.0f};
		glm::vec3 Direction{0.0f};
		PhysicsQueryFilter Filter;
	};

	enum class PhysicsContactPhase : std::uint8_t {
		Began,
		Ended,
	};

	struct PhysicsContactEvent {
		PhysicsBodyId BodyA{};
		PhysicsBodyId BodyB{};
		PhysicsContactPhase Phase = PhysicsContactPhase::Began;
	};

	struct PhysicsStepResult {
		std::vector<PhysicsBodyMotion> Motions;
		std::vector<PhysicsContactEvent> Contacts;
		bool EventsTruncated = false;
	};

	enum class PhysicsOperationStatus : std::uint8_t {
		Success,
		InvalidId,
		InvalidDescription,
		BackendFailure,
	};

	struct PhysicsOperationResult {
		PhysicsOperationStatus Status = PhysicsOperationStatus::Success;
		std::string Message;

		[[nodiscard]] bool Succeeded() const { return Status == PhysicsOperationStatus::Success; }
	};

	struct PhysicsKinematicMotionResult {
		PhysicsOperationStatus Status = PhysicsOperationStatus::Success;
		std::string Message;
		glm::vec3 Position{0.0f};
		glm::vec3 AppliedTranslation{0.0f};
		glm::vec3 Velocity{0.0f};
		glm::vec3 ContactNormal{0.0f};
		glm::vec3 FloorNormal{0.0f};
		bool Collided = false;
		bool HasFloor = false;
		bool PlanesTruncated = false;

		[[nodiscard]] bool Succeeded() const { return Status == PhysicsOperationStatus::Success; }
	};

	struct PhysicsRaycastHit {
		PhysicsBodyId Body{};
		glm::vec3 Position{0.0f};
		glm::vec3 Normal{0.0f};
		float Distance = 0.0f;
	};

	struct PhysicsRaycastResult {
		PhysicsOperationStatus Status = PhysicsOperationStatus::Success;
		std::string Message;
		std::vector<PhysicsRaycastHit> Candidates;
		bool CandidatesTruncated = false;

		[[nodiscard]] bool Succeeded() const {
			return Status == PhysicsOperationStatus::Success;
		}
		[[nodiscard]] bool HasHit() const {
			return !Candidates.empty();
		}
	};
}

template <> struct std::hash<gargantuan::PhysicsBodyId> {
	std::size_t operator()(const gargantuan::PhysicsBodyId &Id) const noexcept {
		const auto Combined = (static_cast<std::uint64_t>(Id.Generation) << 32) | Id.Slot;
		return std::hash<std::uint64_t>{}(Combined);
	}
};

template <> struct std::hash<gargantuan::PhysicsConstraintId> {
	std::size_t operator()(const gargantuan::PhysicsConstraintId &Id) const noexcept {
		const auto Combined = (static_cast<std::uint64_t>(Id.Generation) << 32) | Id.Slot;
		return std::hash<std::uint64_t>{}(Combined);
	}
};
