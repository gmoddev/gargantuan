// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/runtime/JobSystem.hpp"
#include "terrain/RenderStage.hpp"
#include "terrain/VisualDensity.hpp"
#include "terrain/VisualDensityPrefetch.hpp"
#include "terrain/VisualDensityView.hpp"
#include "TerrainVisualOverlapAudit.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace gargantuan;
using namespace gargantuan::terrain;

namespace {
    using Clock = std::chrono::steady_clock;
    constexpr ChunkKey Key{-1, 0, 0};
    constexpr ObjectId Owner{29, 4};

    void Require(bool Condition, const char *Message) {
        if (!Condition) throw std::runtime_error(Message);
    }

    enum class Fixture { Flat, Rolling, ThinCave, RollingOverlap };
    enum class Change { None, Paint, Geometry, Halo };

    std::vector<Sample> MakeSamples(Fixture Scene, Change Revision = Change::None) {
        constexpr auto Padded = ChunkSide + 3;
        std::vector<Sample> Samples;
        Samples.reserve(Padded * Padded * Padded);
        for (int Z = -1; Z <= ChunkSide + 1; ++Z)
            for (int Y = -1; Y <= ChunkSide + 1; ++Y)
                for (int X = -1; X <= ChunkSide + 1; ++X) {
                    double Density = Y - 12.67;
                    if (Scene == Fixture::Rolling)
                        Density -= 1.3 * std::sin(X * 0.17) + 0.8 * std::cos(Z * 0.39);
                    if (Scene == Fixture::ThinCave)
                        Density = std::max(Density, 0.82 - std::hypot(Y - 7.0 - 0.16 * std::sin(X * 0.16), Z - 9.0));
                    if (Scene == Fixture::RollingOverlap)
                        Density = Y - (8.11 + 1.3 * std::sin((X + ChunkSide) * 0.5 * 0.34) + 0.8 * std::cos(Z * 0.39));
                    if (Revision == Change::Geometry || (Revision == Change::Halo && X > ChunkSide))
                        Density += 0.2;
                    const auto Magnitude = std::clamp(std::llround(std::abs(Density) * 901), 1LL, 32767LL);
                    const auto Signed = static_cast<std::int16_t>(Density < 0 ? -Magnitude : Magnitude);
                    const auto Material = Revision == Change::Paint ? 2u : 1u;
                    Samples.push_back({Signed, Signed < 0 ? Material : 0});
                }
        return Samples;
    }

    std::shared_ptr<const SurfaceMesh> Extract(std::span<const Sample> Samples) {
        auto Fine = std::make_shared<SurfaceMesh>();
        Require(ExtractSurface({Samples, ChunkSide, ChunkSide, ChunkSide, 1.0}, {}, *Fine) ==
            SurfaceStatus::Success, "authored MC33 extraction failed");
        return Fine;
    }

    struct PublicationResult {
        RenderProjectionChanges Changes;
        std::shared_ptr<const RenderState> State;
    };

    PublicationResult Publish(RenderStage &Stage, RenderProjection &Projection,
        const std::shared_ptr<const SurfaceMesh> &Mesh, RenderPublicationId Id, bool GeometryChanged) {
        const auto Before = Stage.Capture();
        const std::array Chunks{ChunkPublicationCandidate{Key, Mesh, {}, GeometryChanged}};
        auto Prepared = Stage.Stage({}, Chunks);
        Require(Prepared.has_value() && Stage.Capture() == Before, "CPU stage must publish atomically");
        (*Prepared)->Install(Id);
        Require((*Prepared)->IsInstalled(), "render stage did not install");
        const auto After = Stage.Capture();
        RenderPublication Publication{};
        Publication.Id = Id;
        Publication.BaseId = Id - 1;
        Publication.FullResync = Id == 1;
        Publication.Frame.ViewportWidth = 640;
        Publication.Frame.ViewportHeight = 480;
        AppendTerrainRenderState(Publication, Before, After);
        const auto Changes = Projection.Apply(Publication);
        Require(Changes.MeshesCreated == 1 && Changes.MeshesRemoved == static_cast<std::size_t>(Id != 1) &&
            Projection.GetMeshCount() == 1 && Projection.GetTerrainDraws().size() == 1 &&
            Projection.GetTerrainDraws().front().Item.Object == Owner &&
            Projection.GetTerrainDraws().front().Item.ModelMatrix[3].x == -32.0f, "owned draw replacement");
        const auto &Resource = After->Chunks.at(Key).Resource;
        Require(Changes.VertexUploadBytes == Mesh->Vertices.size() * sizeof(RenderVertex) +
            Mesh->Indices.size() * sizeof(std::uint32_t) && *Resource.Indices == Mesh->Indices &&
            Resource.Vertices->size() == Mesh->Vertices.size(), "published vertex and index upload payload");
        return {Changes, After};
    }

    double Microseconds(Clock::duration Duration) {
        return std::chrono::duration<double, std::micro>(Duration).count();
    }

    struct Timing {
        double Cold;
        std::size_t Bytes;
    };

    Timing Measure(std::span<const Sample> Samples, bool Reduced) {
        RenderStage Stage(Owner);
        RenderProjection Projection;
        const auto Begin = Clock::now();
        const auto Fine = Extract(Samples);
        auto Visual = Fine;
        if (Reduced) {
            const auto Variant = VisualDensityVariant::Prepare(Fine);
            Require(Variant.has_value(), "visual density preparation failed");
            Visual = Variant->Select(Fine, true);
            Require(Visual != Fine, "no qualified reduction in benchmark fixture");
        }
        const auto Result = Publish(Stage, Projection, Visual, 1, true);
        const auto End = Clock::now();
        return {Microseconds(End - Begin), Result.Changes.VertexUploadBytes};
    }

    double MeasurePublication(const std::shared_ptr<const SurfaceMesh> &Mesh) {
        RenderStage Stage(Owner);
        RenderProjection Projection;
        const auto Begin = Clock::now();
        const auto Value = Publish(Stage, Projection, Mesh, 1, true);
        return Microseconds(Clock::now() - Begin);
    }

    void Benchmark(Fixture Scene, const char *Name) {
        const auto Samples = MakeSamples(Scene);
        constexpr std::size_t Repetitions = 9;
        std::array<double, Repetitions> FineCold{}, ReducedCold{}, FinePublication{}, ReducedPublication{};
        std::size_t FineBytes = 0, ReducedBytes = 0;
        const auto ReadyFine = Extract(Samples);
        const auto Variant = VisualDensityVariant::Prepare(ReadyFine).value();
        const auto ReadyReduced = Variant.Select(ReadyFine, true);
        for (std::size_t I = 0; I < Repetitions; ++I) {
            const auto First = Measure(Samples, I % 2 != 0);
            const auto Second = Measure(Samples, I % 2 == 0);
            const auto &Fine = I % 2 == 0 ? First : Second;
            const auto &Reduced = I % 2 == 0 ? Second : First;
            FineCold[I] = Fine.Cold;
            ReducedCold[I] = Reduced.Cold;
            if (I % 2 == 0) {
                FinePublication[I] = MeasurePublication(ReadyFine);
                ReducedPublication[I] = MeasurePublication(ReadyReduced);
            } else {
                ReducedPublication[I] = MeasurePublication(ReadyReduced);
                FinePublication[I] = MeasurePublication(ReadyFine);
            }
            FineBytes = Fine.Bytes;
            ReducedBytes = Reduced.Bytes;
        }
        std::ranges::sort(FineCold);
        std::ranges::sort(ReducedCold);
        std::ranges::sort(FinePublication);
        std::ranges::sort(ReducedPublication);
        Require(ReducedBytes < FineBytes, "reduced publication did not save mesh bytes");
        constexpr auto Mid = Repetitions / 2;
        std::cout << Name << ',' << Repetitions << ',' << FineCold[Mid] << ',' << ReducedCold[Mid] << ','
                  << FinePublication[Mid] << ',' << ReducedPublication[Mid] << ',' << FineBytes << ',' << ReducedBytes << '\n';
    }

    void TestFineFallback() {
        RenderStage Stage(Owner);
        RenderProjection Projection;
        const auto Fine = Extract(MakeSamples(Fixture::Flat));
        CollisionSurfaceMesh Physical;
        Require(BuildCollisionSurface(*Fine, {}, Physical) == SurfaceStatus::Success &&
            Physical.Indices == Fine->Indices, "collision uses every authored face");
        const auto Variant = VisualDensityVariant::Prepare(Fine).value();
        const auto Reduced = Variant.Select(Fine, true);
        Require(Reduced != Fine && Reduced->Indices.size() < Physical.Indices.size() &&
            Variant.Select(Fine, false) == Fine, "visual-only selection preserves authored collision resolution");
        const auto First = Publish(Stage, Projection, Reduced, 1, true);
        const auto FirstMesh = First.State->Chunks.at(Key).Resource.Mesh;

        auto Painted = std::make_shared<SurfaceMesh>(*Fine);
        Require(RefreshSurfaceMaterials(MakeSamples(Fixture::Flat, Change::Paint), *Painted) == SurfaceStatus::Success,
            "paint-only input failed");
        auto Geometry = Extract(MakeSamples(Fixture::Flat, Change::Geometry));
        auto Halo = Extract(MakeSamples(Fixture::Flat, Change::Halo));
        bool HaloChangesNormal = false;
        Require(Halo->Indices == Fine->Indices && Halo->Vertices.size() == Fine->Vertices.size(), "halo-only mesh layout");
        for (std::size_t I = 0; I < Halo->Vertices.size(); ++I) {
            Require(Halo->Vertices[I].Position == Fine->Vertices[I].Position, "halo-only update moved an authored point");
            HaloChangesNormal |= Halo->Vertices[I].Normal != Fine->Vertices[I].Normal;
        }
        Require(HaloChangesNormal, "halo-only update never changed an authored gradient");

        const std::array<std::shared_ptr<const SurfaceMesh>, 4> Sources{Painted, Geometry, Halo,
            std::make_shared<const SurfaceMesh>(*Fine)};
        RenderPublicationId Id = 2;
        for (const auto &Current : Sources) {
            const auto Selected = Variant.Select(Current, true);
            Require(Selected == Current, "stale derivative replaced a current authored mesh");
            const auto Published = Publish(Stage, Projection, Selected, Id, Id != 2);
            if (Id == 2) {
                constexpr std::array<std::uint8_t, 4> PaintedSwatch{97, 94, 84, 255};
                Require(Published.State->Chunks.at(Key).Resource.Vertices->front().Tint == PaintedSwatch,
                    "paint published current authored material");
            }
            ++Id;
        }
        const auto Next = VisualDensityVariant::Prepare(Halo).value();
        const auto NextReduced = Next.Select(Halo, true);
        Require(NextReduced != Halo && Next.Select(Geometry, true) == Geometry, "new variant stays bound to its own source");
        auto Last = Publish(Stage, Projection, NextReduced, Id, false);
        Require(!Projection.GetMeshBounds(FirstMesh) &&
            First.State->Chunks.at(Key).Resource.Indices->size() == Reduced->Indices.size() &&
            Last.Changes.VertexUploadBytes < Fine->Vertices.size() * sizeof(RenderVertex) +
                Fine->Indices.size() * sizeof(std::uint32_t) && Physical.Indices == Fine->Indices,
            "projection retires old identity, retained state and collider remain immutable");
        Require(!Variant.Select({}, true), "retired source must not publish stale coverage");
        auto Sparse = std::make_shared<const SurfaceMesh>();
        Require(VisualDensityVariant::Prepare(Sparse).value().Select(Sparse, true) == Sparse,
            "empty source must not allocate a distinct visual variant");
        std::cout << "Checks,PaintGeometryHaloIdentityAndFineCollision,Pass\n";
    }

    void TestPrefetchAndCancellation() {
        JobSystem Jobs(1, JobSystem::StartPolicy::OnFirstSubmission);
        VisualDensityPrefetch Prefetch(Jobs);
        const auto First = Extract(MakeSamples(Fixture::Flat));
        const auto Current = Extract(MakeSamples(Fixture::Rolling));
        const auto Cave = Extract(MakeSamples(Fixture::ThinCave));
        constexpr ChunkKey Neighbor{0, 0, 0};
        const std::array Initial{VisualDensityInterest{Key, First}};
        const std::array Changed{VisualDensityInterest{Key, Current}, VisualDensityInterest{Neighbor, Cave}};
        const std::array Duplicate{VisualDensityInterest{Key, Current}, VisualDensityInterest{Key, Cave}};
        const std::array<VisualDensityInterest, VisualDensityPrefetch::MaximumInterests + 1> Overflow{};
        Require(Prefetch.Advance(Initial) && Prefetch.IsPreparing() && Prefetch.Select(Key, First, true) == First,
            "pending worker retains fine coverage");
        Require(!Prefetch.Advance(Duplicate) && !Prefetch.Advance(Overflow) && Prefetch.IsPreparing(),
            "invalid or oversized interest list preserves admission");
        Require(Prefetch.Advance(Changed), "current source prefetch update");
        for (unsigned Poll = 0; Poll < 3; ++Poll) {
            Require(Prefetch.Select(Key, First, true) == First, "canceled source unexpectedly published");
            Jobs.Drain();
            Require(Prefetch.Advance(Changed), "worker completion must admit current interest");
            if (Prefetch.Select(Neighbor, Cave, true) != Cave)
                Require(Prefetch.Select(Key, Current, true) != Current, "later interest bypassed the prioritized source");
        }
        Require(!Prefetch.IsPreparing() && Prefetch.GetCachedBytes() > 0 &&
            Prefetch.GetCachedBytes() <= VisualDensityPrefetch::MaximumCachedBytes &&
            Prefetch.Select(Key, Current, true) != Current && Prefetch.Select(Key, Current, false) == Current &&
            Prefetch.Select(Neighbor, Cave, true) != Cave && Prefetch.Select(Key, Cave, true) == Cave,
            "cache owns bounded exact-source derivatives and preserves near detail");
        Require(Prefetch.Advance({}) && Prefetch.GetCachedBytes() == 0 &&
            Prefetch.Select(Key, Current, true) == Current, "leaving view releases cached derivatives");
        {
            VisualDensityPrefetch Closing(Jobs);
            Require(Closing.Advance(Initial), "closing cache must initially queue");
        }
        Jobs.Drain();
        std::cout << "Checks,BoundedPrioritizedPrefetchCancellationAndRetirement,Pass\n";
    }

    void TestGuardedFallback() {
        auto Witness = std::make_shared<SurfaceMesh>(*Extract(MakeSamples(Fixture::RollingOverlap)));
        gargantuan::terrain::test::AppendOverlapWitness(*Witness);
        const std::shared_ptr<const SurfaceMesh> Fine = Witness;
        Witness.reset();
        JobSystem Jobs(1, JobSystem::StartPolicy::OnFirstSubmission);
        VisualDensityPrefetch Prefetch(Jobs);
        RenderStage Stage(Owner);
        RenderProjection Projection;
        const std::array Interests{VisualDensityInterest{Key, Fine}};
        Require(Prefetch.Advance(Interests) && Prefetch.IsPreparing(), "overlap witness preparation queued");
        Jobs.Drain();
        Require(Prefetch.Advance(Interests) && !Prefetch.IsPreparing() && Prefetch.GetCachedBytes() == 0,
            "denied overlap witness must stay fine without resubmitting unchanged work");
        const auto Chosen = Prefetch.Select(Key, Fine, true);
        Require(Chosen == Fine && Prefetch.Advance(Interests) && !Prefetch.IsPreparing(), "prefetch fine fallback");
        const auto Published = Publish(Stage, Projection, Chosen, 1, true);
        Require(Published.State->Chunks.at(Key).Resource.Indices->size() == Fine->Indices.size(),
            "native projection must receive every original face after overlap denial");
        std::cout << "Checks,OverlapDenialPublishesFine,Pass\n";
    }

    void TestPerspectiveBound() {
        constexpr ChunkKey Chunk{-32'700, 25'000, 32'700};
        for (const auto Grid : {GridSpec{-1}, GridSpec{0}, GridSpec{1}}) {
            const auto H = Grid.GetSpacing();
            const auto Span = ChunkSide * H;
            const WorldPoint Origin{Chunk.X * Span, Chunk.Y * Span, Chunk.Z * Span};
            const VisualDensityCamera Camera{{Origin.X - 0.5 * Span, Origin.Y + 0.6 * Span, Origin.Z - 60 * H},
                {0, 0, 2}, 800};
            const auto Screen = [&](WorldPoint P) {
                return std::array{Camera.FocalPixels * (P.X - Camera.Position.X) / (P.Z - Camera.Position.Z),
                    Camera.FocalPixels * (P.Y - Camera.Position.Y) / (P.Z - Camera.Position.Z)};
            };
            const auto Bound = GetVisualDensityPixelBound(Grid, Chunk, Camera);
            for (const double X : {2.0, 16.0, 30.0})
                for (const double Y : {2.0, 16.0, 30.0})
                    for (const double Z : {2.0, 16.0, 30.0}) {
                        const WorldPoint P{Origin.X + X * H, Origin.Y + Y * H, Origin.Z + Z * H};
                        for (const double Sign : {-1.0, 1.0}) {
                            const WorldPoint Moved{P.X + Sign * 0.75 * H, P.Y, P.Z + Sign * H};
                            const auto A = Screen(P);
                            const auto B = Screen(Moved);
                            Require(std::hypot(A[0] - B[0], A[1] - B[1]) <= Bound,
                                "conservative pixel certificate covers lateral and depth displacement");
                        }
                    }
        }
    }

    void TestViewSweep() {
        JobSystem Jobs(1, JobSystem::StartPolicy::OnFirstSubmission);
        RenderStage Stage(Owner);
        VisualDensityView Selector(Jobs, Stage);
        RenderProjection Projection;
        constexpr auto ChunkCount = VisualDensityView::MaximumCandidates;
        constexpr ChunkKey StartKey{-30'000, 20'000, 30'000};
        const double CenterX = (StartKey.X + ChunkCount * 0.5) * ChunkSide;
        const double CenterY = (StartKey.Y + 0.5) * ChunkSide;
        const double CenterZ = (StartKey.Z + 0.5) * ChunkSide;
        const auto AtDistance = [&](double Distance) {
            return VisualDensityCamera{{CenterX, CenterY, CenterZ - Distance}, {0, 0, 1}, 800};
        };
        const auto Near = AtDistance(400);
        const auto Band = AtDistance(820);
        const auto Far = AtDistance(1'240);
        const auto OtherSide = AtDistance(-400);
        const auto Fine = Extract(MakeSamples(Fixture::Flat));
        const auto Painted = Extract(MakeSamples(Fixture::Flat, Change::Paint));
        std::array<VisualDensityInterest, ChunkCount> Visible;
        std::array<ChunkPublicationCandidate, ChunkCount> Cohort;
        for (std::size_t I = 0; I < ChunkCount; ++I) {
            const ChunkKey CandidateKey{StartKey.X + static_cast<std::int64_t>(I), StartKey.Y, StartKey.Z};
            const auto Enter = GetVisualDensityPixelBound({}, CandidateKey, Far);
            const auto Hold = GetVisualDensityPixelBound({}, CandidateKey, Band);
            Require(Enter < VisualDensityView::EnterPixels && Hold > VisualDensityView::EnterPixels &&
                Hold < VisualDensityView::ExitPixels &&
                GetVisualDensityPixelBound({}, CandidateKey, Near) > VisualDensityView::PrefetchPixels &&
                std::isinf(GetVisualDensityPixelBound({}, CandidateKey, OtherSide)), "camera hysteresis qualification positions");
            const VisualDensityCamera Rebased{{Far.Position.X - CandidateKey.X * ChunkSide, Far.Position.Y - CandidateKey.Y * ChunkSide,
                Far.Position.Z - CandidateKey.Z * ChunkSide}, Far.Forward, Far.FocalPixels};
            Require(std::abs(Enter - GetVisualDensityPixelBound({}, {}, Rebased)) < 1.0e-12,
                "projected bound is camera relative at large signed coordinates");
            auto Source = std::make_shared<const SurfaceMesh>(*Fine);
            Visible[ChunkCount - 1 - I] = {CandidateKey, Source};
            Cohort[I] = {CandidateKey, std::move(Source), {}, true};
        }
        RenderPublicationId PublicationId = 0;
        const auto Project = [&](const auto &Before, bool Resync = false) {
            RenderPublication Publication{};
            Publication.Id = ++PublicationId;
            Publication.FullResync = PublicationId == 1 || Resync;
            Publication.BaseId = Publication.FullResync ? 0 : PublicationId - 1;
            Publication.Frame.ViewportWidth = 1'280;
            Publication.Frame.ViewportHeight = 960;
            AppendTerrainRenderState(Publication, Before, Stage.Capture());
            const auto Changes = Projection.Apply(Publication);
            Require(Projection.GetMeshCount() == Stage.Capture()->Chunks.size() + Stage.Capture()->VisualVariants.size() &&
                Projection.GetTerrainDraws().size() == Stage.Capture()->Chunks.size(),
                "inactive meshes remain resident with exactly one selected draw per chunk");
            return Changes;
        };
        Stage.Stage({}, Cohort).value()->Install(1);
        const auto Initial = Stage.Capture();
        Require(Project(std::shared_ptr<const RenderState>{}).MeshesCreated == ChunkCount,
            "initial view coverage uses fine meshes");
        Require(Initial->Chunks.at(StartKey).Draw.Item.ModelMatrix[3].x == StartKey.X * ChunkSide &&
            Initial->Chunks.at(StartKey).Draw.Item.ModelMatrix[3].z == StartKey.Z * ChunkSide,
            "large signed model origin is preserved by native staging");

        std::vector<double> StableUs, PromoteUs, RestoreUs;
        std::size_t PeakRollbackBytes = 0, PeakVariantBytes = 0;
        std::size_t PeakPromoteUploadBytes = 0, PeakRestoreUploadBytes = 0;
        std::size_t PeakWarmUploadBytes = 0;
        const auto Advance = [&](const std::optional<VisualDensityCamera> &Camera, std::span<const VisualDensityInterest> Values) {
            const auto Before = Stage.Capture();
            const auto Begin = Clock::now();
            const auto Result = Selector.Advance({}, Camera, Values, PublicationId + 1);
            Require(Result.has_value(), "view update must stage");
            const auto Changes = Project(Before);
            const auto Elapsed = Microseconds(Clock::now() - Begin);
            auto &Timing = Result->Restored ? RestoreUs : (Result->Promoted ? PromoteUs : StableUs);
            Timing.push_back(Elapsed);
            Require(Result->Promoted <= VisualDensityView::MaximumPromotions &&
                Result->Warmed <= RenderStage::MaximumWarmVariants &&
                Changes.MeshesCreated == Result->Warmed + Result->FineReplaced &&
                Changes.MeshesRemoved == Result->Evicted + Result->FineReplaced && Result->ReducedCount <= ChunkCount,
                "view update publishes exact resource ownership for warm, switch and retire");
            PeakRollbackBytes = std::max(PeakRollbackBytes, Result->FineRollbackBytes);
            PeakVariantBytes = std::max(PeakVariantBytes, Result->CachedVariantBytes);
            if (Result->Warmed) {
                Require(Changes.VertexUploadBytes <= RenderStage::MaximumWarmBytes && Before->Draws == Stage.Capture()->Draws,
                    "warming stays within upload budget and preserves the draw table");
                PeakWarmUploadBytes = std::max(PeakWarmUploadBytes, Changes.VertexUploadBytes);
            }
            if (Result->Promoted) PeakPromoteUploadBytes = std::max(PeakPromoteUploadBytes, Changes.VertexUploadBytes);
            if (Result->Restored && !Result->FineReplaced)
                PeakRestoreUploadBytes = std::max(PeakRestoreUploadBytes, Changes.VertexUploadBytes);
            return *Result;
        };

        for (std::size_t I = 0; I <= ChunkCount; ++I) {
            const auto Warm = Advance(Band, Visible);
            Require(Warm.Promoted == 0 && Warm.ReducedCount == 0, "prefetch does not force near or hysteresis-band promotion");
            if (I < ChunkCount) Jobs.Drain();
            else Require(!Warm.Preparing && Warm.CachedVariantBytes > 0, "bounded visible set prepared");
        }

        auto Preferred = Visible;
        std::ranges::sort(Preferred, [&](const auto &A, const auto &B) {
            const auto X = GetVisualDensityPixelBound({}, A.Key, Far);
            const auto Y = GetVisualDensityPixelBound({}, B.Key, Far);
            return X != Y ? X < Y : A.Key < B.Key;
        });
        for (std::size_t Count = 0; Count < ChunkCount; Count += VisualDensityView::MaximumPromotions) {
            const auto FarStep = Advance(Far, Visible);
            Require(FarStep.Promoted == VisualDensityView::MaximumPromotions &&
                FarStep.ReducedCount == Count + FarStep.Promoted, "warm variants promote in bounded frame slices");
            for (std::size_t I = 0; I < ChunkCount; ++I) {
                const auto &Row = Stage.Capture()->Chunks.at(Preferred[I].Key);
                const bool Reduced = Row.Resource.Indices->size() < Fine->Indices.size();
                Require(Reduced == (I < FarStep.ReducedCount), "pixel bound and stable key determine promotion priority");
                Require(Row.AdmissionBytes == Initial->Chunks.at(Preferred[I].Key).AdmissionBytes,
                    "each reduced draw reserves its exact authored render allocation");
            }
        }
        auto Held = Advance(Band, Visible);
        Require(Held.ReducedCount == ChunkCount && Held.Promoted == 0 && Held.Restored == 0,
            "approaching the enter threshold preserves reduced coverage in the band");
        Require(PeakRollbackBytes > 0 && PeakRollbackBytes <= VisualDensityView::MaximumFineRollbackBytes &&
            PeakVariantBytes > 0 && PeakVariantBytes <= RenderStage::MaximumVisualVariantBytes,
            "view and worker retention stay under their separate byte ceilings");
        const auto BeforeResync = Stage.Capture();
        Require(Project(BeforeResync, true).MeshesCreated == 2 * ChunkCount &&
            Projection.GetMeshBounds(BeforeResync->VisualVariants.at(StartKey).Fine.Resource.Mesh) &&
            Projection.GetMeshBounds(BeforeResync->VisualVariants.at(StartKey).Reduced.Resource.Mesh),
            "full resync reproduces both live identities and the exact selected table");

        constexpr std::size_t MaximumRendererBytes = 32 * 1024 * 1024;
        const auto ReducedBeforeFiller = Stage.Capture();
        const auto ReservedSurplus = ReducedBeforeFiller->AdmissionBytes - ReducedBeforeFiller->Bytes;
        Require(ReducedBeforeFiller->AdmissionBytes == Initial->AdmissionBytes &&
            ReservedSurplus >= Initial->Chunks.at(StartKey).Bytes, "camera reduction lowers real payload and preserves admission");
        const std::array Filler{ChunkPublicationCandidate{{0, 0, 0}, Fine, {}, true,
            MaximumRendererBytes - Initial->AdmissionBytes}};
        Stage.Stage({}, Filler).value()->Install(PublicationId + 1);
        Require(Project(ReducedBeforeFiller).MeshesCreated == 1 && Stage.Capture()->AdmissionBytes == MaximumRendererBytes,
            "unrelated coverage consumes the remaining admissible bytes");
        const std::array Overcommit{ChunkPublicationCandidate{{1, 0, 0}, Fine, {}, true}};
        Require(Stage.Stage({}, Overcommit).error() == EditStatus::LimitExceeded,
            "unrelated coverage cannot claim bytes reserved for fine rollback");

        const auto BeforeBusy = Stage.Capture();
        auto Busy = Stage.Stage({}, {}).value();
        Require(Selector.Advance({}, Near, Visible, PublicationId + 1).error() == EditStatus::Unavailable &&
            Stage.Capture() == BeforeBusy, "staging denial retains the complete previous render state");
        Busy.reset();
        const auto Returned = Advance(Near, Visible);
        Require(Returned.Restored == ChunkCount && Returned.ReducedCount == 0 && Returned.FineRollbackBytes == 0 &&
            Returned.CachedVariantBytes == Held.CachedVariantBytes && Returned.PreparedVariantBytes == 0 &&
            Stage.Capture()->AdmissionBytes == MaximumRendererBytes && PeakRestoreUploadBytes == 0,
            "all newly near draws restore the resident fine resource within its admission");
        for (const auto &Input : Visible)
            Require(Stage.Capture()->Chunks.at(Input.Key).Resource.Mesh == Initial->Chunks.at(Input.Key).Resource.Mesh,
                "fine rollback retains the original exact render identity");
        Require(Advance(Far, Visible).Promoted == VisualDensityView::MaximumPromotions && PeakPromoteUploadBytes == 0,
            "cached variants survive the warm near-to-far handoff");
        Require(Advance(Near, Visible).Restored == VisualDensityView::MaximumPromotions, "ready entries can go inactive again");
        const VisualDensityCamera FillerView{{16, 16, -1'224}, {0, 0, 1}, 800};
        const std::array FillerInterest{VisualDensityInterest{{0, 0, 0}, Fine}};
        Require(Advance(FillerView, FillerInterest).Preparing, "an interest outside the full cache can prepare");
        Jobs.Drain();
        const auto Eviction = Advance(FillerView, FillerInterest);
        Require(Eviction.Evicted == 1 && Eviction.Warmed == 0 && Eviction.Restored == 0 &&
            Stage.Capture()->VisualVariants.size() == ChunkCount - 1,
            "cache pressure evicts an inactive unused variant and keeps its fine draw");
        Require(Advance(FillerView, FillerInterest).Warmed == 1 && Stage.Capture()->VisualVariants.size() == ChunkCount &&
            Advance(FillerView, FillerInterest).Promoted == 1, "a new interest warms into the reclaimed slot");
        Require(Advance(std::nullopt, {}).Evicted == ChunkCount && Stage.Capture()->VisualVariantBytes == 0,
            "explicit disable releases every reduced cache reservation");

        const auto First = std::span(Visible).first(1);
        const auto Second = std::span(Visible).subspan(1, 1);
        Require(Advance(Far, First).Preparing, "first retarget job queued");
        Require(Advance(Far, Second).Promoted == 0, "retarget cannot publish a superseded job");
        Jobs.Drain();
        Advance(Far, Second);
        Jobs.Drain();
        Advance(Far, Second);
        Require(Advance(Far, Second).ReducedCount == 1 && Stage.Capture()->Chunks.at(First.front().Key).Resource.Indices->size() ==
            Fine->Indices.size(), "retarget publishes only the current interest");

        const auto EditKey = Second.front().Key;
        const std::array Changed{VisualDensityInterest{EditKey, Painted}};
        const auto PaintFallback = Advance(Far, Changed);
        Require(PaintFallback.Restored == 1 && PaintFallback.ReducedCount == 0 &&
            *Stage.Capture()->Chunks.at(EditKey).Resource.Indices == Painted->Indices,
            "current paint source atomically replaces an old reduced mesh");
        Jobs.Drain();
        Advance(Far, Changed);
        Require(Advance(Far, Changed).ReducedCount == 1, "new committed source can prepare its own derivative");

        const auto ExternalBefore = Stage.Capture();
        const auto StaleFine = ExternalBefore->VisualVariants.at(EditKey).Fine.Resource.Mesh;
        const std::array Authoritative{ChunkPublicationCandidate{EditKey, Fine, {}, true}};
        Stage.Stage({}, Authoritative).value()->Install(PublicationId + 1);
        const auto AuthoritativeUpdate = Project(ExternalBefore);
        Require(AuthoritativeUpdate.MeshesCreated == 1 && AuthoritativeUpdate.MeshesRemoved == 2 &&
            !Projection.GetMeshBounds(StaleFine), "authoritative edit replaces both old render identities");
        const std::array StaleWarm{RenderVisualChange{EditKey, StaleFine, RenderVisualAction::Warm, Fine, Fine}};
        Require(Stage.StageVisualDensity({}, {}, StaleWarm).error() == EditStatus::Stale,
            "stale exact-source warm command cannot recreate an older render revision");
        const std::array WrongSource{RenderVisualChange{EditKey,
            Stage.Capture()->Chunks.at(EditKey).Resource.Mesh, RenderVisualAction::Warm, Painted, Fine}};
        Require(Stage.StageVisualDensity({}, {}, WrongSource).error() == EditStatus::Stale,
            "the current render identity cannot certify an older committed source");
        const auto CurrentFine = Stage.Capture();
        const auto Yielded = Advance(std::nullopt, {});
        Require(Yielded.ReducedCount == 0 && Yielded.Restored == 0 && Stage.Capture() == CurrentFine,
            "view selector yields to a newer authoritative render generation");

        const std::array Reopened{VisualDensityInterest{EditKey, Fine}};
        Advance(Far, Reopened);
        Jobs.Drain();
        Advance(Far, Reopened);
        Require(Advance(Far, Reopened).ReducedCount == 1, "source prepared again after view re-entry");
        Require(Advance(std::nullopt, {}).Restored == 1, "disabling an active selector restores fine coverage");
        Advance(Far, Reopened);
        Jobs.Drain();
        Advance(Far, Reopened);
        Require(Advance(Far, Reopened).ReducedCount == 1, "retirement begins with a visible derivative");
        const auto BeforeRetire = Stage.Capture();
        const std::array Retired{ChunkPublicationCandidate{EditKey, {}, {}, true}};
        Stage.Stage({}, Retired).value()->Install(PublicationId + 1);
        Require(Project(BeforeRetire).MeshesRemoved == 2 && Advance(Near, Reopened).ReducedCount == 0 &&
            !Stage.Capture()->Chunks.contains(EditKey), "view state cannot resurrect a retired render resource");

        Require(Selector.Advance({}, Far, Visible, PublicationId).error() == EditStatus::Stale,
            "late view update cannot replay an earlier selection");
        std::array<VisualDensityInterest, ChunkCount + 1> TooMany{};
        Require(Selector.Advance({}, Far, TooMany, PublicationId + 1).error() == EditStatus::InvalidInput,
            "over-cap candidate input is denied");
        const auto Largest = [](const auto &Values) { return *std::ranges::max_element(Values); };
        std::cout << "ViewSweep," << StableUs.size() << ',' << PromoteUs.size() << ',' << RestoreUs.size() << ','
            << Largest(StableUs) << ',' << Largest(PromoteUs) << ',' << Largest(RestoreUs) << ','
            << PeakRollbackBytes << ',' << PeakVariantBytes << ',' << PeakWarmUploadBytes << ',' <<
            PeakPromoteUploadBytes << ',' << PeakRestoreUploadBytes << '\n';
        std::cout << "Checks,ProjectedBandFrameSlicingRetargetAndFineRestoration,Pass\n";
    }

    class RecordingProvider final : public CohortPublisher {
    public:
        bool Deny{};
        std::shared_ptr<const SurfaceMesh> Committed;
        std::shared_ptr<const SurfaceMesh> Proposed;
        std::expected<std::unique_ptr<StagedPublication>, EditStatus> Stage(GridSpec,
            std::span<const ChunkPublicationCandidate> Chunks) override {
            Require(Chunks.size() == 1 && Chunks.front().Key == Key && Chunks.front().Visual &&
                !Chunks.front().Collision.Indices.empty(), "provider receives an authored cohort");
            Proposed = Chunks.front().Visual;
            if (Deny) return std::unexpected(EditStatus::PublicationDenied);
            return std::make_unique<Token>(*this, Proposed);
        }

    private:
        struct Token final : StagedPublication {
            Token(RecordingProvider &Owner, std::shared_ptr<const SurfaceMesh> Mesh)
                : Owner(Owner), Mesh(std::move(Mesh)) {}
            void Install(Revision) noexcept override { Owner.Committed = std::move(Mesh); }
            RecordingProvider &Owner;
            std::shared_ptr<const SurfaceMesh> Mesh;
        };
    };

    void TestProviderDenialDoesNotAdmitSource() {
        Field World;
        JobSystem Jobs(1, JobSystem::StartPolicy::OnFirstSubmission);
        EditCoordinator Coordinator(World, Jobs);
        RecordingProvider Provider;
        VisualDensityPrefetch Prefetch(Jobs);
        SphereBrush Brush{{-16.0, 14.37, 15.61}, 6.25, BrushMode::Add, 1};
        Require(Coordinator.Submit(Brush).has_value(), "initial authored edit submission");
        Jobs.Drain();
        const auto Initial = Coordinator.Poll(Provider);
        Require(Initial.size() == 1 && Initial.front().Status == EditStatus::Committed && Provider.Committed,
            "initial authored edit commit");
        const auto Previous = Provider.Committed;
        const std::array Interests{VisualDensityInterest{Key, Previous}};
        Require(Prefetch.Advance(Interests), "initial committed source prefetch");
        Jobs.Drain();
        Require(Prefetch.Advance(Interests) && !Prefetch.IsPreparing(), "initial committed source collected");
        const auto Visual = Prefetch.Select(Key, Previous, true);
        Require(Visual != Previous, "provider test needs a reduced committed candidate");

        Provider.Deny = true;
        Brush.Mode = BrushMode::Paint;
        Brush.Material = 2;
        Require(Coordinator.Submit(Brush).has_value(), "paint submission");
        Jobs.Drain();
        const auto Denied = Coordinator.Poll(Provider);
        Require(Denied.size() == 1 && Denied.front().Status == EditStatus::PublicationDenied && World.GetRevision() == 1 &&
            Provider.Committed == Previous && Provider.Proposed != Previous &&
            Prefetch.Advance(Interests) && !Prefetch.IsPreparing() &&
            Prefetch.Select(Key, Provider.Committed, true) == Visual &&
            Prefetch.Select(Key, Provider.Proposed, true) == Provider.Proposed,
            "provider denial must preserve committed source, revision, and cached derivative");
        std::cout << "Checks,ProviderDenialPreservesCommittedSource,Pass\n";
    }
}

int main() try {
    std::cout << "Scene,Repetitions,FineColdP50Us,ReducedColdP50Us,FinePublishP50Us,ReducedPublishP50Us,"
                 "FineUploadBytes,ReducedUploadBytes\n";
    Benchmark(Fixture::Flat, "Flat");
    Benchmark(Fixture::Rolling, "Rolling");
    Benchmark(Fixture::ThinCave, "ThinCave");
    TestFineFallback();
    TestPrefetchAndCancellation();
    TestGuardedFallback();
    TestPerspectiveBound();
    TestViewSweep();
    TestProviderDenialDoesNotAdmitSource();
} catch (const std::exception &Error) {
    std::cerr << "[Terrain:VisualDensityRender] " << Error.what() << '\n';
    return 1;
}
