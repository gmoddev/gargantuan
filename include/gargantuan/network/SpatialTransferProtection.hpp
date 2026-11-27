#pragma once

#include "gargantuan/network/SpatialTransfer.hpp"
#include <any>
#include <string_view>

namespace gargantuan {
	class Instance;
	class DataModel;
}
namespace gargantuan::network {
	class SpatialTransferProtection final {
	  public:
		explicit SpatialTransferProtection(std::function<bool(ObjectId)> Controlled);
		SpatialTransferReservations Reservations;
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
