#include "gargantuan/platform/HostEvent.hpp"

#include <algorithm>

namespace gargantuan {
	namespace {
		bool IsValidUtf8(std::string_view Text) {
			std::size_t Index = 0;
			while (Index < Text.size()) {
				const auto Lead = static_cast<unsigned char>(Text[Index]);
				std::size_t Continuations = 0;
				std::uint32_t CodePoint = 0;
				if (Lead <= 0x7f) CodePoint = Lead;
				else if ((Lead & 0xe0) == 0xc0) { Continuations = 1; CodePoint = Lead & 0x1f; }
				else if ((Lead & 0xf0) == 0xe0) { Continuations = 2; CodePoint = Lead & 0x0f; }
				else if ((Lead & 0xf8) == 0xf0) { Continuations = 3; CodePoint = Lead & 0x07; }
				else return false;
				if (Index + Continuations >= Text.size()) return false;
				for (std::size_t Offset = 1; Offset <= Continuations; ++Offset) {
					const auto Byte = static_cast<unsigned char>(Text[Index + Offset]);
					if ((Byte & 0xc0) != 0x80) return false;
					CodePoint = (CodePoint << 6) | (Byte & 0x3f);
				}
				if ((Continuations == 1 && CodePoint < 0x80) || (Continuations == 2 && CodePoint < 0x800) ||
					(Continuations == 3 && CodePoint < 0x10000) || CodePoint > 0x10ffff ||
					(CodePoint >= 0xd800 && CodePoint <= 0xdfff)) return false;
				Index += Continuations + 1;
			}
			return true;
		}
	}

	std::optional<BoundedUtf8> BoundedUtf8::From(std::string_view Text) {
		if (Text.size() > MAX_TEXT_INPUT_BYTES || !IsValidUtf8(Text)) return std::nullopt;
		BoundedUtf8 Result;
		std::copy(Text.begin(), Text.end(), Result.Bytes.begin());
		Result.Size = static_cast<std::uint8_t>(Text.size());
		return Result;
	}

	std::optional<BoundedCompositionUtf8> BoundedCompositionUtf8::From(std::string_view Text) {
		if (Text.size() > MAX_COMPOSITION_INPUT_BYTES || !IsValidUtf8(Text)) return std::nullopt;
		BoundedCompositionUtf8 Result;
		std::copy(Text.begin(), Text.end(), Result.Bytes.begin());
		Result.Size = static_cast<std::uint16_t>(Text.size());
		return Result;
	}
}
