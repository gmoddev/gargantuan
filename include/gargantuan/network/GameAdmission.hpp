#pragma once

#include "gargantuan/identity/PlayerIdentity.hpp"
#include "gargantuan/network/Connection.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace gargantuan::network {
	inline constexpr std::size_t GameAdmissionProofBytes = 32;
	inline constexpr std::size_t MaximumPendingGameAdmissions = 64;

	// The proof is opaque to Engine. A startup-owned provider verifies its
	// audience, one-use semantics, current lease, and qualified Player identity.
	struct GameAdmissionRequest {
		std::uint64_t RequestId = 0;
		ConnectionId Connection;
		std::uint64_t ClientNonce = 0;
		std::array<std::byte, GameAdmissionProofBytes> Proof{};
	};

	using GameAdmissionCompletion = std::function<void(std::optional<PlayerIdentity>)>;

	class IGameAdmissionProvider {
	  public:
		virtual ~IGameAdmissionProvider() = default;
		// A false return is an immediate denial. Completion may run on any thread,
		// including inline, and must be called at most once for an accepted request.
		[[nodiscard]] virtual bool Begin(GameAdmissionRequest Request, GameAdmissionCompletion Completion) = 0;
		// Cancellation must not wait for a callback. Late callbacks are ignored.
		virtual void Cancel(std::uint64_t RequestId) noexcept = 0;
	};
}
