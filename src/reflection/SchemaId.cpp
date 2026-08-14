// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/reflection/SchemaId.hpp"

#include <array>

namespace gargantuan {
	namespace {
		constexpr char HexDigits[] = "0123456789abcdef";

		std::optional<std::uint64_t> ParseHalf(std::string_view value) {
			std::uint64_t result = 0;
			for (const char character : value) {
				result <<= 4;
				if (character >= '0' && character <= '9') result |= static_cast<std::uint64_t>(character - '0');
				else if (character >= 'a' && character <= 'f') result |= static_cast<std::uint64_t>(character - 'a' + 10);
				else if (character >= 'A' && character <= 'F') result |= static_cast<std::uint64_t>(character - 'A' + 10);
				else return std::nullopt;
			}
			return result;
		}
	}

	std::string SchemaId::ToString() const {
		std::array<char, 32> encoded{};
		for (std::size_t index = 0; index < 16; ++index) {
			const auto shift = static_cast<unsigned>((15 - index) * 4);
			encoded[index] = HexDigits[(High >> shift) & 0xf];
			encoded[index + 16] = HexDigits[(Low >> shift) & 0xf];
		}
		return {encoded.data(), encoded.size()};
	}

	std::optional<SchemaId> SchemaId::Parse(std::string_view value) {
		if (value.size() != 32) return std::nullopt;
		auto high = ParseHalf(value.substr(0, 16));
		auto low = ParseHalf(value.substr(16, 16));
		if (!high || !low) return std::nullopt;
		SchemaId result{*high, *low};
		return result.IsValid() ? std::optional(result) : std::nullopt;
	}
}
