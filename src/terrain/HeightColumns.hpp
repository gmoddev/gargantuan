// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/terrain/Types.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace gargantuan::terrain::field_detail {
	// The predictor provides an exact base only where its integer values match.
	// Density overrides and material runs retain the original lattice tuple.
	template<int Side> class Columns {
		static_assert(Side == BrickSide || Side == ChunkSide);
		using MaskType = std::conditional_t<Side == BrickSide, std::uint8_t, std::uint32_t>;
		struct Column {
			std::int32_t Intercept{};
			std::uint16_t Slope{};
			std::uint16_t DensityBase{};
			std::uint16_t MaterialBase{};
			std::uint8_t Layers{};
			MaskType DensityMask{};
		};

		static std::int16_t Predict(const Column &Value, int Y) noexcept {
			return static_cast<std::int16_t>(std::clamp(Value.Intercept + Y * Value.Slope,
				-static_cast<int>(SaturatedDensity), static_cast<int>(SaturatedDensity)));
		}

		static void Fit(Column &Output, const std::array<Sample, Side> &Values, int SolidCount) noexcept {
			const auto First = Values.front().Density;
			if (std::ranges::all_of(Values, [First](Sample Value) { return Value.Density == First; })) {
				Output.Intercept = First;
				return;
			}
			// A default fit works even when a cliff saturates on both sides.
			Output.Slope = 8192;
			Output.Intercept = -SolidCount * static_cast<int>(Output.Slope) + Output.Slope / 2;
			int BestScore = std::numeric_limits<int>::max();
			for (int Y = 0; Y < Side - 1; ++Y) {
				const int Low = Values[Y].Density;
				const int High = Values[Y + 1].Density;
				const int Score = std::abs(Low) + std::abs(High);
				if (std::abs(Low) >= SaturatedDensity || std::abs(High) >= SaturatedDensity ||
					High <= Low || High - Low > SaturatedDensity || Score >= BestScore) continue;
				BestScore = Score;
				Output.Slope = static_cast<std::uint16_t>(High - Low);
				Output.Intercept = Low - Y * Output.Slope;
			}
		}

	public:
		static constexpr std::size_t SampleCount = Side * Side * Side;
		static constexpr std::size_t MaximumLayers = 4;

		static std::unique_ptr<const Columns> Pack(std::span<const Sample, SampleCount> Source) {
			auto Result = std::make_unique<Columns>();
			for (int Z = 0; Z < Side; ++Z) {
				for (int X = 0; X < Side; ++X) {
					std::array<Sample, Side> Values;
					int SolidCount = 0;
					for (int Y = 0; Y < Side; ++Y) {
						const auto Value = Source[(Z * Side + Y) * Side + X];
						if (Value.Density < 0) {
							if (SolidCount != Y) return {};
							++SolidCount;
						}
						Values[Y] = Value;
					}
					auto &ColumnValue = Result->Profiles[Z * Side + X];
					ColumnValue.MaterialBase = static_cast<std::uint16_t>(Result->MaterialIds.size());
					for (int Y = 0; Y < SolidCount; ++Y) {
						const auto Material = Values[Y].Material;
						if (Y + 1 < SolidCount && Material == Values[Y + 1].Material) continue;
						if (++ColumnValue.Layers > MaximumLayers) return {};
						Result->MaterialIds.push_back(Material);
						Result->LayerEnds.push_back(static_cast<std::uint8_t>(Y + 1));
					}
					Fit(ColumnValue, Values, SolidCount);
					ColumnValue.DensityBase = static_cast<std::uint16_t>(Result->DensityValues.size());
					for (int Y = 0; Y < Side; ++Y) {
						if (Values[Y].Density == Predict(ColumnValue, Y)) continue;
						ColumnValue.DensityMask |= static_cast<MaskType>(std::uint32_t{1} << Y);
						Result->DensityValues.push_back(Values[Y].Density);
					}
				}
			}
			Result->DensityValues.shrink_to_fit();
			Result->MaterialIds.shrink_to_fit();
			Result->LayerEnds.shrink_to_fit();
			return Result;
		}

		Sample Read(int X, int Y, int Z) const noexcept {
			const auto &Value = Profiles[Z * Side + X];
			const auto Bit = std::uint32_t{1} << Y;
			const auto Mask = static_cast<std::uint32_t>(Value.DensityMask);
			const auto Density = Mask & Bit ? DensityValues[Value.DensityBase + std::popcount(Mask & (Bit - 1))]
				: Predict(Value, Y);
			if (Density >= 0) return {Density, 0};
			auto Layer = Value.MaterialBase;
			while (Y >= LayerEnds[Layer]) ++Layer;
			return {Density, MaterialIds[Layer]};
		}

		std::size_t Bytes() const noexcept {
			return sizeof(*this) + DensityValues.capacity() * sizeof(std::int16_t) +
				MaterialIds.capacity() * sizeof(MaterialId) + LayerEnds.capacity();
		}

		std::size_t GetResidualCount() const noexcept { return DensityValues.size(); }

	private:
		std::array<Column, Side * Side> Profiles;
		std::vector<std::int16_t> DensityValues;
		std::vector<MaterialId> MaterialIds;
		std::vector<std::uint8_t> LayerEnds;
	};
}
