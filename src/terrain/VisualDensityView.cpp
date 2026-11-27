// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "terrain/VisualDensityView.hpp"

#include "gargantuan/Profiler.hpp"
#include "terrain/RenderStage.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace gargantuan::terrain {
    static_assert(VisualDensityView::MaximumCandidates == RenderStage::MaximumVisualVariants);
    namespace {
        double Length(WorldPoint V) noexcept {
            return std::hypot(V.X, V.Y, V.Z);
        }

        bool SameFine(const std::shared_ptr<const SurfaceMesh> &A,
            const std::shared_ptr<const SurfaceMesh> &B) noexcept {
            return A.get() == B.get() && !A.owner_before(B) && !B.owner_before(A);
        }

        std::size_t GetMinimumRenderBytes(const SurfaceMesh &Mesh) noexcept {
            return sizeof(RenderChunk) + Mesh.Vertices.size() * sizeof(RenderVertex) +
                Mesh.Indices.size() * sizeof(std::uint32_t);
        }
    }

    double GetVisualDensityPixelBound(GridSpec Grid, ChunkKey Key, const VisualDensityCamera &View) noexcept {
        constexpr auto Unbounded = std::numeric_limits<double>::infinity();
        if (!Grid.IsValid() || !IsValid(Key) || !(View.FocalPixels > 0) || !std::isfinite(View.FocalPixels))
            return Unbounded;
        const auto ForwardLength = Length(View.Forward);
        if (!(ForwardLength > 0) || !std::isfinite(ForwardLength))
            return Unbounded;

        const WorldPoint Forward{View.Forward.X / ForwardLength, View.Forward.Y / ForwardLength,
            View.Forward.Z / ForwardLength};
        const auto ChunkSpan = ChunkSide * Grid.GetSpacing();
        const auto Half = ChunkSpan * 0.5;
        const auto Radius = std::sqrt(3.0) * Half;
        const WorldPoint Delta{Key.X * ChunkSpan + Half - View.Position.X,
            Key.Y * ChunkSpan + Half - View.Position.Y, Key.Z * ChunkSpan + Half - View.Position.Z};
        const auto Depth = Delta.X * Forward.X + Delta.Y * Forward.Y + Delta.Z * Forward.Z;
        const auto MinimumDepth = Depth - Radius;
        if (!(MinimumDepth > 0) || !std::isfinite(MinimumDepth))
            return Unbounded;
        const auto Lateral = Length({Delta.X - Depth * Forward.X, Delta.Y - Depth * Forward.Y, Delta.Z - Depth * Forward.Z});
        const auto ScreenSlope = (Lateral + Radius) / MinimumDepth;
        // Both corresponding points have at least MinimumDepth. The bound
        // combines lateral motion and the depth term of perspective division.
        const auto Pixels = (View.FocalPixels / MinimumDepth) * (MaximumVisualDensityMoveInSamples * Grid.GetSpacing()) *
            std::hypot(1.0, ScreenSlope);
        return std::isfinite(Pixels) ? Pixels : Unbounded;
    }

    std::expected<VisualDensityViewReceipt, EditStatus> VisualDensityView::Advance(GridSpec Grid,
        const std::optional<VisualDensityCamera> &Camera, std::span<const VisualDensityInterest> Visible,
        Revision Publication) {
        G_PROFILE("Terrain::VisualDensityView");
        if (!Grid.IsValid() || Visible.size() > MaximumCandidates || (BoundGrid && *BoundGrid != Grid))
            return std::unexpected(EditStatus::InvalidInput);
        if (Publication <= LastPublication)
            return std::unexpected(EditStatus::Stale);
        for (std::size_t I = 0; I < Visible.size(); ++I) {
            const auto &Value = Visible[I];
            if (!IsValid(Value.Key) || !Value.CommittedFine || Value.CommittedFine->Indices.empty() ||
                Value.CommittedFine->Spacing != Grid.GetSpacing() ||
                std::ranges::any_of(Visible.first(I), [&](const auto &Earlier) { return Earlier.Key == Value.Key; }))
                return std::unexpected(EditStatus::InvalidInput);
        }

        struct ScoredInterest {
            const VisualDensityInterest *Input;
            double Pixels;
        };
        std::array<ScoredInterest, MaximumCandidates> Scores{};
        const auto Before = Render.Capture();
        auto CurrentScores = std::span(Scores).first(Visible.size());
        for (std::size_t I = 0; I < Visible.size(); ++I)
            CurrentScores[I] = {&Visible[I], Camera ? GetVisualDensityPixelBound(Grid, Visible[I].Key, *Camera) :
                std::numeric_limits<double>::infinity()};
        std::ranges::sort(CurrentScores, [](const auto &A, const auto &B) {
            if (A.Pixels != B.Pixels) return A.Pixels < B.Pixels;
            return A.Input->Key < B.Input->Key;
        });

        VisualDensityViewReceipt Receipt;
        std::array<RenderVisualChange, MaximumCandidates> Changes{};
        std::array<ChunkPublicationCandidate, MaximumCandidates> FineChanges{};
        std::size_t ChangeCount = 0;
        std::size_t FineCount = 0;
        const auto FindScore = [&](ChunkKey Key) {
            return std::ranges::find_if(CurrentScores, [Key](const auto &Entry) {
                return Entry.Input->Key == Key;
            });
        };
        if (Before) {
            for (const auto &[Key, Pair] : Before->VisualVariants) {
                const bool Reduced = Before->Chunks.at(Key).Resource.Mesh == Pair.Reduced.Resource.Mesh;
                const auto Score = FindScore(Key);
                if (Score != CurrentScores.end() && !SameFine(Pair.Fine.Source.lock(), Score->Input->CommittedFine)) {
                    FineChanges[FineCount++] = {Key, Score->Input->CommittedFine, {}, false};
                    Receipt.Restored += Reduced;
                } else if (!Camera) {
                    Changes[ChangeCount++] = {Key, Pair.Fine.Resource.Mesh, RenderVisualAction::Evict};
                    Receipt.Restored += Reduced;
                } else if (Reduced && (Score == CurrentScores.end() || Score->Pixels > ExitPixels)) {
                    Changes[ChangeCount++] = {Key, Pair.Fine.Resource.Mesh, RenderVisualAction::Fine};
                    ++Receipt.Restored;
                } else if (Reduced) {
                    Receipt.FineRollbackBytes += Pair.Fine.Bytes;
                }
            }
        }

        std::array<VisualDensityInterest, MaximumCandidates> Work{};
        std::size_t WorkCount = 0;
        if (Before) {
            for (const auto &Score : CurrentScores) {
                if (Score.Pixels > PrefetchPixels) break;
                const auto &Input = *Score.Input;
                const auto Draw = Before->Chunks.find(Input.Key);
                if (Draw == Before->Chunks.end()) continue;
                const auto Pair = Before->VisualVariants.find(Input.Key);
                if ((Pair == Before->VisualVariants.end() && SameFine(Draw->second.Source.lock(), Input.CommittedFine)) ||
                    (Pair != Before->VisualVariants.end() && !SameFine(Pair->second.Fine.Source.lock(), Input.CommittedFine)))
                    Work[WorkCount++] = Input;
            }
        }
        const bool Prepared = Prefetch.Advance(std::span(Work).first(WorkCount));
        if (Before && ChangeCount == 0 && FineCount == 0) {
            for (const auto &Score : CurrentScores) {
                if (Score.Pixels > EnterPixels || Receipt.Promoted == MaximumPromotions) break;
                const auto &Input = *Score.Input;
                const auto Pair = Before->VisualVariants.find(Input.Key);
                if (Pair == Before->VisualVariants.end() ||
                    Before->Chunks.at(Input.Key).Resource.Mesh == Pair->second.Reduced.Resource.Mesh ||
                    Pair->second.Fine.Bytes > MaximumFineRollbackBytes - Receipt.FineRollbackBytes)
                    continue;
                Receipt.FineRollbackBytes += Pair->second.Fine.Bytes;
                Changes[ChangeCount++] = {Input.Key, Pair->second.Fine.Resource.Mesh, RenderVisualAction::Reduced,
                    Input.CommittedFine};
                ++Receipt.Promoted;
            }
        }

        if (Prepared && Before && ChangeCount == 0 && FineCount == 0) {
            auto RemainingSlots = RenderStage::MaximumVisualVariants - Before->VisualVariants.size();
            auto RemainingBytes = RenderStage::MaximumVisualVariantBytes - Before->VisualVariantBytes;
            auto WarmBytes = RenderStage::MaximumWarmBytes;
            for (const auto &Input : std::span(Work).first(WorkCount)) {
                const auto Reduced = Prefetch.Select(Input.Key, Input.CommittedFine, true);
                if (SameFine(Reduced, Input.CommittedFine)) continue;
                const auto &FineDraw = Before->Chunks.at(Input.Key);
                const auto Size = GetMinimumRenderBytes(*Reduced);
                if (Size >= FineDraw.Bytes || Size > WarmBytes) continue;
                if (RemainingSlots == 0 || Size > RemainingBytes) {
                    if (ChangeCount != 0) break;
                    // Existing view interests keep priority. An inactive pair
                    // outside them can release its separate cache charge.
                    for (const auto &[Key, Pair] : Before->VisualVariants) {
                        const auto Score = FindScore(Key);
                        if ((Score != CurrentScores.end() && Score->Pixels <= PrefetchPixels) ||
                            Before->Chunks.at(Key).Resource.Mesh != Pair.Fine.Resource.Mesh)
                            continue;
                        Changes[ChangeCount++] = {Key, Pair.Fine.Resource.Mesh, RenderVisualAction::Evict};
                        break;
                    }
                    break;
                }
                --RemainingSlots;
                RemainingBytes -= Size;
                WarmBytes -= Size;
                Changes[ChangeCount++] = {Input.Key, FineDraw.Resource.Mesh, RenderVisualAction::Warm,
                    Input.CommittedFine, Reduced};
                if (ChangeCount == RenderStage::MaximumWarmVariants) break;
            }
        }

        if (ChangeCount || FineCount) {
            auto Changed = std::span(Changes).first(ChangeCount);
            std::ranges::sort(Changed, {}, &RenderVisualChange::Key);
            auto Candidate = Render.StageVisualDensity(Grid, std::span(FineChanges).first(FineCount), Changed);
            if (!Candidate) return std::unexpected(Candidate.error());
            (*Candidate)->Install(Publication);
            if (!(*Candidate)->IsInstalled()) return std::unexpected(EditStatus::Unavailable);
        }
        const auto After = Render.Capture();
        if (After) {
            Receipt.CachedVariantBytes = After->VisualVariantBytes;
            for (const auto &[Key, Pair] : After->VisualVariants) {
                Receipt.Warmed += !Before || !Before->VisualVariants.contains(Key);
                Receipt.ReducedCount += After->Chunks.at(Key).Resource.Mesh == Pair.Reduced.Resource.Mesh;
            }
            if (Before) Receipt.Evicted = Before->VisualVariants.size() + Receipt.Warmed - After->VisualVariants.size();
        }
        BoundGrid = Grid;
        LastPublication = Publication;
        Receipt.FineReplaced = FineCount;
        Receipt.PreparedVariantBytes = Prefetch.GetCachedBytes();
        Receipt.Preparing = Prefetch.IsPreparing();
        return Receipt;
    }
}
