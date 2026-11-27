// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/terrain/Field.hpp"
#include "gargantuan/Profiler.hpp"
#include "terrain/HeightColumns.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <variant>

namespace gargantuan::terrain {
    namespace field_detail {
        inline constexpr std::size_t BrickSamples = BrickSide * BrickSide * BrickSide;
        inline constexpr int BrickGrid = ChunkSide / BrickSide;
        inline constexpr std::size_t BricksPerChunk = BrickGrid * BrickGrid * BrickGrid;

        struct DenseBrick {
            std::array<std::int16_t, BrickSamples> Density;
            std::array<std::uint16_t, BrickSamples> MaterialIndices;
            std::vector<MaterialId> Palette;
        };

        struct Brick {
            using HeightBrick = Columns<BrickSide>;
            std::variant<Sample, std::unique_ptr<const DenseBrick>, std::unique_ptr<const HeightBrick>> Storage;

            [[nodiscard]] Sample Read(std::size_t Index) const noexcept {
                if (const auto *Dense = std::get_if<std::unique_ptr<const DenseBrick>>(&Storage))
                    return {(*Dense)->Density[Index], (*Dense)->Palette[(*Dense)->MaterialIndices[Index]]};
                if (const auto *Height = std::get_if<std::unique_ptr<const HeightBrick>>(&Storage))
                    return (*Height)->Read(static_cast<int>(Index % BrickSide),
                        static_cast<int>(Index / BrickSide % BrickSide),
                        static_cast<int>(Index / (BrickSide * BrickSide)));
                return std::get<Sample>(Storage);
            }

            [[nodiscard]] std::size_t Bytes() const noexcept {
                if (const auto *Dense = std::get_if<std::unique_ptr<const DenseBrick>>(&Storage))
                    return sizeof(Brick) + sizeof(DenseBrick) + (*Dense)->Palette.capacity() * sizeof(MaterialId);
                if (const auto *Height = std::get_if<std::unique_ptr<const HeightBrick>>(&Storage))
                    return sizeof(Brick) + (*Height)->Bytes();
                return sizeof(Brick);
            }
        };

        struct Page {
            std::array<std::shared_ptr<const Brick>, BricksPerChunk> Bricks;
            std::size_t Bytes{};
            static_assert(BricksPerChunk == 64);
            std::uint64_t SolidBricks{};
        };
    }

    namespace {
        using namespace field_detail;
        constexpr std::size_t DirectoryRecordBytes = sizeof(ChunkKey) + sizeof(Entry);
        constexpr std::size_t EmptyIndexBytes = sizeof(ChunkKey);
        std::atomic<std::uint64_t> NextFieldIdentity{1};

        std::uint64_t AllocateIdentity() {
            auto Identity = NextFieldIdentity.load(std::memory_order_relaxed);
            do {
                if (Identity == std::numeric_limits<std::uint64_t>::max())
                    throw std::overflow_error("Terrain field lifetime limit reached");
            } while (!NextFieldIdentity.compare_exchange_weak(Identity, Identity + 1, std::memory_order_relaxed));
            return Identity;
        }

        struct Address {
            ChunkKey Key;
            std::size_t BrickIndex;
            std::size_t SampleIndex;
            auto operator<=>(const Address &) const = default;
        };

        Address GetAddress(SamplePoint Point) noexcept {
            const auto Key = GetChunkKey(Point);
            const auto X = static_cast<std::size_t>(Point.X - Key.X * ChunkSide);
            const auto Y = static_cast<std::size_t>(Point.Y - Key.Y * ChunkSide);
            const auto Z = static_cast<std::size_t>(Point.Z - Key.Z * ChunkSide);
            const auto Brick = (Z / BrickSide * BrickGrid + Y / BrickSide) * BrickGrid + X / BrickSide;
            const auto SampleIndex = (Z % BrickSide * BrickSide + Y % BrickSide) * BrickSide + X % BrickSide;
            return {Key, Brick, SampleIndex};
        }

        Sample ReadPage(const std::shared_ptr<const Page> &Data, Address At) noexcept {
            if (!Data) return AirSample;
            const auto &BrickData = Data->Bricks[At.BrickIndex];
            return BrickData ? BrickData->Read(At.SampleIndex) : AirSample;
        }

        std::shared_ptr<const Brick> PackBrick(const std::array<Sample, BrickSamples> &Samples, BrickStorageMode Mode) {
            const auto Head = Samples.front();
            if (std::ranges::all_of(Samples, [&](Sample Value) { return Value == Head; })) {
                if (Head == AirSample) return {};
                return std::make_shared<const Brick>(Brick{Head});
            }

            auto Height = Mode == BrickStorageMode::LosslessColumns ? Brick::HeightBrick::Pack(Samples) : nullptr;
            if (Height && Height->Bytes() < sizeof(DenseBrick) + sizeof(MaterialId))
                return std::make_shared<const Brick>(Brick{std::move(Height)});
            std::array<MaterialId, BrickSamples> Materials{};
            std::ranges::transform(Samples, Materials.begin(), &Sample::Material);
            std::ranges::sort(Materials);
            const auto Unique = std::unique(Materials.begin(), Materials.end());
            auto Dense = std::make_unique<DenseBrick>();
            Dense->Palette.assign(Materials.begin(), Unique);
            if (Height && Height->Bytes() < sizeof(DenseBrick) + Dense->Palette.capacity() * sizeof(MaterialId))
                return std::make_shared<const Brick>(Brick{std::move(Height)});
            for (std::size_t I = 0; I < Samples.size(); ++I) {
                Dense->Density[I] = Samples[I].Density;
                const auto Material = std::lower_bound(Dense->Palette.begin(), Dense->Palette.end(), Samples[I].Material);
                Dense->MaterialIndices[I] = static_cast<std::uint16_t>(Material - Dense->Palette.begin());
            }
            return std::make_shared<const Brick>(Brick{std::move(Dense)});
        }

        bool IsValid(SampleBox Bounds, int Halo) noexcept {
            constexpr auto Limit = MaximumSampleCoordinate;
            if (Halo < 0 || Halo > ChunkSide) return false;
            const auto AxisValid = [Halo](std::int64_t Min, std::int64_t Max) {
                return Min >= -Limit + Halo && Max <= Limit + 1 - Halo && Min < Max;
            };
            return AxisValid(Bounds.Min.X, Bounds.MaxExclusive.X) &&
                AxisValid(Bounds.Min.Y, Bounds.MaxExclusive.Y) &&
                AxisValid(Bounds.Min.Z, Bounds.MaxExclusive.Z);
        }

        std::expected<std::size_t, FieldError> GetCount(std::int64_t X, std::int64_t Y, std::int64_t Z,
            std::size_t Limit) noexcept {
            const auto A = static_cast<std::size_t>(X);
            const auto B = static_cast<std::size_t>(Y);
            const auto C = static_cast<std::size_t>(Z);
            if (A > Limit / B || A * B > Limit / C) return std::unexpected(FieldError::LimitExceeded);
            return A * B * C;
        }

        std::expected<std::size_t, FieldError> GetEditCount(SampleBox Bounds, int Halo = 0) noexcept {
            if (!IsValid(Bounds, Halo)) return std::unexpected(FieldError::InvalidBounds);
            return GetCount(Bounds.MaxExclusive.X - Bounds.Min.X, Bounds.MaxExclusive.Y - Bounds.Min.Y,
                Bounds.MaxExclusive.Z - Bounds.Min.Z, MaximumEditSamples);
        }

        bool ValidBrush(BrushMode Mode, MaterialId Material) noexcept {
            switch (Mode) {
                case BrushMode::Add:
                case BrushMode::Paint: return Material != 0;
                case BrushMode::Subtract: return Material == 0;
            }
            return false;
        }

        std::expected<SampleBox, FieldError> BrushBounds(GridSpec Grid, WorldPoint Center, WorldPoint Radius,
            BrushMode Mode, MaterialId Material) {
            if (!Grid.IsValid() || !ValidBrush(Mode, Material)) return std::unexpected(FieldError::InvalidBrush);
            const auto Spacing = Grid.GetSpacing();
            const auto Band = Mode == BrushMode::Paint ? 0.0 : 4.0 * Spacing;
            const auto Axis = [&](double Position, double Extent) -> std::expected<std::pair<std::int64_t, std::int64_t>, FieldError> {
                if (!std::isfinite(Position) || !std::isfinite(Extent) || Extent <= 0)
                    return std::unexpected(FieldError::InvalidBrush);
                const auto First = std::floor((Position - Extent - Band) / Spacing);
                const auto Last = std::floor((Position + Extent + Band) / Spacing);
                if (First < -MaximumSampleCoordinate || Last > MaximumSampleCoordinate)
                    return std::unexpected(FieldError::InvalidCoordinate);
                return std::pair{static_cast<std::int64_t>(First), static_cast<std::int64_t>(Last) + 1};
            };
            const auto X = Axis(Center.X, Radius.X);
            if (!X) return std::unexpected(X.error());
            const auto Y = Axis(Center.Y, Radius.Y);
            if (!Y) return std::unexpected(Y.error());
            const auto Z = Axis(Center.Z, Radius.Z);
            if (!Z) return std::unexpected(Z.error());
            const SampleBox Bounds{{X->first, Y->first, Z->first}, {X->second, Y->second, Z->second}};
            if (const auto Count = GetEditCount(Bounds); !Count) return std::unexpected(Count.error());
            return Bounds;
        }

        std::int16_t Quantize(double Distance, double Band) noexcept {
            const auto Rounded = std::lround(std::clamp(Distance / Band, -1.0, 1.0) * SaturatedDensity);
            return static_cast<std::int16_t>(Rounded != 0 ? Rounded : Distance < 0.0 ? -1 : 1);
        }

        template<class Transform>
        std::expected<PreparedEdit, FieldError> PrepareRegion(Snapshot Base, SampleBox Bounds, int Halo, Transform Edit) {
            const auto Count = GetEditCount(Bounds, Halo);
            if (!Count) return std::unexpected(Count.error());
            const auto Keys = GetRegionChunks(Bounds, Halo);
            if (!Keys) return std::unexpected(Keys.error());
            for (auto Key : *Keys) {
                if (const auto Stamp = Base.GetVersion(Key); !Stamp) return std::unexpected(Stamp.error());
            }
            try {
                std::vector<SampleWrite> Writes;
                Writes.reserve(*Count);
                for (auto Z = Bounds.Min.Z; Z < Bounds.MaxExclusive.Z; ++Z)
                    for (auto Y = Bounds.Min.Y; Y < Bounds.MaxExclusive.Y; ++Y)
                        for (auto X = Bounds.Min.X; X < Bounds.MaxExclusive.X; ++X) {
                            const SamplePoint Point{X, Y, Z};
                            const Sample Previous = *Base.Read(Point);
                            const Sample Next = Edit(Base, Point, Previous);
                            if (Previous != Next) Writes.push_back({Point, Next});
                        }
                return Field::Prepare(std::move(Base), Writes);
            } catch (const std::bad_alloc &) {
                return std::unexpected(FieldError::OutOfMemory);
            }
        }

        template<class Distance>
        std::expected<PreparedEdit, FieldError> PrepareBrush(Snapshot Base, SampleBox Bounds,
            BrushMode Mode, MaterialId Material, Distance GetDistance) {
            const double Spacing = Base.GetGrid().GetSpacing();
            return PrepareRegion(std::move(Base), Bounds, 0, [=](const Snapshot &, SamplePoint Point, Sample Old) {
                const double D = GetDistance({Point.X * Spacing, Point.Y * Spacing, Point.Z * Spacing});
                if (Mode == BrushMode::Paint) {
                    return D <= 0 && Old.Density < 0 ? Sample{Old.Density, Material} : Old;
                }
                const auto Density = Mode == BrushMode::Add
                    ? std::min(Old.Density, Quantize(D, 4 * Spacing))
                    : std::max(Old.Density, Quantize(-D, 4 * Spacing));
                return Sample{Density, Density >= 0 ? 0 : (Old.Material != 0 ? Old.Material : Material)};
            });
        }

        std::size_t GetBytes(const Entry &Value) noexcept {
            return Value.Data ? Value.Data->Bytes : 0;
        }
    }

    std::string_view GetFieldErrorText(FieldError Error) noexcept {
        switch (Error) {
            case FieldError::InvalidCoordinate: return "Terrain sample coordinate is out of range";
            case FieldError::InvalidBounds: return "Terrain region bounds are invalid";
            case FieldError::InvalidSample: return "Terrain density or material encoding is invalid";
            case FieldError::InvalidBrush: return "Terrain brush parameters are invalid";
            case FieldError::UncapturedDependency: return "Terrain snapshot does not include the required chunk";
            case FieldError::DuplicateSample: return "Terrain batch writes one sample more than once";
            case FieldError::LimitExceeded: return "Terrain operation exceeds its configured budget";
            case FieldError::OutOfMemory: return "Terrain operation could not reserve memory";
            case FieldError::StaleDependency: return "Terrain dependencies changed before commit";
            case FieldError::WrongField: return "Terrain candidate belongs to a different field lifetime";
            case FieldError::RevisionExhausted: return "Terrain revision limit reached";
        }
        return "Unknown terrain field error";
    }

    const Entry *Snapshot::Find(ChunkKey Key) const noexcept {
        const auto Found = std::ranges::lower_bound(Entries, Key, {}, &Record::first);
        return Found != Entries.end() && Found->first == Key ? &Found->second : nullptr;
    }

    std::expected<Sample, FieldError> Snapshot::Read(SamplePoint Point) const noexcept {
        if (!terrain::IsValid(Point)) return std::unexpected(FieldError::InvalidCoordinate);
        const auto At = GetAddress(Point);
        const auto *Entry = Find(At.Key);
        if (!Entry) return std::unexpected(FieldError::UncapturedDependency);
        return ReadPage(Entry->Data, At);
    }

    std::expected<ChunkVersion, FieldError> Snapshot::GetVersion(ChunkKey Key) const noexcept {
        if (!terrain::IsValid(Key)) return std::unexpected(FieldError::InvalidCoordinate);
        if (const auto *Entry = Find(Key)) return Entry->Version;
        return std::unexpected(FieldError::UncapturedDependency);
    }

    std::expected<bool, FieldError> Snapshot::IsMaterialized(ChunkKey Key) const noexcept {
        if (!terrain::IsValid(Key)) return std::unexpected(FieldError::InvalidCoordinate);
        if (const auto *Entry = Find(Key)) return static_cast<bool>(Entry->Data);
        return std::unexpected(FieldError::UncapturedDependency);
    }

    std::expected<bool, FieldError> Snapshot::ContainsSolid(ChunkKey Key) const noexcept {
        if (!terrain::IsValid(Key)) return std::unexpected(FieldError::InvalidCoordinate);
        if (const auto *Entry = Find(Key)) return Entry->Data && Entry->Data->SolidBricks != 0;
        return std::unexpected(FieldError::UncapturedDependency);
    }

    std::optional<Snapshot::ChunkInfo> Snapshot::GetChunkAt(std::size_t Index) const noexcept {
        if (Index >= Entries.size())
            return std::nullopt;
        const auto &[Key, Entry] = Entries[Index];
        return ChunkInfo{Key, Entry.Version};
    }

    std::expected<std::optional<Sample>, FieldError> Snapshot::ReadBrick(ChunkKey Key, std::size_t BrickIndex,
        std::span<Sample, BrickSide * BrickSide * BrickSide> Mixed) const noexcept {
        if (!terrain::IsValid(Key) || BrickIndex >= BricksPerChunk)
            return std::unexpected(FieldError::InvalidCoordinate);
        const auto *Owner = Find(Key);
        if (!Owner)
            return std::unexpected(FieldError::UncapturedDependency);
        if (!Owner->Data || !Owner->Data->Bricks[BrickIndex])
            return std::optional{AirSample};
        const auto &Brick = *Owner->Data->Bricks[BrickIndex];
        if (const auto *Constant = std::get_if<Sample>(&Brick.Storage)) return std::optional{*Constant};
        if (const auto *Dense = std::get_if<std::unique_ptr<const DenseBrick>>(&Brick.Storage)) {
            for (std::size_t I = 0; I < Mixed.size(); ++I)
                Mixed[I] = {(*Dense)->Density[I], (*Dense)->Palette[(*Dense)->MaterialIndices[I]]};
        } else {
            const auto &Height = *std::get<std::unique_ptr<const field_detail::Brick::HeightBrick>>(Brick.Storage);
            std::size_t I = 0;
            for (int Z = 0; Z < BrickSide; ++Z)
                for (int Y = 0; Y < BrickSide; ++Y)
                    for (int X = 0; X < BrickSide; ++X)
                        Mixed[I++] = Height.Read(X, Y, Z);
        }
        return std::optional<Sample>{};
    }

    PreparedEdit::PreparedEdit(Snapshot Base) : Preview(std::move(Base)) {}

    std::expected<std::vector<ChunkKey>, FieldError> GetRegionChunks(SampleBox Bounds, int Halo) {
        if (!IsValid(Bounds, Halo)) return std::unexpected(FieldError::InvalidBounds);
        const auto Min = GetChunkKey({Bounds.Min.X - Halo, Bounds.Min.Y - Halo, Bounds.Min.Z - Halo});
        const auto Max =
            GetChunkKey({Bounds.MaxExclusive.X - 1 + Halo, Bounds.MaxExclusive.Y - 1 + Halo, Bounds.MaxExclusive.Z - 1 + Halo});
        const auto Count = GetCount(Max.X - Min.X + 1, Max.Y - Min.Y + 1, Max.Z - Min.Z + 1, MaximumCapturedChunks);
        if (!Count) return std::unexpected(Count.error());
        try {
            std::vector<ChunkKey> Result;
            Result.reserve(*Count);
            for (auto X = Min.X; X <= Max.X; ++X)
                for (auto Y = Min.Y; Y <= Max.Y; ++Y)
                    for (auto Z = Min.Z; Z <= Max.Z; ++Z)
                        Result.push_back({X, Y, Z});
            return Result;
        } catch (const std::bad_alloc &) {
            return std::unexpected(FieldError::OutOfMemory);
        }
    }

    std::expected<SampleBox, FieldError> GetBrushBounds(GridSpec Grid, const SphereBrush &Brush) {
        return BrushBounds(Grid, Brush.Center, {Brush.Radius, Brush.Radius, Brush.Radius}, Brush.Mode, Brush.Material);
    }

    std::expected<SampleBox, FieldError> GetBrushBounds(GridSpec Grid, const BoxBrush &Brush) {
        return BrushBounds(Grid, Brush.Center, Brush.HalfExtent, Brush.Mode, Brush.Material);
    }

    Field::Field(GridSpec Spec, FieldLimits Budget)
        : Grid(Spec), Limits(Budget), Identity(AllocateIdentity()), Lease(std::make_shared<const SnapshotLease>()) {
        if (!Grid.IsValid() || Limits.DirectoryEntries == 0 || Limits.OwnedBytes < DirectoryRecordBytes)
            throw std::invalid_argument("Invalid terrain grid or field limits");
    }

    Entry Field::GetEntry(ChunkKey Key) const noexcept {
        const auto Found = Directory.find(Key);
        return Found == Directory.end() ? Entry{} : Found->second;
    }

    std::expected<Sample, FieldError> Field::Read(SamplePoint Point) const noexcept {
        if (!terrain::IsValid(Point)) return std::unexpected(FieldError::InvalidCoordinate);
        const auto At = GetAddress(Point);
        return ReadPage(GetEntry(At.Key).Data, At);
    }

    std::expected<ChunkVersion, FieldError> Field::GetVersion(ChunkKey Key) const noexcept {
        if (!terrain::IsValid(Key)) return std::unexpected(FieldError::InvalidCoordinate);
        const auto Found = Directory.find(Key);
        return Found == Directory.end() ? ChunkVersion{} : Found->second.Version;
    }

    std::expected<bool, FieldError> Field::IsMaterialized(ChunkKey Key) const noexcept {
        if (!terrain::IsValid(Key)) return std::unexpected(FieldError::InvalidCoordinate);
        return static_cast<bool>(GetEntry(Key).Data);
    }

    std::expected<bool, FieldError> Field::ContainsSolid(ChunkKey Key) const noexcept {
        if (!terrain::IsValid(Key)) return std::unexpected(FieldError::InvalidCoordinate);
        const auto Entry = GetEntry(Key);
        return Entry.Data && Entry.Data->SolidBricks != 0;
    }

    std::expected<Snapshot, FieldError> Field::Capture(std::span<const ChunkKey> Keys) const {
        G_PROFILE("Terrain::Capture");
        if (Keys.size() > MaximumCapturedChunks) return std::unexpected(FieldError::LimitExceeded);
        try {
            Snapshot Result;
            Result.Grid = Grid;
            Result.BrickStorage = Limits.BrickStorage;
            Result.CapturedRevision = CurrentRevision;
            Result.FieldIdentity = Identity;
            Result.Lease = Lease;
            Result.Entries.reserve(Keys.size());
            for (const auto Key : Keys) {
                if (!terrain::IsValid(Key)) return std::unexpected(FieldError::InvalidCoordinate);
                Result.Entries.emplace_back(Key, GetEntry(Key));
            }
            using Record = Snapshot::Record;
            std::ranges::sort(Result.Entries, {}, &Record::first);
            const auto Duplicates = std::ranges::unique(Result.Entries, {}, &Record::first);
            Result.Entries.erase(Duplicates.begin(), Duplicates.end());
            return Result;
        } catch (const std::bad_alloc &) {
            return std::unexpected(FieldError::OutOfMemory);
        }
    }

    std::expected<CommittedFieldCapture, FieldError> Field::CaptureMaterialized(std::size_t Maximum) const {
        G_PROFILE("Terrain::CaptureMaterialized");
        if (Stats.MaterializedPages > Maximum)
            return std::unexpected(FieldError::LimitExceeded);
        try {
            CommittedFieldCapture Capture;
            Capture.FieldLifetime = Identity;
            auto &Result = Capture.SnapshotValue;
            Result.Grid = Grid;
            Result.BrickStorage = Limits.BrickStorage;
            Result.CapturedRevision = CurrentRevision;
            Result.FieldIdentity = Identity;
            Result.Lease = Lease;
            Result.Entries.reserve(Stats.MaterializedPages);
            for (const auto &Record : Directory)
                if (Record.second.Data)
                    Result.Entries.emplace_back(Record);
            return Capture;
        } catch (const std::bad_alloc &) {
            return std::unexpected(FieldError::OutOfMemory);
        }
    }

    std::expected<std::vector<ChunkKey>, FieldError> Field::GetMaterializedChunkKeys(std::size_t Maximum) const {
        if (Stats.MaterializedPages > Maximum)
            return std::unexpected(FieldError::LimitExceeded);
        try {
            std::vector<ChunkKey> Result;
            Result.reserve(Stats.MaterializedPages);
            for (const auto &[Key, Entry] : Directory)
                if (Entry.Data)
                    Result.push_back(Key);
            return Result;
        } catch (const std::bad_alloc &) {
            return std::unexpected(FieldError::OutOfMemory);
        }
    }

    bool Field::IsCurrent(const Snapshot &Base) const noexcept {
        if (Base.FieldIdentity != Identity) return false;
        return std::ranges::all_of(Base.Entries, [this](const auto &Record) {
            return GetEntry(Record.first).Version == Record.second.Version;
        });
    }

    std::size_t Field::RetireEmptyEntries(std::size_t Maximum) noexcept {
        if (Lease.use_count() != 1) return 0;
        const auto Count = std::min({Maximum, EmptyEntries.size(), std::size_t{128}});
        for (std::size_t I = 0; I < Count; ++I) {
            const auto Key = EmptyEntries.begin();
            Directory.erase(*Key);
            EmptyEntries.erase(Key);
        }
        Stats.Entries -= Count;
        Stats.OwnedBytes -= Count * (DirectoryRecordBytes + EmptyIndexBytes);
        return Count;
    }

    std::expected<PreparedEdit, FieldError> Field::Prepare(Snapshot Base, std::span<const SampleWrite> Writes) {
        G_PROFILE("Terrain::Prepare");
        if (Base.FieldIdentity == 0) return std::unexpected(FieldError::WrongField);
        if (Writes.size() > MaximumEditSamples) return std::unexpected(FieldError::LimitExceeded);
        try {
            struct AddressedWrite {
                Address At;
                Sample Value;
            };
            std::vector<AddressedWrite> Ordered;
            Ordered.reserve(Writes.size());
            for (const auto &Write : Writes) {
                if (!terrain::IsValid(Write.Point)) return std::unexpected(FieldError::InvalidCoordinate);
                if (!terrain::IsValid(Write.Value)) return std::unexpected(FieldError::InvalidSample);
                Ordered.push_back({GetAddress(Write.Point), Write.Value});
            }
            std::ranges::sort(Ordered, {}, &AddressedWrite::At);
            if (std::ranges::adjacent_find(Ordered, {}, &AddressedWrite::At) != Ordered.end())
                return std::unexpected(FieldError::DuplicateSample);

            PreparedEdit Result(std::move(Base));
            auto &Entries = Result.Preview.Entries;
            Result.Expected.reserve(Entries.size());
            Result.Changes.reserve(Entries.size());
            for (const auto &Entry : Entries) Result.Expected.push_back(Entry.second.Version);

            auto Next = Ordered.begin();
            while (Next != Ordered.end()) {
                const ChunkKey Key = Next->At.Key;
                auto Target = std::ranges::lower_bound(Entries, Key, {}, &Snapshot::Record::first);
                if (Target == Entries.end() || Target->first != Key)
                    return std::unexpected(FieldError::UncapturedDependency);
                ChangedChunk Change{Key};
                const auto &Original = Target->second.Data;
                std::shared_ptr<Page> Modified;

                while (Next != Ordered.end() && Next->At.Key == Key) {
                    const auto BrickIndex = Next->At.BrickIndex;
                    const auto *OriginalBrick = Original ? Original->Bricks[BrickIndex].get() : nullptr;
                    std::array<Sample, BrickSamples> Samples;
                    bool BrickChanged = false;
                    while (Next != Ordered.end() && Next->At.Key == Key && Next->At.BrickIndex == BrickIndex) {
                        const auto SampleIndex = Next->At.SampleIndex;
                        const auto Previous = OriginalBrick ? OriginalBrick->Read(SampleIndex) : AirSample;
                        const auto Replacement = Next->Value;
                        if (Previous != Replacement) {
                            if (!BrickChanged) {
                                for (std::size_t I = 0; I < Samples.size(); ++I)
                                    Samples[I] = OriginalBrick ? OriginalBrick->Read(I) : AirSample;
                                BrickChanged = true;
                            }
                            Samples[SampleIndex] = Replacement;
                            Change.Geometry |= Previous.Density != Replacement.Density;
                            Change.Material |= Previous.Material != Replacement.Material;
                        }
                        ++Next;
                    }
                    if (BrickChanged) {
                        if (!Modified) Modified = Original ? std::make_shared<Page>(*Original) : std::make_shared<Page>();
                        Modified->Bricks[BrickIndex] = PackBrick(Samples, Result.Preview.BrickStorage);
                        const auto Mask = std::uint64_t{1} << BrickIndex;
                        if (std::ranges::any_of(Samples, [](Sample Value) { return Value.Density < 0; }))
                            Modified->SolidBricks |= Mask;
                        else
                            Modified->SolidBricks &= ~Mask;
                    }
                }
                if (!Modified) continue;
                std::size_t BrickBytes = 0;
                for (const auto &Brick : Modified->Bricks) if (Brick) BrickBytes += Brick->Bytes();
                Modified->Bytes = sizeof(Page) + BrickBytes;
                if (BrickBytes == 0) Modified.reset();
                const auto Bytes = DirectoryRecordBytes + (Modified ? Modified->Bytes : 0);
                if (Bytes > MaximumPreparedBytes - Result.ReservedBytes)
                    return std::unexpected(FieldError::LimitExceeded);
                Result.ReservedBytes += Bytes;
                Target->second.Data = std::move(Modified);
                Result.Changes.push_back(Change);
            }
            return Result;
        } catch (const std::bad_alloc &) {
            return std::unexpected(FieldError::OutOfMemory);
        }
    }

    std::expected<PreparedEdit, FieldError> Field::Prepare(Snapshot Base, const SphereBrush &Brush) {
        const auto Bounds = GetBrushBounds(Base.GetGrid(), Brush);
        if (!Bounds) return std::unexpected(Bounds.error());
        return PrepareBrush(std::move(Base), *Bounds, Brush.Mode, Brush.Material, [Brush](WorldPoint Point) {
            const auto X = Point.X - Brush.Center.X, Y = Point.Y - Brush.Center.Y, Z = Point.Z - Brush.Center.Z;
            return std::sqrt(X*X + Y*Y + Z*Z) - Brush.Radius;
        });
    }

    std::expected<PreparedEdit, FieldError> Field::Prepare(Snapshot Base, const BoxBrush &Brush) {
        const auto Bounds = GetBrushBounds(Base.GetGrid(), Brush);
        if (!Bounds) return std::unexpected(Bounds.error());
        return PrepareBrush(std::move(Base), *Bounds, Brush.Mode, Brush.Material, [Brush](WorldPoint Point) {
            const auto X = std::abs(Point.X - Brush.Center.X) - Brush.HalfExtent.X;
            const auto Y = std::abs(Point.Y - Brush.Center.Y) - Brush.HalfExtent.Y;
            const auto Z = std::abs(Point.Z - Brush.Center.Z) - Brush.HalfExtent.Z;
            const auto A = std::max(0.0, X), B = std::max(0.0, Y), C = std::max(0.0, Z);
            return std::sqrt(A*A + B*B + C*C) + std::min(0.0, std::max({X, Y, Z}));
        });
    }

    std::expected<PreparedEdit, FieldError> Field::PrepareSmooth(Snapshot Base, SampleBox Bounds, std::uint8_t Strength) {
        return PrepareRegion(std::move(Base), Bounds, 1, [Strength](const Snapshot &Source, SamplePoint Point, Sample Old) {
            const auto [X, Y, Z] = Point;
            const std::array<SamplePoint, 6> Neighbors{{
                {X - 1, Y, Z}, {X + 1, Y, Z}, {X, Y - 1, Z}, {X, Y + 1, Z}, {X, Y, Z - 1}, {X, Y, Z + 1}
            }};
            std::int32_t Sum = 0;
            MaterialId Material = Old.Material;
            for (const auto Neighbor : Neighbors) {
                const auto Value = *Source.Read(Neighbor);
                Sum += Value.Density;
                if (!Material) Material = Value.Material;
            }
            const auto Density = static_cast<std::int16_t>(
                (Old.Density * (255 - Strength) * 6 + Sum * Strength) / (255 * 6));
            return Sample{Density, Density < 0 ? Material : 0};
        });
    }

    std::expected<CommitResult, FieldError> Field::Commit(PreparedEdit &&Candidate) {
        G_PROFILE("Terrain::Commit");
        auto &Preview = Candidate.Preview;
        if (Preview.FieldIdentity != Identity) return std::unexpected(FieldError::WrongField);
        if (Preview.Entries.size() != Candidate.Expected.size()) return std::unexpected(FieldError::WrongField);
        for (std::size_t I = 0; I < Preview.Entries.size(); ++I) {
            if (GetEntry(Preview.Entries[I].first).Version != Candidate.Expected[I])
                return std::unexpected(FieldError::StaleDependency);
        }
        if (Candidate.Changes.empty()) {
            Preview.FieldIdentity = 0;
            Preview.Lease.reset();
            return CommitResult{CurrentRevision, {}};
        }
        if (CurrentRevision == std::numeric_limits<Revision>::max())
            return std::unexpected(FieldError::RevisionExhausted);

        try {
            // Allocate all new map nodes before making changes to the live directory.
            std::map<ChunkKey, Entry> NewNodes;
            std::set<ChunkKey> NewEmptyEntries;
            auto NextStats = Stats;
            const Revision NextRevision = CurrentRevision + 1;
            for (const auto &Change : Candidate.Changes) {
                const auto Previous = Directory.find(Change.Key);
                if (Previous != Directory.end()) {
                    NextStats.OwnedBytes -= GetBytes(Previous->second);
                    if (!Previous->second.Data) NextStats.OwnedBytes -= EmptyIndexBytes;
                    NextStats.MaterializedPages -= Previous->second.Data != nullptr;
                }
            }
            for (const auto &Change : Candidate.Changes) {
                const auto &Next = *Preview.Find(Change.Key);
                if (!Directory.contains(Change.Key)) {
                    if (NextStats.Entries == Limits.DirectoryEntries ||
                        DirectoryRecordBytes > Limits.OwnedBytes - NextStats.OwnedBytes)
                        return std::unexpected(FieldError::LimitExceeded);
                    ++NextStats.Entries;
                    NextStats.OwnedBytes += DirectoryRecordBytes;
                    NewNodes.emplace(Change.Key, Next);
                }
                if (GetBytes(Next) > Limits.OwnedBytes - NextStats.OwnedBytes)
                    return std::unexpected(FieldError::LimitExceeded);
                NextStats.OwnedBytes += GetBytes(Next);
                NextStats.MaterializedPages += Next.Data != nullptr;
                if (!Next.Data) {
                    if (EmptyIndexBytes > Limits.OwnedBytes - NextStats.OwnedBytes)
                        return std::unexpected(FieldError::LimitExceeded);
                    NextStats.OwnedBytes += EmptyIndexBytes;
                    if (!EmptyEntries.contains(Change.Key)) NewEmptyEntries.insert(Change.Key);
                }
            }

            for (const auto &Change : Candidate.Changes) {
                auto Node = Directory.find(Change.Key);
                if (Node == Directory.end())
                    Node = Directory.insert(NewNodes.extract(Change.Key)).position;
                Node->second.Data = Preview.Find(Change.Key)->Data;
                if (Node->second.Data) EmptyEntries.erase(Change.Key);
                if (Change.Geometry) Node->second.Version.Geometry = NextRevision;
                if (Change.Material) Node->second.Version.Material = NextRevision;
            }
            EmptyEntries.merge(NewEmptyEntries);
            Stats = NextStats;
            CurrentRevision = NextRevision;
            Preview.FieldIdentity = 0;
            Preview.Lease.reset();
            return CommitResult{CurrentRevision, std::move(Candidate.Changes)};
        } catch (const std::bad_alloc &) {
            return std::unexpected(FieldError::OutOfMemory);
        }
    }
}
