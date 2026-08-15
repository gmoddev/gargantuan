#include "gargantuan/network/Remote.hpp"

#include "gargantuan/runtime/ProtocolInput.hpp"

namespace gargantuan::network {
	bool StructuredRemoteError::IsValid() const {
		try {
			ValidateProtocolString(Code, 128, "Remote error code");
			ValidateProtocolString(Message, 4096, "Remote error message");
			return !Code.empty();
		} catch (...) {
			return false;
		}
	}

	bool RemoteRequestOutcome::IsValid(const NetworkLimits &Limits) const {
		if (!Limits.IsValid() || !Request.IsValid() || ResultPayload.size() > Limits.MaximumDecodedMessageBytes)
			return false;
		switch (Status) {
		case RemoteRequestTerminalStatus::Success:
			return !Error;
		case RemoteRequestTerminalStatus::RemoteError:
			return ResultPayload.empty() && Error && Error->IsValid();
		case RemoteRequestTerminalStatus::Timeout:
		case RemoteRequestTerminalStatus::Cancelled:
		case RemoteRequestTerminalStatus::Disconnected:
		case RemoteRequestTerminalStatus::ProtocolRejected:
		case RemoteRequestTerminalStatus::ResourceRejected:
			return ResultPayload.empty() && !Error;
		}
		return false;
	}
}
