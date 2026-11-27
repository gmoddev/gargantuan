#pragma once

#include "gargantuan/network/SpatialTransfer.hpp"
#include "gargantuan/runtime/SpatialResidency.hpp"
#include <any>
#include <expected>
#include <string_view>

namespace gargantuan {
	class Instance;
	class DataModel;
	class Player;
}
namespace gargantuan::network {
	struct SpatialTransferTicket {
		std::shared_ptr<const SpatialTransferStatus> Result;
		ConnectionId Connection;
		std::uint64_t SessionEpoch{}, Serial{};
	};
	[[nodiscard]] std::shared_ptr<BasePart> GetSpatialTransferSubject(const Player &Owner);
	class SpatialTransferProtection final {
	  public:
		explicit SpatialTransferProtection(std::function<bool(ObjectId)> Controlled);
		SpatialTransferReservations Reservations;
		using CreatorRequest = std::function<std::expected<SpatialTransferTicket, SpatialTransferStatus>(
			const Player &, ObjectId, const CFrame &, const SpatialDestinationBounds &, SpatialResidencyAudience
		)>;
		// Installed by this DataModel's authenticated server GameSession. Retire clears both callbacks.
		CreatorRequest RequestCreatorTransfer;
		std::function<void(const SpatialTransferTicket &)> CancelCreatorTransfer;
		[[nodiscard]] bool Controls(const Instance &Object) const;
		[[nodiscard]] bool
		AllowsProperty(const Instance &Object, std::string_view Property, const std::any &Value) const;
		[[nodiscard]] bool AllowsResource(const Instance &Object) const;
		[[nodiscard]] bool
		AllowsMotion(const Instance &Object, const WorldRoot &World, const SpatialDestinationBounds &Swept) const;
		[[nodiscard]] bool AllowsSimulationPose(const BasePart &Object, const CFrame &Pose) const;
		[[nodiscard]] bool AllowsReparent(const Instance &Object, const WorldRoot *NewWorld) const;
		[[nodiscard]] bool
		RequiresGrant(const Instance &Subject, const WorldRoot &Target, const SpatialDestinationBounds &Bounds) const;
		void CheckDestruction(const Instance &Root);
		void Retire();

	  private:
		std::function<bool(ObjectId)> Controlled;
	};
}
