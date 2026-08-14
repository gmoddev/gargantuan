// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace gargantuan {
	struct SchemaId {
		std::uint64_t High = 0;
		std::uint64_t Low = 0;

		[[nodiscard]] constexpr bool IsValid() const { return High != 0 || Low != 0; }
		[[nodiscard]] std::string ToString() const;

		[[nodiscard]] static std::optional<SchemaId> Parse(std::string_view value);
		[[nodiscard]] static constexpr SchemaId FromParts(std::uint64_t high, std::uint64_t low) {
			return {high, low};
		}

		// Native IDs are deterministic, domain-separated hashes of a qualified
		// engine-owned name. They are emitted into generated definitions and are
		// never derived from a process address or registration order.
		[[nodiscard]] static constexpr SchemaId FromNativeName(
			std::string_view schemaNamespace,
			std::string_view name
		) {
			std::uint64_t high = Hash("Gargantuan.SchemaId.Native.High.v1", 14695981039346656037ull);
			high = Hash(schemaNamespace, high);
			high = Hash(std::string_view("\0", 1), high);
			high = Hash(name, high);

			std::uint64_t low = Hash("Gargantuan.SchemaId.Native.Low.v1", 1099511628211ull);
			low = Hash(schemaNamespace, low);
			low = Hash(std::string_view("\0", 1), low);
			low = Hash(name, low);
			return {high, low};
		}

		auto operator<=>(const SchemaId &) const = default;

	  private:
		[[nodiscard]] static constexpr std::uint64_t Hash(std::string_view value, std::uint64_t seed) {
			std::uint64_t hash = seed;
			for (const unsigned char character : value) {
				hash ^= character;
				hash *= 1099511628211ull;
			}
			return hash;
		}
	};

	struct SchemaIdHash {
		[[nodiscard]] std::size_t operator()(SchemaId id) const noexcept {
			return std::hash<std::uint64_t>{}(id.High) ^
				(std::hash<std::uint64_t>{}(id.Low) + 0x9e3779b97f4a7c15ull + (id.High << 6) + (id.High >> 2));
		}
	};
}
