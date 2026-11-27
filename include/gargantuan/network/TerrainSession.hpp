// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/terrain/Persistence.hpp"

namespace gargantuan::network {
	inline constexpr std::size_t MaximumTerrainBootstrapBytes = 1024 * 1024;
	inline constexpr std::size_t MaximumTerrainFragmentBytes = 16 * 1024;
	inline constexpr std::size_t TerrainFragmentHeaderBytes = 28;
	inline constexpr std::size_t MaximumTerrainBootstrapPeers = 4;
	inline constexpr int TerrainWindowRadius = 1;
	inline constexpr std::size_t TerrainWindowChunkCount = 27;
	inline constexpr std::size_t MaximumTerrainWindowManifestBytes =
		terrain::TerrainManifestHeaderBytes + TerrainWindowChunkCount * terrain::TerrainManifestEntryBytes;

	// Pinned package directory. Only the server retains the immutable leaves.
	struct TerrainSessionData final {
		terrain::TerrainManifest Directory;
		AssetContentId Binding;
		std::vector<terrain::TerrainArtifact> Source;
	};

	struct TerrainSessionWindow final {
		struct ArtifactRange {
			terrain::TerrainArtifactId Id;
			std::size_t Offset;
			std::size_t Size;
			std::shared_ptr<const std::vector<std::uint8_t>> Source;
			bool Reused{};
		};
		ProjectId Project;
		terrain::GridSpec Grid;
		terrain::ChunkKey Center;
		AssetContentId ManifestDigest;
		std::vector<std::uint8_t> ManifestBytes;
		std::size_t Size = 0;
		std::vector<ArtifactRange> Artifacts;
	};

	// Client-side managed bootstrap pin. No server chunk coordinates or private sample bytes are required.
	[[nodiscard]] std::shared_ptr<TerrainSessionData> MakeTerrainReceiver(ProjectId Project, std::uint64_t PackageVersion,
		AssetContentId Manifest, terrain::GridSpec Grid);
	[[nodiscard]] std::shared_ptr<const TerrainSessionData> MakeTerrainSessionData(
		ProjectId Project, std::uint64_t PackageVersion, std::span<const terrain::TerrainArtifact> Artifacts,
		bool Sender);

	[[nodiscard]] std::optional<terrain::ChunkKey> GetTerrainSessionCenter(
		terrain::GridSpec Grid, terrain::WorldPoint Position);
	[[nodiscard]] TerrainSessionWindow SelectTerrainSessionWindow(
		const TerrainSessionData &Data, terrain::ChunkKey Center);
	[[nodiscard]] std::array<terrain::ChunkKey, TerrainWindowChunkCount> GetTerrainSessionKeys(terrain::ChunkKey Center);
	[[nodiscard]] TerrainSessionWindow CaptureTerrainSessionWindow(
		const terrain::Snapshot &Samples, ProjectId Project, terrain::ChunkKey Center, std::stop_token Stop);
	[[nodiscard]] TerrainSessionWindow ReadTerrainSessionWindow(const TerrainSessionData &Data, terrain::ChunkKey Center,
		AssetContentId ManifestDigest, std::span<const std::uint8_t> Manifest);
	// Complete leaf digests for the previous acknowledged window are available on both peers.
	void ReuseTerrainSessionLeaves(TerrainSessionWindow &Window, const TerrainSessionWindow &Previous);
	[[nodiscard]] std::span<const std::byte> GetTerrainSessionFragment(
		const TerrainSessionWindow &Window, std::size_t Offset);
	[[nodiscard]] std::unique_ptr<terrain::Field> LoadTerrainSessionField(
		TerrainSessionWindow &Window, std::span<const std::uint8_t> Bytes);
	[[nodiscard]] bool ContainsTerrainMotion(const TerrainSessionWindow &Window,
		terrain::WorldPoint Position, terrain::WorldPoint Extents);

	struct TerrainWindowIdentity {
		std::uint64_t SessionEpoch{};
		std::uint64_t Serial{};
		terrain::Revision FieldRevision{};
		AssetContentId ManifestDigest;
		bool operator==(const TerrainWindowIdentity &) const = default;
	};

	struct TerrainWindowOffer {
		TerrainWindowIdentity Identity;
		terrain::ChunkKey Center;
		std::uint32_t ManifestByteCount{};
		std::uint32_t LeafByteCount{};

		[[nodiscard]] std::size_t GetTransferBytes() const noexcept {
			return static_cast<std::size_t>(ManifestByteCount) + LeafByteCount;
		}
	};

	[[nodiscard]] bool IsTerrainWindowOffer(std::span<const std::byte> Bytes);
	[[nodiscard]] std::vector<std::byte> EncodeTerrainWindowOffer(TerrainWindowOffer Offer);
	[[nodiscard]] std::optional<TerrainWindowOffer> DecodeTerrainWindowOffer(std::span<const std::byte> Bytes);
	[[nodiscard]] bool IsTerrainWindowReady(std::span<const std::byte> Bytes);
	[[nodiscard]] std::vector<std::byte> EncodeTerrainWindowReady(const TerrainWindowIdentity &Identity);
	[[nodiscard]] std::optional<TerrainWindowIdentity> DecodeTerrainWindowReady(std::span<const std::byte> Bytes);

	struct TerrainFragmentView {
		std::uint64_t SessionEpoch;
		std::uint64_t Serial;
		std::uint32_t Offset;
		std::span<const std::byte> Bytes;
	};

	[[nodiscard]] bool IsTerrainFragment(std::span<const std::byte> Bytes);
	[[nodiscard]] std::vector<std::byte> EncodeTerrainFragment(TerrainFragmentView Fragment);
	[[nodiscard]] std::optional<TerrainFragmentView> DecodeTerrainFragment(std::span<const std::byte> Bytes);
}
