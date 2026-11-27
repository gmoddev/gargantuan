// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/terrain/Field.hpp"
#include "gargantuan/terrain/Surface.hpp"

#include <optional>
#include <variant>

namespace gargantuan {
    class JobSystem;
}

namespace gargantuan::terrain {
    struct SmoothEdit {
        SampleBox Bounds;
        std::uint8_t Strength{};
    };

    using EditRequest = std::variant<std::vector<SampleWrite>, SphereBrush, BoxBrush, SmoothEdit>;
    using EditTicket = std::uint64_t;

    enum class EditStatus {
        Committed,
        Canceled,
        InvalidInput,
        LimitExceeded,
        Stale,
        PreparationFailed,
        PublicationDenied,
        Unavailable,
        ReopenCapacityExceeded,
    };

    struct EditReceipt {
        EditTicket Ticket{};
        EditStatus Status{};
        Revision CommittedRevision{};
    };

    // Pure admission shared by persisted scenes and runtime startup.
    [[nodiscard]] std::expected<void, EditStatus> ValidateTerrainReopenProfile(const Field &Storage);

    struct ChunkPublicationCandidate {
        ChunkKey Key;
        std::shared_ptr<const SurfaceMesh> Visual;
        CollisionSurfaceMesh Collision;
        bool GeometryChanged{};
        std::size_t RenderBytesFloor{};
    };

    class StagedPublication {
    public:
        virtual ~StagedPublication() = default;
        // Installation runs on Main with no simulation or query interleave.
        // Destruction without installation releases all staged resources.
        virtual void Install(Revision Value) noexcept = 0;
        [[nodiscard]] virtual bool IsInstalled() const noexcept { return true; }
    };

    class CohortPublisher {
    public:
        virtual ~CohortPublisher() = default;
        [[nodiscard]] virtual std::expected<std::unique_ptr<StagedPublication>, EditStatus>
        Stage(GridSpec Grid, std::span<const ChunkPublicationCandidate> Chunks) = 0;
    };

    [[nodiscard]] std::expected<void, EditStatus> PrepareTerrainPreviewCohorts(
        const Field &Storage, JobSystem &Jobs, CohortPublisher &Publisher);

    // The caller exclusively owns the detached field throughout preparation.
    // A 3³ source neighborhood has at most 4³ contour owners.
    inline constexpr std::size_t MaximumReplicaContourChunks = 64;
    [[nodiscard]] std::expected<std::vector<ChunkPublicationCandidate>, EditStatus>
    PrepareTerrainReplica(const Field &Storage, std::stop_token Stop);

    // A terrain owner calls these methods only on Main. Workers own their
    // snapshots, stop state and results and never retain this or a Field pointer.
    class EditCoordinator final {
    public:
        EditCoordinator(Field &Storage, JobSystem &Jobs);
        ~EditCoordinator();
        EditCoordinator(const EditCoordinator &) = delete;
        EditCoordinator &operator=(const EditCoordinator &) = delete;

        [[nodiscard]] std::expected<EditTicket, EditStatus> Submit(EditRequest Request,
            std::optional<Revision> ExpectedRevision = std::nullopt);
        [[nodiscard]] bool Cancel(EditTicket Ticket) noexcept;
        [[nodiscard]] std::vector<EditReceipt> Poll(CohortPublisher &Publisher);
        [[nodiscard]] std::size_t GetPendingCount() const noexcept;
        [[nodiscard]] std::size_t GetActiveCount() const noexcept;

    private:
        friend class TerrainRuntime;
        // Only used during detached runtime construction. Failure destroys the
        // owning publisher and any earlier batches admitted to it.
        [[nodiscard]] std::expected<void, EditStatus> BuildInitialResidency(CohortPublisher &Publisher);
        [[nodiscard]] std::map<ChunkKey, std::shared_ptr<const SurfaceMesh>> TakeResidentSurfaces();
        [[nodiscard]] const std::map<ChunkKey, std::shared_ptr<const SurfaceMesh>> &GetResidentSurfaces() const;

        struct Impl;
        std::unique_ptr<Impl> State;
    };
}
