// Copyright (c) 2026 Team Fireworks. SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gargantuan {
	inline constexpr std::string_view PreRunCollectionPrefix = "!Gantria.PreRun/";
	inline constexpr std::string_view PreRunCollectionHeader = "!Gantria.PreRun/1\n";

	struct PreRunChunk {
		std::string_view Id, Name, Source;
	};

	// Views borrow the complete source snapshot. Length framing preserves arbitrary Luau text.
	inline std::vector<PreRunChunk> ReadPreRunChunks(std::string_view Bytes) {
		if (!Bytes.starts_with(PreRunCollectionPrefix)) return {{"default", "Default", Bytes}};
		if (!Bytes.starts_with(PreRunCollectionHeader)) throw std::invalid_argument("Unsupported PreRun collection version");
		Bytes.remove_prefix(PreRunCollectionHeader.size());
		std::vector<PreRunChunk> Chunks;
		auto Lower = [](char C) { return C >= 'A' && C <= 'Z' ? C + ('a' - 'A') : int(C); };
		while (!Bytes.empty()) {
			const auto End = Bytes.find('\n');
			if (End == Bytes.npos || End > 110 || Chunks.size() >= 64) throw std::invalid_argument("Invalid PreRun entry header or count");
			const auto Line = Bytes.substr(0, End);
			const auto A = Line.find('\t');
			const auto B = A == Line.npos ? Line.npos : Line.find('\t', A + 1);
			if (B == Line.npos) throw std::invalid_argument("Missing PreRun entry fields");
			const auto Id = Line.substr(0, A), Name = Line.substr(A + 1, B - A - 1), Length = Line.substr(B + 1);
			auto Alnum = [](char C) { return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9'); };
			if (Id.empty() || Id.size() > 32 || !std::all_of(Id.begin(), Id.end(), Alnum) ||
				Name.empty() || Name.size() > 64 || Name.front() == ' ' || Name.back() == ' ' ||
				!std::all_of(Name.begin(), Name.end(), [&](char C) { return Alnum(C) || C == ' ' || C == '-' || C == '_'; }))
				throw std::invalid_argument("Invalid PreRun script ID or display name");
			if (Chunks.empty() && (Id != "default" || Name != "Default")) throw std::invalid_argument("Default must be first");
			for (const auto &Other : Chunks)
				if (Id == Other.Id || std::equal(Name.begin(), Name.end(), Other.Name.begin(), Other.Name.end(),
					[&](char X, char Y) { return Lower(X) == Lower(Y); })) throw std::invalid_argument("Duplicate PreRun script ID or name");
			std::size_t Count = 0;
			const auto [Next, Error] = std::from_chars(Length.data(), Length.data() + Length.size(), Count);
			Bytes.remove_prefix(End + 1);
			if (Error != std::errc{} || Next != Length.data() + Length.size() || (Length.size() > 1 && Length.front() == '0') || Count > Bytes.size())
				throw std::invalid_argument("Invalid PreRun source length");
			Chunks.push_back({Id, Name, Bytes.substr(0, Count)});
			Bytes.remove_prefix(Count);
		}
		if (Chunks.empty()) throw std::invalid_argument("Missing Default PreRun script");
		return Chunks;
	}
}
