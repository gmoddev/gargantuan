// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/terrain/Field.hpp"

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace gargantuan::terrain;

namespace {
    void Require(bool Condition, const char *Message) {
        if (!Condition) throw std::runtime_error(Message);
    }

    Snapshot Capture(Field &Target, ChunkKey Key) {
        return Target.Capture(std::array{Key}).value();
    }

    PreparedEdit PrepareWrites(Field &Target, std::span<const SampleWrite> Writes) {
        std::vector<ChunkKey> Keys;
        for (const auto &Write : Writes) Keys.push_back(GetChunkKey(Write.Point));
        return Field::Prepare(Target.Capture(Keys).value(), Writes).value();
    }

    void TestAddressingAndRetention() {
        static_assert(FloorChunk(-33) == -2 && FloorChunk(-32) == -1 && FloorChunk(-1) == -1);
        static_assert(FloorChunk(31) == 0 && FloorChunk(32) == 1);
        Field World;
        const std::array Writes{
            SampleWrite{{-32, -1, 0}, {-123, 54321}},
            SampleWrite{{-1, 0, 0}, {-60, 123456}},
            SampleWrite{{0, 0, 0}, {-100, 123456}},
            SampleWrite{{32, 0, 0}, {-200, 654321}},
        };
        auto Old = Capture(World, {0, 0, 0});
        auto Candidate = PrepareWrites(World, Writes);
        Require(Candidate.GetPreview().Read({-32, -1, 0}) == Writes[0].Value, "negative preview");
        Require(World.Read({-32, -1, 0}) == AirSample, "prepare leaves live field intact");
        Require(World.Commit(std::move(Candidate))->Changed.size() == 4, "four unique chunk addresses");
        Require(!World.Commit(std::move(Candidate)), "a committed candidate is consumed");
        for (const auto &Write : Writes) Require(World.Read(Write.Point) == Write.Value, "exact sample retained");
        Require(Old.Read({0, 0, 0}) == AirSample && !World.IsCurrent(Old), "old snapshot remains immutable");
        Require(Old.Read({32, 0, 0}).error() == FieldError::UncapturedDependency, "uncaptured is distinguishable from air");
        Require(World.Read({std::numeric_limits<std::int64_t>::min(), 0, 0}).error() ==
            FieldError::InvalidCoordinate, "overflow coordinate rejected");
    }

    void TestOptimisticCommit() {
        Field World;
        Field DifferentWorld;
        const SamplePoint A{1, 1, 1};
        const SamplePoint B{33, 1, 1};
        auto Stale = PrepareWrites(World, std::array{SampleWrite{A, {-11, 12}}});
        auto AEdit = PrepareWrites(World, std::array{SampleWrite{A, {-19, 16}}});
        auto BEdit = PrepareWrites(World, std::array{SampleWrite{B, {-23, 19}}});
        Require(DifferentWorld.Commit(std::move(AEdit)).error() == FieldError::WrongField, "wrong lifetime rejected");
        Require(World.Commit(std::move(AEdit))->CommittedRevision == 1, "first commit");
        Require(World.Commit(std::move(BEdit))->CommittedRevision == 2, "disjoint commit");
        Require(World.Commit(std::move(Stale)).error() == FieldError::StaleDependency, "overlapping stale commit");

        const auto Keys = std::array{GetChunkKey(A), GetChunkKey(B)};
        auto Dependent = Field::Prepare(World.Capture(Keys).value(), std::array{SampleWrite{A, {-19, 5000}}}).value();
        auto Clear = PrepareWrites(World, std::array{SampleWrite{B, AirSample}});
        Require(World.Commit(std::move(Clear))->CommittedRevision == 3, "clear retains version");
        Require(World.Commit(std::move(Dependent)).error() == FieldError::StaleDependency, "unchanged write halo invalidated");
        Require(World.GetStats().Entries == 2 && World.GetStats().MaterializedPages == 1, "empty version record");

        auto Paint = PrepareWrites(World, std::array{SampleWrite{A, {-19, 5000}}});
        const auto New = World.Commit(std::move(Paint)).value();
        Require(New.Changed.size() == 1 && !New.Changed[0].Geometry && New.Changed[0].Material, "paint revision domain");
        const auto Version = Capture(World, GetChunkKey(A)).GetVersion(GetChunkKey(A)).value();
        Require(Version.Geometry == 1 && Version.Material == 4, "geometry unchanged by paint");
        auto NoOp = PrepareWrites(World, std::array{SampleWrite{A, {-19, 5000}}});
        Require(World.Commit(std::move(NoOp))->CommittedRevision == 4, "no-op has no revision");
    }

    void TestBoundsAndFailure() {
        Field World({}, {1, 3000});
        const std::array Writes{SampleWrite{{1, 1, 1}, {-3, 1}}, SampleWrite{{33, 1, 1}, {-6, 2}}};
        auto Candidate = PrepareWrites(World, Writes);
        Require(World.Commit(std::move(Candidate)).error() == FieldError::LimitExceeded, "over budget");
        Require(World.GetRevision() == 0 && World.GetStats().Entries == 0 && World.Read({1, 1, 1}) == AirSample,
            "denied batch installs no page");
        const std::array Invalid{SampleWrite{{1, 1, 1}, {-1, 0}}};
        const std::array Duplicate{Writes.front(), Writes.front()};
        Require(Field::Prepare(Capture(World, {0, 0, 0}), Invalid).error() == FieldError::InvalidSample, "material sign rule");
        Require(Field::Prepare(Capture(World, {0, 0, 0}), Duplicate).error() == FieldError::DuplicateSample, "duplicate denied");
        Require(GetRegionChunks({{-MaximumSampleCoordinate, 0, 0}, {0, 1, 1}}, 1).error() ==
            FieldError::InvalidBounds, "halo checked");
        Require(GetRegionChunks({{-MaximumSampleCoordinate, 0, 0}, {0, 1, 1}}).error() ==
            FieldError::LimitExceeded, "bounded region key enumeration");
        const SphereBrush InvalidSphere{{0, 0, 0}, std::numeric_limits<double>::infinity(), BrushMode::Add, 1};
        Require(GetBrushBounds(World.GetGrid(), InvalidSphere).error() == FieldError::InvalidBrush, "non-finite radius");
    }

    void TestTombstoneRetirement() {
        Field World({}, {1, 4096});
        const SamplePoint At{0, 1, 0};
        {
            const auto Empty = Capture(World, GetChunkKey(At));
            auto Fill = PrepareWrites(World, std::array{SampleWrite{At, {-102, 65}}});
            Require(World.Commit(std::move(Fill)).has_value(), "create populated entry");
            auto Clear = PrepareWrites(World, std::array{SampleWrite{At, AirSample}});
            Require(World.Commit(std::move(Clear)).has_value(), "clear populated entry");
            Require(World.RetireEmptyEntries() == 0 && !World.IsCurrent(Empty), "active snapshot preserves ABA guard");
        }
        Require(World.RetireEmptyEntries() == 1 && World.GetStats().Entries == 0 && World.GetStats().OwnedBytes == 0,
            "empty version retired with no captures");
        auto Next = PrepareWrites(World, std::array{SampleWrite{{33, 0, 0}, {-111, 65}}});
        Require(World.Commit(std::move(Next))->CommittedRevision == 3, "directory capacity reused without resetting field revision");
    }

    void TestBrushesAndSharedReads() {
        Field World(GridSpec{-1});
        const BoxBrush Add{{0, 0, 0}, {2, 2, 2}, BrushMode::Add, 97865};
        const auto Bounds = GetBrushBounds(World.GetGrid(), Add).value();
        const auto Keys = GetRegionChunks(Bounds).value();
        auto Fill = Field::Prepare(World.Capture(Keys).value(), Add).value();
        Require(Fill.GetChanges().size() == 8 && World.Commit(std::move(Fill)).has_value(), "cross-boundary box fill");
        Require(World.Read({0, 0, 0})->Material == Add.Material, "box solid interior");
        Require(World.Read({4, 0, 0})->Density > 0, "surface lattice tie is air");
        const auto Before = World.Capture(Keys).value();
        const SphereBrush Cut{{0, 0, 0}, 1.0, BrushMode::Subtract, 0};
        auto CutEdit = Field::Prepare(World.Capture(Keys).value(), Cut).value();
        auto Concurrent = std::jthread([Before, Material=Add.Material] {
            Require(Before.Read({0, 0, 0})->Material == Material, "worker snapshot remains immutable");
        });
        Require(World.Commit(std::move(CutEdit)).has_value(), "subtract commits");
        Concurrent.join();
        Require(World.Read({0, 0, 0})->Density > 0 && World.Read({3, 0, 0})->Material == Add.Material,
            "excavation preserves material of remaining solid");
        const auto SmoothRegion = SampleBox{{-2, -2, -2}, {3, 3, 3}};
        auto Smooth = Field::PrepareSmooth(World.Capture(GetRegionChunks(SmoothRegion, 1).value()).value(),
            SmoothRegion, 128).value();
        Require(World.Commit(std::move(Smooth)).has_value(), "smooth captures halo dependencies");
    }
}

int main() try {
    TestAddressingAndRetention();
    TestOptimisticCommit();
    TestBoundsAndFailure();
    TestTombstoneRetirement();
    TestBrushesAndSharedReads();
    std::cout << "[Terrain:FieldTests] Passed\n";
    return 0;
} catch (const std::exception &Error) {
    std::cerr << "[Terrain:FieldTests] " << Error.what() << '\n';
    return 1;
}
