#include "gargantuan/network/Outcome.hpp"

#include "gargantuan/runtime/ProtocolInput.hpp"

namespace gargantuan::network {
	bool DisconnectInfo::IsValid() const {
		if (!IsTerminalDisconnectReason(Reason)) return false;
		try {
			ValidateProtocolString(Diagnostic, 4096, "Disconnect diagnostic");
			return true;
		} catch (...) {
			return false;
		}
	}

	bool TransportOperationResult::IsValid() const {
		if (TerminalDisconnect && !TerminalDisconnect->IsValid()) return false;
		switch (Status) {
		case TransportOperationStatus::Succeeded:
		case TransportOperationStatus::WouldBlock:
			return !TerminalDisconnect;
		case TransportOperationStatus::InvalidState:
		case TransportOperationStatus::InvalidConnection:
		case TransportOperationStatus::MessageRejected:
		case TransportOperationStatus::ResourceExhausted:
		case TransportOperationStatus::TransportFailure:
			return true;
		}
		return false;
	}
}
