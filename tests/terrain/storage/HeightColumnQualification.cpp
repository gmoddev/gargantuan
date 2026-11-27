// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "HeightColumnPage.hpp"

#include "gargantuan/terrain/ChunkCodec.hpp"
#include "gargantuan/terrain/Surface.hpp"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
	using namespace gargantuan::terrain;
	using namespace gargantuan::terrain::experiment;
	using Clock = std::chrono::steady_clock;
	using PageMap = std::map<ChunkKey, Page>;
	constexpr std::size_t DirectoryBytes = sizeof(ChunkKey) + sizeof(field_detail::Entry);
	constexpr std::array<ChunkKey, 2> NeighborKeys{{{-1, 0, -1}, {0, 0, -1}}};
	constexpr std::array<ChunkKey, 1> WorldEdge{{{FloorChunk(-MaximumSampleCoordinate), 0, -1}}};
	constexpr std::size_t ReadCount = 200'000;

	enum class TerrainStyle { Flat, Rolling, Cliff, Layered, Noisy, MaterialNoise, Constant, Cavern };

	void Require(bool Value, std::string_view Message) {
		if (!Value) throw std::runtime_error(std::string(Message));
	}

	double ElapsedNs(Clock::time_point Start) {
		return std::chrono::duration<double, std::nano>(Clock::now() - Start).count();
	}

	SamplePoint World(ChunkKey Key, int X, int Y, int Z) {
		return {Key.X * ChunkSide + X, Key.Y * ChunkSide + Y, Key.Z * ChunkSide + Z};
	}

	std::uint32_t Noise(std::uint32_t Seed) {
		Seed ^= Seed >> 16;
		Seed *= 0x7feb352du;
		Seed ^= Seed >> 15;
		Seed *= 0x846ca68bu;
		return Seed ^ (Seed >> 16);
	}

	int Wave(std::int64_t Value, int Period) {
		const auto Modulo = static_cast<int>((Value % Period + Period) % Period);
		return std::min(Modulo, Period - Modulo);
	}

	Sample AuthoredSample(SamplePoint Point, TerrainStyle Style) {
		constexpr int Scale = 8192;
		if (Style == TerrainStyle::Constant) return {-SaturatedDensity, 9};
		int Height = 17 * Scale + 953;
		if (Style == TerrainStyle::Cliff) Height = (Point.X < 0 ? 8 : 23) * Scale + 15;
		if (Style == TerrainStyle::Rolling || Style == TerrainStyle::Layered || Style == TerrainStyle::Noisy ||
			Style == TerrainStyle::MaterialNoise)
			Height = 8 * Scale + Wave(Point.X, 31) * 4700 + Wave(Point.Z, 27) * 2600;
		const auto Unbounded = Point.Y * Scale - Height;
		const auto Key = static_cast<std::uint32_t>(Point.X) * 73'856'093u ^
			static_cast<std::uint32_t>(Point.Y) * 19'349'663u ^ static_cast<std::uint32_t>(Point.Z) * 83'492'791u;
		const auto CoordinateNoise = Noise(Key);
		int Density = static_cast<int>(std::clamp<std::int64_t>(Unbounded, -SaturatedDensity, SaturatedDensity));
		if (Style == TerrainStyle::Noisy && std::abs(Unbounded) < SaturatedDensity) {
			const int NoisyDensity = Density + static_cast<int>(CoordinateNoise % 2001) - 1000;
			Density = Density < 0 ? std::clamp(NoisyDensity, -static_cast<int>(SaturatedDensity), -1)
				: std::clamp(NoisyDensity, 0, static_cast<int>(SaturatedDensity));
		}
		MaterialId Material = 0;
		if (Density < 0) {
			Material = 9;
			if (Style == TerrainStyle::Layered) {
				const auto Depth = -Unbounded / Scale;
				constexpr std::array<MaterialId, 4> Materials{11, 9, 3, 0xfedcba98u};
				Material = Materials[static_cast<std::size_t>(std::min<std::int64_t>(Depth / 2, 3))];
			}
			if (Style == TerrainStyle::MaterialNoise) Material = (CoordinateNoise | 1u);
		}
		return {static_cast<std::int16_t>(Density), Material};
	}

	void CommitWrites(Field &Store, ChunkKey Key, std::span<const SampleWrite> Writes) {
		auto Snapshot = Store.Capture(std::span(&Key, 1)).value();
		auto Candidate = Field::Prepare(std::move(Snapshot), Writes).value();
		Require(Store.Commit(std::move(Candidate)).has_value(), "commit exact real Field writes");
	}

	void CommitBrush(Field &Store, SphereBrush Brush) {
		const auto Bounds = GetBrushBounds(Store.GetGrid(), Brush).value();
		const auto Inputs = GetRegionChunks(Bounds).value();
		auto Base = Store.Capture(Inputs).value();
		auto Candidate = Field::Prepare(std::move(Base), Brush).value();
		Require(Store.Commit(std::move(Candidate)).has_value(), "commit actual field sphere brush");
	}

	void Author(Field &Store, TerrainStyle Style, std::span<const ChunkKey> Keys) {
		for (const auto Key : Keys) {
			std::vector<SampleWrite> Writes;
			Writes.reserve(PageSamples);
			for (int Z = 0; Z < ChunkSide; ++Z) {
				for (int Y = 0; Y < ChunkSide; ++Y) {
					for (int X = 0; X < ChunkSide; ++X) {
						const auto P = World(Key, X, Y, Z);
						if (!IsValid(P)) continue;
						const auto Value = AuthoredSample(P, Style);
						if (Value != AirSample) Writes.push_back({P, Value});
					}
				}
			}
			CommitWrites(Store, Key, Writes);
		}
		if (Style == TerrainStyle::Cavern) {
			CommitBrush(Store, {{-12.25, 8.2, -11.4}, 3.2, BrushMode::Subtract, 0});
			CommitBrush(Store, {{17.25, 25.7, -11.4}, 2.7, BrushMode::Add, 13});
		}
	}

	Sample ReadPageField(const PageMap &Pages, SamplePoint P) {
		const auto Key = GetChunkKey(P);
		const auto Found = Pages.find(Key);
		if (Found == Pages.end()) return AirSample;
		const auto Base = World(Key, 0, 0, 0);
		return Found->second.Read(static_cast<int>(P.X - Base.X), static_cast<int>(P.Y - Base.Y),
			static_cast<int>(P.Z - Base.Z));
	}

	struct PackedSummary {
		std::size_t Bytes{}, ColumnTiles{}, ColumnBricks{}, VolumeBricks{}, Residuals{};
	};

	PackedSummary Describe(const PageMap &Pages) {
		PackedSummary Result;
		for (const auto &[Key, Value] : Pages) {
			Result.Bytes += DirectoryBytes + Value.Bytes();
			if (Value.Height) {
				++Result.ColumnTiles;
				Result.Residuals += Value.Height->GetResidualCount();
				continue;
			}
			for (const auto &Brick : Value.Bricks->Bricks) {
				if (!Brick) continue;
				if (Brick->GetDense()) ++Result.VolumeBricks;
				if (const auto *Height = Brick->GetHeight()) {
					++Result.ColumnBricks;
					Result.Residuals += Height->GetResidualCount();
				}
			}
		}
		return Result;
	}

	void VerifyRoundTrip(const Field &Source, const Snapshot &Captured, const PageMap &Pages,
		const std::map<ChunkKey, std::vector<std::uint8_t>> &Artifacts) {
		Field Restored(Source.GetGrid());
		for (const auto &[Key, Value] : Pages) {
			std::vector<SampleWrite> Writes;
			Writes.reserve(PageSamples);
			for (int Z = 0; Z < ChunkSide; ++Z) {
				for (int Y = 0; Y < ChunkSide; ++Y) {
					for (int X = 0; X < ChunkSide; ++X) {
						const auto P = World(Key, X, Y, Z);
						const auto Decoded = Value.Read(X, Y, Z);
						if (!IsValid(P)) {
							Require(Decoded == AirSample, "world-boundary padding is canonical air");
							continue;
						}
						Require(Captured.Read(P).value() == Decoded, "exact signed density and uint32 material");
						if (Decoded != AirSample) Writes.push_back({P, Decoded});
					}
				}
			}
			CommitWrites(Restored, Key, Writes);
			const auto Snapshot = Restored.Capture(std::span(&Key, 1)).value();
			Require(EncodeChunk(Snapshot, Key).value() == Artifacts.at(Key), "reconstructed page has identical canonical GTCH bytes");
		}
		Require(Restored.GetStats().OwnedBytes == Source.GetStats().OwnedBytes,
			"reconstruction returns to equivalent production brick storage");
	}

	template <class Read>
	void VerifyMeshes(const Field &Source, std::span<const ChunkKey> Keys, Read ReadCandidate) {
		constexpr int HaloSide = ChunkSide + 3;
		std::vector<Sample> Ordinary(HaloSide * HaloSide * HaloSide), Candidate(Ordinary.size());
		for (const auto Key : Keys) {
			for (int Z = -1; Z <= ChunkSide + 1; ++Z) {
				for (int Y = -1; Y <= ChunkSide + 1; ++Y) {
					for (int X = -1; X <= ChunkSide + 1; ++X) {
						const auto P = World(Key, X, Y, Z);
						const auto Index = At(HaloSide, X + 1, Y + 1, Z + 1);
						Ordinary[Index] = IsValid(P) ? Source.Read(P).value() : AirSample;
						Candidate[Index] = IsValid(P) ? ReadCandidate(P) : AirSample;
					}
				}
			}
			Require(Ordinary == Candidate, "halo lattice tuples are byte-semantic equals across owners");
			const auto Spacing = Source.GetGrid().GetSpacing();
			SurfaceMesh Current, Rebuilt;
			const auto A = ExtractSurface({Ordinary, ChunkSide, ChunkSide, ChunkSide, Spacing}, {}, Current);
			const auto B = ExtractSurface({Candidate, ChunkSide, ChunkSide, ChunkSide, Spacing}, {}, Rebuilt);
			Require(A == SurfaceStatus::Success && B == A && Current.Vertices == Rebuilt.Vertices &&
				Current.Indices == Rebuilt.Indices, "real MC33 vertices, normals, materials and connectivity are exact");
			CollisionSurfaceMesh OriginalCollision, CandidateCollision;
			const auto Native = BuildCollisionSurface(Current, {}, OriginalCollision);
			const auto Experimental = BuildCollisionSurface(Rebuilt, {}, CandidateCollision);
			Require(Native == Experimental && OriginalCollision.Positions == CandidateCollision.Positions &&
				OriginalCollision.Indices == CandidateCollision.Indices &&
				OriginalCollision.TriangleMaterials == CandidateCollision.TriangleMaterials &&
				OriginalCollision.MaximumDisplacement == CandidateCollision.MaximumDisplacement,
				"collision conditioning receives identical input and produces identical output or admission failure");
		}
	}

	std::vector<SamplePoint> ReadOrder(std::span<const ChunkKey> Keys) {
		std::vector<SamplePoint> Points;
		Points.reserve(ReadCount);
		for (std::uint32_t Index = 0; Index < ReadCount; ++Index) {
			const auto Bits = Noise(Index + 737u);
			const auto Key = Keys[(Bits >> 16) % Keys.size()];
			const auto P = World(Key, Bits & 31, (Bits >> 5) & 31, (Bits >> 10) & 31);
			if (IsValid(P)) Points.push_back(P);
		}
		return Points;
	}

	template<class Reader> double ReadTime(std::span<const SamplePoint> Points, Reader Read, std::uint64_t &Sum) {
		std::array<double, 3> Runs;
		for (auto &Ns : Runs) {
			std::uint64_t Observed = 0;
			const auto Start = Clock::now();
			for (const auto Point : Points) {
				const auto Value = Read(Point);
				Observed += static_cast<std::uint16_t>(Value.Density) + static_cast<std::uint64_t>(Value.Material);
			}
			Ns = ElapsedNs(Start) / Points.size();
			Sum = Observed;
		}
		std::ranges::sort(Runs);
		return Runs[1];
	}

	void Compare(std::string_view Name, TerrainStyle Style, std::span<const ChunkKey> Keys, GridSpec Grid = {}) {
		Field Source(Grid);
		const auto BaselineStarted = Clock::now();
		Author(Source, Style, Keys);
		const auto BaselineAuthorNs = ElapsedNs(BaselineStarted);
		const auto Stored = Source.GetMaterializedChunkKeys(MaximumCapturedChunks).value();
		const auto Captured = Source.Capture(Stored).value();
		std::map<ChunkKey, std::vector<std::uint8_t>> GTCH;
		std::size_t WireBytes = 0;
		std::array<double, 3> WireTimes;
		for (auto &Ns : WireTimes) {
			const auto Start = Clock::now();
			for (const auto Key : Stored) GTCH.insert_or_assign(Key, EncodeChunk(Captured, Key).value());
			Ns = ElapsedNs(Start) / Stored.size();
		}
		std::ranges::sort(WireTimes);
		for (const auto &[Key, Bytes] : GTCH) WireBytes += Bytes.size();
		const auto Points = ReadOrder(Stored);
		std::uint64_t Expected{};
		const auto FieldNs = ReadTime(Points, [&](SamplePoint P) { return Source.Read(P).value(); }, Expected);
		for (const auto Mode : {Strategy::WholeTile, Strategy::SelectiveBrick}) {
			PageMap Pages;
			std::array<double, 3> PackTimes;
			for (auto &Ns : PackTimes) {
				const auto Start = Clock::now();
				for (const auto Key : Stored) Pages.insert_or_assign(Key, PackPage(Captured, Key, Mode));
				Ns = ElapsedNs(Start) / Stored.size();
			}
			std::ranges::sort(PackTimes);
			const auto Stats = Describe(Pages);
			std::uint64_t Observed{};
			const auto ReadNs = ReadTime(Points, [&](SamplePoint P) { return ReadPageField(Pages, P); }, Observed);
			Require(Expected == Observed, "random reads have equal checksum");
			VerifyRoundTrip(Source, Captured, Pages, GTCH);
			VerifyMeshes(Source, Stored, [&](SamplePoint P) { return ReadPageField(Pages, P); });
			std::cout << Name << ',' << (Mode == Strategy::WholeTile ? "whole" : "selective") << ',' << Stored.size()
				<< ',' << Source.GetStats().OwnedBytes << ',' << WireBytes << ',' << Stats.Bytes << ',' << Stats.ColumnTiles
				<< ',' << Stats.ColumnBricks << ',' << Stats.VolumeBricks << ',' << Stats.Residuals << ',' << FieldNs << ','
				<< ReadNs << ',' << WireTimes[1] / 1000 << ',' << PackTimes[1] / 1000 << '\n';
		}

		Field Adaptive(Grid, {.BrickStorage = BrickStorageMode::LosslessColumns});
		const auto AdaptiveStarted = Clock::now();
		Author(Adaptive, Style, Keys);
		const auto AdaptiveAuthorNs = ElapsedNs(AdaptiveStarted);
		const auto AdaptiveCapture = Adaptive.Capture(Stored).value();
		Require(Adaptive.GetStats().OwnedBytes <= Source.GetStats().OwnedBytes &&
			Adaptive.GetMaterializedChunkKeys(MaximumCapturedChunks).value() == Stored,
			"production adaptive storage retains page identity and fits the same byte budget");
		for (const auto Key : Stored)
			Require(EncodeChunk(AdaptiveCapture, Key).value() == GTCH.at(Key), "production adaptive GTCH equals baseline");
		VerifyMeshes(Source, Stored, [&](SamplePoint P) { return Adaptive.Read(P).value(); });
		std::uint64_t Observed = 0;
		const auto AdaptiveReadNs = ReadTime(Points, [&](SamplePoint P) { return Adaptive.Read(P).value(); }, Observed);
		Require(Expected == Observed, "production adaptive random reads match exact sample tuples");
		std::cout << "[Terrain:Columns] native " << Name << " dense_bytes=" << Source.GetStats().OwnedBytes
			<< " adaptive_bytes=" << Adaptive.GetStats().OwnedBytes << " dense_read_ns=" << FieldNs
			<< " adaptive_read_ns=" << AdaptiveReadNs << " dense_author_ms=" << BaselineAuthorNs / 1e6
			<< " adaptive_author_ms=" << AdaptiveAuthorNs / 1e6 << '\n';
	}

	void VerifyPromotionAndRepair() {
		const auto Key = NeighborKeys.front();
		Field Source;
		Author(Source, TerrainStyle::Flat, std::span(&Key, 1));
		const auto Before = Source.Capture(std::span(&Key, 1)).value();
		const auto Tile = PackPage(Before, Key, Strategy::WholeTile);
		const auto Bricks = PackPage(Before, Key, Strategy::SelectiveBrick);
		constexpr int X = 10, Y = 12, Z = 12;
		constexpr std::array Changed{BrickAt(X, Y, Z)};
		Require(Tile.Height && Bricks.Bricks->Bricks[Changed.front()]->GetHeight(), "ordinary surface uses both column modes");
		const auto Point = World(Key, X, Y, Z);
		const auto Original = Before.Read(Point).value();
		std::array Edit{SampleWrite{Point, AirSample}};
		CommitWrites(Source, Key, Edit);
		const auto Carved = Source.Capture(std::span(&Key, 1)).value();
		const auto PromotedTile = PackPage(Carved, Key, Strategy::WholeTile);
		const auto PromotedBrick = RepackBricks(Carved, Key, Bricks, Changed);
		Require(!PromotedTile.Height && PromotedBrick.Bricks->Bricks[Changed.front()]->GetDense(),
			"interior excavation promotes whole tile or only the incompatible brick");
		for (std::size_t Index = 0; Index < BrickCount; ++Index) {
			const bool Same = Bricks.Bricks->Bricks[Index] == PromotedBrick.Bricks->Bricks[Index];
			Require(Same == (Index != Changed.front()), "selective promotion shares every untouched brick");
		}
		Require(Tile.Read(X, Y, Z) == Original && Bricks.Read(X, Y, Z) == Original &&
			PromotedTile.Read(X, Y, Z) == AirSample && PromotedBrick.Read(X, Y, Z) == AirSample,
			"holding old captures never lets local repacking change their samples");
		Edit.front().Value = Original;
		CommitWrites(Source, Key, Edit);
		const auto Repaired = Source.Capture(std::span(&Key, 1)).value();
		const auto RepairedTile = PackPage(Repaired, Key, Strategy::WholeTile);
		const auto RepairedBrick = RepackBricks(Repaired, Key, PromotedBrick, Changed);
		Require(RepairedTile.Height && RepairedBrick.Bricks->Bricks[Changed.front()]->GetHeight() &&
			EncodeChunk(Repaired, Key).value() == EncodeChunk(Before, Key).value(),
			"repair demotes both storage forms and restores original canonical bytes");
		Field Adaptive({}, {.BrickStorage = BrickStorageMode::LosslessColumns});
		Author(Adaptive, TerrainStyle::Flat, std::span(&Key, 1));
		const auto NativeBefore = Adaptive.Capture(std::span(&Key, 1)).value();
		const auto CompressedBytes = Adaptive.GetStats().OwnedBytes;
		Edit.front().Value = AirSample;
		CommitWrites(Adaptive, Key, Edit);
		const auto NativeCarved = Adaptive.Capture(std::span(&Key, 1)).value();
		Require(Adaptive.GetStats().OwnedBytes > CompressedBytes && NativeBefore.Read(Point).value() == Original &&
			EncodeChunk(NativeCarved, Key).value() == EncodeChunk(Carved, Key).value(), "native edit promotes one brick");
		Edit.front().Value = Original;
		CommitWrites(Adaptive, Key, Edit);
		Require(Adaptive.GetStats().OwnedBytes == CompressedBytes && NativeCarved.Read(Point).value() == AirSample &&
			EncodeChunk(Adaptive.Capture(std::span(&Key, 1)).value(), Key).value() == EncodeChunk(Before, Key).value(),
			"native repair restores compact bytes and preserves the prior edit snapshot");
		std::uint64_t Witness = 0;
		constexpr int Iterations = 24;
		const auto WholeStart = Clock::now();
		for (int Iteration = 0; Iteration < Iterations; ++Iteration) {
			Witness += PackPage(Carved, Key, Strategy::WholeTile).Bytes();
			Witness += PackPage(Repaired, Key, Strategy::WholeTile).Bytes();
		}
		const auto WholeUs = ElapsedNs(WholeStart) / (Iterations * 2 * 1000);
		const auto LocalStart = Clock::now();
		for (int Iteration = 0; Iteration < Iterations; ++Iteration) {
			Witness += RepackBricks(Carved, Key, Bricks, Changed).Bytes();
			Witness += RepackBricks(Repaired, Key, PromotedBrick, Changed).Bytes();
		}
		const auto LocalUs = ElapsedNs(LocalStart) / (Iterations * 2 * 1000);
		std::cout << "[Terrain:Columns] local_repack whole_us=" << WholeUs << " selective_us=" << LocalUs
			<< " whole_samples=" << PageSamples << " selective_samples=" << BrickSamples
			<< " before_page_bytes=" << Tile.Bytes() << " promoted_page_bytes=" << PromotedTile.Bytes()
			<< " selective_before_bytes=" << Bricks.Bytes() << " selective_promoted_bytes=" << PromotedBrick.Bytes()
			<< " checksum=" << Witness << '\n';
	}

	void MeasureSparseWorld() {
		std::cout << "pages,storage,field_bytes,author_ms,warm_random_read_ns,codec_sample_ms,checksum\n";
		for (const std::size_t Count : {32, 128, 512}) {
			std::vector<ChunkKey> Keys;
			Keys.reserve(Count);
			// Narrow rolling ridges offset into distant sparse regions.
			for (std::size_t I = 0; I < Count; ++I) {
				const auto Cluster = static_cast<std::int64_t>(I / 32);
				Keys.push_back({Cluster * 64 - 490, 0, static_cast<std::int64_t>(I % 32) - 16});
			}
			const auto Points = ReadOrder(Keys);
			std::optional<std::uint64_t> ExpectedRead;
			std::map<ChunkKey, std::vector<std::uint8_t>> ExpectedArtifacts;
			for (const auto Mode : {BrickStorageMode::DensePalette, BrickStorageMode::LosslessColumns}) {
				Field Storage({}, {.BrickStorage = Mode});
				const auto BuildStart = Clock::now();
				Author(Storage, TerrainStyle::Rolling, Keys);
				const auto BuildMs = ElapsedNs(BuildStart) / 1e6;
				std::uint64_t Checksum{};
				const auto ReadNs = ReadTime(Points, [&](SamplePoint P) { return Storage.Read(P).value(); }, Checksum);
				if (ExpectedRead) Require(*ExpectedRead == Checksum, "large-world storage reads match");
				else ExpectedRead = Checksum;
				const auto CodecStart = Clock::now();
				for (std::size_t I = 0; I < Keys.size(); I += 16) {
					const auto Key = Keys[I];
					const auto Samples = Storage.Capture(std::span{&Key, 1}).value();
					auto Bytes = EncodeChunk(Samples, Key).value();
					if (Mode == BrickStorageMode::DensePalette) ExpectedArtifacts.emplace(Key, std::move(Bytes));
					else Require(Bytes == ExpectedArtifacts.at(Key), "large-world chunk artifact is canonical");
				}
				const auto CodecMs = ElapsedNs(CodecStart) / 1e6;
				std::cout << Count << ',' << (Mode == BrickStorageMode::DensePalette ? "dense" : "adaptive") << ','
					<< Storage.GetStats().OwnedBytes << ',' << BuildMs << ',' << ReadNs << ',' << CodecMs << ',' << Checksum << '\n';
			}
		}
	}
}

int main(int ArgumentCount, char **Arguments) try {
	std::cout << std::fixed << std::setprecision(2);
	if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--sparse-world") {
		MeasureSparseWorld();
		return 0;
	}
	std::cout << "scenario,strategy,pages,field_bytes,gtch_bytes,candidate_bytes,column_tiles,column_bricks,volume_bricks,"
		"density_residual_samples,field_read_ns,candidate_read_ns,gtch_us_per_page,candidate_pack_us_per_page\n";
	Compare("flat", TerrainStyle::Flat, NeighborKeys);
	Compare("rolling", TerrainStyle::Rolling, NeighborKeys);
	Compare("cliff", TerrainStyle::Cliff, NeighborKeys);
	Compare("layered", TerrainStyle::Layered, NeighborKeys);
	Compare("noise", TerrainStyle::Noisy, NeighborKeys);
	Compare("material_noise", TerrainStyle::MaterialNoise, NeighborKeys);
	Compare("uniform", TerrainStyle::Constant, NeighborKeys);
	Compare("cave_and_island", TerrainStyle::Cavern, NeighborKeys);
	Compare("world_edge_half_spacing", TerrainStyle::Rolling, WorldEdge, {-1});
	VerifyPromotionAndRepair();
	std::cout << "[Terrain:Columns] Exact samples, canonical artifacts, MC33 halos and local edits passed\n";
	return 0;
} catch (const std::exception &Error) {
	std::cerr << "[Terrain:Columns] " << Error.what() << '\n';
	return 1;
}
