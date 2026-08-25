// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace gargantuan {
	namespace LatestFrameMailboxLayout {
		inline constexpr std::uint32_t Version = 2;
		inline constexpr std::uint32_t SlotCount = 2;
		inline constexpr std::uint32_t HeaderBytes = 128;
		inline constexpr std::uint32_t SlotHeaderBytes = 128;
		inline constexpr std::uint32_t MaximumWidth = 3840;
		inline constexpr std::uint32_t MaximumHeight = 2160;
		inline constexpr std::uint64_t MaximumPixels =
			static_cast<std::uint64_t>(MaximumWidth) * MaximumHeight;
		inline constexpr std::uint32_t MaximumPayloadBytes =
			static_cast<std::uint32_t>(MaximumPixels * 4);
		inline constexpr std::uint32_t SlotStride = SlotHeaderBytes + MaximumPayloadBytes;
		inline constexpr std::uint64_t MappingBytes =
			HeaderBytes + static_cast<std::uint64_t>(SlotCount) * SlotStride;
		inline constexpr std::uint32_t PixelFormatBgra8 = 2;

		enum class SlotState : std::uint32_t { Empty = 0, Writing = 1, Complete = 2 };
		enum class Mode : std::uint32_t { Edit = 1, Play = 2 };
	}

	struct LatestViewportFrameMetadata {
		std::uint64_t Generation = 0;
		LatestFrameMailboxLayout::Mode Mode = LatestFrameMailboxLayout::Mode::Edit;
		std::uint64_t PlaySessionId = 0;
		std::uint64_t CameraRevision = 0;
		std::uint64_t SourceRevision = 0;
		std::chrono::microseconds RenderSubmission{};
		std::chrono::microseconds GpuReadbackWait{};
		std::chrono::microseconds CpuExtraction{};
	};

	class LatestFrameMailbox final {
	  public:
		LatestFrameMailbox();
		~LatestFrameMailbox();

		LatestFrameMailbox(const LatestFrameMailbox &) = delete;
		LatestFrameMailbox &operator=(const LatestFrameMailbox &) = delete;

		[[nodiscard]] static bool IsSupported();
		[[nodiscard]] const std::string &GetName() const { return Name; }
		[[nodiscard]] const std::string &GetEventName() const { return EventName; }
		[[nodiscard]] std::uint64_t GetMappingBytes() const { return LatestFrameMailboxLayout::MappingBytes; }
		[[nodiscard]] std::uint64_t GetLatestSequence() const { return LatestSequence; }

		std::uint64_t Publish(
			std::uint32_t Width,
			std::uint32_t Height,
			std::span<const std::uint8_t> BgraPixels,
			const LatestViewportFrameMetadata &Metadata
		);
		void Close();

	  private:
		std::string Name;
		std::string EventName;
		void *MappingHandle = nullptr;
		void *FrameEventHandle = nullptr;
		std::uint8_t *Memory = nullptr;
		std::uint64_t LatestSequence = 0;
	};
}
