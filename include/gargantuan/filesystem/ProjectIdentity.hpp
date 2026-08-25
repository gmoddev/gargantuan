#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace gargantuan {
	struct ProjectId {
		std::array<std::uint8_t, 16> Bytes{};

		[[nodiscard]] bool IsValid() const;
		[[nodiscard]] std::string ToString() const;
		[[nodiscard]] static std::optional<ProjectId> Parse(std::string_view Value);
		[[nodiscard]] static ProjectId New();
		auto operator<=>(const ProjectId &) const = default;
	};
}
