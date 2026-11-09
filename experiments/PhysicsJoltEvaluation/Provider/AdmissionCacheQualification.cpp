#include "UnifiedJoltProvider.hpp"
#include "../ExperimentSupport.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

#ifndef PHYSICS_EVALUATION_ADMISSION_PROFILE
#error "AdmissionCacheQualification requires PHYSICS_EVALUATION_ADMISSION_PROFILE"
#endif

namespace {
    using namespace gargantuan;

    void Require(bool Condition, const char *Message) {
        if (!Condition) throw std::runtime_error(Message);
    }

    SoftBodyDefinition Cloth(std::uint32_t Resolution) {
        SoftBodyDefinition Definition;
        Definition.Kind = SoftBodyKind::Cloth;
        Definition.Position = {0, 3, 0};
        Definition.Size = {8, 8, 0.5f};
        Definition.ResolutionX = Resolution;
        Definition.ResolutionY = Resolution;
        for (std::uint32_t X = 0; X < Resolution; ++X) {
            const float Horizontal = float(X) / float(Resolution - 1) - 0.5f;
            Definition.Attachments.push_back({X, Definition.Position +
                glm::vec3(Definition.Size.x * Horizontal, Definition.Size.y * 0.5f, 0)});
        }
        return Definition;
    }

    SoftBodyId Create(gargantuan::ISoftBodyBackend &Backend,
                      const SoftBodyDefinition &Definition, std::uint64_t ExpectedHits) {
        PhysicsEvaluation::ResetJoltAdmissionProfile();
        const auto Id = Backend.CreateBody(Definition);
        Require(Id.IsValid(), "Admission failed");
        Require(PhysicsEvaluation::GetJoltAdmissionProfile().CacheHits == ExpectedHits,
            "Unexpected topology cache hit/miss");
        return Id;
    }

    void Advance(PhysicsEvaluation::UnifiedJoltProvider &Provider,
                 std::size_t ExpectedBodies) {
        const auto RigidResult = Provider.Rigid->Step({.DeltaTime = 1.0f / 60.0f,
            .SubStepCount = 16});
        Require(!RigidResult.EventsTruncated, "Unified rigid step truncated events");
        const auto Result = Provider.Soft->Step({.DeltaTime = 1.0f / 60.0f,
            .Gravity = {0, 0, 0}});
        Require(!Result.BodiesTruncated && !Result.VerticesTruncated &&
            !Result.CollidersTruncated &&
            Result.States.size() == ExpectedBodies &&
            Result.Profile.SimulatedBodies == ExpectedBodies &&
            Result.Profile.FrozenBodies == 0 && Result.Profile.BacklogDrops == 0 &&
            Result.Profile.StaleResults == 0, "Unified soft step lost work");
    }

    void ConcurrentSharedSettings() {
        PhysicsWorldConfig Config;
        Config.Gravity = {0, 0, 0};
        SoftBodyWorldLimits Limits;
        Limits.MaximumWorkers = 1;
        Limits.MaximumBodies = 2;
        auto Shared = PhysicsEvaluation::CreateUnifiedJoltProvider(Config, Limits, 1);
        auto Control = PhysicsEvaluation::CreateUnifiedJoltProvider(Config, Limits, 1);
        Require(Shared.Soft && Control.Soft && Shared.Soft->IsValid() &&
            Control.Soft->IsValid(), "Concurrent control worlds unavailable");
        auto Definition = Cloth(32);
        Definition.CollisionMode = SoftBodyCollisionMode::None;
        const auto A = Create(*Shared.Soft, Definition, 0);
        const auto B = Create(*Shared.Soft, Definition, 1);
        const auto Reference = Create(*Control.Soft, Definition, 0);
        const auto InitialA = Shared.Soft->GetBodyState(A);
        const auto InitialB = Shared.Soft->GetBodyState(B);
        Require(InitialA && InitialB && InitialA->Indices == InitialB->Indices &&
            InitialA->Positions && InitialB->Positions &&
            *InitialA->Positions == *InitialB->Positions,
            "Concurrent bodies did not share immutable topology and initial geometry");
        Require(Shared.Soft->ApplyImpulse(A, {190.0f, 0, 0}).Succeeded(),
            "Concurrent impulse failed");
        for (int Frame = 0; Frame < 2; ++Frame) {
            Advance(Shared, 2);
            Advance(Control, 1);
            const auto StateA = Shared.Soft->GetBodyState(A);
            const auto StateB = Shared.Soft->GetBodyState(B);
            const auto StateReference = Control.Soft->GetBodyState(Reference);
            Require(StateA && StateB && StateReference && StateA->Positions &&
                StateB->Positions && StateReference->Positions &&
                *StateB->Positions == *StateReference->Positions &&
                *StateA->Positions != *StateB->Positions,
                "Shared settings coupled independent mutable motion");
        }
        Require(Shared.Soft->DestroyBody(A).Succeeded(), "Concurrent A retirement failed");
        auto Changed = Definition;
        Changed.Position.x += 20.0f;
        for (auto &Attachment : Changed.Attachments) Attachment.Position.x += 20.0f;
        const auto C = Create(*Shared.Soft, Changed, 0);
        const auto BBefore = Shared.Soft->GetBodyState(B);
        Require(BBefore && BBefore->Indices == InitialB->Indices,
            "Live body lost shared topology on cache eviction");
        for (int Frame = 0; Frame < 2; ++Frame) {
            Advance(Shared, 2);
            Advance(Control, 1);
            const auto StateB = Shared.Soft->GetBodyState(B);
            const auto StateReference = Control.Soft->GetBodyState(Reference);
            Require(StateB && StateReference && StateB->Positions &&
                StateReference->Positions &&
                StateB->Indices == InitialB->Indices &&
                StateB->TopologyRevision == InitialB->TopologyRevision &&
                *StateB->Positions == *StateReference->Positions,
                "Live body changed after cache eviction");
        }
        Require(Shared.Soft->DestroyBody(C).Succeeded() &&
            Shared.Soft->DestroyBody(B).Succeeded() &&
            Control.Soft->DestroyBody(Reference).Succeeded(),
            "Concurrent retirement failed");
    }
}

int main() {
    PhysicsEvaluation::SuppressErrorDialogs();
    PhysicsEvaluation::InstallPhysicsAllocators();
    try {
        PhysicsWorldConfig Config;
        Config.Gravity = {0, -9.81f, 0};
        SoftBodyWorldLimits Limits;
        Limits.MaximumWorkers = 1;
        Limits.MaximumBodies = 1;
        auto Provider = PhysicsEvaluation::CreateUnifiedJoltProvider(Config, Limits, 1);
        Require(Provider.Soft && Provider.Soft->IsValid(), "Unified provider unavailable");
        auto Definition = Cloth(64);

        const auto First = Create(*Provider.Soft, Definition, 0);
        const auto FirstState = Provider.Soft->GetBodyState(First);
        Require(FirstState && FirstState->Positions && FirstState->Positions->size() == 4096,
            "First topology missing");
        const auto FirstInspection = Provider.Soft->GetBodyInspection(First);
        Require(FirstInspection && FirstInspection->ConstraintCount > 0, "First constraints missing");
        PhysicsEvaluation::ResetJoltAdmissionProfile();
        Require(!Provider.Soft->CreateBody(Definition).IsValid(),
            "Live body limit admitted a second body");
        Require(PhysicsEvaluation::GetJoltAdmissionProfile().CacheHits == 0,
            "Rejected body reported successful reuse");
        Require(Provider.Soft->DestroyBody(First).Succeeded(), "First retirement failed");
        Require(!Provider.Soft->IsBodyValid(First), "Retired ID remained valid");

        auto Invalid = Definition;
        Invalid.Size.x = -1.0f;
        Require(!Provider.Soft->CreateBody(Invalid).IsValid(), "Invalid shape admitted");

        const auto Warm = Create(*Provider.Soft, Definition, 1);
        const auto WarmState = Provider.Soft->GetBodyState(Warm);
        Require(Warm != First && WarmState && WarmState->Positions &&
            *WarmState->Positions == *FirstState->Positions &&
            WarmState->Indices == FirstState->Indices,
            "Warm shared topology changed initial state or identity");
        Require(Provider.Soft->DestroyBody(Warm).Succeeded(), "Warm retirement failed");

        auto Changed = Definition;
        Changed.Position.x = 20.0f;
        for (auto &Attachment : Changed.Attachments) Attachment.Position.x += 20.0f;
        const auto Shifted = Create(*Provider.Soft, Changed, 0);
        Require(Provider.Soft->DestroyBody(Shifted).Succeeded(), "Shifted retirement failed");

        Changed = Definition;
        Changed.Material.StretchCompliance *= 2.0f;
        const auto MaterialChanged = Create(*Provider.Soft, Changed, 0);
        Require(Provider.Soft->DestroyBody(MaterialChanged).Succeeded(), "Material retirement failed");

        Changed = Definition;
        Changed.Attachments.front().Position.x += 0.125f;
        const auto PinChanged = Create(*Provider.Soft, Changed, 0);
        Require(Provider.Soft->DestroyBody(PinChanged).Succeeded(), "Pin retirement failed");

        // 129^2 vertices exceed the single-entry cache bound. Both admissions
        // must rebuild, even though the semantic definition is identical.
        const auto Oversized = Cloth(129);
        const auto LargeFirst = Create(*Provider.Soft, Oversized, 0);
        Require(Provider.Soft->DestroyBody(LargeFirst).Succeeded(), "Large retirement failed");
        const auto LargeSecond = Create(*Provider.Soft, Oversized, 0);
        Require(Provider.Soft->DestroyBody(LargeSecond).Succeeded(), "Repeated large retirement failed");

        Provider.Soft.reset();
        Provider.Rigid.reset();
        ConcurrentSharedSettings();
        std::cout << "[Physics:AdmissionCache] PASS exact-key hits, independent live motion, eviction, cap and retirement\n";
        return 0;
    } catch (const std::exception &Error) {
        std::cerr << "[Physics:AdmissionCache] " << Error.what() << '\n';
        return 1;
    }
}
