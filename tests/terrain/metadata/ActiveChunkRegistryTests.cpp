// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0

#include "gargantuan/terrain/ActiveChunkRegistry.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
    using gargantuan::terrain::ChunkKey;
    using gargantuan::terrain::ChunkVersion;
    using gargantuan::terrain::detail::ActiveChunkRegistry;

    void Verify(bool Condition, std::string_view Description) {
        if (!Condition) {
            std::cerr << "[Terrain:Metadata] " << Description << '\n';
            std::exit(2);
        }
    }
}

int main() {
    const ChunkKey First{-2, 7, -11};
    const ChunkKey Second{-2, 8, -11};
    const ChunkKey Third{-2, 9, -11};

    ActiveChunkRegistry Registry(2, 41);
    Verify(Registry.GetCount() == 0 && !Registry.Capture(First) && !Registry.GetAt(0), "empty registry");
    Verify(!Registry.Activate({1LL << 40, 0, 0}), "invalid coordinates");

    const auto Original = Registry.Activate(First, {3, 5});
    const auto Neighbor = Registry.Activate(Second, {6, 8});
    Verify(Original && Neighbor && Registry.IsCurrent(*Original) && Registry.IsCurrent(*Neighbor), "current tickets");
    Verify(Registry.Activate(First, {3, 5}) == Original, "idempotent activation");
    Verify(!Registry.Activate(First, {3, 6}) && !Registry.Activate(Third), "conflicting or overbudget activation");
    Verify(!Registry.Update(First, {2, 9}) && Registry.IsCurrent(*Original), "atomic rejection of old versions");

    Verify(Registry.Update(First, {3, 6}) && !Registry.IsCurrent(*Original), "material revision rejects old ticket");
    const auto Painted = Registry.Capture(First);
    Verify(Painted && Painted->Version == ChunkVersion{3, 6}, "paint leaves geometry revision stable");
    Verify(Registry.Update(First, {4, 6}) && !Registry.IsCurrent(*Painted), "geometry revision rejects old ticket");

    const auto Current = Registry.Capture(First);
    Verify(Current && Registry.Retire(First) && !Registry.Retire(First), "retirement");
    Verify(!Registry.IsCurrent(*Current), "retired ticket");
    Verify(Registry.GetAt(0) == Neighbor && Registry.IsCurrent(*Neighbor), "swap removal preserves neighbor address");

    const auto Reloaded = Registry.Activate(First, {4, 6});
    Verify(Reloaded && Reloaded->Generation != Current->Generation && !Registry.IsCurrent(*Current),
        "reloading same coordinate and revisions cannot reuse generation");
    Verify(!Registry.Reset(40) && !Registry.Reset(41) && Registry.IsCurrent(*Reloaded), "old epoch rejected");
    Verify(Registry.Reset(42) && Registry.GetCount() == 0 && !Registry.IsCurrent(*Reloaded), "reset cancels prior epoch");

    const auto AfterReset = Registry.Activate(First, {4, 6});
    ActiveChunkRegistry Other(2, 42);
    const auto OtherTicket = Other.Activate(First, {4, 6});
    Verify(AfterReset && OtherTicket && Registry.IsCurrent(*AfterReset) && Other.IsCurrent(*OtherTicket),
        "current registry after reset");
    Verify(!Other.IsCurrent(*AfterReset) && !Registry.IsCurrent(*OtherTicket), "same epoch independent registry identity");

    ActiveChunkRegistry NoEpoch(1, 0);
    ActiveChunkRegistry NoCapacity(0, 1);
    Verify(!NoEpoch.Activate(First) && !NoCapacity.Activate(First), "disabled configurations");
    Verify(NoEpoch.Reset(1) && NoEpoch.Activate(First).has_value(), "admit initial world epoch");
    std::cout << "[Terrain:Metadata] active registry checks passed\n";
}
