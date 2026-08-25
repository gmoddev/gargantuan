#include "gargantuan/filesystem/ProjectIdentity.hpp"

#include <atomic>
#include <chrono>
#include <random>

namespace gargantuan {
	namespace {
		constexpr char Hex[] = "0123456789abcdef";

		std::optional<std::uint8_t> DecodeHex(char Value) {
			if (Value >= '0' && Value <= '9') return static_cast<std::uint8_t>(Value - '0');
			if (Value >= 'a' && Value <= 'f') return static_cast<std::uint8_t>(10 + Value - 'a');
			return std::nullopt;
		}
	}

	bool ProjectId::IsValid() const {
		for (const auto Byte : Bytes)
			if (Byte != 0) return true;
		return false;
	}

	std::string ProjectId::ToString() const {
		std::string Result(32, '0');
		for (std::size_t Index = 0; Index < Bytes.size(); ++Index) {
			Result[Index * 2] = Hex[Bytes[Index] >> 4];
			Result[Index * 2 + 1] = Hex[Bytes[Index] & 0xf];
		}
		return Result;
	}

	std::optional<ProjectId> ProjectId::Parse(std::string_view Value) {
		if (Value.size() != 32) return std::nullopt;
		ProjectId Result;
		for (std::size_t Index = 0; Index < Result.Bytes.size(); ++Index) {
			auto High = DecodeHex(Value[Index * 2]);
			auto Low = DecodeHex(Value[Index * 2 + 1]);
			if (!High || !Low) return std::nullopt;
			Result.Bytes[Index] = static_cast<std::uint8_t>((*High << 4) | *Low);
		}
		return Result.IsValid() ? std::optional(Result) : std::nullopt;
	}

	ProjectId ProjectId::New() {
		static std::atomic_uint64_t Counter = 1;
		std::random_device Random;
		ProjectId Result;
		for (std::size_t Index = 0; Index < Result.Bytes.size(); Index += 4) {
			const auto Value = Random();
			for (std::size_t Byte = 0; Byte < 4; ++Byte)
				Result.Bytes[Index + Byte] = static_cast<std::uint8_t>(Value >> (Byte * 8));
		}
		const auto Sequence = Counter.fetch_add(1, std::memory_order_relaxed) ^
							  static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
		for (std::size_t Byte = 0; Byte < 8; ++Byte)
			Result.Bytes[8 + Byte] ^= static_cast<std::uint8_t>(Sequence >> (Byte * 8));
		if (!Result.IsValid()) Result.Bytes.back() = 1;
		return Result;
	}
}
