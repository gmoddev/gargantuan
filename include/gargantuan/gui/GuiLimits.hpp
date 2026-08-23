// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#pragma once

#include <cstddef>

namespace gargantuan {
	struct GuiLimits final {
		static constexpr std::size_t MaximumRoots = 8;
		static constexpr std::size_t MaximumNodesPerRoot = 16'384;
		static constexpr std::size_t MaximumHierarchyDepth = 128;
		static constexpr std::size_t MaximumLayoutPassesPerFrame = 8;
		static constexpr std::size_t MaximumAutomaticSizePasses = 8;
		static constexpr std::size_t MaximumDisplayPrimitives = 131'072;
		static constexpr std::size_t MaximumUiVertices = 524'288;
		static constexpr std::size_t MaximumUiIndices = 786'432;
		static constexpr std::size_t MaximumClipDepth = 32;
		static constexpr std::size_t MaximumPointerRouteDepth = 128;
		static constexpr std::size_t MaximumCapturedPointers = 16;
		static constexpr std::size_t MaximumTextBytesPerObject = 16 * 1024;
		static constexpr std::size_t MaximumGlyphsPerTextObject = 4096;
		static constexpr std::size_t MaximumShapedGlyphsPerFrame = 100'000;
		static constexpr std::size_t GlyphAtlasDimension = 1024;
		static constexpr std::size_t MaximumGlyphAtlasPages = 4;
		static constexpr std::size_t MaximumShapedTextCacheEntries = 16'384;
		static constexpr std::size_t MaximumFontInstances = 64;
		static constexpr std::size_t MaximumImages = 64;
		static constexpr std::size_t MaximumImageDimension = 1024;
		static constexpr std::size_t MaximumImageBytes = 32 * 1024 * 1024;
		static constexpr std::size_t MaximumTextureUploadBytesPerFrame = 8 * 1024 * 1024;
		static constexpr std::size_t MaximumDiagnostics = 64;
		static constexpr std::size_t MaximumDiagnosticBytes = 1024;
	};
}
