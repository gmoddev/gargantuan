// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/editor/SharedFrameRing.hpp"

#include <array>
#include <cstring>
#include <format>
#include <limits>
#include <random>
#include <stdexcept>
#include <type_traits>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace gargantuan {
	namespace {
		constexpr std::array<std::uint8_t, 8> Magic = {'G', 'F', 'R', 'R', 'I', 'N', 'G', '1'};
		constexpr std::size_t HeaderLatestSequenceOffset = 48;
		constexpr std::size_t SlotStateOffset = 0;
		constexpr std::size_t SlotVersionOffset = 4;
		constexpr std::size_t SlotSequenceOffset = 8;
		constexpr std::size_t SlotTimestampOffset = 16;
		constexpr std::size_t SlotWidthOffset = 24;
		constexpr std::size_t SlotHeightOffset = 28;
		constexpr std::size_t SlotPixelFormatOffset = 32;
		constexpr std::size_t SlotPayloadBytesOffset = 36;

		template <typename Value>
		void Write(std::uint8_t *destination, std::size_t offset, Value value) {
			static_assert(std::is_trivially_copyable_v<Value>);
			std::memcpy(destination + offset, &value, sizeof(Value));
		}

		std::string CreateRandomName() {
			std::random_device random;
			return std::format(
				"Local\\GargantuanViewport-{:08x}{:08x}{:08x}{:08x}",
				random(), random(), random(), random()
			);
		}
	}

	bool SharedFrameRing::IsSupported() {
#ifdef _WIN32
		return true;
#else
		return false;
#endif
	}

	SharedFrameRing::SharedFrameRing() {
#ifdef _WIN32
		static_assert(SharedFrameRingLayout::MappingBytes <= std::numeric_limits<DWORD>::max());
		HANDLE mapping = nullptr;
		for (int attempt = 0; attempt < 8; ++attempt) {
			Name = CreateRandomName();
			mapping = CreateFileMappingA(
				INVALID_HANDLE_VALUE,
				nullptr,
				PAGE_READWRITE,
				0,
				static_cast<DWORD>(SharedFrameRingLayout::MappingBytes),
				Name.c_str()
			);
			if (mapping && GetLastError() != ERROR_ALREADY_EXISTS) break;
			if (mapping) CloseHandle(mapping);
			mapping = nullptr;
		}
		if (!mapping) throw std::runtime_error("Failed to create a unique viewport shared-memory mapping");

		MappingHandle = mapping;
		Memory = static_cast<std::uint8_t *>(MapViewOfFile(
			mapping,
			FILE_MAP_ALL_ACCESS,
			0,
			0,
			static_cast<SIZE_T>(SharedFrameRingLayout::MappingBytes)
		));
		if (!Memory) {
			Close();
			throw std::runtime_error("Failed to map the viewport shared-memory ring");
		}

		std::memset(Memory, 0, static_cast<std::size_t>(SharedFrameRingLayout::MappingBytes));
		std::memcpy(Memory, Magic.data(), Magic.size());
		Write(Memory, 8, SharedFrameRingLayout::Version);
		Write(Memory, 12, SharedFrameRingLayout::HeaderBytes);
		Write(Memory, 16, SharedFrameRingLayout::SlotCount);
		Write(Memory, 20, SharedFrameRingLayout::SlotHeaderBytes);
		Write(Memory, 24, SharedFrameRingLayout::SlotStride);
		Write(Memory, 28, SharedFrameRingLayout::MaximumPayloadBytes);
		Write(Memory, 32, SharedFrameRingLayout::PixelFormatRgb8);
		Write(Memory, 40, SharedFrameRingLayout::MappingBytes);
		MemoryBarrier();
#else
		throw std::runtime_error("Shared-memory viewport transport is not supported on this platform");
#endif
	}

	SharedFrameRing::~SharedFrameRing() { Close(); }

	std::uint64_t SharedFrameRing::Publish(
		std::uint32_t width,
		std::uint32_t height,
		std::span<const std::uint8_t> rgbPixels,
		std::uint64_t timestampNanoseconds
	) {
#ifdef _WIN32
		if (!Memory || !MappingHandle) throw std::runtime_error("Viewport shared-memory ring is closed");
		if (width == 0 || height == 0 || width > 1024 || height > 1024)
			throw std::invalid_argument("Shared viewport frame dimensions are invalid");
		const auto pixels = static_cast<std::uint64_t>(width) * height;
		if (pixels > 1024 * 1024 || pixels > std::numeric_limits<std::uint32_t>::max() / 3)
			throw std::invalid_argument("Shared viewport frame dimensions overflow protocol limits");
		const auto payloadBytes = static_cast<std::uint32_t>(pixels * 3);
		if (payloadBytes > SharedFrameRingLayout::MaximumPayloadBytes || rgbPixels.size() != payloadBytes)
			throw std::invalid_argument("Shared viewport frame payload size is invalid");

		const auto sequence = LatestSequence + 1;
		const auto slotIndex = static_cast<std::uint32_t>((sequence - 1) % SharedFrameRingLayout::SlotCount);
		auto *slot = Memory + SharedFrameRingLayout::HeaderBytes +
			static_cast<std::size_t>(slotIndex) * SharedFrameRingLayout::SlotStride;
		auto *state = reinterpret_cast<volatile LONG *>(slot + SlotStateOffset);
		InterlockedExchange(state, static_cast<LONG>(SharedFrameRingLayout::SlotState::Writing));
		Write(slot, SlotVersionOffset, SharedFrameRingLayout::Version);
		Write(slot, SlotSequenceOffset, sequence);
		Write(slot, SlotTimestampOffset, timestampNanoseconds);
		Write(slot, SlotWidthOffset, width);
		Write(slot, SlotHeightOffset, height);
		Write(slot, SlotPixelFormatOffset, SharedFrameRingLayout::PixelFormatRgb8);
		Write(slot, SlotPayloadBytesOffset, payloadBytes);
		std::memcpy(slot + SharedFrameRingLayout::SlotHeaderBytes, rgbPixels.data(), rgbPixels.size());
		MemoryBarrier();
		InterlockedExchange(state, static_cast<LONG>(SharedFrameRingLayout::SlotState::Complete));
		InterlockedExchange64(
			reinterpret_cast<volatile LONG64 *>(Memory + HeaderLatestSequenceOffset),
			static_cast<LONG64>(sequence)
		);
		LatestSequence = sequence;
		return sequence;
#else
		(void)width;
		(void)height;
		(void)rgbPixels;
		(void)timestampNanoseconds;
		throw std::runtime_error("Shared-memory viewport transport is not supported on this platform");
#endif
	}

	void SharedFrameRing::Close() {
#ifdef _WIN32
		if (Memory) UnmapViewOfFile(Memory);
		if (MappingHandle) CloseHandle(static_cast<HANDLE>(MappingHandle));
#endif
		Memory = nullptr;
		MappingHandle = nullptr;
		Name.clear();
	}
}
