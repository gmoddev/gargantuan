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
                           std::uint32_t Updates, std::size_t SoftBodies,
                           int RequestedCollisionSteps = 4) {
        auto Result = Provider.Rigid->Step({.DeltaTime = SoftBodyStepInterval,
            .SubStepCount = RequestedCollisionSteps});
        const auto Schedule = GetJoltStepSchedule(*Provider.Rigid);
        Require(Schedule && Schedule->WorldUpdates == Updates &&
            Schedule->CollisionStepsPerUpdate ==
                (Updates == 1 ? std::uint32_t(std::max(4, RequestedCollisionSteps)) : 8u) &&
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
        Floor.CanTouch = false;
        const auto FloorId = Provider.Rigid->CreateBody(Floor);
        auto MovingDescription = Ball({0, 0, 0});
        MovingDescription.CanTouch = false;
        const auto Moving = Provider.Rigid->CreateBody(MovingDescription);
        Require(FloorId.IsValid() && Moving.IsValid(), "rigid admission failed");
        Step(Provider, 1, 0);
        const auto FirstPairs = PhysicsEvaluation::GetJoltNativeRigidContactPairCount(*Provider.Rigid);
        Require(FirstPairs && *FirstPairs > 0,
            "fast rigid step lost native physical contact");
        Step(Provider, 1, 0, 12);
        Floor.Transform.Position.y = -10.0f;
        Require(Provider.Rigid->UpdateBody(FloorId, Floor).Succeeded(),
            "moving rigid floor failed");
        Step(Provider, 1, 0);
        const auto SeparatedPairs = PhysicsEvaluation::GetJoltNativeRigidContactPairCount(*Provider.Rigid);
        Require(SeparatedPairs && *SeparatedPairs == 0,
            "fast rigid step retained moved collider contact");

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
        Config.EnableSleep = false;
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
        auto Invalid = Sensor;
        Invalid.Shape.Size.x = 0;
        Require(!Provider.Rigid->UpdateBody(SensorId, Invalid).Succeeded(),
            "invalid sensor replacement accepted");
        Step(Provider, 2, 0);
        Sensor.CanCollide = true;
        Require(Provider.Rigid->UpdateBody(SensorId, Sensor).Succeeded(),
            "sensor to solid update failed");
        Step(Provider, 2, 0);
        Sensor.CanTouch = false;
        VisitorDescription.CanTouch = false;
        Require(Provider.Rigid->UpdateBody(SensorId, Sensor).Succeeded() &&
            Provider.Rigid->UpdateBody(Visitor, VisitorDescription).Succeeded(),
            "touch disable failed");
        Step(Provider, 1, 0);
        Sensor.CanCollide = false;
        Sensor.CanTouch = true;
        Require(Provider.Rigid->UpdateBody(SensorId, Sensor).Succeeded(),
            "solid to sensor update failed");
        Step(Provider, 2, 0);
        Require(Provider.Rigid->DestroyBody(SensorId).Succeeded(), "sensor retirement failed");
        Step(Provider, 1, 0);
        Require(!Provider.Rigid->UpdateBody(SensorId, Sensor).Succeeded(),
            "stale sensor identity updated a retired slot");
        Step(Provider, 1, 0);
        Require(Provider.Rigid->DestroyBody(Visitor).Succeeded(), "visitor retirement failed");
    }
    void SolidTouchGuardsFastCrossing() {
        PhysicsWorldConfig Config;
        Config.Gravity = {};
        Config.EnableSleep = false;
        auto Provider = CreateUnifiedJoltProvider(Config);
        PhysicsBodyDesc Wall;
        Wall.Anchored = true;
        Wall.Shape.Size = {1, 1, 1};
        const auto WallId = Provider.Rigid->CreateBody(Wall);
        auto VisitorDescription = Ball({-1, 0, 0});
        VisitorDescription.Shape.Size = glm::vec3(0.2f);
        const auto Visitor = Provider.Rigid->CreateBody(VisitorDescription);
        Require(WallId.IsValid() && Visitor.IsValid(), "solid touch fixture admission failed");
        Require(Provider.Rigid->SetLinearVelocity(Visitor, {120, 0, 0}).Succeeded(),
            "solid touch velocity failed");
        const auto ContactStep = Step(Provider, 2, 0);
        Require(std::ranges::any_of(ContactStep.Contacts, [&](const auto &Contact) {
            return Contact.Phase == PhysicsContactPhase::Began &&
                ((Contact.BodyA == WallId && Contact.BodyB == Visitor) ||
                 (Contact.BodyA == Visitor && Contact.BodyB == WallId));
        }), "solid crossing lost neutral touch begin");
        Wall.CanTouch = false;
        VisitorDescription.CanTouch = false;
        Require(Provider.Rigid->UpdateBody(WallId, Wall).Succeeded() &&
            Provider.Rigid->UpdateBody(Visitor, VisitorDescription).Succeeded(),
            "solid touch disable failed");
        Step(Provider, 1, 0);
    }
    void FailedTouchUpdateFailsClosed() {
        PhysicsWorldConfig Config;
        Config.Gravity = {};
        Config.EnableSleep = false;
        auto Provider = CreateUnifiedJoltProvider(Config);
        auto Description = Ball({0, 0, 0});
        Description.CanTouch = false;
        const auto Body = Provider.Rigid->CreateBody(Description);
        Require(Body.IsValid(), "failure injection rigid admission failed");
        Step(Provider, 1, 0);
        Require(PhysicsEvaluation::FailNextJoltTouchUpdateAfterNativeMutation(*Provider.Rigid),
            "failure injection unavailable");
        Description.CanTouch = true;
        Require(Provider.Rigid->UpdateBody(Body, Description).Status ==
            PhysicsOperationStatus::BackendFailure,
            "injected touch mutation did not fail");
        Step(Provider, 2, 0);
        Require(Provider.Rigid->UpdateBody(Body, Description).Succeeded(),
            "touch retry failed");
        Step(Provider, 2, 0);
        Description.CanTouch = false;
        Require(Provider.Rigid->UpdateBody(Body, Description).Succeeded(),
            "touch disable after partial failure failed");
        Step(Provider, 2, 0);
    }
    void JointMembershipGuardsCadence() {
        PhysicsWorldConfig Config;
        Config.Gravity = {};
        Config.EnableSleep = false;
        auto Provider = CreateUnifiedJoltProvider(Config);
        auto FirstDescription = Ball({0, 0, 0});
        auto SecondDescription = Ball({0, 3, 0});
        FirstDescription.CanTouch = false;
        SecondDescription.CanTouch = false;
        const auto First = Provider.Rigid->CreateBody(FirstDescription);
        const auto Second = Provider.Rigid->CreateBody(SecondDescription);
        Require(First.IsValid() && Second.IsValid(), "joint guard rigid admission failed");
        Step(Provider, 1, 0);

        PhysicsConstraintDesc Joint;
        Joint.Kind = PhysicsConstraintKind::Distance;
        Joint.BodyA = First;
        Joint.BodyB = Second;
        Joint.RestLength = 3.0f;
        auto Invalid = Joint;
        Invalid.BodyB = {};
        Require(!Provider.Rigid->TryCreateConstraint(Invalid).Succeeded(),
            "invalid joint endpoint accepted");
        Step(Provider, 1, 0);
        const auto FirstJoint = Provider.Rigid->TryCreateConstraint(Joint);
        Require(FirstJoint.Succeeded(), "joint guard admission failed");
        Step(Provider, 2, 0);
        Require(Provider.Rigid->DestroyConstraint(FirstJoint.Constraint).Succeeded(),
            "joint guard retirement failed");
        Step(Provider, 1, 0);
        Require(!Provider.Rigid->DestroyConstraint(FirstJoint.Constraint).Succeeded(),
            "stale joint identity retired a replacement");
        Step(Provider, 1, 0);

        const auto SecondJoint = Provider.Rigid->TryCreateConstraint(Joint);
        Require(SecondJoint.Succeeded(), "replacement joint admission failed");
        Step(Provider, 2, 0);
        Require(Provider.Rigid->DestroyBody(First).Succeeded(),
            "joint endpoint retirement failed");
        Step(Provider, 1, 0);
        Require(!Provider.Rigid->DestroyConstraint(SecondJoint.Constraint).Succeeded(),
            "retired endpoint left a live joint identity");
        Step(Provider, 1, 0);
    }
    void SleepEnabledWorldRetainsWakeCadence() {
        PhysicsWorldConfig Config;
        Config.Gravity = {};
        Config.EnableSleep = true;
        auto Provider = CreateUnifiedJoltProvider(Config);
        auto Description = Ball({0, 0, 0});
        Description.CanTouch = false;
        Description.SleepEnabled = true;
        const auto Body = Provider.Rigid->CreateBody(Description);
        Require(Body.IsValid(), "sleep guard rigid admission failed");
        for (int Frame = 0; Frame < 90; ++Frame) Step(Provider, 2, 0);
        const auto Resting = Provider.Rigid->GetBodyState(Body);
        Require(Resting && !Resting->Awake,
            "sleep-enabled no-touch body did not retire on high cadence");
        Require(Provider.Rigid->SetAwake(Body, true).Succeeded(),
            "sleeping body did not accept explicit wake");
        Require(Provider.Rigid->ApplyLinearImpulse(Body, {2, 0, 0}).Succeeded(),
            "woken body did not accept native impulse");
        Step(Provider, 2, 0);
        const auto Woken = Provider.Rigid->GetBodyState(Body);
        Require(Woken && Woken->Awake && Woken->LinearVelocity.x > 0 &&
            Woken->Transform.Position.x > Resting->Transform.Position.x,
            "sleep-enabled body did not move after native wake and impulse");
    }
}

int main() {
    PhysicsEvaluation::SuppressErrorDialogs();
    PhysicsEvaluation::InstallPhysicsAllocators();
    try {
        MembershipAndRigidContact();
        SensorGuardsTransientContact();
        SolidTouchGuardsFastCrossing();
        FailedTouchUpdateFailsClosed();
        JointMembershipGuardsCadence();
        SleepEnabledWorldRetainsWakeCadence();
        std::cout << "[Physics:JoltScheduling] qualification passed\n";
        return 0;
    } catch (const std::exception &Error) {
        std::cerr << "[Physics:JoltScheduling] " << Error.what() << '\n';
        return 2;
    }
}
