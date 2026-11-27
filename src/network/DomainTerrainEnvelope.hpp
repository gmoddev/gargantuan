// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/network/BinaryCodec.hpp"
#include "gargantuan/network/TerrainSession.hpp"
#include <algorithm>

namespace gargantuan::network {
	inline constexpr std::uint32_t DomainTerrainMagic = 0x54445347; // GSDT
	inline constexpr std::size_t DomainTerrainHeaderBytes = 22;
	struct DomainTerrainFrame {
		ObjectId Region;
		std::uint64_t Grant{};
		terrain::GridSpec Grid;
		std::span<const std::byte> Payload;
	};
	inline std::vector<std::byte> EncodeDomainTerrain(const DomainTerrainFrame &Frame) {
		if (!Frame.Region.IsValid() || !Frame.Grid.IsValid() || Frame.Payload.empty() ||
			Frame.Payload.size() > TerrainFragmentHeaderBytes + MaximumTerrainFragmentBytes) return {};
		GameBinaryWriter Writer(DomainTerrainHeaderBytes + Frame.Payload.size());
		Writer.Integer(DomainTerrainMagic);
		Writer.Integer<std::uint8_t>(1);
		WriteBinaryObjectId(Writer, Frame.Region);
		Writer.Integer(Frame.Grant);
		Writer.Integer(Frame.Grid.SpacingShift);
		Writer.Append(Frame.Payload);
		return Writer.Succeeded() ? std::move(Writer.Bytes) : std::vector<std::byte>{};
	}
	inline std::optional<DomainTerrainFrame> DecodeDomainTerrain(std::span<const std::byte> Bytes) {
		if (Bytes.size() <= DomainTerrainHeaderBytes ||
			Bytes.size() > DomainTerrainHeaderBytes + TerrainFragmentHeaderBytes + MaximumTerrainFragmentBytes) return {};
		GameBinaryReader Reader(Bytes);
		std::uint32_t Magic{};
		std::uint8_t Version{};
		DomainTerrainFrame Frame;
		if (!Reader.Integer(Magic) || Magic != DomainTerrainMagic || !Reader.Integer(Version) || Version != 1 ||
			!ReadBinaryObjectId(Reader,Frame.Region) || !Frame.Region.IsValid() ||
			!Reader.Integer(Frame.Grant) || !Reader.Integer(Frame.Grid.SpacingShift) || !Frame.Grid.IsValid() ||
			!Reader.Take(Reader.Remaining(), Frame.Payload)) return {};
		return Frame;
	}
	// A window is ephemeral. Both ends derive its artifact namespace from the authenticated chart.
	inline ProjectId DomainTerrainProject(ObjectId Region, std::uint64_t Epoch) {
		GameBinaryWriter Writer(20);
		Writer.Integer(DomainTerrainMagic);
		WriteBinaryObjectId(Writer, Region);
		Writer.Integer(Epoch);
		const auto Hash = AssetContentId::Hash({reinterpret_cast<const std::uint8_t *>(Writer.Bytes.data()), Writer.Bytes.size()});
		ProjectId Project;
		std::copy_n(Hash.Bytes.begin(), Project.Bytes.size(), Project.Bytes.begin());
		return Project;
	}
}
