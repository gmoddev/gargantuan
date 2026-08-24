#include "gargantuan/identity/PlayerIdentity.hpp"

#include "gargantuan/runtime/ProtocolInput.hpp"

#include <algorithm>
#include <stdexcept>

namespace gargantuan {
	namespace {
		bool ContainsAsciiControl(std::string_view Value) {
			return std::ranges::any_of(Value, [](unsigned char Character) {
				return Character < 0x20 || Character == 0x7f;
			});
		}
	}

	void ValidateIdentityProviderName(std::string_view Value) {
		if (Value.empty() || Value.size() > PlayerIdentity::MaximumProviderBytes)
			throw std::invalid_argument("Player identity provider is invalid");
		for (std::size_t Index = 0; Index < Value.size(); ++Index) {
			const auto Character = static_cast<unsigned char>(Value[Index]);
			if (Index == 0) {
				if (Character < 'a' || Character > 'z')
					throw std::invalid_argument("Player identity provider is invalid");
				continue;
			}
			if ((Character >= 'a' && Character <= 'z') || (Character >= '0' && Character <= '9') || Character == '-' ||
				Character == '_' || Character == '.')
				continue;
			throw std::invalid_argument("Player identity provider is invalid");
		}
	}

	void ValidatePlayerIdentity(const PlayerIdentity &Identity) {
		ValidateIdentityProviderName(Identity.Provider);
		if (Identity.Subject.empty()) throw std::invalid_argument("Player identity subject is required");
		ValidateProtocolString(Identity.Subject, PlayerIdentity::MaximumSubjectBytes, "Player identity subject");
		if (ContainsAsciiControl(Identity.Subject))
			throw std::invalid_argument("Player identity subject contains a control character");
	}
}
