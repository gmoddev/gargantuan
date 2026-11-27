// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "terrain/VisualDensityPrefetch.hpp"
#include "gargantuan/runtime/JobSystem.hpp"

#include <algorithm>

namespace gargantuan::terrain {
    namespace {
        bool SameSource(const std::shared_ptr<const SurfaceMesh> &A,
            const std::shared_ptr<const SurfaceMesh> &B) noexcept {
            return A && A.get() == B.get() && !A.owner_before(B) && !B.owner_before(A);
        }
    }

    struct VisualDensityPrefetch::Work {
        explicit Work(const VisualDensityInterest &Interest)
            : Input(Interest), Completion(std::make_shared<JobGroup>()) {}

        VisualDensityInterest Input;
        std::shared_ptr<JobGroup> Completion;
        std::stop_source Stop;
        std::optional<std::expected<VisualDensityVariant, SurfaceStatus>> Output;
    };

    VisualDensityPrefetch::~VisualDensityPrefetch() {
        if (Active) Active->Stop.request_stop();
    }

    bool VisualDensityPrefetch::Advance(std::span<const VisualDensityInterest> Interests) noexcept {
        if (Interests.size() > MaximumInterests)
            return false;
        for (std::size_t I = 0; I < Interests.size(); ++I) {
            const auto &Input = Interests[I];
            if (!IsValid(Input.Key) || !Input.CommittedFine || std::ranges::any_of(Interests.first(I),
                [&](const auto &Earlier) { return Earlier.Key == Input.Key; }))
                return false;
        }
        const auto Interested = [&](ChunkKey Key, const std::shared_ptr<const SurfaceMesh> &Mesh) {
            return std::ranges::any_of(Interests, [&](const auto &Input) {
                return Input.Key == Key && SameSource(Mesh, Input.CommittedFine);
            });
        };
        for (auto &Entry : Cache) {
            if (Entry && !Interested(Entry->Key, Entry->Source.lock())) {
                if (Entry->Value) CachedBytes -= Entry->Value->GetOwnedBytes();
                Entry.reset();
            }
        }

        if (Active) {
            if (!Interested(Active->Input.Key, Active->Input.CommittedFine))
                Active->Stop.request_stop();
            if (!Active->Completion->IsComplete())
                return true;
            if (!Active->Stop.stop_requested() && Active->Output) {
                // Each admitted key has exactly one slot. Failure or a result
                // beyond the cache cap is remembered until this source leaves.
                auto Slot = std::ranges::find_if(Cache, [](const auto &Entry) { return !Entry; });
                if (Slot != Cache.end()) {
                    Slot->emplace(CacheEntry{Active->Input.Key, Active->Input.CommittedFine, {}});
                    auto &Result = *Active->Output;
                    if (Result && Result->GetOwnedBytes() <= MaximumCachedBytes - CachedBytes) {
                        CachedBytes += Result->GetOwnedBytes();
                        (*Slot)->Value.emplace(std::move(*Result));
                    }
                }
            }
            Active.reset();
        }

        auto Next = std::ranges::find_if(Interests, [&](const auto &Input) {
            return std::ranges::none_of(Cache, [&](const auto &Entry) { return Entry && Entry->Key == Input.Key; });
        });
        if (Next == Interests.end())
            return true;
        try {
            auto Job = std::make_shared<Work>(*Next);
            Jobs.Submit([Job] {
                Job->Output.emplace(VisualDensityVariant::Prepare(Job->Input.CommittedFine, Job->Stop.get_token()));
            }, Job->Completion);
            Active = std::move(Job);
            return true;
        } catch (...) {
            return false;
        }
    }

    std::shared_ptr<const SurfaceMesh> VisualDensityPrefetch::Select(ChunkKey Key,
        const std::shared_ptr<const SurfaceMesh> &CommittedFine, bool PreferReduced) const noexcept {
        if (!PreferReduced)
            return CommittedFine;
        const auto Found = std::ranges::find_if(Cache, [&](const auto &Entry) { return Entry && Entry->Key == Key; });
        if (Found == Cache.end() || !(*Found)->Value)
            return CommittedFine;
        return (*Found)->Value->Select(CommittedFine, true);
    }
}
