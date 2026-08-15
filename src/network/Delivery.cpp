#include "gargantuan/network/Delivery.hpp"

namespace gargantuan::network {
	bool IsValidDeliveryMode(DeliveryMode Mode) {
		switch (Mode) {
		case DeliveryMode::ReliableOrdered:
		case DeliveryMode::UnreliableUnordered:
		case DeliveryMode::UnreliableSequenced:
			return true;
		}
		return false;
	}

	bool IsValidTrafficClass(TrafficClass Traffic) {
		switch (Traffic) {
		case TrafficClass::Control:
		case TrafficClass::StructuralReplication:
		case TrafficClass::ReliableApplication:
		case TrafficClass::RealtimeState:
		case TrafficClass::EphemeralApplication:
		case TrafficClass::Background:
			return true;
		}
		return false;
	}

	bool RequiresSequenceMetadata(DeliveryMode Mode) { return Mode == DeliveryMode::UnreliableSequenced; }
	bool MayDropUnderCongestion(DeliveryMode Mode) { return Mode != DeliveryMode::ReliableOrdered; }
}
