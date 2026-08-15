#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace gargantuan::network {
	enum class DisconnectReason : std::uint8_t {
		LocalShutdown,
		RemoteShutdown,
		Timeout,
		AuthenticationFailure,
		ProtocolViolation,
		ResourceExhaustion,
		TransportFailure,
		IncompatibleVersion,
	};

	struct DisconnectInfo {
		DisconnectReason Reason = DisconnectReason::TransportFailure;
		std::string Diagnostic;

		[[nodiscard]] bool IsValid() const;
	};

	enum class TransportOperationStatus : std::uint8_t {
		Succeeded,
		WouldBlock,
		InvalidState,
		InvalidConnection,
		MessageRejected,
		ResourceExhausted,
		TransportFailure,
	};

	struct TransportOperationResult {
		TransportOperationStatus Status = TransportOperationStatus::TransportFailure;
		std::optional<DisconnectInfo> TerminalDisconnect;

		[[nodiscard]] bool Succeeded() const { return Status == TransportOperationStatus::Succeeded; }
		[[nodiscard]] bool IsTerminal() const { return TerminalDisconnect.has_value(); }
		[[nodiscard]] bool IsValid() const;
	};

	[[nodiscard]] constexpr bool IsTerminalDisconnectReason(DisconnectReason Reason) {
		switch (Reason) {
		case DisconnectReason::LocalShutdown:
		case DisconnectReason::RemoteShutdown:
		case DisconnectReason::Timeout:
		case DisconnectReason::AuthenticationFailure:
		case DisconnectReason::ProtocolViolation:
		case DisconnectReason::ResourceExhaustion:
		case DisconnectReason::TransportFailure:
		case DisconnectReason::IncompatibleVersion:
			return true;
		}
		return false;
	}
}
