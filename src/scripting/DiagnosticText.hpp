#pragma once

#include "gargantuan/runtime/ProtocolInput.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace gargantuan {
	inline std::string BoundedDiagnosticText(std::string_view Value, std::size_t MaximumBytes) {
		constexpr std::string_view Marker = "...<truncated>";
		constexpr std::string_view Invalid = "<invalid utf-8>";
		const bool Truncated = Value.size() > MaximumBytes;
		if (Truncated) {
			const auto MarkerSize = std::min(MaximumBytes, Marker.size());
			auto End = MaximumBytes - MarkerSize;
			while (End > 0 && (static_cast<unsigned char>(Value[End]) & 0xc0) == 0x80) --End;
			Value = Value.substr(0, End);
		}
		if (!IsValidProtocolUtf8(Value)) return std::string(Invalid.substr(0, MaximumBytes));
		std::string Result(Value);
		if (Truncated) Result.append(Marker.substr(0, MaximumBytes - Result.size()));
		return Result;
	}
}
