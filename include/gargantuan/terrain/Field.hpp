// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/terrain/Types.hpp"

#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <vector>

namespace gargantuan::terrain {
    enum class FieldError {
        InvalidCoordinate,
        InvalidBounds,
        InvalidSample,
        InvalidBrush,
        UncapturedDependency,
        DuplicateSample,
        LimitExceeded,
        OutOfMemory,
        StaleDependency,
        WrongField,
        RevisionExhausted,
    };

    [[nodiscard]] std::string_view GetFieldErrorText(FieldError Error) noexcept;

    inline constexpr std::size_t MaximumEditSamples = 32'768;
    inline constexpr std::size_t MaximumCapturedChunks = 128;
    inline constexpr std::size_t MaximumPreparedBytes = 16 * 1024 * 1024;

    enum class BrickStorageMode { DensePalette, LosslessColumns };

    struct FieldLimits {
        std::size_t DirectoryEntries{65'536};
        std::size_t OwnedBytes{64 * 1024 * 1024};
        BrickStorageMode BrickStorage{BrickStorageMode::DensePalette};
    };

    struct ChunkVersion {
        Revision Geometry{};
        Revision Material{};
        auto operator<=>(const ChunkVersion &) const = default;
    };

    struct SampleWrite {
        SamplePoint Point;
        Sample Value;
    };

    enum class BrushMode { Add, Subtract, Paint };

    struct SphereBrush {
        WorldPoint Center;
        double Radius{};
        BrushMode Mode{};
        MaterialId Material{};
    };

    struct BoxBrush {
        WorldPoint Center;
        WorldPoint HalfExtent;
        BrushMode Mode{};
        MaterialId Material{};
    };

    struct ChangedChunk {
        ChunkKey Key;
        bool Geometry{};
        bool Material{};
    };

    struct CommitResult {
        Revision CommittedRevision{};
        std::vector<ChangedChunk> Changed;
    };

    struct FieldStats {
        std::size_t Entries{};
        std::size_t MaterializedPages{};
        std::size_t OwnedBytes{};
    };

    namespace field_detail {
        struct Page;
        struct SnapshotLease {};
        // Accountable storage and dependency versions have separate lifetimes.
        // An empty modified record still remembers its prior versions.
        struct Entry {
            std::shared_ptr<const Page> Data;
            ChunkVersion Version;
        };
    }

    class Field;

    class Snapshot {
    public:
        struct ChunkInfo {
            ChunkKey Key;
            ChunkVersion Version;
        };

        [[nodiscard]] GridSpec GetGrid() const noexcept { return Grid; }
        [[nodiscard]] Revision GetRevision() const noexcept { return CapturedRevision; }
        [[nodiscard]] std::size_t GetChunkCount() const noexcept { return Entries.size(); }
        [[nodiscard]] std::optional<ChunkInfo> GetChunkAt(std::size_t Index) const noexcept;
        [[nodiscard]] std::expected<Sample, FieldError> Read(SamplePoint Point) const noexcept;
        [[nodiscard]] std::expected<ChunkVersion, FieldError> GetVersion(ChunkKey Key) const noexcept;
        [[nodiscard]] std::expected<bool, FieldError> IsMaterialized(ChunkKey Key) const noexcept;
        [[nodiscard]] std::expected<bool, FieldError> ContainsSolid(ChunkKey Key) const noexcept;
        // Uniform bricks return their sample directly. Mixed bricks fill the
        // supplied buffer in owned sample order and return an empty optional.
        [[nodiscard]] std::expected<std::optional<Sample>, FieldError> ReadBrick(ChunkKey Key, std::size_t BrickIndex,
            std::span<Sample, BrickSide * BrickSide * BrickSide> Mixed) const noexcept;

    private:
        friend class Field;
        friend class CommittedFieldCapture;
        using Record = std::pair<ChunkKey, field_detail::Entry>;
        [[nodiscard]] const field_detail::Entry *Find(ChunkKey Key) const noexcept;

        std::vector<Record> Entries;
        GridSpec Grid;
        BrickStorageMode BrickStorage{BrickStorageMode::DensePalette};
        Revision CapturedRevision{};
        std::uint64_t FieldIdentity{};
        std::shared_ptr<const field_detail::SnapshotLease> Lease;
    };

    // Exact committed materialized pages at one Field revision. Unlike a
    // bounded edit snapshot, omission from this capture denotes known air.
    class CommittedFieldCapture {
    public:
        [[nodiscard]] const Snapshot &GetSnapshot() const noexcept { return SnapshotValue; }
        [[nodiscard]] std::uint64_t GetFieldLifetime() const noexcept {
            return SnapshotValue.Lease ? FieldLifetime : 0;
        }

    private:
        friend class Field;
        CommittedFieldCapture() = default;

        Snapshot SnapshotValue;
        std::uint64_t FieldLifetime{};
    };

    class PreparedEdit {
    public:
        PreparedEdit(PreparedEdit &&) noexcept = default;
        PreparedEdit &operator=(PreparedEdit &&) noexcept = default;
        [[nodiscard]] const Snapshot &GetPreview() const noexcept { return Preview; }
        [[nodiscard]] std::span<const ChangedChunk> GetChanges() const noexcept { return Changes; }
        [[nodiscard]] std::size_t GetReservationBytes() const noexcept { return ReservedBytes; }

    private:
        friend class Field;
        explicit PreparedEdit(Snapshot Base);

        Snapshot Preview;
        std::vector<ChunkVersion> Expected;
        std::vector<ChangedChunk> Changes;
        std::size_t ReservedBytes{};
    };

    [[nodiscard]] std::expected<std::vector<ChunkKey>, FieldError> GetRegionChunks(SampleBox Bounds,
        int Halo = 0);
    [[nodiscard]] std::expected<SampleBox, FieldError> GetBrushBounds(GridSpec Grid, const SphereBrush &Brush);
    [[nodiscard]] std::expected<SampleBox, FieldError> GetBrushBounds(GridSpec Grid, const BoxBrush &Brush);

    // Main owns live snapshots, edits and reads through Field. Static prepare
    // methods and const Snapshot reads can run on immutable worker input.
    class Field {
    public:
        explicit Field(GridSpec Grid = {}, FieldLimits Limits = {});
        Field(const Field &) = delete;
        Field &operator=(const Field &) = delete;

        [[nodiscard]] GridSpec GetGrid() const noexcept { return Grid; }
        [[nodiscard]] Revision GetRevision() const noexcept { return CurrentRevision; }
        [[nodiscard]] std::uint64_t GetLifetime() const noexcept { return Identity; }
        [[nodiscard]] FieldStats GetStats() const noexcept { return Stats; }
        [[nodiscard]] std::expected<Sample, FieldError> Read(SamplePoint Point) const noexcept;
        [[nodiscard]] std::expected<ChunkVersion, FieldError> GetVersion(ChunkKey Key) const noexcept;
        [[nodiscard]] std::expected<bool, FieldError> IsMaterialized(ChunkKey Key) const noexcept;
        [[nodiscard]] std::expected<bool, FieldError> ContainsSolid(ChunkKey Key) const noexcept;
        [[nodiscard]] std::expected<Snapshot, FieldError> Capture(std::span<const ChunkKey> Keys) const;
        [[nodiscard]] std::expected<CommittedFieldCapture, FieldError> CaptureMaterialized(std::size_t Maximum) const;
        [[nodiscard]] std::expected<std::vector<ChunkKey>, FieldError> GetMaterializedChunkKeys(std::size_t Maximum) const;
        [[nodiscard]] bool IsCurrent(const Snapshot &Base) const noexcept;
        [[nodiscard]] std::expected<CommitResult, FieldError> Commit(PreparedEdit &&Candidate);
        // Version tombstones can be reclaimed only after captured work releases
        // its leases. This visits the indexed empty keys under a fixed budget.
        [[nodiscard]] std::size_t RetireEmptyEntries(std::size_t Maximum = 64) noexcept;

        [[nodiscard]] static std::expected<PreparedEdit, FieldError> Prepare(Snapshot Base,
            std::span<const SampleWrite> Writes);
        [[nodiscard]] static std::expected<PreparedEdit, FieldError> Prepare(Snapshot Base,
            const SphereBrush &Brush);
        [[nodiscard]] static std::expected<PreparedEdit, FieldError> Prepare(Snapshot Base,
            const BoxBrush &Brush);
        [[nodiscard]] static std::expected<PreparedEdit, FieldError> PrepareSmooth(Snapshot Base,
            SampleBox Bounds, std::uint8_t Strength);

    private:
        [[nodiscard]] field_detail::Entry GetEntry(ChunkKey Key) const noexcept;

        GridSpec Grid;
        FieldLimits Limits;
        const std::uint64_t Identity;
        std::shared_ptr<const field_detail::SnapshotLease> Lease;
        Revision CurrentRevision{};
        FieldStats Stats;
        std::map<ChunkKey, field_detail::Entry> Directory;
        std::set<ChunkKey> EmptyEntries;
    };
}
