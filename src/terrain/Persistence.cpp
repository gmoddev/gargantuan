// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/terrain/Persistence.hpp"

#include "gargantuan/Profiler.hpp"
#include "gargantuan/terrain/ChunkCodec.hpp"

#include <algorithm>
#include <array>
#include <new>
#include <stdexcept>

namespace gargantuan::terrain {
    namespace {
        using Bytes = std::vector<std::uint8_t>;

        bool ValidLimits(TerrainPersistenceLimits Limits) noexcept {
            return Limits.MaximumChunks <= MaximumTerrainManifestEntries &&
                Limits.MaximumClosureBytes <= MaximumTerrainClosureBytes;
        }

        TerrainPersistenceError Convert(FieldError Error) noexcept {
            switch (Error) {
                case FieldError::LimitExceeded: return TerrainPersistenceError::LimitExceeded;
                case FieldError::OutOfMemory: return TerrainPersistenceError::OutOfMemory;
                default: return TerrainPersistenceError::CorruptArtifact;
            }
        }

        TerrainPersistenceError Convert(ChunkCodecError Error) noexcept {
            switch (Error) {
                case ChunkCodecError::LimitExceeded: return TerrainPersistenceError::LimitExceeded;
                case ChunkCodecError::OutOfMemory: return TerrainPersistenceError::OutOfMemory;
                case ChunkCodecError::Canceled: return TerrainPersistenceError::Canceled;
                case ChunkCodecError::UnsupportedVersion: return TerrainPersistenceError::UnsupportedVersion;
                default: return TerrainPersistenceError::CorruptArtifact;
            }
        }

        TerrainPersistenceError Convert(TerrainManifestError Error) noexcept {
            switch (Error) {
                case TerrainManifestError::LimitExceeded: return TerrainPersistenceError::LimitExceeded;
                case TerrainManifestError::OutOfMemory: return TerrainPersistenceError::OutOfMemory;
                case TerrainManifestError::Canceled: return TerrainPersistenceError::Canceled;
                case TerrainManifestError::InvalidBinding: return TerrainPersistenceError::InvalidBinding;
                case TerrainManifestError::IncompatibleSpecification:
                case TerrainManifestError::UnsupportedVersion: return TerrainPersistenceError::UnsupportedVersion;
                default: return TerrainPersistenceError::CorruptArtifact;
            }
        }

        bool AddBytes(std::size_t &Total, std::size_t Size, std::size_t Limit) noexcept {
            if (Size > Limit - Total)
                return false;
            Total += Size;
            return true;
        }

        ChunkKey GetChunkKey(const TerrainCapturedChunk &Entry) noexcept {
            return Entry.Reference.Key;
        }

        bool CanReuse(const TerrainCaptureIndex *Prior, const TerrainCaptureIndex &Current) {
            return Prior && Prior->Project == Current.Project && Prior->Dataset == Current.Dataset &&
                Prior->Grid == Current.Grid && Prior->FieldLifetime == Current.FieldLifetime &&
                Prior->Chunks.size() <= MaximumTerrainManifestEntries &&
                std::ranges::is_sorted(Prior->Chunks, {}, GetChunkKey);
        }

        const TerrainChunkRef *GetReusableChunk(const TerrainCaptureIndex *Prior,
            Snapshot::ChunkInfo Current) noexcept {
            if (!Prior)
                return nullptr;
            const auto At = std::ranges::lower_bound(Prior->Chunks, Current.Key, {}, GetChunkKey);
            return At != Prior->Chunks.end() && At->Reference.Key == Current.Key && At->Version == Current.Version
                ? &At->Reference : nullptr;
        }

        std::expected<Bytes, TerrainPersistenceError> ReadVerified(
            const TerrainArtifactReader &Reader, const TerrainArtifactId &Id, std::size_t MaximumBytes,
            std::optional<std::size_t> ExactBytes, std::stop_token Stop) {
            if (Stop.stop_requested())
                return std::unexpected(TerrainPersistenceError::Canceled);
            if (!Id.Digest.IsValid())
                return std::unexpected(TerrainPersistenceError::InvalidBinding);
            try {
                auto Result = Reader(Id, MaximumBytes);
                if (!Result)
                    return std::unexpected(Result.error());
                if (Stop.stop_requested())
                    return std::unexpected(TerrainPersistenceError::Canceled);
                if (ExactBytes && Result->size() != *ExactBytes)
                    return std::unexpected(TerrainPersistenceError::CorruptArtifact);
                if (Result->size() > MaximumBytes)
                    return std::unexpected(TerrainPersistenceError::LimitExceeded);
                if (AssetContentId::Hash(*Result) != Id.Digest)
                    return std::unexpected(TerrainPersistenceError::CorruptArtifact);
                return Result;
            } catch (const std::bad_alloc &) {
                return std::unexpected(TerrainPersistenceError::OutOfMemory);
            } catch (...) {
                return std::unexpected(TerrainPersistenceError::ReadFailed);
            }
        }
    }

    TerrainDatasetId GetTerrainDatasetId(ProjectId Project) noexcept {
        TerrainDatasetId Result;
        for (std::size_t I = 0; I < 8; ++I) {
            Result.High = (Result.High << 8) | Project.Bytes[I];
            Result.Low = (Result.Low << 8) | Project.Bytes[I + 8];
        }
        return Result;
    }

    std::optional<std::string> GetTerrainArtifactPath(const TerrainArtifactId &Id) {
        if (!Id.Digest.IsValid())
            return std::nullopt;
        switch (Id.Purpose) {
            case TerrainArtifactPurpose::Chunk:
                return ".gargantuan/terrain/chunks/" + Id.Digest.ToString() + ".gtch";
            case TerrainArtifactPurpose::Manifest:
                return ".gargantuan/terrain/manifests/" + Id.Digest.ToString() + ".gtmf";
        }
        return std::nullopt;
    }

    std::string_view GetTerrainPersistenceErrorText(TerrainPersistenceError Error) noexcept {
        switch (Error) {
            case TerrainPersistenceError::InvalidBinding: return "Terrain project, dataset, or manifest identity is invalid";
            case TerrainPersistenceError::MissingArtifact: return "A referenced terrain artifact is missing";
            case TerrainPersistenceError::ReadFailed: return "A terrain artifact could not be read";
            case TerrainPersistenceError::CorruptArtifact: return "A terrain artifact has invalid content or integrity";
            case TerrainPersistenceError::UnsupportedVersion: return "The terrain artifact requires an unsupported version";
            case TerrainPersistenceError::LimitExceeded: return "The terrain capture or loaded field exceeds its resource budget";
            case TerrainPersistenceError::OutOfMemory: return "Memory allocation failed while preparing terrain persistence";
            case TerrainPersistenceError::Canceled: return "Terrain persistence was canceled";
        }
        return "Terrain persistence failed";
    }

    std::expected<TerrainCapture, TerrainPersistenceError> CaptureTerrain(
        const Field &Source, ProjectId Project, TerrainDatasetId Dataset, const TerrainCaptureIndex *PriorDurable,
        TerrainPersistenceLimits Limits, std::stop_token Stop) {
        if (Stop.stop_requested())
            return std::unexpected(TerrainPersistenceError::Canceled);
        if (!Project.IsValid() || !Dataset.IsValid())
            return std::unexpected(TerrainPersistenceError::InvalidBinding);
        if (!ValidLimits(Limits))
            return std::unexpected(TerrainPersistenceError::LimitExceeded);

        const auto Capture = Source.CaptureMaterialized(Limits.MaximumChunks);
        if (!Capture)
            return std::unexpected(Convert(Capture.error()));
        return CaptureTerrain(*Capture, Project, Dataset, PriorDurable, Limits, Stop);
    }

    std::expected<TerrainCapture, TerrainPersistenceError> CaptureTerrain(
        const CommittedFieldCapture &Source, ProjectId Project, TerrainDatasetId Dataset,
        const TerrainCaptureIndex *PriorDurable, TerrainPersistenceLimits Limits, std::stop_token Stop) {
        G_PROFILE("Terrain::CaptureArtifactClosure");
        if (Stop.stop_requested())
            return std::unexpected(TerrainPersistenceError::Canceled);
        if (!Project.IsValid() || !Dataset.IsValid() || Source.GetFieldLifetime() == 0)
            return std::unexpected(TerrainPersistenceError::InvalidBinding);

        const auto &Snapshot = Source.GetSnapshot();
        const auto Count = Snapshot.GetChunkCount();
        if (!ValidLimits(Limits) || Count > Limits.MaximumChunks)
            return std::unexpected(TerrainPersistenceError::LimitExceeded);

        try {
            TerrainCapture Result;
            Result.FieldRevision = Snapshot.GetRevision();
            Result.Index = {Project, Dataset, Snapshot.GetGrid(), Source.GetFieldLifetime(), {}};

            TerrainManifest Manifest{Project, Dataset, Snapshot.GetGrid(), {}};
            const auto ManifestBytes = TerrainManifestHeaderBytes + Count * TerrainManifestEntryBytes;
            if (!AddBytes(Result.ClosureBytes, ManifestBytes, Limits.MaximumClosureBytes))
                return std::unexpected(TerrainPersistenceError::LimitExceeded);

            const auto *Reusable = CanReuse(PriorDurable, Result.Index) ? PriorDurable : nullptr;
            Manifest.Chunks.reserve(Count);
            Result.Index.Chunks.reserve(Count);
            if (!Reusable)
                Result.Artifacts.reserve(Count + 1);

            for (std::size_t I = 0; I < Count; ++I) {
                if (Stop.stop_requested())
                    return std::unexpected(TerrainPersistenceError::Canceled);
                const auto Current = Snapshot.GetChunkAt(I);
                if (!Current)
                    return std::unexpected(TerrainPersistenceError::CorruptArtifact);
                TerrainChunkRef Reference;
                if (const auto *Prior = GetReusableChunk(Reusable, *Current)) {
                    Reference = *Prior;
                    ++Result.ReusedChunks;
                    if (!AddBytes(Result.ClosureBytes, Reference.ByteCount, Limits.MaximumClosureBytes))
                        return std::unexpected(TerrainPersistenceError::LimitExceeded);
                } else {
                    auto Encoded = EncodeChunk(Snapshot, Current->Key, Stop);
                    if (!Encoded)
                        return std::unexpected(Convert(Encoded.error()));
                    if (!AddBytes(Result.ClosureBytes, Encoded->size(), Limits.MaximumClosureBytes))
                        return std::unexpected(TerrainPersistenceError::LimitExceeded);
                    const auto Digest = AssetContentId::Hash(*Encoded);
                    if (!Digest.IsValid())
                        return std::unexpected(TerrainPersistenceError::CorruptArtifact);
                    Reference = {Current->Key, {Digest.Bytes}, static_cast<std::uint32_t>(Encoded->size())};
                    Result.Artifacts.push_back({{TerrainArtifactPurpose::Chunk, Digest},
                        std::make_shared<const Bytes>(std::move(*Encoded))});
                }
                Manifest.Chunks.push_back(Reference);
                Result.Index.Chunks.push_back({Reference, Current->Version});
            }

            auto Encoded = EncodeTerrainManifest(Manifest, Stop);
            if (!Encoded)
                return std::unexpected(Convert(Encoded.error()));
            Result.ManifestDigest = AssetContentId::Hash(*Encoded);
            if (!Result.ManifestDigest.IsValid())
                return std::unexpected(TerrainPersistenceError::CorruptArtifact);
            Result.Artifacts.push_back({{TerrainArtifactPurpose::Manifest, Result.ManifestDigest},
                std::make_shared<const Bytes>(std::move(*Encoded))});
            std::ranges::sort(Result.Artifacts, {}, &TerrainArtifact::Id);
            if (std::ranges::adjacent_find(Result.Artifacts, {}, &TerrainArtifact::Id) != Result.Artifacts.end())
                return std::unexpected(TerrainPersistenceError::CorruptArtifact);
            if (Stop.stop_requested())
                return std::unexpected(TerrainPersistenceError::Canceled);
            return Result;
        } catch (const std::bad_alloc &) {
            return std::unexpected(TerrainPersistenceError::OutOfMemory);
        }
    }

    std::expected<std::unique_ptr<Field>, TerrainPersistenceError> LoadTerrain(
        ProjectId Project, TerrainDatasetId Dataset, AssetContentId ManifestDigest, const TerrainArtifactReader &Reader,
        TerrainPersistenceLimits Limits, std::stop_token Stop) {
        G_PROFILE("Terrain::LoadArtifactClosure");
        if (Stop.stop_requested())
            return std::unexpected(TerrainPersistenceError::Canceled);
        if (!Reader || !Project.IsValid() || !Dataset.IsValid() || !ManifestDigest.IsValid())
            return std::unexpected(TerrainPersistenceError::InvalidBinding);
        if (!ValidLimits(Limits))
            return std::unexpected(TerrainPersistenceError::LimitExceeded);

        const auto MaximumManifestBytes = std::min(Limits.MaximumClosureBytes,
            TerrainManifestHeaderBytes + Limits.MaximumChunks * TerrainManifestEntryBytes);
        std::size_t ClosureBytes = 0;
        auto Manifest = [&]() -> std::expected<TerrainManifest, TerrainPersistenceError> {
            const auto Encoded = ReadVerified(Reader, {TerrainArtifactPurpose::Manifest, ManifestDigest},
                MaximumManifestBytes, std::nullopt, Stop);
            if (!Encoded)
                return std::unexpected(Encoded.error());
            ClosureBytes = Encoded->size();
            auto Value = DecodeTerrainManifest(*Encoded, Project, Dataset, Stop);
            if (!Value)
                return std::unexpected(Convert(Value.error()));
            return std::move(*Value);
        }();
        if (!Manifest)
            return std::unexpected(Manifest.error());
        if (Manifest->Chunks.size() > Limits.RestoredField.DirectoryEntries)
            return std::unexpected(TerrainPersistenceError::LimitExceeded);
        for (const auto &Reference : Manifest->Chunks)
            if (!AddBytes(ClosureBytes, Reference.ByteCount, Limits.MaximumClosureBytes))
                return std::unexpected(TerrainPersistenceError::LimitExceeded);

        try {
            auto Destination = std::make_unique<Field>(Manifest->Grid, Limits.RestoredField);
            for (const auto &Reference : Manifest->Chunks) {
                const auto Encoded = ReadVerified(Reader, {TerrainArtifactPurpose::Chunk, {Reference.Digest.Bytes}},
                    Reference.ByteCount, Reference.ByteCount, Stop);
                if (!Encoded)
                    return std::unexpected(Encoded.error());
                auto Decoded = DecodeChunk(*Encoded, Reference.Key, Manifest->Grid, Stop);
                if (!Decoded)
                    return std::unexpected(Convert(Decoded.error()));

                // Each key is unique and the detached field starts in air.
                // Omit only exact defaults and preserve all other densities.
                std::erase_if(Decoded->Writes, [](const auto &Write) { return Write.Value == AirSample; });
                if (Decoded->Writes.empty())
                    return std::unexpected(TerrainPersistenceError::CorruptArtifact);

                const std::array Keys{Reference.Key};
                auto Base = Destination->Capture(Keys);
                if (!Base)
                    return std::unexpected(Convert(Base.error()));
                auto Candidate = Field::Prepare(std::move(*Base), Decoded->Writes);
                if (!Candidate)
                    return std::unexpected(Convert(Candidate.error()));
                const auto Commit = Destination->Commit(std::move(*Candidate));
                if (!Commit)
                    return std::unexpected(Convert(Commit.error()));
            }
            if (Stop.stop_requested())
                return std::unexpected(TerrainPersistenceError::Canceled);
            return Destination;
        } catch (const std::bad_alloc &) {
            return std::unexpected(TerrainPersistenceError::OutOfMemory);
        } catch (const std::invalid_argument &) {
            return std::unexpected(TerrainPersistenceError::LimitExceeded);
        } catch (const std::overflow_error &) {
            return std::unexpected(TerrainPersistenceError::LimitExceeded);
        }
    }
}
