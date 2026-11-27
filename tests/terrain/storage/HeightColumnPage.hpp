// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/terrain/Field.hpp"
#include "terrain/HeightColumns.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <limits>
#include <memory>
#include <ranges>
#include <type_traits>
#include <utility>
#include <variant>

namespace gargantuan::terrain::experiment {
	inline constexpr int BrickAxis = ChunkSide / BrickSide;
	inline constexpr std::size_t BrickCount = BrickAxis * BrickAxis * BrickAxis;
	inline constexpr std::size_t BrickSamples = BrickSide * BrickSide * BrickSide;
	inline constexpr std::size_t PageSamples = ChunkSide * ChunkSide * ChunkSide;
	using BrickValues = std::array<Sample, BrickSamples>;

	constexpr std::size_t At(int Side, int X, int Y, int Z) noexcept {
		return static_cast<std::size_t>((Z * Side + Y) * Side + X);
	}

	constexpr std::size_t BrickAt(int X, int Y, int Z) noexcept {
		return At(BrickAxis, X / BrickSide, Y / BrickSide, Z / BrickSide);
	}

	using field_detail::Columns;

	// Experimental fallback uses the same density and palette index widths as
	// the production Field brick. The benchmark also measures the actual Field.
	struct DenseBrick {
		std::array<std::int16_t, BrickSamples> Density;
		std::array<std::uint16_t, BrickSamples> Indices;
		std::vector<MaterialId> Palette;

		explicit DenseBrick(const BrickValues &Values) {
			std::array<MaterialId, BrickSamples> Sorted;
			std::ranges::transform(Values, Sorted.begin(), &Sample::Material);
			std::ranges::sort(Sorted);
			Palette.assign(Sorted.begin(), std::unique(Sorted.begin(), Sorted.end()));
			for (std::size_t Index = 0; Index < Values.size(); ++Index) {
				Density[Index] = Values[Index].Density;
				const auto At = std::ranges::lower_bound(Palette, Values[Index].Material);
				Indices[Index] = static_cast<std::uint16_t>(At - Palette.begin());
			}
		}
	};

	struct Brick {
		std::variant<Sample, std::unique_ptr<const DenseBrick>, std::unique_ptr<const Columns<BrickSide>>> Value;

		const DenseBrick *GetDense() const noexcept {
			const auto Found = std::get_if<std::unique_ptr<const DenseBrick>>(&Value);
			return Found ? Found->get() : nullptr;
		}

		const Columns<BrickSide> *GetHeight() const noexcept {
			const auto Found = std::get_if<std::unique_ptr<const Columns<BrickSide>>>(&Value);
			return Found ? Found->get() : nullptr;
		}

		Sample Read(int X, int Y, int Z) const noexcept {
			if (const auto *Dense = GetDense()) {
				const auto Index = At(BrickSide, X, Y, Z);
				return {Dense->Density[Index], Dense->Palette[Dense->Indices[Index]]};
			}
			if (const auto *Height = GetHeight()) return Height->Read(X, Y, Z);
			return std::get<Sample>(Value);
		}

		std::size_t Bytes() const noexcept {
			if (const auto *Dense = GetDense())
				return sizeof(*this) + sizeof(DenseBrick) + Dense->Palette.capacity() * sizeof(MaterialId);
			if (const auto *Height = GetHeight()) return sizeof(*this) + Height->Bytes();
			return sizeof(*this);
		}
	};

	struct BrickPage {
		std::array<std::shared_ptr<const Brick>, BrickCount> Bricks;

		Sample Read(int X, int Y, int Z) const noexcept {
			const auto &Value = Bricks[BrickAt(X, Y, Z)];
			return Value ? Value->Read(X % BrickSide, Y % BrickSide, Z % BrickSide) : AirSample;
		}

		std::size_t Bytes() const noexcept {
			std::size_t Count = sizeof(*this);
			for (const auto &Value : Bricks) if (Value) Count += Value->Bytes();
			return Count;
		}
	};

	enum class Strategy { WholeTile, SelectiveBrick };

	struct Page {
		std::shared_ptr<const BrickPage> Bricks;
		std::shared_ptr<const Columns<ChunkSide>> Height;

		Sample Read(int X, int Y, int Z) const noexcept {
			return Height ? Height->Read(X, Y, Z) : Bricks->Read(X, Y, Z);
		}

		std::size_t Bytes() const noexcept {
			return sizeof(*this) + (Height ? Height->Bytes() : Bricks->Bytes());
		}
	};

	inline std::shared_ptr<const Brick> PackBrick(const Snapshot &Source, ChunkKey Key, std::size_t Index, bool Selective) {
		BrickValues Values;
		const auto Uniform = Source.ReadBrick(Key, Index, Values).value();
		if (Uniform) return *Uniform == AirSample ? nullptr : std::make_shared<Brick>(Brick{*Uniform});
		auto Height = Selective ? Columns<BrickSide>::Pack(Values) : nullptr;
		if (Height && Height->Bytes() < sizeof(DenseBrick) + sizeof(MaterialId))
			return std::make_shared<Brick>(Brick{std::move(Height)});
		auto Dense = std::make_unique<DenseBrick>(Values);
		if (Height && Height->Bytes() < sizeof(DenseBrick) + Dense->Palette.capacity() * sizeof(MaterialId))
			return std::make_shared<Brick>(Brick{std::move(Height)});
		return std::make_shared<Brick>(Brick{std::move(Dense)});
	}

	inline Page PackPage(const Snapshot &Source, ChunkKey Key, Strategy Mode) {
		auto Bricks = std::make_shared<BrickPage>();
		for (std::size_t Index = 0; Index < Bricks->Bricks.size(); ++Index)
			Bricks->Bricks[Index] = PackBrick(Source, Key, Index, Mode == Strategy::SelectiveBrick);
		if (Mode == Strategy::WholeTile && Bricks->Bytes() > sizeof(Columns<ChunkSide>)) {
			std::vector<Sample> Values(PageSamples);
			for (int Z = 0; Z < ChunkSide; ++Z)
				for (int Y = 0; Y < ChunkSide; ++Y)
					for (int X = 0; X < ChunkSide; ++X)
						Values[At(ChunkSide, X, Y, Z)] = Bricks->Read(X, Y, Z);
			auto Height = Columns<ChunkSide>::Pack(std::span<const Sample, PageSamples>(Values));
			if (Height && Height->Bytes() < Bricks->Bytes()) return {.Height = std::move(Height)};
		}
		return {.Bricks = std::move(Bricks)};
	}

	// Copy the pointer table and encode exactly the touched bricks from a fully
	// captured new revision. The caller supplies owned dirty brick indices.
	inline Page RepackBricks(const Snapshot &Source, ChunkKey Key, const Page &Previous,
		std::span<const std::size_t> Changed) {
		auto Result = std::make_shared<BrickPage>(*Previous.Bricks);
		for (const auto Index : Changed) Result->Bricks.at(Index) = PackBrick(Source, Key, Index, true);
		return {.Bricks = std::move(Result)};
	}
}
