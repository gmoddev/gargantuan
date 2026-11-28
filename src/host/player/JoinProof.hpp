#pragma once

#include "gargantuan/network/GameAdmission.hpp"
#include "gargantuan/network/Transport.hpp"

#include <array>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace gargantuan::host::detail {
	using PlayerJoinProof = std::array<std::byte, network::GameAdmissionProofBytes>;

	[[nodiscard]] inline std::optional<PlayerJoinProof> DecodePlayerJoinProof(std::string_view Value) {
		PlayerJoinProof Proof{};
		if (Value.size() != Proof.size() * 2) return {};
		bool Nonzero = false;
		for (std::size_t Index = 0; Index < Value.size(); ++Index) {
			const char Character = Value[Index];
			const int Digit = Character >= '0' && Character <= '9' ? Character - '0'
				: Character >= 'a' && Character <= 'f' ? Character - 'a' + 10 : -1;
			if (Digit < 0) return {};
			if ((Index & 1) == 0) Proof[Index / 2] = static_cast<std::byte>(Digit << 4);
			else Proof[Index / 2] |= static_cast<std::byte>(Digit);
			Nonzero |= Digit != 0;
		}
		return Nonzero ? std::optional(Proof) : std::nullopt;
	}

	// This is only an opaque short-lived player ticket. Node workload credentials
	// and provider identities never enter the public Player host.
	[[nodiscard]] inline std::optional<PlayerJoinProof> ReadPlayerJoinProof(std::string_view EnvironmentName) {
		if (EnvironmentName.empty() || EnvironmentName.size() > 64) return std::nullopt;
		for (std::size_t Index = 0; Index < EnvironmentName.size(); ++Index) {
			const auto Character = EnvironmentName[Index];
			const bool Letter = (Character >= 'A' && Character <= 'Z') || (Character >= 'a' && Character <= 'z');
			if (!Letter && Character != '_' && !(Index != 0 && Character >= '0' && Character <= '9'))
				return std::nullopt;
		}
		const std::string Name(EnvironmentName);
		const auto *Value = std::getenv(Name.c_str());
		if (!Value) return std::nullopt;
		std::size_t Length = 0;
		while (Length <= network::GameAdmissionProofBytes * 2 && Value[Length] != '\0') ++Length;
		return DecodePlayerJoinProof(std::string_view(Value, Length));
	}

	// The same bounded hex decoder reads a public GNS leaf key. It is never
	// interpreted as a workload credential or an authority token.
	[[nodiscard]] inline std::optional<network::TransportServerPublicKey> ReadPlayerServerPublicKey(
		std::string_view EnvironmentName) {
		return ReadPlayerJoinProof(EnvironmentName);
	}
}
