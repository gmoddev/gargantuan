// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/editor/LatestFrameMailbox.hpp"

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
		constexpr std::array<std::uint8_t, 8> Magic = {'G', 'F', 'M', 'A', 'I', 'L', 'B', '2'};
		constexpr std::size_t HeaderLatestSequenceOffset = 64;
		constexpr std::size_t SlotStateOffset = 0;
		constexpr std::size_t SlotVersionOffset = 4;
		constexpr std::size_t SlotSequenceOffset = 8;
		constexpr std::size_t SlotCompletionTimestampOffset = 16;
		constexpr std::size_t SlotGenerationOffset = 24;
		constexpr std::size_t SlotWidthOffset = 32;
		constexpr std::size_t SlotHeightOffset = 36;
		constexpr std::size_t SlotPixelFormatOffset = 40;
		constexpr std::size_t SlotPayloadBytesOffset = 44;
		constexpr std::size_t SlotModeOffset = 48;
		constexpr std::size_t SlotPlaySessionIdOffset = 56;
		constexpr std::size_t SlotCameraRevisionOffset = 64;
		constexpr std::size_t SlotSourceRevisionOffset = 72;
		constexpr std::size_t SlotRenderSubmissionUsOffset = 80;
		constexpr std::size_t SlotGpuReadbackUsOffset = 88;
		constexpr std::size_t SlotCpuExtractionUsOffset = 96;

		template <typename Value>
		void Write(std::uint8_t *Destination, std::size_t Offset, Value Item) {
			static_assert(std::is_trivially_copyable_v<Value>);
			std::memcpy(Destination + Offset, &Item, sizeof(Value));
		}

		std::string CreateRandomName(std::string_view Prefix) {
			std::random_device Random;
			return std::format(
				"Local\\{}-{:08x}{:08x}{:08x}{:08x}",
				Prefix, Random(), Random(), Random(), Random()
			);
		}
	}

	bool LatestFrameMailbox::IsSupported() {
#ifdef _WIN32
		return true;
#else
		return false;
#endif
	}

	LatestFrameMailbox::LatestFrameMailbox() {
#ifdef _WIN32
		static_assert(LatestFrameMailboxLayout::MappingBytes <= std::numeric_limits<DWORD>::max());
		HANDLE Mapping = nullptr;
		for (int Attempt = 0; Attempt < 8; ++Attempt) {
			Name = CreateRandomName("GargantuanViewportMailbox");
			Mapping = CreateFileMappingA(
				INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
				static_cast<DWORD>(LatestFrameMailboxLayout::MappingBytes), Name.c_str()
			);
			if (Mapping && GetLastError() != ERROR_ALREADY_EXISTS) break;
			if (Mapping) CloseHandle(Mapping);
			Mapping = nullptr;
		}
		if (!Mapping) throw std::runtime_error("Failed to create a unique viewport latest-frame mapping");
		MappingHandle = Mapping;
		Memory = static_cast<std::uint8_t *>(MapViewOfFile(
			Mapping, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(LatestFrameMailboxLayout::MappingBytes)
		));
		if (!Memory) {
			Close();
			throw std::runtime_error("Failed to map the viewport latest-frame mailbox");
		}

		HANDLE Event = nullptr;
		for (int Attempt = 0; Attempt < 8; ++Attempt) {
			EventName = CreateRandomName("GargantuanViewportFrame");
			Event = CreateEventA(nullptr, false, false, EventName.c_str());
			if (Event && GetLastError() != ERROR_ALREADY_EXISTS) break;
			if (Event) CloseHandle(Event);
			Event = nullptr;
		}
		if (!Event) {
			Close();
			throw std::runtime_error("Failed to create a unique viewport frame event");
		}
		FrameEventHandle = Event;

		LARGE_INTEGER Frequency{};
		if (!QueryPerformanceFrequency(&Frequency) || Frequency.QuadPart <= 0) {
			Close();
			throw std::runtime_error("Failed to obtain the viewport presentation clock frequency");
		}
		std::memset(Memory, 0, static_cast<std::size_t>(LatestFrameMailboxLayout::MappingBytes));
		std::memcpy(Memory, Magic.data(), Magic.size());
		Write(Memory, 8, LatestFrameMailboxLayout::Version);
		Write(Memory, 12, LatestFrameMailboxLayout::HeaderBytes);
		Write(Memory, 16, LatestFrameMailboxLayout::SlotCount);
		Write(Memory, 20, LatestFrameMailboxLayout::SlotHeaderBytes);
		Write(Memory, 24, LatestFrameMailboxLayout::SlotStride);
		Write(Memory, 28, LatestFrameMailboxLayout::MaximumPayloadBytes);
		Write(Memory, 32, LatestFrameMailboxLayout::MaximumWidth);
		Write(Memory, 36, LatestFrameMailboxLayout::MaximumHeight);
		Write(Memory, 40, LatestFrameMailboxLayout::PixelFormatBgra8);
		Write(Memory, 48, LatestFrameMailboxLayout::MappingBytes);
		Write(Memory, 56, static_cast<std::uint64_t>(Frequency.QuadPart));
		MemoryBarrier();
#else
		throw std::runtime_error("Latest-frame viewport transport is not supported on this platform");
#endif
	}

	LatestFrameMailbox::~LatestFrameMailbox() { Close(); }

	std::uint64_t LatestFrameMailbox::Publish(
		std::uint32_t Width,
		std::uint32_t Height,
		std::span<const std::uint8_t> BgraPixels,
		const LatestViewportFrameMetadata &Metadata
	) {
#ifdef _WIN32
		if (!Memory || !MappingHandle || !FrameEventHandle)
			throw std::runtime_error("Viewport latest-frame mailbox is closed");
		if (Width == 0 || Height == 0 || Width > LatestFrameMailboxLayout::MaximumWidth ||
			Height > LatestFrameMailboxLayout::MaximumHeight)
			throw std::invalid_argument("Latest viewport frame dimensions are invalid");
		const auto Pixels = static_cast<std::uint64_t>(Width) * Height;
		if (Pixels > LatestFrameMailboxLayout::MaximumPixels || Pixels > std::numeric_limits<std::uint32_t>::max() / 4)
			throw std::invalid_argument("Latest viewport frame dimensions overflow protocol limits");
		const auto PayloadBytes = static_cast<std::uint32_t>(Pixels * 4);
		if (PayloadBytes > LatestFrameMailboxLayout::MaximumPayloadBytes || BgraPixels.size() != PayloadBytes)
			throw std::invalid_argument("Latest viewport frame payload size is invalid");
		if (Metadata.Generation == 0 ||
			(Metadata.Mode == LatestFrameMailboxLayout::Mode::Edit && Metadata.PlaySessionId != 0) ||
			(Metadata.Mode == LatestFrameMailboxLayout::Mode::Play && Metadata.PlaySessionId == 0))
			throw std::invalid_argument("Latest viewport frame identity is invalid");

		LARGE_INTEGER CompletionTimestamp{};
		QueryPerformanceCounter(&CompletionTimestamp);
		const auto Sequence = LatestSequence + 1;
		const auto SlotIndex = static_cast<std::uint32_t>((Sequence - 1) % LatestFrameMailboxLayout::SlotCount);
		auto *Slot = Memory + LatestFrameMailboxLayout::HeaderBytes +
			static_cast<std::size_t>(SlotIndex) * LatestFrameMailboxLayout::SlotStride;
		auto *State = reinterpret_cast<volatile LONG *>(Slot + SlotStateOffset);
		InterlockedExchange(State, static_cast<LONG>(LatestFrameMailboxLayout::SlotState::Writing));
		Write(Slot, SlotVersionOffset, LatestFrameMailboxLayout::Version);
		Write(Slot, SlotSequenceOffset, Sequence);
		Write(Slot, SlotCompletionTimestampOffset, static_cast<std::uint64_t>(CompletionTimestamp.QuadPart));
		Write(Slot, SlotGenerationOffset, Metadata.Generation);
		Write(Slot, SlotWidthOffset, Width);
		Write(Slot, SlotHeightOffset, Height);
		Write(Slot, SlotPixelFormatOffset, LatestFrameMailboxLayout::PixelFormatBgra8);
		Write(Slot, SlotPayloadBytesOffset, PayloadBytes);
		Write(Slot, SlotModeOffset, static_cast<std::uint32_t>(Metadata.Mode));
		Write(Slot, SlotPlaySessionIdOffset, Metadata.PlaySessionId);
		Write(Slot, SlotCameraRevisionOffset, Metadata.CameraRevision);
		Write(Slot, SlotSourceRevisionOffset, Metadata.SourceRevision);
		Write(Slot, SlotRenderSubmissionUsOffset, static_cast<std::uint64_t>(Metadata.RenderSubmission.count()));
		Write(Slot, SlotGpuReadbackUsOffset, static_cast<std::uint64_t>(Metadata.GpuReadbackWait.count()));
		Write(Slot, SlotCpuExtractionUsOffset, static_cast<std::uint64_t>(Metadata.CpuExtraction.count()));
		std::memcpy(Slot + LatestFrameMailboxLayout::SlotHeaderBytes, BgraPixels.data(), BgraPixels.size());
		MemoryBarrier();
		InterlockedExchange(State, static_cast<LONG>(LatestFrameMailboxLayout::SlotState::Complete));
		InterlockedExchange64(
			reinterpret_cast<volatile LONG64 *>(Memory + HeaderLatestSequenceOffset), static_cast<LONG64>(Sequence)
		);
		LatestSequence = Sequence;
		SetEvent(static_cast<HANDLE>(FrameEventHandle));
		return Sequence;
#else
		(void)Width;
		(void)Height;
		(void)BgraPixels;
		(void)Metadata;
		throw std::runtime_error("Latest-frame viewport transport is not supported on this platform");
#endif
	}

	void LatestFrameMailbox::Close() {
#ifdef _WIN32
		if (Memory) UnmapViewOfFile(Memory);
		if (FrameEventHandle) CloseHandle(static_cast<HANDLE>(FrameEventHandle));
		if (MappingHandle) CloseHandle(static_cast<HANDLE>(MappingHandle));
#endif
		Memory = nullptr;
		FrameEventHandle = nullptr;
		MappingHandle = nullptr;
		Name.clear();
		EventName.clear();
	}
}
