// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/terrain/Persistence.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <latch>
#include <map>
#include <string_view>
#include <thread>

namespace {
    using namespace gargantuan;
    using namespace gargantuan::terrain;
    using Bytes = std::vector<std::uint8_t>;
    using Clock = std::chrono::steady_clock;

    constexpr ProjectId Project{{3, 0, 1, 2, 3, 0xEF, 0x28, 0xFF, 11, 12, 0x22, 14, 21, 0x30, 255, 1}};
    constexpr ProjectId OtherProject{{4, 0, 1}};
    constexpr GridSpec Grid{-1};
    constexpr ChunkKey Uniform{-2, 0, 0};
    constexpr ChunkKey Cave{-1, 0, 0};
    constexpr ChunkKey Island{15, 2, -9};
    constexpr ChunkKey Retired{7, 0, 0};
    constexpr std::array LiveKeys{Uniform, Cave, Island};
    constexpr MaterialId Rock = 0xFEDCBA98;
    constexpr MaterialId Paint = 0xFF89AB34;

    void Verify(bool Condition, std::string_view Description) {
        if (!Condition) {
            std::cerr << "[Terrain:Persistence] " << Description << '\n';
            std::exit(2);
        }
    }

    template <typename T, typename E>
    T Take(std::expected<T, E> Result, std::string_view Description) {
        Verify(Result.has_value(), Description);
        return std::move(*Result);
    }

    template <typename T>
    void Deny(const std::expected<T, TerrainPersistenceError> &Result, TerrainPersistenceError Error,
        std::string_view Description) {
        Verify(!Result && Result.error() == Error, Description);
    }

    SamplePoint PointAt(ChunkKey Key, int X = 0, int Y = 0, int Z = 0) {
        return {Key.X * ChunkSide + X, Key.Y * ChunkSide + Y, Key.Z * ChunkSide + Z};
    }

    void Write(Field &Target, ChunkKey Key, std::span<const SampleWrite> Values) {
        const std::array Keys{Key};
        auto Snapshot = Take(Target.Capture(Keys), "capture write");
        auto Prepared = Take(Field::Prepare(std::move(Snapshot), Values), "prepare write");
        Verify(Target.Commit(std::move(Prepared)).has_value(), "commit write");
    }

    void WriteOne(Field &Target, ChunkKey Key, SamplePoint Point, Sample Value) {
        const std::array Writes{SampleWrite{Point, Value}};
        Write(Target, Key, Writes);
    }

    void FillRock(Field &Target, ChunkKey Key) {
        std::vector<SampleWrite> Values;
        Values.reserve(MaximumEditSamples);
        for (int Z = 0; Z < ChunkSide; ++Z)
            for (int Y = 0; Y < ChunkSide; ++Y)
                for (int X = 0; X < ChunkSide; ++X)
                    Values.push_back({PointAt(Key, X, Y, Z), {-32767, Rock}});
        Write(Target, Key, Values);
    }

    void Build(Field &Target) {
        FillRock(Target, Uniform);
        FillRock(Target, Cave);

        const std::array CaveKey{Cave};
        auto Solid = Take(Target.Capture(CaveKey), "capture cave solid");
        auto Sculpt = Take(Field::Prepare(std::move(Solid), SphereBrush{{-8, 8, 8}, 3.5, BrushMode::Subtract, 0}),
            "prepare cave excavation");
        Verify(Target.Commit(std::move(Sculpt)).has_value(), "commit excavation");
        Verify(Target.Read(PointAt(Cave, 16, 16, 16))->Density > 0, "cave contains air");
        Verify(Target.Read(PointAt(Cave, 2, 2, 2))->Material == Rock, "cave retains rock");

        const std::array IslandWrites{
            SampleWrite{PointAt(Island, 10, 10, 10), {-1024, 0xFFFF0001u}},
            SampleWrite{PointAt(Island, 11, 10, 10), {-1000, 0xFFFF0002u}},
            SampleWrite{PointAt(Island, 10, 10, 11), {0, 0}},
        };
        Write(Target, Island, IslandWrites);
        WriteOne(Target, Retired, PointAt(Retired), {-1, Rock});
        WriteOne(Target, Retired, PointAt(Retired), AirSample);

        const auto Keys = Take(Target.GetMaterializedChunkKeys(LiveKeys.size()), "admit live key enumeration");
        Verify(std::ranges::equal(Keys, LiveKeys), "enumeration excludes version tombstones");
        const auto RejectedKeys = Target.GetMaterializedChunkKeys(LiveKeys.size() - 1);
        Verify(!RejectedKeys && RejectedKeys.error() == FieldError::LimitExceeded,
            "key enumeration rejects max before reserving");
    }

    struct Store {
        std::map<TerrainArtifactId, std::shared_ptr<const Bytes>> Artifacts;
        std::size_t LeafReads{};
        std::size_t ManifestReads{};

        void Put(const TerrainCapture &Capture) {
            for (const auto &Artifact : Capture.Artifacts) {
                Verify(AssetContentId::Hash(*Artifact.Bytes) == Artifact.Id.Digest, "canonical content-id SHA-256");
                Artifacts[Artifact.Id] = Artifact.Bytes;
            }
        }

        TerrainArtifactReader GetReader() {
            return [this](const TerrainArtifactId &Id, std::size_t Limit) -> std::expected<Bytes, TerrainPersistenceError> {
                if (Id.Purpose == TerrainArtifactPurpose::Manifest)
                    ++ManifestReads;
                else
                    ++LeafReads;
                const auto Found = Artifacts.find(Id);
                if (Found == Artifacts.end())
                    return std::unexpected(TerrainPersistenceError::MissingArtifact);
                if (Found->second->size() > Limit)
                    return std::unexpected(TerrainPersistenceError::LimitExceeded);
                return *Found->second;
            };
        }

        AssetContentId AddManifest(const TerrainManifest &Manifest) {
            auto Encoded = Take(EncodeTerrainManifest(Manifest), "encode altered manifest");
            const auto Digest = AssetContentId::Hash(Encoded);
            Artifacts[{TerrainArtifactPurpose::Manifest, Digest}] = std::make_shared<const Bytes>(std::move(Encoded));
            return Digest;
        }
    };

    void CheckFieldParity(const Snapshot &Source, const Field &Destination) {
        Verify(Destination.GetGrid() == Source.GetGrid(), "lattice spacing preserved");
        for (const auto Key : LiveKeys) {
            for (int Z = 0; Z < ChunkSide; ++Z)
                for (int Y = 0; Y < ChunkSide; ++Y)
                    for (int X = 0; X < ChunkSide; ++X) {
                        const auto Point = PointAt(Key, X, Y, Z);
                        const auto Expected = Source.Read(Point);
                        const auto Actual = Destination.Read(Point);
                        Verify(Expected && Actual && *Expected == *Actual, "exact authoritative sample restoration");
                    }
        }
        Verify(Destination.Read(PointAt(Retired)) == AirSample, "omitted key resolves known air after load");
    }

    double Micros(Clock::time_point Start) {
        return std::chrono::duration<double, std::micro>(Clock::now() - Start).count();
    }

    void MeasureNative(const CommittedFieldCapture &Current, const TerrainCapture &Before, const TerrainCapture &After,
        Store &Artifacts) {
        constexpr std::size_t Repetitions = 31;
        const auto Dataset = GetTerrainDatasetId(Project);
        std::array<double, Repetitions> Full{}, Incremental{}, Load{};
        for (std::size_t I = 0; I < Repetitions; ++I) {
            auto Start = Clock::now();
            const auto A = CaptureTerrain(Current, Project, Dataset);
            Full[I] = Micros(Start);
            Start = Clock::now();
            const auto B = CaptureTerrain(Current, Project, Dataset, &Before.Index);
            Incremental[I] = Micros(Start);
            Start = Clock::now();
            const auto C = LoadTerrain(Project, Dataset, After.ManifestDigest, Artifacts.GetReader());
            Load[I] = Micros(Start);
            Verify(A && B && C && A->ManifestDigest == After.ManifestDigest && B->ManifestDigest == After.ManifestDigest,
                "native performance fixture remains semantically matched");
        }
        const auto Report = [](auto &Times, std::string_view Name) {
            std::ranges::sort(Times);
            std::cout << "[Terrain:Persistence] " << Name << " p50/p95/max us "
                << Times[15] << '/' << Times[29] << '/' << Times.back() << '\n';
        };
        Report(Full, "full capture");
        Report(Incremental, "incremental capture");
        Report(Load, "verified detached load");
    }

    void CheckClosure() {
        Field Source(Grid);
        Build(Source);
        const auto Dataset = GetTerrainDatasetId(Project);
        Verify(Dataset.High == 0x0300010203EF28FF && Dataset.Low == 0x0B0C220E1530FF01 &&
            !GetTerrainDatasetId({}).IsValid(), "stable complete project-derived dataset ID");

        // A worker prepares against an immutable edit snapshot. Its result
        // remains detached while Main freezes the complete committed terrain.
        const std::array CaveKeys{Cave};
        auto EditBase = Take(Source.Capture(CaveKeys), "capture concurrent edit");
        std::optional<PreparedEdit> Pending;
        std::latch Prepared{1};
        std::latch Continue{1};
        const auto Point = PointAt(Cave, 2, 2, 2);
        const std::array PaintWrites{SampleWrite{Point, {-32767, Paint}}};
        std::jthread Worker([&Pending, &Prepared, &Continue, &PaintWrites, Base = std::move(EditBase)]() mutable {
            auto Edit = Field::Prepare(std::move(Base), PaintWrites);
            if (Edit)
                Pending.emplace(std::move(*Edit));
            Prepared.count_down();
            Continue.wait();
        });
        Prepared.wait();
        auto OriginalCapture = Source.CaptureMaterialized(LiveKeys.size());
        Continue.count_down();
        Worker.join();
        const auto Frozen = Take(std::move(OriginalCapture), "freeze while worker holds output");
        Verify(Pending && Frozen.GetSnapshot().Read(Point)->Material == Rock &&
            Source.Read(Point)->Material == Rock, "pending worker cannot enter save");
        Verify(Source.Commit(std::move(*Pending)).has_value(), "commit paint after capture");
        Verify(Source.Read(Point)->Material == Paint && Frozen.GetSnapshot().Read(Point)->Material == Rock,
            "frozen capture keeps former revision and immutable pages");

        const auto FullStart = Clock::now();
        const auto First = Take(CaptureTerrain(Frozen, Project, Dataset), "capture frozen complete closure");
        const auto FullMicros = Micros(FullStart);
        Verify(First.FieldRevision == Frozen.GetSnapshot().GetRevision() &&
            First.FieldRevision < Source.GetRevision(), "save publication records exact capture revision");
        Verify(First.Artifacts.size() == 4 && First.Index.Chunks.size() == 3 &&
            First.ReusedChunks == 0, "complete closure includes one leaf per materialized chunk");
        Verify(std::ranges::is_sorted(First.Artifacts, {}, &TerrainArtifact::Id), "generated artifact ordering");
        const auto ManifestPath = GetTerrainArtifactPath({TerrainArtifactPurpose::Manifest, First.ManifestDigest});
        Verify(ManifestPath && ManifestPath->starts_with(".gargantuan/terrain/manifests/") &&
            ManifestPath->ends_with(First.ManifestDigest.ToString() + ".gtmf"), "fixed content-addressed manifest path");
        Verify(!GetTerrainArtifactPath({static_cast<TerrainArtifactPurpose>(2), First.ManifestDigest}) &&
            !GetTerrainArtifactPath({TerrainArtifactPurpose::Chunk, {}}), "invalid artifact path denied");

        auto Idle = Take(CaptureTerrain(Frozen, Project, Dataset, &First.Index), "capture unchanged durable closure");
        Verify(Idle.ManifestDigest == First.ManifestDigest && Idle.Artifacts.size() == 1 &&
            Idle.ReusedChunks == LiveKeys.size(), "reuse unchanged typed references");

        const auto IncrementalStart = Clock::now();
        const auto Second = Take(CaptureTerrain(Source, Project, Dataset, &First.Index), "capture changed durable closure");
        const auto IncrementalMicros = Micros(IncrementalStart);
        Verify(Second.ManifestDigest != First.ManifestDigest && Second.Artifacts.size() == 2 && Second.ReusedChunks == 2,
            "paint re-encodes one leaf and the manifest");
        Verify(Second.Index.Chunks[0].Reference == First.Index.Chunks[0].Reference &&
            Second.Index.Chunks[2].Reference == First.Index.Chunks[2].Reference &&
            Second.Index.Chunks[1].Version.Geometry == First.Index.Chunks[1].Version.Geometry &&
            Second.Index.Chunks[1].Version.Material > First.Index.Chunks[1].Version.Material,
            "paint preserves geometry revision and unrelated digests");

        const auto Exported = Take(CaptureTerrain(Source, OtherProject, GetTerrainDatasetId(OtherProject), &Second.Index),
            "capture Save As closure");
        Verify(Exported.ReusedChunks == 0 && Exported.Artifacts.size() == 4 &&
            Exported.ManifestDigest != Second.ManifestDigest &&
            Exported.Index.Chunks[1].Reference == Second.Index.Chunks[1].Reference,
            "new project owns complete closure under a new manifest identity");

        Store Artifacts;
        Artifacts.Put(First);
        Artifacts.Put(Second);
        const auto LoadStart = Clock::now();
        const auto Reopened = Take(LoadTerrain(Project, Dataset, Second.ManifestDigest, Artifacts.GetReader()),
            "verify and detach complete authored field");
        const auto LoadMicros = Micros(LoadStart);
        Verify(Reopened->GetLifetime() != Source.GetLifetime() && !Reopened->IsCurrent(Frozen.GetSnapshot()),
            "prior field authority cannot validate in reopened field");
        const auto Current = Take(Source.CaptureMaterialized(LiveKeys.size()), "capture current authority");
        CheckFieldParity(Current.GetSnapshot(), *Reopened);
        const auto Recapture = Take(CaptureTerrain(*Reopened, Project, Dataset, &Second.Index), "recapture restored content");
        Verify(Recapture.ManifestDigest == Second.ManifestDigest && Recapture.ReusedChunks == 0,
            "same logical bytes but fresh authority lifetime");

        const auto Old = Take(LoadTerrain(Project, Dataset, First.ManifestDigest, Artifacts.GetReader()),
            "prior root remains independently resolvable");
        CheckFieldParity(Frozen.GetSnapshot(), *Old);

        const TerrainArtifactId ChangedLeaf{TerrainArtifactPurpose::Chunk, {Second.Index.Chunks[1].Reference.Digest.Bytes}};
        Store Broken = Artifacts;
        Broken.Artifacts.erase(ChangedLeaf);
        Deny(LoadTerrain(Project, Dataset, Second.ManifestDigest, Broken.GetReader()), TerrainPersistenceError::MissingArtifact,
            "listed missing bytes never resolve as air");
        Broken = Artifacts;
        auto Tampered = *Broken.Artifacts[ChangedLeaf];
        Tampered.back() ^= 1;
        Broken.Artifacts[ChangedLeaf] = std::make_shared<const Bytes>(std::move(Tampered));
        Deny(LoadTerrain(Project, Dataset, Second.ManifestDigest, Broken.GetReader()), TerrainPersistenceError::CorruptArtifact,
            "mutated leaf with unchanged byte count fails digest");
        const TerrainArtifactReader WrongLength = [&](const TerrainArtifactId &Id, std::size_t Limit)
            -> std::expected<Bytes, TerrainPersistenceError> {
            auto Encoded = Artifacts.GetReader()(Id, Limit);
            if (Encoded && Id == ChangedLeaf)
                Encoded->push_back(0);
            return Encoded;
        };
        Deny(LoadTerrain(Project, Dataset, Second.ManifestDigest, WrongLength), TerrainPersistenceError::CorruptArtifact,
            "provider cannot bypass exact leaf byte length");
        Broken = Artifacts;
        Bytes Spoofed = *Broken.Artifacts[{TerrainArtifactPurpose::Manifest, Second.ManifestDigest}];
        Spoofed.front() ^= 1;
        Broken.Artifacts[{TerrainArtifactPurpose::Manifest, Second.ManifestDigest}] = std::make_shared<const Bytes>(std::move(Spoofed));
        Deny(LoadTerrain(Project, Dataset, Second.ManifestDigest, Broken.GetReader()), TerrainPersistenceError::CorruptArtifact,
            "trusted root cannot resolve mutated manifest bytes");
        Deny(LoadTerrain(OtherProject, GetTerrainDatasetId(OtherProject), Second.ManifestDigest, Artifacts.GetReader()),
            TerrainPersistenceError::InvalidBinding, "wrong project and dataset");

        auto Altered = Take(DecodeTerrainManifest(*Second.Artifacts.back().Bytes, Project, Dataset), "read fixture manifest");
        Altered.Chunks[1].Digest = Altered.Chunks[2].Digest;
        Altered.Chunks[1].ByteCount = Altered.Chunks[2].ByteCount;
        const auto WrongKey = Artifacts.AddManifest(Altered);
        Deny(LoadTerrain(Project, Dataset, WrongKey, Artifacts.GetReader()), TerrainPersistenceError::CorruptArtifact,
            "digest-valid artifact under wrong terrain key");
        auto Unsupported = *Artifacts.Artifacts[ChangedLeaf];
        Unsupported[4] = 2;
        const auto FutureDigest = AssetContentId::Hash(Unsupported);
        Artifacts.Artifacts[{TerrainArtifactPurpose::Chunk, FutureDigest}] = std::make_shared<const Bytes>(std::move(Unsupported));
        Altered.Chunks[1] = Second.Index.Chunks[1].Reference;
        Altered.Chunks[1].Digest = {FutureDigest.Bytes};
        const auto FutureLeaf = Artifacts.AddManifest(Altered);
        Deny(LoadTerrain(Project, Dataset, FutureLeaf, Artifacts.GetReader()), TerrainPersistenceError::UnsupportedVersion,
            "hash-valid future leaf version denied");

        TerrainPersistenceLimits Small;
        Small.MaximumClosureBytes = Second.ClosureBytes - 1;
        const auto LeavesBefore = Artifacts.LeafReads;
        Deny(LoadTerrain(Project, Dataset, Second.ManifestDigest, Artifacts.GetReader(), Small),
            TerrainPersistenceError::LimitExceeded, "whole closure budget");
        Verify(Artifacts.LeafReads == LeavesBefore, "budget rejects before reading leaf data");
        Deny(CaptureTerrain(Source, Project, Dataset, &Second.Index, Small),
            TerrainPersistenceError::LimitExceeded, "incremental complete closure obeys aggregate");
        Small = {};
        Small.RestoredField.OwnedBytes = 64;
        Deny(LoadTerrain(Project, Dataset, Second.ManifestDigest, Artifacts.GetReader(), Small),
            TerrainPersistenceError::LimitExceeded, "detached field imposes owned byte cap");

        std::stop_source Stop;
        Verify(Stop.request_stop(), "cancel fixture");
        Deny(CaptureTerrain(Source, Project, Dataset, nullptr, {}, Stop.get_token()),
            TerrainPersistenceError::Canceled, "canceled capture");
        Deny(LoadTerrain(Project, Dataset, Second.ManifestDigest, Artifacts.GetReader(), {}, Stop.get_token()),
            TerrainPersistenceError::Canceled, "canceled restore");
        Verify(Source.Read(Point)->Material == Paint, "all failed restores leave original authority untouched");

        const auto NewLeafBytes = Second.Artifacts[0].Bytes->size();
        std::cout << "[Terrain:Persistence] full " << First.ClosureBytes << " B, " << FullMicros
            << " us, painted closure " << Second.ClosureBytes << " B with "
            << (NewLeafBytes + Second.Artifacts.back().Bytes->size())
            << " B to write, " << IncrementalMicros << " us native capture, load " << LoadMicros << " us\n";
        MeasureNative(Current, First, Second, Artifacts);
    }

    void CheckEmpty() {
        Field Empty;
        const auto Dataset = GetTerrainDatasetId(Project);
        auto Original = Take(Empty.CaptureMaterialized(0), "freeze empty dataset");
        const auto Frozen = std::move(Original);
        Deny(CaptureTerrain(Original, Project, Dataset), TerrainPersistenceError::InvalidBinding,
            "moved-from frozen ownership cannot produce a manifest");
        const auto Capture = Take(CaptureTerrain(Frozen, Project, Dataset), "empty canonical dataset");
        Verify(Capture.Artifacts.size() == 1 && Capture.Index.Chunks.empty() &&
            Capture.ClosureBytes == TerrainManifestHeaderBytes, "empty closure writes only manifest");
        Store Artifacts;
        Artifacts.Put(Capture);
        const auto New = Take(LoadTerrain(Project, Dataset, Capture.ManifestDigest, Artifacts.GetReader()),
            "restore empty dataset without synthetic pages");
        Verify(New->GetRevision() == 0 && New->GetStats().Entries == 0, "empty authority after reopen");
    }
}

int main() {
    CheckEmpty();
    CheckClosure();
    std::cout << "[Terrain:Persistence] verified exact native capture, restore, reuse and denial checks\n";
}
