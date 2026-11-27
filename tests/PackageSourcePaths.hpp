// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <istream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gargantuan::test {
	inline constexpr std::size_t PackageScanChunkBytes = 64 * 1024;

	inline void NormalizePathBytes(std::span<char> Bytes) {
#ifdef _WIN32
		for (auto &Byte : Bytes) {
			if (Byte == '\\') Byte = '/';
			else if (Byte >= 'A' && Byte <= 'Z') Byte += 'a' - 'A';
		}
#else
		(void)Bytes;
#endif
	}

	inline std::string PackagePathRoot(const std::filesystem::path &Path) {
		if (!Path.is_absolute()) throw std::invalid_argument("Package path root must be absolute");
		auto Normal = Path.lexically_normal();
		if (Normal.has_relative_path() && !Normal.has_filename()) Normal = Normal.parent_path();
		auto Root = Normal.generic_string();
		NormalizePathBytes(Root);
		return Root;
	}

	struct PackagePathMatch {
		std::uintmax_t Offset;
		std::size_t RootIndex;
	};

	inline bool PathDelimiter(unsigned char Byte) {
		return Byte <= ' ' || Byte == 127 || std::string_view("\"'=:,;()[]{}<>|").find(static_cast<char>(Byte)) != std::string_view::npos;
	}

	inline std::optional<PackagePathMatch> FindPackagePath(std::istream &Input, std::span<const std::string> Roots) {
		std::size_t Longest = 0;
		for (const auto &Root : Roots) {
			if (Root.empty()) throw std::invalid_argument("Empty package path root");
			Longest = std::max(Longest, Root.size());
		}
		// Retain the previous character, the longest candidate and its next delimiter.
		std::string Buffer(PackageScanChunkBytes + Longest + 1, '\0');
		std::size_t Retained = 0;
		std::uintmax_t Base = 0;
		for (;;) {
			Input.read(Buffer.data() + Retained, PackageScanChunkBytes);
			if (Input.bad() || (Input.fail() && !Input.eof())) throw std::runtime_error("Package scan read failed");
			const auto Count = static_cast<std::size_t>(Input.gcount());
			NormalizePathBytes(std::span(Buffer.data() + Retained, Count));
			const std::string_view Window(Buffer.data(), Retained + Count);
			for (std::size_t I = 0; I < Roots.size(); ++I) {
				const auto &Root = Roots[I];
				for (auto At = Window.find(Root); At != Window.npos; At = Window.find(Root, At + 1)) {
					// A drive-qualified Windows path supplies its own start marker. A POSIX
					// root such as /src must begin a path, including in binary string tables.
					const bool StartsPath = Root.front() != '/' || (At ? PathDelimiter(Window[At-1]) : Base == 0);
					const auto End = At + Root.size();
					const bool EndsRoot = Root.back() == '/' || (End == Window.size() ? Input.eof() :
						Window[End] == '/' || PathDelimiter(Window[End]));
					if (StartsPath && EndsRoot) return PackagePathMatch{Base + At, I};
				}
			}
			if (Input.eof()) return std::nullopt;
			Retained = std::min(Window.size(), Longest + 1);
			const auto Consumed = Window.size() - Retained;
			std::memmove(Buffer.data(), Buffer.data() + Consumed, Retained);
			Base += Consumed;
		}
	}
}
