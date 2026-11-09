#include "UnifiedJoltProvider.hpp"
#include "../ExperimentSupport.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
    using namespace gargantuan;
    using PhysicsEvaluation::CreateUnifiedJoltProvider;
    using PhysicsEvaluation::GetJoltStepSchedule;

    void Require(bool Condition, const char *Message) {
        if (!Condition) throw std::runtime_error(Message);
    }
    PhysicsBodyDesc Ball(glm::vec3 Position) {
        PhysicsBodyDesc Description;
        Description.Shape.Kind = PhysicsShapeKind::Ball;
        Description.Shape.Size = glm::vec3(0.9f);
        Description.Transform.Position = Position;
        Description.GravityScale = 0.0f;
        Description.SleepEnabled = false;
        return Description;
    }
    PhysicsStepResult Step(PhysicsEvaluation::UnifiedJoltProvider &Provider,
                           std::uint32_t Updates, std::size_t SoftBodies) {
        auto Result = Provider.Rigid->Step({.DeltaTime = SoftBodyStepInterval, .SubStepCount = 16});
        const auto Schedule = GetJoltStepSchedule(*Provider.Rigid);
        Require(Schedule && Schedule->WorldUpdates == Updates &&
            Schedule->CollisionStepsPerUpdate == (Updates == 1 ? 4u : 8u) &&
            Schedule->EnabledSoftBodies == SoftBodies, "wrong native step schedule");
        (void)Provider.Soft->Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0, 0, 0}});
        return Result;
    }
    SoftBodyDefinition Cloth(bool Enabled) {
        SoftBodyDefinition Definition;
        Definition.Position = {0, 0, 0};
        Definition.Size = {4, 4, 0.2f};
        Definition.ResolutionX = Definition.ResolutionY = 9;
        Definition.Enabled = Enabled;
        for (std::uint32_t X = 0; X < 9; ++X)
            Definition.Attachments.push_back({X, {-2.0f + 0.5f * float(X), 2, 0}});
        return Definition;
    }
    void MembershipAndRigidContact() {
        PhysicsWorldConfig Config;
        Config.Gravity = {};
        Config.EnableSleep = false;
        auto Provider = CreateUnifiedJoltProvider(Config);
        Require(Provider.Rigid && Provider.Soft, "provider creation failed");
        Require(!GetJoltStepSchedule(*Provider.Rigid), "unstepped provider exposed a schedule");

        auto Floor = Ball({0, -0.8f, 0});
        Floor.Anchored = true;
        const auto FloorId = Provider.Rigid->CreateBody(Floor);
        const auto Moving = Provider.Rigid->CreateBody(Ball({0, 0, 0}));
        Require(FloorId.IsValid() && Moving.IsValid(), "rigid admission failed");
        auto First = Step(Provider, 1, 0);
        Require(std::ranges::any_of(First.Contacts, [](const auto &Contact) {
            return Contact.Phase == PhysicsContactPhase::Began;
        }), "fast rigid step lost contact begin");
        Floor.Transform.Position.y = -10.0f;
        Require(Provider.Rigid->UpdateBody(FloorId, Floor).Succeeded(),
            "moving rigid floor failed");
        const auto Separated = Step(Provider, 1, 0);
        Require(std::ranges::any_of(Separated.Contacts, [](const auto &Contact) {
            return Contact.Phase == PhysicsContactPhase::Ended;
        }), "fast rigid step retained moved collider contact");

        auto Disabled = Cloth(false);
        const auto Fabric = Provider.Soft->CreateBody(Disabled);
        Require(Fabric.IsValid(), "disabled soft admission failed");
        Step(Provider, 1, 0);
        auto Invalid = Disabled;
        Invalid.ResolutionX = 0;
        Require(!Provider.Soft->UpdateBody(Fabric, Invalid).Succeeded(),
            "invalid soft replacement accepted");
        Step(Provider, 1, 0);

        auto Enabled = Cloth(true);
        Require(Provider.Soft->UpdateBody(Fabric, Enabled).Succeeded(),
            "soft enable failed");
        Step(Provider, 2, 1);
        const auto Inspection = Provider.Soft->GetBodyInspection(Fabric);
        Require(Inspection && Inspection->Simulated, "enabled soft body missed its first coupled step");
        Enabled.Position.x = 8.0f;
        for (auto &Attachment : Enabled.Attachments) Attachment.Position.x += 8.0f;
        const auto Second = Provider.Soft->CreateBody(Enabled);
        Require(Second.IsValid(), "second enabled soft admission failed");
        Step(Provider, 2, 2);
        Require(Provider.Soft->DestroyBody(Fabric).Succeeded(), "first soft retirement failed");
        Step(Provider, 2, 1);
        Require(Provider.Soft->UpdateBody(Second, Disabled).Succeeded(),
            "soft disable failed");
        Step(Provider, 1, 0);
        Require(Provider.Soft->DestroyBody(Second).Succeeded(), "second soft retirement failed");
        Step(Provider, 1, 0);
        const auto Rigid = Provider.Rigid->GetBodyState(Moving);
        Require(Provider.Rigid->IsBodyValid(Moving) && Rigid &&
            std::isfinite(Rigid->Transform.Position.y),
            "rigid identity or state lost across schedule transitions");
    }
    void SensorGuardsTransientContact() {
        PhysicsWorldConfig Config;
        Config.Gravity = {};
        auto Provider = CreateUnifiedJoltProvider(Config);
        PhysicsBodyDesc Sensor;
        Sensor.Anchored = true;
        Sensor.Shape.Size = {1, 1, 1};
        Sensor.CanCollide = false;
        Sensor.CanTouch = true;
        const auto SensorId = Provider.Rigid->CreateBody(Sensor);
        auto VisitorDescription = Ball({-1, 0, 0});
        VisitorDescription.Shape.Size = glm::vec3(0.2f);
        const auto Visitor = Provider.Rigid->CreateBody(VisitorDescription);
        Require(SensorId.IsValid() && Visitor.IsValid(), "sensor fixture admission failed");
        Require(Provider.Rigid->SetLinearVelocity(Visitor, {120, 0, 0}).Succeeded(),
            "sensor visitor velocity failed");
        const auto Transient = Step(Provider, 2, 0);
        bool Began = false;
        bool Ended = false;
        for (const auto &Contact : Transient.Contacts) {
            if (!((Contact.BodyA == SensorId && Contact.BodyB == Visitor) ||
                  (Contact.BodyA == Visitor && Contact.BodyB == SensorId))) continue;
            Began |= Contact.Phase == PhysicsContactPhase::Began;
            Ended |= Contact.Phase == PhysicsContactPhase::Ended;
        }
        Require(Began && Ended, "transient sensor contact lost across half-updates");
        Sensor.CanCollide = true;
        Require(Provider.Rigid->UpdateBody(SensorId, Sensor).Succeeded(),
            "sensor to solid update failed");
        Step(Provider, 1, 0);
        Sensor.CanCollide = false;
        Require(Provider.Rigid->UpdateBody(SensorId, Sensor).Succeeded(),
            "solid to sensor update failed");
        Step(Provider, 2, 0);
        Require(Provider.Rigid->DestroyBody(SensorId).Succeeded(), "sensor retirement failed");
        Step(Provider, 1, 0);
        Require(Provider.Rigid->DestroyBody(Visitor).Succeeded(), "visitor retirement failed");
    }
}

int main() {
    PhysicsEvaluation::SuppressErrorDialogs();
    PhysicsEvaluation::InstallPhysicsAllocators();
    try {
        MembershipAndRigidContact();
        SensorGuardsTransientContact();
        std::cout << "[Physics:JoltScheduling] qualification passed\n";
        return 0;
    } catch (const std::exception &Error) {
        std::cerr << "[Physics:JoltScheduling] " << Error.what() << '\n';
        return 2;
    }
}
