#include "UnifiedJoltProvider.hpp"
#include "../ExperimentSupport.hpp"

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
}

int main() {
    PhysicsEvaluation::SuppressErrorDialogs();
    PhysicsEvaluation::InstallPhysicsAllocators();
    try {
        PhysicsWorldConfig Config;
        Config.Gravity = {0, -9.81f, 0};
        SoftBodyWorldLimits Limits;
        Limits.MaximumWorkers = 1;
        auto Provider = PhysicsEvaluation::CreateUnifiedJoltProvider(Config, Limits, 1);
        Require(Provider.Soft && Provider.Soft->IsValid(), "Unified provider unavailable");
        auto Definition = Cloth(64);

        const auto First = Create(*Provider.Soft, Definition, 0);
        const auto FirstState = Provider.Soft->GetBodyState(First);
        Require(FirstState && FirstState->Positions && FirstState->Positions->size() == 4096,
            "First topology missing");
        const auto FirstInspection = Provider.Soft->GetBodyInspection(First);
        Require(FirstInspection && FirstInspection->ConstraintCount > 0, "First constraints missing");
        Require(Provider.Soft->DestroyBody(First).Succeeded(), "First retirement failed");
        Require(!Provider.Soft->IsBodyValid(First), "Retired ID remained valid");

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
        std::cout << "[Physics:AdmissionCache] PASS exact-key hits, invalidation, cap and retirement\n";
        return 0;
    } catch (const std::exception &Error) {
        std::cerr << "[Physics:AdmissionCache] " << Error.what() << '\n';
        return 1;
    }
}
