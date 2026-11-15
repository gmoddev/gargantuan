#pragma once

#include "gargantuan/network/GameAdmission.hpp"

#include <array>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace gargantuan::host::detail {
	using PlayerJoinProof = std::array<std::byte, network::GameAdmissionProofBytes>;

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
		if (Length != network::GameAdmissionProofBytes * 2) return std::nullopt;
		PlayerJoinProof Proof{};
		bool Nonzero = false;
		for (std::size_t Index = 0; Index < Proof.size() * 2; ++Index) {
			const auto Character = Value[Index];
			int Digit = -1;
			if (Character >= '0' && Character <= '9') Digit = Character - '0';
			if (Character >= 'a' && Character <= 'f') Digit = Character - 'a' + 10;
			if (Digit < 0) return std::nullopt;
			if ((Index & 1) == 0)
				Proof[Index / 2] = static_cast<std::byte>(Digit << 4);
			else
				Proof[Index / 2] |= static_cast<std::byte>(Digit);
			Nonzero = Nonzero || Digit != 0;
		}
		if (Value[Proof.size() * 2] != '\0' || !Nonzero) return std::nullopt;
		return Proof;
	}
}
