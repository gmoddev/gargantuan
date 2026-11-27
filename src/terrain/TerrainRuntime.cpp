// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "terrain/TerrainRuntime.hpp"

#include "terrain/PhysicsStage.hpp"
#include "terrain/VisualDensityView.hpp"
#include "gargantuan/Log.hpp"
#include "gargantuan/Profiler.hpp"
#include "gargantuan/render/RenderExtractor.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace gargantuan::terrain {
    namespace {
        class CombinedInstall final : public StagedPublication {
        public:
            explicit CombinedInstall(PhysicsStage &Provider) : Provider(Provider) {}
            void Install(Revision Value) noexcept override {
                Physical->Install(Value);
                if (!Physical->IsInstalled() || !Provider.IsHealthy()) return;
                Visual->Install(Value);
                Installed = Visual->IsInstalled();
            }
            [[nodiscard]] bool IsInstalled() const noexcept override { return Installed; }
            PhysicsStage &Provider;
            std::unique_ptr<StagedPublication> Visual;
            std::unique_ptr<StagedPublication> Physical;
            bool Installed{};
        };

        class RuntimePublisher final : public CohortPublisher {
        public:
            RuntimePublisher(RenderStage &Render, PhysicsStage &Physics) : Render(Render), Physics(Physics) {}
            std::expected<std::unique_ptr<StagedPublication>, EditStatus> Stage(GridSpec Grid,
                std::span<const ChunkPublicationCandidate> Chunks) override {
                auto Result = std::make_unique<CombinedInstall>(Physics);
                auto Visual = Render.Stage(Grid, Chunks);
                if (!Visual) return std::unexpected(Visual.error());
                auto Physical = Physics.Stage(Grid, Chunks);
                if (!Physical) return std::unexpected(Physical.error());
                Result->Visual = std::move(*Visual);
                Result->Physical = std::move(*Physical);
                return Result;
            }
        private:
            RenderStage &Render;
            PhysicsStage &Physics;
        };
    }

    TerrainRuntime::TerrainRuntime(Field &World, ObjectId TerrainOwner, PhysicsWorld &Physics, bool AllowEdits,
        bool AllowReplicaReplacement, bool ExperimentalVisualDensity, std::shared_ptr<const TerrainMaterialPalette> Palette)
        : Grid(World.GetGrid()), AllowEdits(AllowEdits), AllowReplicaReplacement(AllowReplicaReplacement),
          Physics(AllowReplicaReplacement ? std::make_unique<PhysicsStage>(Physics,
              PhysicsStageLimits{.MaxCohortChunks = 2 * MaximumReplicaContourChunks})
                                          : std::make_unique<PhysicsStage>(Physics)),
          Render(TerrainOwner, std::move(Palette)), Edits(std::in_place, World, Jobs) {
        if (ExperimentalVisualDensity) Detail = std::make_unique<VisualDensityView>(Jobs, Render);
    }

    TerrainRuntime::~TerrainRuntime() = default;

    std::expected<std::unique_ptr<TerrainRuntime>, EditStatus>
    TerrainRuntime::Start(Field &World, ObjectId TerrainOwner, PhysicsWorld &Physics, bool AllowEdits,
        bool AllowReplicaReplacement, bool ExperimentalVisualDensity, std::shared_ptr<const TerrainMaterialPalette> Palette) {
        if (!TerrainOwner.IsValid() || !Physics.IsValid() || (AllowEdits && AllowReplicaReplacement))
            return std::unexpected(EditStatus::Unavailable);
        try {
            auto Candidate = std::unique_ptr<TerrainRuntime>(
                new TerrainRuntime(World, TerrainOwner, Physics, AllowEdits, AllowReplicaReplacement,
                    ExperimentalVisualDensity, std::move(Palette)));
            RuntimePublisher Publisher(Candidate->Render, *Candidate->Physics);
            const auto Ready = Candidate->Edits->BuildInitialResidency(Publisher);
            if (!Ready) return std::unexpected(Ready.error());
            if (AllowReplicaReplacement) {
                Candidate->ReplicaSurfaces = Candidate->Edits->TakeResidentSurfaces();
                Candidate->Edits.reset();
            }
            return Candidate;
        } catch (const std::bad_alloc &) {
            return std::unexpected(EditStatus::LimitExceeded);
        }
    }

    std::expected<EditTicket, EditStatus> TerrainRuntime::Submit(EditRequest Request,
        std::optional<Revision> ExpectedRevision) {
        if (!AllowEdits || !Physics->IsHealthy()) return std::unexpected(EditStatus::Unavailable);
        return Edits->Submit(std::move(Request), ExpectedRevision);
    }

    bool TerrainRuntime::Cancel(EditTicket Ticket) noexcept {
        return Edits && Edits->Cancel(Ticket);
    }

    std::vector<EditReceipt> TerrainRuntime::Poll() {
        RuntimePublisher Publisher(Render, *Physics);
        return Edits ? Edits->Poll(Publisher) : std::vector<EditReceipt>{};
    }

    std::shared_ptr<const RenderState> TerrainRuntime::CaptureRender() const noexcept {
        return Render.Capture();
    }

    void TerrainRuntime::UpdateVisualDensity(const RenderCameraInput &Camera, std::uint32_t Width, std::uint32_t Height) {
        if (!Detail || VisualPublication == std::numeric_limits<Revision>::max()) return;
        G_PROFILE("Terrain::VisualInterest");
        std::optional<VisualDensityCamera> View;
        std::array<VisualDensityInterest, VisualDensityView::MaximumCandidates> Interests;
        std::array<double, VisualDensityView::MaximumCandidates> Scores;
        std::size_t Count = 0;
        const auto Fov = static_cast<double>(Camera.VerticalFieldOfView) * std::numbers::pi_v<double> / 180.0;
        const auto Look = glm::dvec3(Camera.LookDirection);
        const auto InitialRight = glm::cross(Look, glm::dvec3(Camera.UpDirection));
        if (Height && Width && Fov > 0.0 && Fov < std::numbers::pi_v<double> &&
            glm::dot(InitialRight, InitialRight) > 0.0) {
            const auto Forward = glm::normalize(Look);
            const auto Right = glm::normalize(InitialRight);
            const auto Up = glm::cross(Right, Forward);
            const auto TanY = std::tan(Fov * 0.5);
            const auto TanX = TanY * Width / Height;
            const auto Position = glm::dvec3(Camera.Position);
            View.emplace(VisualDensityCamera{{Position.x, Position.y, Position.z},
                {Forward.x, Forward.y, Forward.z}, Height * 0.5 / TanY});

            const auto Span = ChunkSide * Grid.GetSpacing();
            const auto Radius = std::sqrt(3.0) * Span * 0.5;
            const auto &Surfaces = Edits ? Edits->GetResidentSurfaces() : ReplicaSurfaces;
            for (const auto &[Key, Fine] : Surfaces) {
                const glm::dvec3 Center((Key.X + 0.5) * Span, (Key.Y + 0.5) * Span, (Key.Z + 0.5) * Span);
                const auto Delta = Center - Position;
                const auto Depth = glm::dot(Delta, Forward);
                if (Depth - Radius <= Camera.NearPlane || Depth + Radius >= Camera.FarPlane ||
                    std::abs(glm::dot(Delta, Right)) > (Depth + Radius) * TanX + Radius ||
                    std::abs(glm::dot(Delta, Up)) > (Depth + Radius) * TanY + Radius) continue;
                const auto Pixels = GetVisualDensityPixelBound(Grid, Key, *View);
                if (Pixels > VisualDensityView::PrefetchPixels) continue;
                std::size_t Index = Count;
                if (Count == Interests.size()) {
                    const auto Worst = std::ranges::max_element(Scores);
                    if (Pixels >= *Worst) continue;
                    Index = static_cast<std::size_t>(Worst - Scores.begin());
                } else {
                    ++Count;
                }
                Interests[Index] = {Key, Fine};
                Scores[Index] = Pixels;
            }
        }
        const auto Updated = Detail->Advance(Grid, View, std::span(Interests).first(Count), ++VisualPublication);
        if (!Updated && !VisualAdmissionLogged) {
            LOG_WARN(App, "[Terrain:LOD] Visual detail update exceeded its admission limits");
            VisualAdmissionLogged = true;
        }
    }

    std::expected<void, EditStatus> TerrainRuntime::InstallReplica(
        GridSpec Grid, std::vector<ChunkPublicationCandidate> Prepared, Revision Publication) {
        if (!AllowReplicaReplacement || !Physics->IsHealthy()) return std::unexpected(EditStatus::Unavailable);
        if (Publication <= ReplicaPublication) return std::unexpected(EditStatus::Stale);
        if (Prepared.size() > MaximumReplicaContourChunks) return std::unexpected(EditStatus::LimitExceeded);
        try {
            std::map<ChunkKey, std::shared_ptr<const SurfaceMesh>> NextSurfaces;
            std::vector<ChunkPublicationCandidate> Changed;
            const auto Previous = Render.Capture();
            Changed.reserve(Prepared.size() + (Previous ? Previous->Chunks.size() : 0));
            for (auto &Chunk : Prepared) {
                if (!Chunk.Visual || !NextSurfaces.emplace(Chunk.Key, Chunk.Visual).second)
                    return std::unexpected(EditStatus::InvalidInput);
                const auto Old = ReplicaSurfaces.find(Chunk.Key);
                if (Old != ReplicaSurfaces.end() && Old->second->Spacing == Chunk.Visual->Spacing &&
                    Old->second->Indices == Chunk.Visual->Indices) {
                    if (Old->second->Vertices == Chunk.Visual->Vertices) {
                        NextSurfaces.find(Chunk.Key)->second = Old->second;
                        continue;
                    }
                    // Material and normal changes leave the collision shape in place.
                    Chunk.GeometryChanged = !std::ranges::equal(Old->second->Vertices, Chunk.Visual->Vertices,
                        [](const auto &Left, const auto &Right) {
                            return Left.Position == Right.Position && Left.EdgeAxis == Right.EdgeAxis &&
                                Left.EdgeStart == Right.EdgeStart;
                        });
                }
                Changed.push_back(std::move(Chunk));
            }
            if (Previous)
                for (const auto &[Key, Chunk] : Previous->Chunks) {
                    static_cast<void>(Chunk);
                    if (!NextSurfaces.contains(Key)) Changed.push_back({.Key = Key, .GeometryChanged = true});
                }
            if (!Changed.empty()) {
                std::ranges::sort(Changed, {}, &ChunkPublicationCandidate::Key);
                RuntimePublisher Publisher(Render, *Physics);
                auto Staged = Publisher.Stage(Grid, Changed);
                if (!Staged) return std::unexpected(Staged.error());
                (*Staged)->Install(Publication);
                if (!(*Staged)->IsInstalled()) return std::unexpected(EditStatus::Unavailable);
            }
            ReplicaSurfaces.swap(NextSurfaces);
            ReplicaPublication = Publication;
            return {};
        } catch (const std::bad_alloc &) {
            return std::unexpected(EditStatus::LimitExceeded);
        }
    }
}
