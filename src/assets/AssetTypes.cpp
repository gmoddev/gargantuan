// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/assets/AssetTypes.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <format>
#include <random>

namespace gargantuan {
	namespace {
		constexpr char Hex[] = "0123456789abcdef";

		std::optional<std::uint8_t> DecodeHex(char Value) {
			if (Value >= '0' && Value <= '9') return static_cast<std::uint8_t>(Value - '0');
			if (Value >= 'a' && Value <= 'f') return static_cast<std::uint8_t>(10 + Value - 'a');
			return std::nullopt;
		}

		std::uint64_t Fnv(std::string_view Value, std::uint64_t Seed) {
			for (const auto Character : Value) {
				Seed ^= static_cast<unsigned char>(Character);
				Seed *= 1099511628211ull;
			}
			return Seed;
		}

		constexpr std::array<std::uint32_t, 64> ShaConstants{
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
		};
	}

	std::string_view GetAssetKindName(AssetKind Kind) {
		switch (Kind) {
			case AssetKind::Image: return "Image";
			case AssetKind::Mesh: return "Mesh";
			case AssetKind::Font: return "Font";
			case AssetKind::Material: return "Material";
		}
		return "Unknown";
	}

	std::optional<AssetKind> ParseAssetKind(std::string_view Value) {
		if (Value == "Image") return AssetKind::Image;
		if (Value == "Mesh") return AssetKind::Mesh;
		if (Value == "Font") return AssetKind::Font;
		if (Value == "Material") return AssetKind::Material;
		return std::nullopt;
	}

	std::string_view GetAssetStateName(AssetState State) {
		switch (State) {
			case AssetState::Missing: return "Missing";
			case AssetState::Importing: return "Importing";
			case AssetState::Ready: return "Ready";
			case AssetState::Failed: return "Failed";
			case AssetState::Stale: return "Stale";
		}
		return "Unknown";
	}

	std::optional<AssetState> ParseAssetState(std::string_view Value) {
		if (Value == "Missing") return AssetState::Missing;
		if (Value == "Importing") return AssetState::Importing;
		if (Value == "Ready") return AssetState::Ready;
		if (Value == "Failed") return AssetState::Failed;
		if (Value == "Stale") return AssetState::Stale;
		return std::nullopt;
	}

	std::string AssetId::ToString() const { return std::format("{:016x}{:016x}", High, Low); }

	std::optional<AssetId> AssetId::Parse(std::string_view Value) {
		if (Value.size() != 32) return std::nullopt;
		AssetId Result;
		const auto [HighEnd, HighError] = std::from_chars(Value.data(), Value.data() + 16, Result.High, 16);
		const auto [LowEnd, LowError] = std::from_chars(Value.data() + 16, Value.data() + 32, Result.Low, 16);
		if (HighError != std::errc{} || LowError != std::errc{} || HighEnd != Value.data() + 16 ||
			LowEnd != Value.data() + 32 || !Result.IsValid()) return std::nullopt;
		for (const auto Character : Value)
			if (!(Character >= '0' && Character <= '9') && !(Character >= 'a' && Character <= 'f')) return std::nullopt;
		return Result;
	}

	AssetId AssetId::New() {
		static std::atomic_uint64_t Counter = 1;
		std::random_device Random;
		const auto Sequence = Counter.fetch_add(1, std::memory_order_relaxed);
		AssetId Result{
			(static_cast<std::uint64_t>(Random()) << 32) | Random(),
			(static_cast<std::uint64_t>(Random()) << 32) | Random(),
		};
		Result.High ^= static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
		Result.Low ^= Sequence;
		if (!Result.IsValid()) Result.Low = Sequence ? Sequence : 1;
		return Result;
	}

	AssetId AssetId::FromBuiltInName(std::string_view Name) {
		return {
			Fnv(Name, Fnv("Gargantuan.AssetId.BuiltIn.High.v1", 14695981039346656037ull)),
			Fnv(Name, Fnv("Gargantuan.AssetId.BuiltIn.Low.v1", 1099511628211ull)),
		};
	}

	bool AssetContentId::IsValid() const {
		for (const auto Byte : Bytes) if (Byte != 0) return true;
		return false;
	}

	std::string AssetContentId::ToString() const {
		std::string Result(64, '0');
		for (std::size_t Index = 0; Index < Bytes.size(); ++Index) {
			Result[Index * 2] = Hex[Bytes[Index] >> 4];
			Result[Index * 2 + 1] = Hex[Bytes[Index] & 0xf];
		}
		return Result;
	}

	std::optional<AssetContentId> AssetContentId::Parse(std::string_view Value) {
		if (Value.size() != 64) return std::nullopt;
		AssetContentId Result;
		for (std::size_t Index = 0; Index < Result.Bytes.size(); ++Index) {
			auto High = DecodeHex(Value[Index * 2]);
			auto Low = DecodeHex(Value[Index * 2 + 1]);
			if (!High || !Low) return std::nullopt;
			Result.Bytes[Index] = static_cast<std::uint8_t>((*High << 4) | *Low);
		}
		return Result.IsValid() ? std::optional(Result) : std::nullopt;
	}

	AssetContentId AssetContentId::Hash(std::span<const std::uint8_t> Input) {
		std::vector<std::uint8_t> Padded(Input.begin(), Input.end());
		const auto BitLength = static_cast<std::uint64_t>(Input.size()) * 8;
		Padded.push_back(0x80);
		while ((Padded.size() + 8) % 64 != 0) Padded.push_back(0);
		for (int Shift = 56; Shift >= 0; Shift -= 8) Padded.push_back(static_cast<std::uint8_t>(BitLength >> Shift));

		std::array<std::uint32_t, 8> State{
			0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
			0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
		};
		for (std::size_t Offset = 0; Offset < Padded.size(); Offset += 64) {
			std::array<std::uint32_t, 64> Words{};
			for (std::size_t Index = 0; Index < 16; ++Index) {
				const auto Position = Offset + Index * 4;
				Words[Index] = (static_cast<std::uint32_t>(Padded[Position]) << 24) |
					(static_cast<std::uint32_t>(Padded[Position + 1]) << 16) |
					(static_cast<std::uint32_t>(Padded[Position + 2]) << 8) | Padded[Position + 3];
			}
			for (std::size_t Index = 16; Index < 64; ++Index) {
				const auto S0 = std::rotr(Words[Index - 15], 7) ^ std::rotr(Words[Index - 15], 18) ^ (Words[Index - 15] >> 3);
				const auto S1 = std::rotr(Words[Index - 2], 17) ^ std::rotr(Words[Index - 2], 19) ^ (Words[Index - 2] >> 10);
				Words[Index] = Words[Index - 16] + S0 + Words[Index - 7] + S1;
			}
			auto [A, B, C, D, E, F, G, H] = State;
			for (std::size_t Index = 0; Index < 64; ++Index) {
				const auto S1 = std::rotr(E, 6) ^ std::rotr(E, 11) ^ std::rotr(E, 25);
				const auto Choice = (E & F) ^ (~E & G);
				const auto Temporary1 = H + S1 + Choice + ShaConstants[Index] + Words[Index];
				const auto S0 = std::rotr(A, 2) ^ std::rotr(A, 13) ^ std::rotr(A, 22);
				const auto Majority = (A & B) ^ (A & C) ^ (B & C);
				const auto Temporary2 = S0 + Majority;
				H = G; G = F; F = E; E = D + Temporary1; D = C; C = B; B = A; A = Temporary1 + Temporary2;
			}
			State[0] += A; State[1] += B; State[2] += C; State[3] += D;
			State[4] += E; State[5] += F; State[6] += G; State[7] += H;
		}
		AssetContentId Result;
		for (std::size_t Index = 0; Index < State.size(); ++Index)
			for (std::size_t Byte = 0; Byte < 4; ++Byte)
				Result.Bytes[Index * 4 + Byte] = static_cast<std::uint8_t>(State[Index] >> (24 - Byte * 8));
		return Result;
	}

	std::optional<AssetReference> AssetReference::Parse(std::string_view Value) {
		if (Value.starts_with("asset://")) {
			auto Id = AssetId::Parse(Value.substr(8));
			if (!Id) return std::nullopt;
			return AssetReference{std::string(Value), *Id, false};
		}
		if (!Value.starts_with("builtin://") || Value.size() <= 10 || Value.size() > 256) return std::nullopt;
		for (const auto Character : Value.substr(10)) {
			const bool Valid = (Character >= 'a' && Character <= 'z') || (Character >= '0' && Character <= '9') ||
				Character == '/' || Character == '-' || Character == '_';
			if (!Valid) return std::nullopt;
		}
		return AssetReference{std::string(Value), std::nullopt, true};
	}

	AssetReference AssetReference::FromAssetId(AssetId Id) {
		return {"asset://" + Id.ToString(), Id, false};
	}
}
