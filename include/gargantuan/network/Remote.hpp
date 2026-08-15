#pragma once

#include "gargantuan/network/Limits.hpp"
#include "gargantuan/network/Sequence.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gargantuan::network {
	enum class RemoteRequestTerminalStatus : std::uint8_t {
		Success,
		Timeout,
		Cancelled,
		Disconnected,
		RemoteError,
		ProtocolRejected,
		ResourceRejected,
	};

	struct StructuredRemoteError {
		std::string Code;
		std::string Message;
		[[nodiscard]] bool IsValid() const;
	};

	struct RemoteRequestOutcome {
		RemoteRequestId Request;
		RemoteRequestTerminalStatus Status = RemoteRequestTerminalStatus::ProtocolRejected;
		std::vector<std::byte> ResultPayload;
		std::optional<StructuredRemoteError> Error;

		[[nodiscard]] bool IsValid(const NetworkLimits &Limits) const;
		[[nodiscard]] constexpr bool IsTerminal() const { return true; }
	};
}
