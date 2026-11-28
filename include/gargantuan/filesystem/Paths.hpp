#pragma once

#include <filesystem>

namespace gargantuan::Paths {
	std::filesystem::path GetExecutableDirectory();

	inline std::string ToUtf8(const std::filesystem::path &path) {
		auto utf8 = path.u8string();
		return std::string(reinterpret_cast<const char *>(utf8.data()), utf8.size());
	}
	inline std::string ToGenericUtf8(const std::filesystem::path &Path) {
		const auto Text = Path.generic_u8string();
		return std::string(reinterpret_cast<const char *>(Text.data()), Text.size());
	}
} // namespace gargantuan::Paths
