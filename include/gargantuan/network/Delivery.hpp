#pragma once

#include <cstdint>

namespace gargantuan::network {
	enum class DeliveryMode : std::uint8_t {
		ReliableOrdered,
		UnreliableUnordered,
		UnreliableSequenced,
	};

	enum class TrafficClass : std::uint8_t {
		Control,
		StructuralReplication,
		ReliableApplication,
		RealtimeState,
		EphemeralApplication,
		Background,
	};

	[[nodiscard]] bool IsValidDeliveryMode(DeliveryMode Mode);
	[[nodiscard]] bool IsValidTrafficClass(TrafficClass Traffic);
	[[nodiscard]] bool RequiresSequenceMetadata(DeliveryMode Mode);
	[[nodiscard]] bool MayDropUnderCongestion(DeliveryMode Mode);
}
