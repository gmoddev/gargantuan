// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace gargantuan {
	namespace SharedFrameRingLayout {
		inline constexpr std::uint32_t Version = 1;
		inline constexpr std::uint32_t SlotCount = 3;
		inline constexpr std::uint32_t HeaderBytes = 64;
		inline constexpr std::uint32_t SlotHeaderBytes = 64;
		inline constexpr std::uint32_t MaximumPayloadBytes = 1024 * 1024 * 3;
		inline constexpr std::uint32_t SlotStride = SlotHeaderBytes + MaximumPayloadBytes;
		inline constexpr std::uint64_t MappingBytes = HeaderBytes + static_cast<std::uint64_t>(SlotCount) * SlotStride;
		inline constexpr std::uint32_t PixelFormatRgb8 = 1;

		enum class SlotState : std::uint32_t { Empty = 0, Writing = 1, Complete = 2 };
	}

	class SharedFrameRing final {
	  public:
		SharedFrameRing();
		~SharedFrameRing();

		SharedFrameRing(const SharedFrameRing &) = delete;
		SharedFrameRing &operator=(const SharedFrameRing &) = delete;

		[[nodiscard]] static bool IsSupported();
		[[nodiscard]] const std::string &GetName() const { return Name; }
		[[nodiscard]] std::uint64_t GetMappingBytes() const { return SharedFrameRingLayout::MappingBytes; }
		[[nodiscard]] std::uint64_t GetLatestSequence() const { return LatestSequence; }

		std::uint64_t Publish(
			std::uint32_t width,
			std::uint32_t height,
			std::span<const std::uint8_t> rgbPixels,
			std::uint64_t timestampNanoseconds
		);
		void Close();

	  private:
		std::string Name;
		void *MappingHandle = nullptr;
		std::uint8_t *Memory = nullptr;
		std::uint64_t LatestSequence = 0;
	};
}
