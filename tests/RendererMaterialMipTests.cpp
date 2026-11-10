#include "render/sdl/SDLMaterialMips.hpp"

#include <iostream>
#include <stdexcept>

namespace {
	using namespace gargantuan;

	void Check(bool Condition, const char *Message) {
		if (!Condition) throw std::runtime_error(Message);
	}

	void CheckColorAndData() {
		const auto Layout = GetMaterialMipLayout(2, 2, 20);
		Check(Layout && Layout->Count == 2 && Layout->Bytes == 20 && Layout->Levels[1].Offset == 16,
			"2x2 mip layout must contain its tightly packed 1x1 level");
		const std::array<std::uint8_t, 16> Source{0, 0, 255, 0, 255, 0, 128, 255, 0, 0, 64, 0, 255, 0, 0, 255};
		std::array<std::uint8_t, 20> Linear{}, Srgb{};
		Check(BuildMaterialMipPixels(Linear, Source, *Layout, false) && BuildMaterialMipPixels(Srgb, Source, *Layout, true),
			"valid source must build both texture representations");
		Check(std::ranges::equal(Source, std::span(Linear).first(16)) &&
			std::ranges::equal(Source, std::span(Srgb).first(16)), "both representations must preserve exact level-zero bytes");
		Check(Linear[16] == 128 && Linear[17] == 0 && Linear[18] == 112,
			"data mips must average channels directly");
		Check(Srgb[16] == 188 && Srgb[17] == 0 && Srgb[18] > Linear[18],
			"color mips must average decoded sRGB and encode after filtering");
		Check(Linear[19] == 128 && Srgb[19] == Linear[19], "alpha must remain linear in both representations");
	}

	void CheckOddExtent() {
		const auto Layout = GetMaterialMipLayout(5, 3, 72);
		Check(Layout && Layout->Count == 3 && Layout->Bytes == 72 &&
			Layout->Levels[1].Width == 2 && Layout->Levels[1].Height == 1 && Layout->Levels[2].Offset == 68,
			"odd textures must floor each mip dimension without dropping the trailing source coverage");
		std::array<std::uint8_t, 60> Source{};
		Source[56] = 255;
		Source[59] = 255;
		std::array<std::uint8_t, 72> Mips{};
		Check(BuildMaterialMipPixels(Mips, Source, *Layout, false), "odd source must build");
		Check(Mips[60] == 0 && Mips[64] == 34 && Mips[68] == 17 && Mips[71] == 17,
			"a trailing corner texel must survive both odd reductions with area weighting");
		const auto Narrow = GetMaterialMipLayout(1, 7, 44);
		Check(Narrow && Narrow->Count == 3 && Narrow->Levels[1].Height == 3 && Narrow->Levels[2].Height == 1,
			"one-pixel-wide textures must keep their valid height chain");
	}

	void CheckBounds() {
		Check(!GetMaterialMipLayout(0, 1, 4), "zero dimensions must be rejected");
		Check(!GetMaterialMipLayout(2, 2, 19), "every level must fit the byte limit");
		Check(!GetMaterialMipLayout(UINT32_MAX, UINT32_MAX, UINT32_MAX), "oversized dimensions must fail without overflow");
		const auto Layout = GetMaterialMipLayout(1, 1, 4);
		Check(Layout && Layout->Count == 1 && Layout->Bytes == 4, "single-pixel inputs need only one level");
		std::array<std::uint8_t, 4> Output{};
		const std::array<std::uint8_t, 3> Truncated{};
		Check(!BuildMaterialMipPixels(Output, Truncated, *Layout, true), "truncated input must fail before copying bytes");
	}
}

int main() {
	try {
		CheckColorAndData();
		CheckOddExtent();
		CheckBounds();
		std::cout << "[Renderer:MaterialMips] Color, data, alpha, odd extents and allocation limits passed\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Renderer:MaterialMips] " << Error.what() << '\n';
		return 1;
	}
}
