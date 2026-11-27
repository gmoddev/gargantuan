// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/runtime/JobSystem.hpp"
#include "terrain/EditCoordinator.hpp"

#include <iostream>
#include <stdexcept>

using namespace gargantuan;
using namespace gargantuan::terrain;

namespace {
    void Require(bool Condition, const char *Message) {
        if (!Condition) throw std::runtime_error(Message);
    }

    class RecordingPublisher final : public CohortPublisher {
    public:
        bool Deny{};
        unsigned Stages{}, Installs{}, Rollbacks{};
        Revision LastRevision{};
        bool PaintOnly{};
        unsigned FailInstallOnStage{};

        std::expected<std::unique_ptr<StagedPublication>, EditStatus>
        Stage(GridSpec, std::span<const ChunkPublicationCandidate> Chunks) override {
            ++Stages;
            if (Deny) return std::unexpected(EditStatus::PublicationDenied);
            for (const auto &Chunk : Chunks) {
                if (Chunk.Visual && Chunk.Collision.Indices.empty())
                    throw std::logic_error("uncoupled visual and collision candidate");
                if (!Chunk.GeometryChanged) PaintOnly = true;
            }
            return std::make_unique<Token>(*this, Stages != FailInstallOnStage);
        }

    private:
        struct Token final : StagedPublication {
            Token(RecordingPublisher &Owner, bool Readiness) : Owner(Owner), Readiness(Readiness) {}
            ~Token() override { if (!Installed) ++Owner.Rollbacks; }
            void Install(Revision Value) noexcept override {
                Installed = Readiness;
                if (!Installed) return;
                ++Owner.Installs;
                Owner.LastRevision = Value;
            }
            [[nodiscard]] bool IsInstalled() const noexcept override { return Installed; }
            RecordingPublisher &Owner;
            bool Readiness;
            bool Installed{};
        };
    };

    SphereBrush Brush(double X, BrushMode Mode = BrushMode::Add) {
        return {{X, 14.37, 15.61}, 3.25, Mode, 67434};
    }

    void TestCompleteAndStale() {
        Field World;
        JobSystem Jobs(2, JobSystem::StartPolicy::OnFirstSubmission);
        EditCoordinator Coordinator(World, Jobs);
        RecordingPublisher Publisher;
        const auto First = Coordinator.Submit(Brush(14.21)).value();
        const auto Overlap = Coordinator.Submit(Brush(16.42)).value();
        const auto Disjoint = Coordinator.Submit(Brush(142.81)).value();
        Require(World.GetRevision() == 0 && Coordinator.GetPendingCount() == 3, "no early field publication");
        Jobs.Drain();
        const auto Receipts = Coordinator.Poll(Publisher);
        Require(Receipts.size() == 3 && Receipts[0].Ticket == First && Receipts[0].Status == EditStatus::Committed,
            "first edit committed");
        Require(Receipts[1].Ticket == Overlap && Receipts[1].Status == EditStatus::Stale, "overlapping base rejected");
        Require(Receipts[2].Ticket == Disjoint && Receipts[2].Status == EditStatus::Committed,
            "disjoint base remains valid");
        Require(Publisher.Stages == 2 && Publisher.Installs == 2 && Publisher.LastRevision == 2,
            "field and publisher share committed revision");
        Require(World.Read({14, 14, 16})->Density < 0 && Coordinator.GetActiveCount() == 2,
            "resident chunks own committed visual candidates");

        auto Paint = Brush(14.21, BrushMode::Paint);
        Paint.Material = 223145;
        Require(Coordinator.Submit(Paint).has_value(), "paint submitted");
        Jobs.Drain();
        Require(Coordinator.Poll(Publisher).front().Status == EditStatus::Committed && Publisher.PaintOnly &&
            World.Read({14, 14, 16})->Material == Paint.Material, "paint commits without geometry extraction path");

        const auto Expected = World.GetRevision();
        Require(Coordinator.Submit(Brush(270.4), Expected).has_value() &&
            Coordinator.Submit(Brush(398.4), Expected).has_value(), "disjoint explicit preconditions admitted");
        Jobs.Drain();
        const auto Conditional = Coordinator.Poll(Publisher);
        Require(Conditional.size() == 2 && Conditional[0].Status == EditStatus::Committed &&
            Conditional[1].Status == EditStatus::Stale && World.GetRevision() == Expected + 1,
            "explicit revision checked again before disjoint commit");
    }

    void TestDenialAndCancellation() {
        Field World;
        JobSystem Jobs(2, JobSystem::StartPolicy::OnFirstSubmission);
        EditCoordinator Coordinator(World, Jobs);
        RecordingPublisher Publisher;
        Publisher.Deny = true;
        const auto Denied = Coordinator.Submit(Brush(14.21)).value();
        const auto Canceled = Coordinator.Submit(Brush(142.81)).value();
        Require(Coordinator.Cancel(Canceled), "queued cancellation admitted");
        Jobs.Drain();
        const auto Receipts = Coordinator.Poll(Publisher);
        Require(Receipts.size() == 2 && Receipts[0].Ticket == Denied &&
            Receipts[0].Status == EditStatus::PublicationDenied && Receipts[1].Ticket == Canceled &&
            Receipts[1].Status == EditStatus::Canceled, "expected terminal denial and cancellation");
        Require(World.GetRevision() == 0 && World.GetStats().Entries == 0 && Publisher.Installs == 0 &&
            Coordinator.GetActiveCount() == 0, "failed edits release field and registry reservations");
    }

    void TestFieldAdmissionRollback() {
        Field World({}, {32, 64});
        JobSystem Jobs(1, JobSystem::StartPolicy::OnFirstSubmission);
        EditCoordinator Coordinator(World, Jobs);
        RecordingPublisher Publisher;
        Require(Coordinator.Submit(Brush(14.21)).has_value(), "tight field admits asynchronous preparation");
        Jobs.Drain();
        Require(Coordinator.Poll(Publisher).front().Status == EditStatus::LimitExceeded, "owner byte cap checked at commit");
        Require(Publisher.Stages == 1 && Publisher.Installs == 0 && Publisher.Rollbacks == 1 &&
            World.GetRevision() == 0 && Coordinator.GetActiveCount() == 0, "stage is discarded after commit denial");
    }

    void TestRetirementWhileJobsRun() {
        JobSystem Jobs(1, JobSystem::StartPolicy::OnFirstSubmission);
        {
            Field World;
            EditCoordinator Coordinator(World, Jobs);
            for (int I = 0; I < 4; ++I) Require(Coordinator.Submit(Brush(14.21 + 128 * I)).has_value(), "four jobs admitted");
            Require(Coordinator.Submit(Brush(526.21)).error() == EditStatus::LimitExceeded, "outstanding limit checked");
        }
        Jobs.Drain();
    }

    void TestPostCommitProviderFault() {
        Field World;
        JobSystem Jobs(2, JobSystem::StartPolicy::OnFirstSubmission);
        EditCoordinator Coordinator(World, Jobs);
        RecordingPublisher Publisher;
        Publisher.FailInstallOnStage = 2;
        Require(Coordinator.Submit(Brush(14.21)).has_value() && Coordinator.Submit(Brush(142.81)).has_value(),
            "two disjoint jobs admitted");
        Jobs.Drain();
        const auto Receipts = Coordinator.Poll(Publisher);
        Require(Receipts.size() == 2 && Receipts[0].Status == EditStatus::Committed &&
            Receipts[0].CommittedRevision == 1, "prior successful install stays acknowledged");
        Require(Receipts[1].Status == EditStatus::Unavailable && Receipts[1].CommittedRevision == 2 &&
            World.GetRevision() == 2, "provider fault cannot masquerade as readiness or precommit denial");
    }
}

int main() try {
    TestCompleteAndStale();
    TestDenialAndCancellation();
    TestFieldAdmissionRollback();
    TestRetirementWhileJobsRun();
    TestPostCommitProviderFault();
    std::cout << "[Terrain:CoordinatorTests] Passed\n";
    return 0;
} catch (const std::exception &Error) {
    std::cerr << "[Terrain:CoordinatorTests] " << Error.what() << '\n';
    return 1;
}
