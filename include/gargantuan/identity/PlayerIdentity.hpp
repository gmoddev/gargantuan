#pragma once

#include <compare>
#include <cstddef>
#include <string>
#include <string_view>

namespace gargantuan {
	struct PlayerIdentity {
		static constexpr std::size_t MaximumProviderBytes = 64;
		static constexpr std::size_t MaximumSubjectBytes = 256;

		std::string Provider;
		std::string Subject;

		auto operator<=>(const PlayerIdentity &) const = default;
	};

	void ValidateIdentityProviderName(std::string_view Value);
	void ValidatePlayerIdentity(const PlayerIdentity &Identity);
}
