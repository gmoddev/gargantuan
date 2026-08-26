#pragma once

#include "gargantuan/datatypes/CFrame.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
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
	};

	struct PhysicsShapeDesc {
		PhysicsShapeKind Kind = PhysicsShapeKind::Box;
		glm::vec3 Size{1.0f};
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
	};

	struct PhysicsConstraintDesc {
		PhysicsConstraintKind Kind = PhysicsConstraintKind::Weld;
		PhysicsBodyId BodyA{};
		PhysicsBodyId BodyB{};
		bool CollideConnected = true;
	};

	struct PhysicsWorldConfig {
		glm::vec3 Gravity{0.0f, -196.2f, 0.0f};
		bool EnableSleep = true;
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
