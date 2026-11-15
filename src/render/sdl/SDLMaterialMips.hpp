#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace gargantuan {
	struct SDLMaterialMipLevel {
		std::uint32_t Width, Height, Offset;
	};

	struct SDLMaterialMipLayout {
		std::array<SDLMaterialMipLevel, 32> Levels{};
		std::uint32_t Count = 0;
		std::uint32_t Bytes = 0;
	};

	inline std::optional<SDLMaterialMipLayout> GetMaterialMipLayout(std::uint32_t Width, std::uint32_t Height,
		std::uint32_t ByteLimit) {
		if (!Width || !Height) return std::nullopt;
		SDLMaterialMipLayout Layout;
		for (;;) {
			const auto Pixels = static_cast<std::uint64_t>(Width) * Height;
			if (Pixels > (ByteLimit - Layout.Bytes) / 4u) return std::nullopt;
			Layout.Levels[Layout.Count++] = {Width, Height, Layout.Bytes};
			Layout.Bytes += static_cast<std::uint32_t>(Pixels * 4u);
			if (Width == 1 && Height == 1) return Layout;
			Width = std::max(1u, Width / 2);
			Height = std::max(1u, Height / 2);
		}
	}

	inline bool BuildMaterialMipPixels(std::span<std::uint8_t> Output, std::span<const std::uint8_t> Source,
		const SDLMaterialMipLayout &Layout, bool Srgb) {
		const auto &Base = Layout.Levels.front();
		if (!Layout.Count || Output.size() != Layout.Bytes ||
			Source.size() != static_cast<std::uint64_t>(Base.Width) * Base.Height * 4u) return false;
		std::ranges::copy(Source, Output.begin());
		static const auto Color = [] {
			struct Tables {
				std::array<double, 256> Decode;
				std::array<double, 255> Cutoffs;
				std::array<std::uint8_t, 4097> EncodeFloor;
			} Values;
			const auto Decode = [](double Value) {
				return Value <= 0.04045 ? Value / 12.92 : std::pow((Value + 0.055) / 1.055, 2.4);
			};
			for (std::size_t Byte = 0; Byte < Values.Decode.size(); ++Byte)
				Values.Decode[Byte] = Decode(static_cast<double>(Byte) / 255.0);
			for (std::size_t Byte = 0; Byte < Values.Cutoffs.size(); ++Byte)
				Values.Cutoffs[Byte] = Decode((static_cast<double>(Byte) + 0.5) / 255.0);
			std::size_t Byte = 0;
			for (std::size_t Bin = 0; Bin < Values.EncodeFloor.size(); ++Bin) {
				while (Byte < Values.Cutoffs.size() && static_cast<double>(Bin) / 4096.0 >= Values.Cutoffs[Byte]) ++Byte;
				Values.EncodeFloor[Bin] = static_cast<std::uint8_t>(Byte);
			}
			return Values;
		}();
		const auto EncodeColor = [&](double Value) {
			const auto Bin = static_cast<std::size_t>(std::clamp(Value, 0.0, 1.0) * 4096.0);
			auto Byte = Color.EncodeFloor[Bin];
			// Each bin is narrower than the closest pair of rounding cutoffs.
			if (Byte < Color.Cutoffs.size() && Value >= Color.Cutoffs[Byte]) ++Byte;
			return Byte;
		};
		const auto Encode = [&](double Value, unsigned Channel) {
			if (Srgb && Channel < 3) return EncodeColor(Value);
			return static_cast<std::uint8_t>(std::clamp(std::lround(Value), 0L, 255L));
		};
		for (std::uint32_t Level = 1; Level < Layout.Count; ++Level) {
			const auto Previous = Layout.Levels[Level - 1];
			const auto Next = Layout.Levels[Level];
			if (Previous.Width == Next.Width * 2 && Previous.Height == Next.Height * 2) {
				const auto RowStride = static_cast<std::size_t>(Previous.Width) * 4;
				for (std::size_t Y = 0; Y < Next.Height; ++Y) {
					for (std::size_t X = 0; X < Next.Width; ++X) {
						const auto Pixel = Previous.Offset + Y * RowStride * 2 + X * 8;
						const auto Destination = Next.Offset + (Y * Next.Width + X) * 4;
						for (unsigned Channel = 0; Channel < 4; ++Channel) {
							const auto A = Output[Pixel + Channel];
							const auto B = Output[Pixel + Channel + 4];
							const auto C = Output[Pixel + Channel + RowStride];
							const auto D = Output[Pixel + Channel + RowStride + 4];
							Output[Destination + Channel] = Srgb && Channel < 3 ? EncodeColor(
								(Color.Decode[A] + Color.Decode[B] + Color.Decode[C] + Color.Decode[D]) * 0.25) :
								static_cast<std::uint8_t>((A + B + C + D + 2) / 4);
						}
					}
				}
				continue;
			}
			const double Weight = static_cast<double>(Previous.Width) * Previous.Height;
			// Integrate the full source footprint even when an odd dimension shrinks.
			// Integer overlap weights represent the fractional source texel coverage.
			for (std::uint64_t Y = 0; Y < Next.Height; ++Y) {
				const auto Top = Y * Previous.Height;
				const auto Bottom = Top + Previous.Height;
				for (std::uint64_t X = 0; X < Next.Width; ++X) {
					const auto Left = X * Previous.Width;
					const auto Right = Left + Previous.Width;
					std::array<double, 4> Sum{};
					for (auto SrcY = Top / Next.Height; SrcY <= (Bottom - 1) / Next.Height; ++SrcY) {
						const auto WeightY = std::min(Bottom, (SrcY + 1) * Next.Height) - std::max(Top, SrcY * Next.Height);
						for (auto SrcX = Left / Next.Width; SrcX <= (Right - 1) / Next.Width; ++SrcX) {
							const auto WeightX = std::min(Right, (SrcX + 1) * Next.Width) - std::max(Left, SrcX * Next.Width);
							const auto Pixel = Previous.Offset + (SrcY * Previous.Width + SrcX) * 4;
							for (unsigned Channel = 0; Channel < Sum.size(); ++Channel) {
								const auto Byte = Output[Pixel + Channel];
								Sum[Channel] += (Srgb && Channel < 3 ? Color.Decode[Byte] : Byte) *
									static_cast<double>(WeightX * WeightY);
							}
						}
					}
					const auto Pixel = Next.Offset + (Y * Next.Width + X) * 4;
					for (unsigned Channel = 0; Channel < Sum.size(); ++Channel)
						Output[Pixel + Channel] = Encode(Sum[Channel] / Weight, Channel);
				}
			}
		}
		return true;
	}
}
