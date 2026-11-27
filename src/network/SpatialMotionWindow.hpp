// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "SpatialAudience.hpp"
#include "gargantuan/assets/AssetTypes.hpp"
#include <cstring>

namespace gargantuan::network {
	inline constexpr std::uint32_t SpatialMotionWindowMagic = 0x4d445347; // GSDM
	struct SpatialMotionWindow {
		bool Installed{};
		std::uint64_t Epoch{}, Grant{}, Serial{}, Possession{};
		ObjectId Domain, Subject;
		SpatialDestinationBounds Bounds;
		AssetContentId Digest;
		bool operator==(const SpatialMotionWindow &) const = default;
	};
	inline bool ValidMotionWindow(const SpatialMotionWindow &Window) {
		return Window.Epoch && Window.Serial && Window.Domain.IsValid() && Window.Subject.IsValid() &&
			Window.Domain!=Window.Subject && Window.Digest.IsValid() && ValidSpatialArea(Window.Bounds);
	}
	inline std::vector<std::byte> EncodeMotionWindow(const SpatialMotionWindow &Window) {
		if (!ValidMotionWindow(Window)) return {};
		GameBinaryWriter Out(128);
		Out.Integer(SpatialMotionWindowMagic);
		Out.Integer<std::uint8_t>(1);
		Out.Integer<std::uint8_t>(Window.Installed);
		Out.Integer(Window.Epoch); Out.Integer(Window.Grant); Out.Integer(Window.Serial); Out.Integer(Window.Possession);
		WriteBinaryObjectId(Out,Window.Domain); WriteBinaryObjectId(Out,Window.Subject);
		for (int I=0; I<3; ++I) { Out.Float(Window.Bounds.Minimum[I]); Out.Float(Window.Bounds.Maximum[I]); }
		Out.Append(std::as_bytes(std::span(Window.Digest.Bytes)));
		return std::move(Out.Bytes);
	}
	inline std::optional<SpatialMotionWindow> DecodeMotionWindow(std::span<const std::byte> Bytes) {
		GameBinaryReader In(Bytes);
		SpatialMotionWindow Window;
		std::uint32_t Magic{};
		std::uint8_t Version{}, Installed{};
		if (!In.Integer(Magic) || Magic!=SpatialMotionWindowMagic || !In.Integer(Version) || Version!=1 ||
			!In.Integer(Installed) || Installed>1 || !In.Integer(Window.Epoch) || !In.Integer(Window.Grant) ||
			!In.Integer(Window.Serial) || !In.Integer(Window.Possession) || !ReadBinaryObjectId(In,Window.Domain) ||
			!ReadBinaryObjectId(In,Window.Subject)) return {};
		Window.Installed=Installed!=0;
		for (int I=0; I<3; ++I) if (!In.Float(Window.Bounds.Minimum[I]) || !In.Float(Window.Bounds.Maximum[I])) return {};
		std::span<const std::byte> Digest;
		if (!In.Take(Window.Digest.Bytes.size(),Digest) || !In.Complete()) return {};
		std::memcpy(Window.Digest.Bytes.data(),Digest.data(),Digest.size());
		return ValidMotionWindow(Window) ? std::optional(Window) : std::nullopt;
	}
}
