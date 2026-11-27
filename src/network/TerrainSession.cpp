// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/network/TerrainSession.hpp"
#include "gargantuan/network/BinaryCodec.hpp"
#include "gargantuan/terrain/ChunkCodec.hpp"
#include "terrain/ScenePersistence.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace gargantuan::network {
	namespace {
		constexpr std::uint32_t TerrainMagic = 0x424e5447; // GTNB
		constexpr std::uint32_t TerrainWindowMagic = 0x574e5447; // GTNW
		constexpr std::uint32_t TerrainReadyMagic = 0x524e5447; // GTNR
		constexpr std::uint16_t TerrainStreamVersion = 4;
		constexpr int TerrainMotionHaloSamples = 4;

		bool HasMagic(std::span<const std::byte> Bytes, std::uint32_t Expected) {
			GameBinaryReader Input(Bytes);
			std::uint32_t Magic;
			return Input.Integer(Magic) && Magic == Expected;
		}

		bool ValidCenter(terrain::ChunkKey Center) {
			constexpr auto Min = terrain::FloorChunk(-terrain::MaximumSampleCoordinate) + TerrainWindowRadius;
			constexpr auto Max = terrain::FloorChunk(terrain::MaximumSampleCoordinate) - TerrainWindowRadius;
			const auto InRange = [](auto Value) { return Value >= Min && Value <= Max; };
			return InRange(Center.X) && InRange(Center.Y) && InRange(Center.Z);
		}

		bool ValidFragment(TerrainFragmentView Fragment) {
			return Fragment.SessionEpoch && Fragment.Serial && !Fragment.Bytes.empty() &&
				Fragment.Bytes.size() <= MaximumTerrainFragmentBytes &&
				Fragment.Offset <= MaximumTerrainBootstrapBytes - Fragment.Bytes.size();
		}

		bool ValidIdentity(const TerrainWindowIdentity &Identity) {
			return Identity.SessionEpoch && Identity.Serial && Identity.ManifestDigest.IsValid();
		}

		void WriteIdentity(GameBinaryWriter &Output, std::uint32_t Magic, const TerrainWindowIdentity &Identity) {
			Output.Integer(Magic);
			Output.Integer(TerrainStreamVersion);
			Output.Integer(std::uint16_t{});
			Output.Integer(Identity.SessionEpoch);
			Output.Integer(Identity.Serial);
			Output.Integer(Identity.FieldRevision);
			Output.Append(std::as_bytes(std::span(Identity.ManifestDigest.Bytes)));
		}

		bool ReadIdentity(GameBinaryReader &Input, std::uint32_t Expected, TerrainWindowIdentity &Identity) {
			std::uint32_t Magic;
			std::uint16_t Version, Reserved;
			std::span<const std::byte> Digest;
			if (!Input.Integer(Magic) || Magic != Expected || !Input.Integer(Version) ||
				Version != TerrainStreamVersion || !Input.Integer(Reserved) || Reserved ||
				!Input.Integer(Identity.SessionEpoch) || !Input.Integer(Identity.Serial) ||
				!Input.Integer(Identity.FieldRevision) || !Input.Take(Identity.ManifestDigest.Bytes.size(), Digest))
				return false;
			std::ranges::transform(Digest, Identity.ManifestDigest.Bytes.begin(),
				[](std::byte Value) { return std::to_integer<std::uint8_t>(Value); });
			return ValidIdentity(Identity);
		}

		bool ValidOffer(const TerrainWindowOffer &Offer) {
			return ValidIdentity(Offer.Identity) && ValidCenter(Offer.Center) &&
				(!Offer.ManifestByteCount || Offer.ManifestByteCount >= terrain::TerrainManifestHeaderBytes) &&
				Offer.ManifestByteCount <= MaximumTerrainWindowManifestBytes &&
				Offer.LeafByteCount <= MaximumTerrainBootstrapBytes - Offer.ManifestByteCount;
		}

		void AddLeaf(TerrainSessionWindow &Window, const terrain::TerrainChunkRef &Ref,
			std::shared_ptr<const std::vector<std::uint8_t>> Bytes) {
			if (Ref.ByteCount > MaximumTerrainBootstrapBytes - Window.Size)
				throw std::length_error("[Terrain:Network] Terrain window exceeds 1 MiB");
			Window.Artifacts.push_back({{terrain::TerrainArtifactPurpose::Chunk, AssetContentId{Ref.Digest.Bytes}},
				Window.Size, Ref.ByteCount, std::move(Bytes)});
			Window.Size += Ref.ByteCount;
		}

		void SetManifest(TerrainSessionWindow &Window, const terrain::TerrainManifest &Manifest) {
			auto Encoded = terrain::EncodeTerrainManifest(Manifest);
			if (!Encoded) throw terrain::TerrainPersistenceFailure(terrain::TerrainPersistenceError::CorruptArtifact);
			Window.ManifestDigest = AssetContentId::Hash(*Encoded);
			Window.ManifestBytes = std::move(*Encoded);
		}
	}

	std::shared_ptr<const TerrainSessionData> MakeTerrainSessionData(ProjectId Project, std::uint64_t PackageVersion,
		std::span<const terrain::TerrainArtifact> Artifacts, bool Sender) {
		if (Artifacts.empty()) return {};
		if (!Project.IsValid() || !PackageVersion || Artifacts.size() > terrain::MaximumTerrainManifestEntries + 1 ||
			!std::ranges::is_sorted(Artifacts, {}, &terrain::TerrainArtifact::Id) ||
			std::ranges::adjacent_find(Artifacts, {}, &terrain::TerrainArtifact::Id) != Artifacts.end())
			throw std::invalid_argument("[Terrain:Network] Invalid packaged terrain directory");

		const auto &Manifest = Artifacts.back();
		if (Manifest.Id.Purpose != terrain::TerrainArtifactPurpose::Manifest || !Manifest.Bytes ||
			Manifest.Bytes->size() > terrain::MaximumTerrainManifestBytes ||
			AssetContentId::Hash(*Manifest.Bytes) != Manifest.Id.Digest)
			throw std::invalid_argument("[Terrain:Network] Missing or invalid terrain manifest");
		auto Directory = terrain::DecodeTerrainManifest(*Manifest.Bytes, Project, terrain::GetTerrainDatasetId(Project));
		if (!Directory || ((Sender || Artifacts.size() > 1) && Directory->Chunks.size() + 1 != Artifacts.size()))
			throw std::invalid_argument("[Terrain:Network] Invalid packaged terrain closure");
		if (Sender || Artifacts.size() > 1)
			for (const auto &Ref : Directory->Chunks) {
				const auto *Artifact = terrain::FindTerrainArtifact(Artifacts, {terrain::TerrainArtifactPurpose::Chunk,
					AssetContentId{Ref.Digest.Bytes}});
				if (!Artifact || !Artifact->Bytes || Artifact->Bytes->size() != Ref.ByteCount)
					throw std::invalid_argument("[Terrain:Network] Missing packaged terrain leaf");
			}

		auto Data = std::make_shared<TerrainSessionData>();
		GameBinaryWriter Binding(58);
		Binding.Append(std::as_bytes(std::span(Project.Bytes)));
		Binding.Integer(PackageVersion);
		Binding.Append(std::as_bytes(std::span(Manifest.Id.Digest.Bytes)));
		Binding.Integer(TerrainStreamVersion);
		Data->Binding = AssetContentId::Hash({reinterpret_cast<const std::uint8_t *>(Binding.Bytes.data()), Binding.Bytes.size()});
		Data->Directory = std::move(*Directory);
		if (Sender) Data->Source.assign(Artifacts.begin(), Artifacts.end() - 1);
		return Data;
	}

	std::optional<terrain::ChunkKey> GetTerrainSessionCenter(terrain::GridSpec Grid, terrain::WorldPoint Position) {
		if (!Grid.IsValid()) return std::nullopt;
		const auto Spacing = Grid.GetSpacing();
		const auto Coordinate = [Spacing](double Value) -> std::optional<std::int64_t> {
			const auto Sample = std::floor(Value / Spacing);
			if (!std::isfinite(Sample) || std::abs(Sample) > terrain::MaximumSampleCoordinate) return std::nullopt;
			return terrain::FloorChunk(static_cast<std::int64_t>(Sample));
		};
		const auto X = Coordinate(Position.X), Y = Coordinate(Position.Y), Z = Coordinate(Position.Z);
		if (!X || !Y || !Z) return std::nullopt;
		const terrain::ChunkKey Center{*X, *Y, *Z};
		return ValidCenter(Center) ? std::optional{Center} : std::nullopt;
	}

	TerrainSessionWindow SelectTerrainSessionWindow(const TerrainSessionData &Data, terrain::ChunkKey Center) {
		const auto &Directory = Data.Directory;
		terrain::TerrainManifest Manifest{Directory.Project, Directory.Dataset, Directory.Grid, {}};
		TerrainSessionWindow Window{.Project = Directory.Project, .Grid = Directory.Grid, .Center = Center};
		// The sorted directory provides direct lookups for the bounded cubic halo.
		for (const auto Key : GetTerrainSessionKeys(Center)) {
			const auto Ref = std::ranges::lower_bound(Directory.Chunks, Key, {}, &terrain::TerrainChunkRef::Key);
			if (Ref == Directory.Chunks.end() || Ref->Key != Key) continue;
			const auto *Artifact = terrain::FindTerrainArtifact(Data.Source,
				{terrain::TerrainArtifactPurpose::Chunk, AssetContentId{Ref->Digest.Bytes}});
			AddLeaf(Window, *Ref, Artifact ? Artifact->Bytes : nullptr);
			Manifest.Chunks.push_back(*Ref);
		}
		SetManifest(Window, Manifest);
		return Window;
	}

	std::array<terrain::ChunkKey, TerrainWindowChunkCount> GetTerrainSessionKeys(terrain::ChunkKey Center) {
		if (!ValidCenter(Center)) throw std::invalid_argument("[Terrain:Network] Invalid terrain window center");
		std::array<terrain::ChunkKey, TerrainWindowChunkCount> Keys;
		std::size_t At = 0;
		for (int X = -TerrainWindowRadius; X <= TerrainWindowRadius; ++X)
			for (int Y = -TerrainWindowRadius; Y <= TerrainWindowRadius; ++Y)
				for (int Z = -TerrainWindowRadius; Z <= TerrainWindowRadius; ++Z)
					Keys[At++] = {Center.X + X, Center.Y + Y, Center.Z + Z};
		return Keys;
	}

	TerrainSessionWindow CaptureTerrainSessionWindow(
		const terrain::Snapshot &Samples, ProjectId Project, terrain::ChunkKey Center, std::stop_token Stop) {
		terrain::TerrainManifest Manifest{Project, terrain::GetTerrainDatasetId(Project), Samples.GetGrid(), {}};
		TerrainSessionWindow Window{.Project = Project, .Grid = Samples.GetGrid(), .Center = Center};
		for (const auto Key : GetTerrainSessionKeys(Center)) {
			const auto Materialized = Samples.IsMaterialized(Key);
			if (Stop.stop_requested() || !Materialized)
				throw std::runtime_error("[Terrain:Network] Unable to capture terrain window");
			if (!*Materialized) continue;
			auto Encoded = terrain::EncodeChunk(Samples, Key, Stop);
			if (!Encoded) throw std::runtime_error("[Terrain:Network] Unable to encode terrain leaf");
			const auto Id = AssetContentId::Hash(*Encoded);
			const terrain::TerrainChunkRef Ref{Key, {Id.Bytes}, static_cast<std::uint32_t>(Encoded->size())};
			AddLeaf(Window, Ref, std::make_shared<const std::vector<std::uint8_t>>(std::move(*Encoded)));
			Manifest.Chunks.push_back(Ref);
		}
		SetManifest(Window, Manifest);
		if (Window.ManifestBytes.size() > MaximumTerrainBootstrapBytes - Window.Size)
			throw std::length_error("[Terrain:Network] Terrain transfer exceeds 1 MiB");
		return Window;
	}

	TerrainSessionWindow ReadTerrainSessionWindow(const TerrainSessionData &Data, terrain::ChunkKey Center,
		AssetContentId ManifestDigest, std::span<const std::uint8_t> Bytes) {
		const auto Keys = GetTerrainSessionKeys(Center);
		const auto &Directory = Data.Directory;
		if (Bytes.size() > MaximumTerrainWindowManifestBytes || AssetContentId::Hash(Bytes) != ManifestDigest)
			throw std::invalid_argument("[Terrain:Network] Invalid terrain window digest");
		const auto Manifest = terrain::DecodeTerrainManifest(Bytes, Directory.Project, Directory.Dataset);
		if (!Manifest || Manifest->Grid != Directory.Grid)
			throw std::invalid_argument("[Terrain:Network] Invalid terrain window profile");
		TerrainSessionWindow Window{.Project = Directory.Project, .Grid = Directory.Grid, .Center = Center,
			.ManifestDigest = ManifestDigest, .ManifestBytes = {Bytes.begin(), Bytes.end()}};
		for (const auto &Ref : Manifest->Chunks) {
			if (!std::ranges::binary_search(Keys, Ref.Key))
				throw std::invalid_argument("[Terrain:Network] Terrain leaf is outside its window");
			AddLeaf(Window, Ref, nullptr);
		}
		return Window;
	}

	void ReuseTerrainSessionLeaves(TerrainSessionWindow &Window, const TerrainSessionWindow &Previous) {
		Window.Size = 0;
		for (auto &Range : Window.Artifacts) {
			const auto Prior = std::ranges::find(Previous.Artifacts, Range.Id, &TerrainSessionWindow::ArtifactRange::Id);
			Range.Offset = Window.Size;
			Range.Reused = Prior != Previous.Artifacts.end() && (Range.Source || Prior->Source) && Prior->Size == Range.Size;
			if (Range.Reused) {
				if (!Range.Source) Range.Source = Prior->Source;
			} else {
				Window.Size += Range.Size;
			}
		}
	}

	std::span<const std::byte> GetTerrainSessionFragment(const TerrainSessionWindow &Window, std::size_t Offset) {
		for (const auto &Range : Window.Artifacts)
			if (!Range.Reused && Range.Source && Offset >= Range.Offset && Offset - Range.Offset < Range.Size) {
				const auto Bytes = std::as_bytes(std::span(*Range.Source)).subspan(Offset - Range.Offset);
				return Bytes.first(std::min(Bytes.size(), MaximumTerrainFragmentBytes));
			}
		return {};
	}

	std::unique_ptr<terrain::Field> LoadTerrainSessionField(TerrainSessionWindow &Window,
		std::span<const std::uint8_t> Bytes) {
		if (Bytes.size() != Window.Size) throw std::invalid_argument("[Terrain:Network] Incomplete terrain bootstrap");
		for (auto &Range : Window.Artifacts) {
			if (Range.Reused) continue;
			const auto Leaf = Bytes.subspan(Range.Offset, Range.Size);
			Range.Source = std::make_shared<const std::vector<std::uint8_t>>(Leaf.begin(), Leaf.end());
		}
		return terrain::LoadSceneTerrain(Window.Project, Window.ManifestDigest,
			[&](const terrain::TerrainArtifactId &Id, std::size_t Maximum) ->
				std::expected<std::vector<std::uint8_t>, terrain::TerrainPersistenceError> {
				if (Id == terrain::TerrainArtifactId{terrain::TerrainArtifactPurpose::Manifest, Window.ManifestDigest}) {
					if (Window.ManifestBytes.size() > Maximum)
						return std::unexpected(terrain::TerrainPersistenceError::LimitExceeded);
					return Window.ManifestBytes;
				}
				const auto At = std::ranges::find(Window.Artifacts, Id, &TerrainSessionWindow::ArtifactRange::Id);
				if (At == Window.Artifacts.end())
					return std::unexpected(terrain::TerrainPersistenceError::MissingArtifact);
				if (!At->Source || At->Source->size() != At->Size || At->Size > Maximum)
					return std::unexpected(terrain::TerrainPersistenceError::LimitExceeded);
				return *At->Source;
			});
	}

	bool ContainsTerrainMotion(const TerrainSessionWindow &Window, terrain::WorldPoint Position,
		terrain::WorldPoint Extents) {
		if (!Window.Grid.IsValid() || !ValidCenter(Window.Center)) return false;
		const auto Spacing = Window.Grid.GetSpacing();
		const auto Fits = [Spacing](std::int64_t Center, double Value, double Extent) {
			// Leave the outer samples for contour gradients and conditioned collision.
			const auto Lower = ((Center - TerrainWindowRadius) * terrain::ChunkSide + TerrainMotionHaloSamples) * Spacing;
			const auto Upper = ((Center + TerrainWindowRadius + 1) * terrain::ChunkSide - TerrainMotionHaloSamples) * Spacing;
			// The entire actor or query must fit. A window change cannot admit an
			// actor larger than the common coverage of the two windows.
			return std::isfinite(Value) && std::isfinite(Extent) && Extent >= 0 &&
				Value - Extent >= Lower && Value + Extent < Upper;
		};
		return Fits(Window.Center.X, Position.X, Extents.X) && Fits(Window.Center.Y, Position.Y, Extents.Y) &&
			Fits(Window.Center.Z, Position.Z, Extents.Z);
	}

	bool IsTerrainWindowOffer(std::span<const std::byte> Bytes) {
		return HasMagic(Bytes, TerrainWindowMagic);
	}

	std::vector<std::byte> EncodeTerrainWindowOffer(TerrainWindowOffer Offer) {
		if (!ValidOffer(Offer)) return {};
		GameBinaryWriter Output(96);
		WriteIdentity(Output, TerrainWindowMagic, Offer.Identity);
		Output.Integer(Offer.Center.X);
		Output.Integer(Offer.Center.Y);
		Output.Integer(Offer.Center.Z);
		Output.Integer(Offer.ManifestByteCount);
		Output.Integer(Offer.LeafByteCount);
		return std::move(Output.Bytes);
	}

	std::optional<TerrainWindowOffer> DecodeTerrainWindowOffer(std::span<const std::byte> Bytes) {
		GameBinaryReader Input(Bytes);
		TerrainWindowOffer Offer{};
		if (!ReadIdentity(Input, TerrainWindowMagic, Offer.Identity) || !Input.Integer(Offer.Center.X) ||
			!Input.Integer(Offer.Center.Y) || !Input.Integer(Offer.Center.Z) || !Input.Integer(Offer.ManifestByteCount) ||
			!Input.Integer(Offer.LeafByteCount) || !Input.Complete() || !ValidOffer(Offer)) return std::nullopt;
		return Offer;
	}

	bool IsTerrainWindowReady(std::span<const std::byte> Bytes) {
		return HasMagic(Bytes, TerrainReadyMagic);
	}

	std::vector<std::byte> EncodeTerrainWindowReady(const TerrainWindowIdentity &Identity) {
		if (!ValidIdentity(Identity)) return {};
		GameBinaryWriter Output(64);
		WriteIdentity(Output, TerrainReadyMagic, Identity);
		return std::move(Output.Bytes);
	}

	std::optional<TerrainWindowIdentity> DecodeTerrainWindowReady(std::span<const std::byte> Bytes) {
		GameBinaryReader Input(Bytes);
		TerrainWindowIdentity Identity;
		if (!ReadIdentity(Input, TerrainReadyMagic, Identity) || !Input.Complete()) return std::nullopt;
		return Identity;
	}

	bool IsTerrainFragment(std::span<const std::byte> Bytes) {
		return HasMagic(Bytes, TerrainMagic);
	}

	std::vector<std::byte> EncodeTerrainFragment(TerrainFragmentView Fragment) {
		if (!ValidFragment(Fragment)) return {};
		GameBinaryWriter Output(TerrainFragmentHeaderBytes + MaximumTerrainFragmentBytes);
		Output.Integer(TerrainMagic);
		Output.Integer(TerrainStreamVersion);
		Output.Integer(static_cast<std::uint16_t>(Fragment.Bytes.size()));
		Output.Integer(Fragment.SessionEpoch);
		Output.Integer(Fragment.Serial);
		Output.Integer(Fragment.Offset);
		Output.Append(Fragment.Bytes);
		return std::move(Output.Bytes);
	}

	std::optional<TerrainFragmentView> DecodeTerrainFragment(std::span<const std::byte> Bytes) {
		if (Bytes.size() > TerrainFragmentHeaderBytes + MaximumTerrainFragmentBytes) return std::nullopt;
		GameBinaryReader Input(Bytes);
		std::uint32_t Magic;
		std::uint16_t Version, Length;
		TerrainFragmentView Fragment{};
		if (!Input.Integer(Magic) || Magic != TerrainMagic || !Input.Integer(Version) ||
			Version != TerrainStreamVersion || !Input.Integer(Length) || !Input.Integer(Fragment.SessionEpoch) ||
			!Input.Integer(Fragment.Serial) ||
			!Input.Integer(Fragment.Offset) || !Input.Take(Length, Fragment.Bytes) || !Input.Complete() ||
			!ValidFragment(Fragment)) return std::nullopt;
		return Fragment;
	}
}
