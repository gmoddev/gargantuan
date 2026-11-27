// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/network/BinaryCodec.hpp"
#include "gargantuan/network/ReplicationRelevance.hpp"
#include "gargantuan/runtime/SpatialDestination.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace gargantuan::network {
	inline constexpr std::uint32_t SpatialAudienceMagic = 0x41445347; // GSDA
	enum class SpatialAudienceAction : std::uint8_t { Stage = 1, Release, Promote };
	struct SpatialAudienceFrame {
		SpatialAudienceAction Action{};
		std::uint64_t Epoch{}, Serial{};
		ObjectId Destination;
		SpatialDestinationBounds Bounds;
		bool EmptyTerrain{};
		std::vector<ObjectId> Objects;
	};
	inline bool ValidSpatialArea(const SpatialDestinationBounds &Area) {
		for (int Axis = 0; Axis < 3; ++Axis) {
			const auto Min=Area.Minimum[Axis], Max=Area.Maximum[Axis];
			if (!std::isfinite(Min) || !std::isfinite(Max) || Min>Max || Max-Min>512.f ||
				std::abs(Min)>1'048'576.f || std::abs(Max)>1'048'576.f) return false;
		}
		return true;
	}
	inline bool ValidSpatialAudience(const SpatialAudienceFrame &Frame) {
		if (!Frame.Epoch || !Frame.Serial || !Frame.Destination.IsValid()) return false;
		if (Frame.Action != SpatialAudienceAction::Stage)
			return Frame.Objects.empty() && (Frame.Action == SpatialAudienceAction::Release || Frame.Action == SpatialAudienceAction::Promote);
		return ValidSpatialArea(Frame.Bounds) && !Frame.Objects.empty() && Frame.Objects.size()<=MaximumSpatialStagingObjects &&
			std::ranges::all_of(Frame.Objects, [](ObjectId Id){return Id.IsValid();}) && std::ranges::is_sorted(Frame.Objects) &&
			std::adjacent_find(Frame.Objects.begin(),Frame.Objects.end())==Frame.Objects.end() &&
			std::ranges::binary_search(Frame.Objects,Frame.Destination);
	}
	inline std::vector<std::byte> EncodeSpatialAudience(const SpatialAudienceFrame &Frame) {
		if (!ValidSpatialAudience(Frame)) throw std::invalid_argument("[Spatial:Network] Invalid audience binding");
		GameBinaryWriter Out(64+8*MaximumSpatialStagingObjects);
		Out.Integer(SpatialAudienceMagic);
		Out.Integer<std::uint8_t>(1);
		Out.Integer(static_cast<std::uint8_t>(Frame.Action));
		Out.Integer(Frame.Epoch);
		Out.Integer(Frame.Serial);
		WriteBinaryObjectId(Out,Frame.Destination);
		if (Frame.Action==SpatialAudienceAction::Stage) {
			for (int I=0; I<3; ++I) { Out.Float(Frame.Bounds.Minimum[I]); Out.Float(Frame.Bounds.Maximum[I]); }
			Out.Integer<std::uint8_t>(Frame.EmptyTerrain);
			Out.Integer(static_cast<std::uint16_t>(Frame.Objects.size()));
			for (const auto Id:Frame.Objects) WriteBinaryObjectId(Out,Id);
		}
		return std::move(Out.Bytes);
	}
	inline std::optional<SpatialAudienceFrame> DecodeSpatialAudience(std::span<const std::byte> Bytes) {
		if (Bytes.size()>64+8*MaximumSpatialStagingObjects) return {};
		GameBinaryReader In(Bytes);
		SpatialAudienceFrame Frame;
		std::uint32_t Magic{};
		std::uint8_t Version{}, Action{};
		if (!In.Integer(Magic) || Magic!=SpatialAudienceMagic || !In.Integer(Version) || Version!=1 ||
			!In.Integer(Action) || !In.Integer(Frame.Epoch) || !In.Integer(Frame.Serial) || !ReadBinaryObjectId(In,Frame.Destination)) return {};
		Frame.Action=static_cast<SpatialAudienceAction>(Action);
		if (Frame.Action==SpatialAudienceAction::Stage) {
			for (int I=0; I<3; ++I) if (!In.Float(Frame.Bounds.Minimum[I]) || !In.Float(Frame.Bounds.Maximum[I])) return {};
			std::uint8_t Empty{};
			std::uint16_t Count{};
			if (!In.Integer(Empty) || Empty>1 || !In.Integer(Count) || !Count || Count>MaximumSpatialStagingObjects ||
				In.Remaining()!=sizeof(std::uint32_t)*2*Count) return {};
			Frame.EmptyTerrain=Empty!=0;
			Frame.Objects.resize(Count);
			for (auto &Id:Frame.Objects) if (!ReadBinaryObjectId(In,Id)) return {};
		}
		return In.Complete() && ValidSpatialAudience(Frame) ? std::optional(std::move(Frame)) : std::nullopt;
	}
}
