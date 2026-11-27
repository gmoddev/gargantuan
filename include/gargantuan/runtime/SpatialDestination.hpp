#pragma once

#include "gargantuan/physics/PhysicsTypes.hpp"
#include "gargantuan/runtime/ObjectId.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gargantuan {
	using SpatialMovementLease = std::shared_ptr<const std::function<bool()>>;
	struct SpatialDestinationBounds {
		glm::vec3 Minimum{}, Maximum{};
		bool operator==(const SpatialDestinationBounds &) const = default;
	};

	struct SpatialMovementRequest {
		ObjectId Member, SourceDomain;
		std::uint64_t SourceActivation{};
		CFrame SourcePose, DestinationPose;
	};

	enum class SpatialCollisionProvider : std::uint8_t { Rigid, Terrain, BlockGrid };

	struct SpatialInstalledCollider {
		PhysicsBodyId Body;
		std::array<std::int64_t, 3> Chunk{};
		std::uint64_t DerivativeRevision{};
		std::uint8_t CatalogGeometry{}, Orientation{}, Piece{};
		bool operator==(const SpatialInstalledCollider &) const = default;
	};

	struct SpatialProviderReceipt {
		ObjectId Owner;
		SpatialCollisionProvider Provider{};
		std::uint64_t Lifetime{}, ContentRevision{};
		CFrame Pose;
		glm::vec3 Size{};
		PhysicsShapeKind Shape{};
		std::uint16_t CollisionGroup{}, CollisionMask{};
		bool AuthoritativeEmpty{};
		std::string Resource;
		std::vector<SpatialInstalledCollider> Installed;
		bool operator==(const SpatialProviderReceipt &Other) const {
			return Owner == Other.Owner && Provider == Other.Provider && Lifetime == Other.Lifetime &&
				ContentRevision == Other.ContentRevision && Pose.Position == Other.Pose.Position &&
				Pose.Rotation == Other.Pose.Rotation && Size == Other.Size && Shape == Other.Shape &&
				CollisionGroup == Other.CollisionGroup && CollisionMask == Other.CollisionMask &&
				AuthoritativeEmpty == Other.AuthoritativeEmpty && Resource == Other.Resource && Installed == Other.Installed;
		}
	};

	// Local Main admission. Object and native body handles belong to this DataModel.
	struct SpatialDestinationReceipt {
		ObjectId Realm, Domain;
		std::uint64_t Activation{};
		SpatialDestinationBounds Bounds;
		std::vector<ObjectId> RequiredOwners;
		std::vector<SpatialProviderReceipt> Providers;
		bool operator==(const SpatialDestinationReceipt &) const = default;
	};
}
