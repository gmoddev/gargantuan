#pragma once

#include "gargantuan/network/ReplicationProtocol.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gargantuan::test {

// Preserved scalar contract, independent of the optimized word-at-a-time path.
inline bool ScalarProtocolUtf8(std::string_view Value) {
	for (std::size_t Index = 0; Index < Value.size();) {
		const auto First = static_cast<unsigned char>(Value[Index]);
		std::size_t Count = 0;
		std::uint32_t CodePoint = 0;
		if (First <= 0x7f) { Count = 1; CodePoint = First; }
		else if ((First & 0xe0) == 0xc0) { Count = 2; CodePoint = First & 0x1f; }
		else if ((First & 0xf0) == 0xe0) { Count = 3; CodePoint = First & 0x0f; }
		else if ((First & 0xf8) == 0xf0) { Count = 4; CodePoint = First & 0x07; }
		else return false;
		if (Index + Count > Value.size()) return false;
		for (std::size_t Offset = 1; Offset < Count; ++Offset) {
			const auto Continuation = static_cast<unsigned char>(Value[Index + Offset]);
			if ((Continuation & 0xc0) != 0x80) return false;
			CodePoint = (CodePoint << 6) | (Continuation & 0x3f);
		}
		if ((Count == 2 && CodePoint < 0x80) || (Count == 3 && CodePoint < 0x800) ||
			(Count == 4 && CodePoint < 0x10000) || CodePoint > 0x10ffff ||
			(CodePoint >= 0xd800 && CodePoint <= 0xdfff)) return false;
		Index += Count;
	}
	return true;
}

inline bool RunProtocolUtf8ParityTests() {
	std::size_t Cases = 0;
	auto Compare = [&](std::string_view Value) {
		++Cases;
		if (IsValidProtocolUtf8(Value) != ScalarProtocolUtf8(Value))
			throw std::runtime_error("UTF-8 scalar/word parity failed in case " + std::to_string(Cases));
	};
	auto Require = [](bool Valid, const char *Message) { if (!Valid) throw std::runtime_error(Message); };
	try {
		Compare({});
		// Exhaust every one- and two-byte input, not just selected valid strings.
		std::array<char, 2> Pair{};
		for (unsigned First = 0; First < 256; ++First) {
			Pair[0] = static_cast<char>(First);
			Compare(std::string_view(Pair.data(), 1));
			for (unsigned Second = 0; Second < 256; ++Second) {
				Pair[1] = static_cast<char>(Second);
				Compare(std::string_view(Pair.data(), Pair.size()));
			}
		}
		// Every Unicode scalar, including both edges of each encoding width.
		for (std::uint32_t Code = 0; Code <= 0x10ffff; ++Code) {
			if (Code >= 0xd800 && Code <= 0xdfff) continue;
			std::string Value;
			if (Code < 0x80) Value += static_cast<char>(Code);
			else if (Code < 0x800) {
				Value += static_cast<char>(0xc0 | (Code >> 6));
				Value += static_cast<char>(0x80 | (Code & 0x3f));
			} else if (Code < 0x10000) {
				Value += static_cast<char>(0xe0 | (Code >> 12));
				Value += static_cast<char>(0x80 | ((Code >> 6) & 0x3f));
				Value += static_cast<char>(0x80 | (Code & 0x3f));
			} else {
				Value += static_cast<char>(0xf0 | (Code >> 18));
				Value += static_cast<char>(0x80 | ((Code >> 12) & 0x3f));
				Value += static_cast<char>(0x80 | ((Code >> 6) & 0x3f));
				Value += static_cast<char>(0x80 | (Code & 0x3f));
			}
			Compare(Value);
			Require(IsValidProtocolUtf8(Value), "valid Unicode scalar was rejected");
		}
		const std::array<std::string, 18> Boundaries{
			"\x7f", "\xc2\x80", "\xdf\xbf", "\xe0\xa0\x80", "\xed\x9f\xbf", "\xee\x80\x80",
			"\xef\xbf\xbf", "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf",
			"\xc0\x80", "\xc1\xbf", "\xe0\x9f\xbf", "\xf0\x8f\xbf\xbf",
			"\xed\xa0\x80", "\xed\xbf\xbf", "\xf4\x90\x80\x80", "\xf8\x80\x80\x80\x80", "\xff"
		};
		for (std::size_t Prefix = 0; Prefix < 25; ++Prefix) {
			for (const auto &Boundary : Boundaries) {
				for (std::size_t Count = 0; Count <= Boundary.size(); ++Count) {
					Compare(std::string(Prefix, 'a') + Boundary.substr(0, Count));
					Compare(std::string(Prefix, 'a') + Boundary.substr(0, Count) + std::string(17, 'z'));
				}
				for (std::size_t Offset = 1; Offset < Boundary.size(); ++Offset)
					for (unsigned Byte = 0; Byte < 256; ++Byte) {
						auto Damaged = Boundary;
						Damaged[Offset] = static_cast<char>(Byte);
						Compare(std::string(Prefix, 'a') + Damaged + "tail");
					}
			}
			for (std::size_t Length = 0; Length <= 33; ++Length) {
				// Offset string_view deliberately includes unaligned starts and exact
				// partial final words. Sanitizer suites exercise its bounded loads.
				const std::string Storage(Prefix + Length, 'x');
				Compare(std::string_view(Storage).substr(Prefix, Length));
			}
		}
		std::uint64_t Random = UINT64_C(0x243f6a8885a308d3);
		auto Next = [&]() {
			Random ^= Random << 13; Random ^= Random >> 7; Random ^= Random << 17;
			return Random;
		};
		for (std::size_t Sample = 0; Sample < 10000; ++Sample) {
			std::string Value(static_cast<std::size_t>(Next() % 1025), 'a');
			for (auto &Byte : Value) {
				const auto Bits = Next();
				Byte = static_cast<char>((Sample % 2 == 0 || Bits % 32 == 0) ? Bits & 0xff : Bits & 0x7f);
			}
			Compare(Value);
		}
		for (const auto Length : {std::size_t{7}, std::size_t{8}, std::size_t{9},
			std::size_t{24 * 1024}, MaximumProtocolStringBytes, MaximumProtocolStringBytes + 1}) {
			const std::string Ascii(Length, 'x');
			Compare(Ascii);
			std::string WithNull = Ascii; WithNull[Length / 2] = '\0';
			Compare(WithNull);
			Require(IsValidProtocolUtf8(WithNull), "raw UTF-8 predicate must still permit the NUL code point");
			bool Rejected = false;
			try { ValidateProtocolString(WithNull, MaximumProtocolStringBytes, "Parity"); }
			catch (const std::invalid_argument &) { Rejected = true; }
			Require(Rejected, "protocol string still rejects embedded NUL independently of UTF-8 validity");
			Rejected = false;
			try { ValidateProtocolString(Ascii, MaximumProtocolStringBytes, "Parity"); }
			catch (const std::invalid_argument &) { Rejected = true; }
			Require(Rejected == (Length > MaximumProtocolStringBytes), "protocol string byte bound is unchanged");
		}

		using namespace network;
		ReplicationFrame Frame{ReplicationProtocolVersion, ReplicationMessageKind::Incremental,
			ReplicationEpoch(1), ReliableReplicationSequence(2)};
		Frame.Operations.push_back({ReplicationEpoch(1), PropertyReplicationUpdate{{1, 2}, "Name", std::string{"A\xce\xa9"}}});
		const auto Encoded = EncodeReplicationFrame(Frame);
		Require(Encoded.has_value(), "real GRPL encoder accepts mixed ASCII and Unicode");
		std::string Hex;
		constexpr std::string_view Digits = "0123456789abcdef";
		for (const auto Byte : *Encoded) {
			const auto Value = std::to_integer<unsigned>(Byte);
			Hex += Digits[Value >> 4]; Hex += Digits[Value & 15];
		}
		Require(Hex == "4752504c010001000100000000000000020000000000000000000000010000001a000000"
			"010100000002000000040000004e616d65050300000041cea900",
			"GRPL output allocation transfer preserves exact established wire bytes");
		Require(DecodeReplicationFrame(*Encoded).has_value(), "exact GRPL bytes remain decodable");
		std::get<PropertyReplicationUpdate>(Frame.Operations.front().Intent).Value = std::string{"\xc0\xaf", 2};
		const auto Invalid = EncodeReplicationFrame(Frame);
		Require(!Invalid && Invalid.error().Code == SerializationErrorCode::InvalidValue &&
			Invalid.error().Message == "Replication frame is invalid", "real encoder retains malformed UTF-8 failure classification");
		// Informational native measurement, never a timing acceptance threshold.
		// A volatile function pointer prevents either pure predicate being hoisted
		// out of the repeated-work loop by this translation unit's optimizer.
		const std::string LargeAscii(24 * 1024, 'n');
		auto Measure = [&](bool (*Validator)(std::string_view)) {
			bool (*volatile Function)(std::string_view) = Validator;
			const auto Started = std::chrono::steady_clock::now();
			std::size_t Accepted = 0;
			for (std::size_t Sample = 0; Sample < 1024; ++Sample) Accepted += Function(LargeAscii) ? 1 : 0;
			const auto Nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - Started).count();
			Require(Accepted == 1024, "native ASCII measurement preserves every verdict");
			return Nanoseconds;
		};
		const auto ScalarNanoseconds = Measure(ScalarProtocolUtf8);
		const auto OptimizedNanoseconds = Measure(IsValidProtocolUtf8);
		std::cout << "[Network:ProtocolUtf8] ascii_validation_bytes=" << LargeAscii.size() * 1024
			<< " scalar_ns=" << ScalarNanoseconds << " optimized_ns=" << OptimizedNanoseconds << '\n';
		std::cout << "[Network:ProtocolUtf8] parity_cases=" << Cases << " exact_GRPL=pass result=pass\n";
		return true;
	} catch (const std::exception &Error) {
		std::cerr << "[Network:ProtocolUtf8] " << Error.what() << '\n';
		return false;
	}
}

}
