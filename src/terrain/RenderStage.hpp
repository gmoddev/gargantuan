// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/render/RenderPublication.hpp"
#include "terrain/EditCoordinator.hpp"

#include <map>

namespace gargantuan::terrain {
    class TerrainMaterialPalette;
    enum class TerrainMaterialRenderMode : std::uint8_t { Tint, PrimitivePbr };

    struct RenderChunk {
        RenderMeshCreate Resource;
        RenderObjectCreate Draw;
        std::size_t Bytes{};
        std::size_t AdmissionBytes{};
        std::weak_ptr<const SurfaceMesh> Source;
    };

    struct RenderVisualVariant {
        RenderChunk Fine;
        RenderChunk Reduced;
    };

    struct RenderState {
        std::map<ChunkKey, RenderChunk> Chunks;
        std::map<ChunkKey, RenderVisualVariant> VisualVariants;
        std::shared_ptr<const RenderTerrainDraws> Draws;
        std::size_t Bytes{};
        std::size_t AdmissionBytes{};
        std::size_t VisualVariantBytes{};
    };

    enum class RenderVisualAction : std::uint8_t { Warm, Fine, Reduced, Evict };

    struct RenderVisualChange {
        ChunkKey Key;
        RenderMeshIdentity ExpectedFine;
        RenderVisualAction Action{};
        std::shared_ptr<const SurfaceMesh> Fine;
        std::shared_ptr<const SurfaceMesh> Reduced;
    };

    // One copy-on-write publication owner per terrain scope. This holds only
    // renderer-neutral immutable CPU values. The GPU cache retires buffers.
    class RenderStage final : public CohortPublisher {
    public:
        static constexpr std::size_t MaximumVisualVariants = 16;
        static constexpr std::size_t MaximumVisualVariantBytes = 4 * 1024 * 1024;
        static constexpr std::size_t MaximumWarmVariants = 2;
        static constexpr std::size_t MaximumWarmBytes = 256 * 1024;

        explicit RenderStage(ObjectId Owner);
        RenderStage(ObjectId Owner, std::shared_ptr<const TerrainMaterialPalette> Palette,
            TerrainMaterialRenderMode Mode = TerrainMaterialRenderMode::Tint);
        ~RenderStage() override;
        RenderStage(const RenderStage &) = delete;
        RenderStage &operator=(const RenderStage &) = delete;
        [[nodiscard]] std::expected<std::unique_ptr<StagedPublication>, EditStatus>
        Stage(GridSpec Grid, std::span<const ChunkPublicationCandidate> Chunks) override;
        // Warm publishes an inactive resource. A later stage may bind it.
        // Fine replacements and visual changes have disjoint, sorted keys.
        [[nodiscard]] std::expected<std::unique_ptr<StagedPublication>, EditStatus>
        StageVisualDensity(GridSpec Grid, std::span<const ChunkPublicationCandidate> Chunks,
            std::span<const RenderVisualChange> Changes);
        [[nodiscard]] std::shared_ptr<const RenderState> Capture() const noexcept;

    private:
        friend class TerrainRenderPreview;
        RenderStage();
        class Token;
        struct Impl;
        std::shared_ptr<Impl> State;
    };

    // A detached editor preview can be prepared before the replacement scene
    // has its canonical Terrain instance. Bind once before publishing draws.
    class TerrainRenderPreview final {
    public:
        TerrainRenderPreview() = default;
        TerrainRenderPreview(TerrainRenderPreview &&) noexcept = default;
        TerrainRenderPreview &operator=(TerrainRenderPreview &&) noexcept = default;
        [[nodiscard]] static std::expected<TerrainRenderPreview, EditStatus> Prepare(const Field &Storage);
        [[nodiscard]] std::shared_ptr<const RenderState> Bind(ObjectId Owner) && noexcept;

    private:
        std::shared_ptr<RenderState> State;
        std::shared_ptr<RenderTerrainDraws> Draws;
    };

    [[nodiscard]] std::shared_ptr<const RenderState> GetFineTerrainRenderState(
        const std::shared_ptr<const RenderState> &State);
    void AppendTerrainRenderState(RenderPublication &Publication, const std::shared_ptr<const RenderState> &Before,
        const std::shared_ptr<const RenderState> &After);
}
