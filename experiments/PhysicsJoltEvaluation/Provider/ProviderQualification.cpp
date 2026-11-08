#include "UnifiedJoltProvider.hpp"
#include "../ExperimentSupport.hpp"
#include "gargantuan/classes/Cloth.hpp"
#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/RubberBody.hpp"
#include "gargantuan/classes/JointConstraint.hpp"
#include "gargantuan/classes/SoftBodyAttachment.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/services/AssetService.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <vector>

namespace gargantuan {
    struct WorldRootTestAccess {
        static void Inject(WorldRoot &World, PhysicsEvaluation::UnifiedJoltProvider Provider) {
            World.Physics = PhysicsWorld(std::move(Provider.Rigid));
            World.Deformables = SoftBodyWorld(std::move(Provider.Soft));
        }
        static PhysicsBodyId Body(const WorldRoot &World, const BasePart &Part) {
            const auto Found = World.PartBodies.find(Part.GetObjectId());
            return Found == World.PartBodies.end() ? PhysicsBodyId{} : Found->second;
        }
        static SoftBodyId SoftBody(const WorldRoot &World, const DeformableBody &Body) {
            const auto Found = World.DeformableIds.find(Body.GetObjectId());
            return Found == World.DeformableIds.end() ? SoftBodyId{} : Found->second;
        }
        static bool IsBodyValid(const WorldRoot &World, PhysicsBodyId Id) {
            return World.Physics.IsBodyValid(Id);
        }
        static PhysicsConstraintId Constraint(const WorldRoot &World, const JointConstraint &Joint) {
            const auto Found = World.ConstraintJoints.find(Joint.GetObjectId());
            return Found == World.ConstraintJoints.end() ? PhysicsConstraintId{} : Found->second;
        }
        static PhysicsCollisionResidency Collision(const WorldRoot &World) {
            return World.Physics.GetCollisionResidency();
        }
        static WorldRaycastResult Raycast(WorldRoot &World, glm::vec3 Origin, glm::vec3 Direction) {
            return World.ResolveRaycast(Origin, Direction);
        }
        static PhysicsKinematicMotionResult Move(WorldRoot &World,
                                                  const PhysicsKinematicMotionRequest &Request) {
            return World.ResolveKinematicMotion(Request);
        }
    };
}

namespace {
    using namespace gargantuan;
    using PhysicsEvaluation::CreateUnifiedJoltProvider;
#ifdef PHYSICS_GANTRIA_ATTACHMENTS
    using PhysicsEvaluation::ConnectJoltRigidSoftAttachment;
    using PhysicsEvaluation::GetJoltRigidSoftAttachmentState;
#endif

    void Require(bool Condition, const char *Message) {
        if (!Condition) throw std::runtime_error(Message);
    }
    PhysicsBodyDesc Ball(glm::vec3 Position, float Radius = 0.45f) {
        PhysicsBodyDesc Result;
        Result.Transform.Position = Position;
        Result.Shape.Kind = PhysicsShapeKind::Ball;
        Result.Shape.Size = glm::vec3(2.0f * Radius);
        Result.GravityScale = 0.0f;
        Result.SleepEnabled = false;
        return Result;
    }
    PhysicsBodyDesc Box(glm::vec3 Position, glm::vec3 Size, bool Anchored) {
        PhysicsBodyDesc Result;
        Result.Transform.Position = Position;
        Result.Shape.Size = Size;
        Result.Anchored = Anchored;
        Result.GravityScale = 0.0f;
        return Result;
    }
    PhysicsStepResult Advance(PhysicsEvaluation::UnifiedJoltProvider &Provider, int Frames = 1) {
        PhysicsStepResult Last;
        for (int Index = 0; Index < Frames; ++Index) {
            Last = Provider.Rigid->Step({.DeltaTime = 1.0f / 60.0f, .SubStepCount = 16});
            (void)Provider.Soft->Step({.DeltaTime = 1.0f / 60.0f, .Gravity = {0, 0, 0}});
        }
        return Last;
    }
    void RigidIdentityAndEvents() {
        PhysicsWorldConfig Config;
        Config.Gravity = {};
        auto Provider = CreateUnifiedJoltProvider(Config);
        Require(Provider.Rigid && Provider.Soft, "provider creation failed");
        auto Ground = Box({0, -1, 0}, {10, 2, 10}, true);
        const auto Static = Provider.Rigid->CreateBody(Ground);
        const auto Moving = Provider.Rigid->CreateBody(Ball({0, 0.35f, 0}));
        Require(Static.IsValid() && Moving.IsValid(), "rigid admission failed");
        auto Events = Advance(Provider);
        Require(std::any_of(Events.Contacts.begin(), Events.Contacts.end(), [](const auto &Event) {
            return Event.Phase == PhysicsContactPhase::Began;
        }), "overlap did not produce begin event");
        auto Replacement = Ball({0, 3, 0}, 0.2f);
        Require(Provider.Rigid->UpdateBody(Moving, Replacement).Succeeded(), "shape replacement failed");
        Events = Advance(Provider);
        Require(std::any_of(Events.Contacts.begin(), Events.Contacts.end(), [](const auto &Event) {
            return Event.Phase == PhysicsContactPhase::Ended;
        }), "separation did not produce end event");
        auto State = Provider.Rigid->GetBodyState(Moving);
        Require(State && std::abs(State->Mass - 0.7f * (4.0f / 3.0f) *
            3.14159265358979323846f * 0.008f) < 0.01f, "replacement mass is wrong");
        Replacement.Anchored = true;
        Require(Provider.Rigid->UpdateBody(Moving, Replacement).Succeeded(),
            "dynamic to static transition failed");
        Replacement.Anchored = false;
        Require(Provider.Rigid->UpdateBody(Moving, Replacement).Succeeded() &&
            Provider.Rigid->GetBodyState(Moving)->Mass > 0,
            "static to dynamic transition lost motion properties");
        Require(Provider.Rigid->DestroyBody(Moving).Succeeded(), "rigid retirement failed");
        const auto Reused = Provider.Rigid->CreateBody(Ball({0, 4, 0}));
        Require(Reused.IsValid() && Reused != Moving && !Provider.Rigid->IsBodyValid(Moving),
            "stale rigid identity resolved after slot reuse");
        Require(Provider.Rigid->DestroyBody(Static).Succeeded(), "static retirement failed");
        Require(Provider.Rigid->DestroyBody(Reused).Succeeded(), "reused body retirement failed");
        auto SensorDescription = Box({0, 0, 0}, {2, 2, 2}, true);
        SensorDescription.CanCollide = false;
        SensorDescription.CanTouch = true;
        const auto Sensor = Provider.Rigid->CreateBody(SensorDescription);
        const auto Visitor = Provider.Rigid->CreateBody(Ball({0, 0, 0}));
        Require(Sensor.IsValid() && Visitor.IsValid(), "sensor admission failed");
        Events = Advance(Provider);
        Require(std::any_of(Events.Contacts.begin(), Events.Contacts.end(), [&](const auto &Event) {
            return Event.Phase == PhysicsContactPhase::Began &&
                ((Event.BodyA == Sensor && Event.BodyB == Visitor) ||
                 (Event.BodyA == Visitor && Event.BodyB == Sensor));
        }), "sensor did not emit begin event");
        Require(Provider.Rigid->DestroyBody(Visitor).Succeeded(), "sensor visitor release failed");
        Require(Provider.Rigid->DestroyBody(Sensor).Succeeded(), "sensor release failed");
    }
    void CapsuleAndMeshResidency() {
        PhysicsWorldConfig Config;
        Config.Gravity = {};
        auto Provider = CreateUnifiedJoltProvider(Config);
        const auto Wall = Provider.Rigid->CreateBody(Box({2, 1, 0}, {1, 4, 8}, true));
        const auto Moved = Provider.Rigid->MoveKinematicCapsule({.Position = {0, 1, 0},
            .Radius = 0.4f, .Height = 2.0f, .Translation = {4, 0, 0}, .Velocity = {4, 0, 0}});
        Require(Moved.Succeeded() && Moved.Collided && Moved.Position.x < 1.2f &&
            Moved.AppliedTranslation.x > 0.5f, "capsule crossed wall or failed to advance");
        auto Mesh = std::make_shared<PhysicsTriangleMesh>();
        Mesh->ResourceId = "provider-fixture";
        Mesh->ContentRevision = 1;
        Mesh->Positions = std::make_shared<const std::vector<glm::vec3>>(
            std::vector<glm::vec3>{{-2, 0, -2}, {2, 0, -2}, {2, 0, 2}, {-2, 0, 2}});
        Mesh->Indices = std::make_shared<const std::vector<std::uint32_t>>(
            std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3});
        auto Description = Box({0, -2, 0}, {1, 1, 1}, true);
        Description.Shape.Kind = PhysicsShapeKind::StaticTriangleMesh;
        Description.Shape.Mesh = Mesh;
        const auto Imported = Provider.Rigid->CreateBody(Description);
        Require(Imported.IsValid(), "canonical mesh admission failed");
        const auto FirstResidency = Provider.Rigid->GetCollisionResidency();
        Require(FirstResidency.ActiveResources == 1 && FirstResidency.ActiveConsumers == 1 &&
            FirstResidency.CanonicalBytes > 0 && FirstResidency.NativeMeshBytes > 0 &&
            FirstResidency.NativeShapeBytesKnown, "mesh residency did not charge canonical and native bytes");
        auto DuplicateDescription = Description;
        DuplicateDescription.Transform.Position.x = 5;
        DuplicateDescription.Shape.Mesh = std::make_shared<PhysicsTriangleMesh>(*Mesh);
        const auto Duplicate = Provider.Rigid->CreateBody(DuplicateDescription);
        Require(Duplicate.IsValid(), "shared canonical mesh consumer admission failed");
        const auto SharedResidency = Provider.Rigid->GetCollisionResidency();
        Require(SharedResidency.ActiveResources == 1 && SharedResidency.ActiveConsumers == 2 &&
            SharedResidency.CanonicalBytes == FirstResidency.CanonicalBytes,
            "shared canonical mesh charged duplicate resource bytes");
        DuplicateDescription.Transform.Position.x = 6;
        DuplicateDescription.Shape.Mesh = std::make_shared<PhysicsTriangleMesh>(*Mesh);
        Require(Provider.Rigid->UpdateBody(Duplicate, DuplicateDescription).Succeeded() &&
            Provider.Rigid->GetCollisionResidency().CreatedMeshShapes == 2,
            "equivalent mesh wrapper rebuilt native shape during transform update");
        const auto Hit = Provider.Rigid->Raycast({.Origin = {0, 2, 0}, .Direction = {0, -5, 0}});
        Require(Hit.Succeeded() && std::any_of(Hit.Candidates.begin(), Hit.Candidates.end(),
            [&](const auto &Candidate) { return Candidate.Body == Imported; }),
            "canonical mesh did not serve a ray");
        auto DynamicFallback = Box({0, 1, 0}, {1, 1, 1}, false);
        const auto Constrained = Provider.Rigid->CreateBody(Box({0, 3, 0}, {1, 1, 1}, false));
        PhysicsConstraintDesc MeshJoint;
        MeshJoint.Kind = PhysicsConstraintKind::Weld;
        MeshJoint.BodyA = Imported;
        MeshJoint.BodyB = Constrained;
        const auto Attached = Provider.Rigid->TryCreateConstraint(MeshJoint);
        Require(Constrained.IsValid() && Attached.Succeeded(), "mesh constraint admission failed");
        const auto BeforeDenied = Provider.Rigid->GetCollisionResidency();
        Require(!Provider.Rigid->UpdateBody(Imported, DynamicFallback).Succeeded() &&
            Provider.Rigid->GetBodyState(Imported)->Description.Shape.Kind ==
                PhysicsShapeKind::StaticTriangleMesh &&
            Provider.Rigid->GetCollisionResidency().ActiveConsumers == BeforeDenied.ActiveConsumers,
            "constrained mesh transition did not reject atomically");
        const auto StillMesh = Provider.Rigid->Raycast({.Origin = {0, 2, 0}, .Direction = {0, -5, 0}});
        Require(StillMesh.Succeeded() && std::any_of(StillMesh.Candidates.begin(), StillMesh.Candidates.end(),
            [&](const auto &Candidate) { return Candidate.Body == Imported; }),
            "rejected constrained transition changed the native mesh shape");
        Require(Provider.Rigid->DestroyConstraint(Attached.Constraint).Succeeded() &&
            Provider.Rigid->DestroyBody(Constrained).Succeeded(),
            "mesh constraint release failed");
        Require(Provider.Rigid->UpdateBody(Imported, DynamicFallback).Succeeded() &&
            Provider.Rigid->IsBodyValid(Imported) &&
            Provider.Rigid->GetBodyState(Imported)->Mass > 0,
            "static mesh to dynamic fallback lost neutral identity");
        Require(Provider.Rigid->UpdateBody(Imported, Description).Succeeded(),
            "dynamic fallback to canonical mesh failed");
        Require(Provider.Rigid->DestroyBody(Imported).Succeeded(), "mesh release failed");
        Require(!Provider.Rigid->IsBodyValid(Imported), "retired mesh body is still live");
        Require(Provider.Rigid->GetCollisionResidency().ActiveResources == 1 &&
            Provider.Rigid->GetCollisionResidency().ActiveConsumers == 1,
            "releasing one mesh consumer retired shared resource early");
        Require(Provider.Rigid->DestroyBody(Duplicate).Succeeded() &&
            Provider.Rigid->GetCollisionResidency().ActiveResources == 0 &&
            Provider.Rigid->GetCollisionResidency().NativeMeshBytes == 0,
            "final mesh consumer failed to retire canonical and native bytes");
        Require(Provider.Rigid->DestroyBody(Wall).Succeeded(), "wall release failed");
    }
    void WeldUnderImpulse() {
        PhysicsWorldConfig Config;
        Config.Gravity = {};
        auto Provider = CreateUnifiedJoltProvider(Config);
        const auto Anchor = Provider.Rigid->CreateBody(Box({0, 0, 0}, {1, 1, 1}, true));
        const auto Link = Provider.Rigid->CreateBody(Box({0, -2, 0}, {1, 1, 1}, false));
        Require(Anchor.IsValid() && Link.IsValid(), "weld bodies failed admission");
        PhysicsConstraintDesc Joint;
        Joint.Kind = PhysicsConstraintKind::Weld;
        Joint.BodyA = Anchor;
        Joint.BodyB = Link;
        Joint.LocalFrameA.Position = {0, -1, 0};
        Joint.LocalFrameB.Position = {0, 1, 0};
        auto InvalidMotor = Joint;
        InvalidMotor.Kind = PhysicsConstraintKind::Hinge;
        InvalidMotor.EnableMotor = true;
        Require(!Provider.Rigid->TryCreateConstraint(InvalidMotor).Succeeded(),
            "zero-force hinge motor was admitted");
        const auto Weld = Provider.Rigid->TryCreateConstraint(Joint);
        Require(Weld.Succeeded(), "weld joint failed admission");
        Require(Provider.Rigid->ApplyLinearImpulse(Link, {10, 0, 0}).Succeeded(),
            "weld impulse failed");
        Advance(Provider, 30);
        const auto State = Provider.Rigid->GetBodyState(Link);
        Require(State && std::abs(State->Transform.Position.x) < 0.1f &&
            std::abs(State->Transform.Position.y + 2.0f) < 0.1f,
            "weld did not hold authored relative pose under impulse");
        Require(Provider.Rigid->DestroyConstraint(Weld.Constraint).Succeeded(), "weld release failed");
    }
    void ConnectedCollisionPolicy() {
        PhysicsWorldConfig Config;
        Config.Gravity = {};
        Config.EnableSleep = false;
        auto Provider = CreateUnifiedJoltProvider(Config);
        auto SensorDescription = Box({0, 0, 0}, {2, 2, 2}, true);
        SensorDescription.CanCollide = false;
        SensorDescription.CanTouch = true;
        const auto Sensor = Provider.Rigid->CreateBody(SensorDescription);
        const auto Moving = Provider.Rigid->CreateBody(Box({0, 0, 0}, {1, 1, 1}, false));
        Require(Sensor.IsValid() && Moving.IsValid(), "connected collision bodies failed admission");
        PhysicsConstraintDesc Joint;
        Joint.Kind = PhysicsConstraintKind::Weld;
        Joint.BodyA = Sensor;
        Joint.BodyB = Moving;
        Joint.CollideConnected = false;
        const auto Weld = Provider.Rigid->TryCreateConstraint(Joint);
        Require(Weld.Succeeded(), "connected collision weld failed admission");
        const auto Suppressed = Provider.Rigid->Step({.DeltaTime = SoftBodyStepInterval, .SubStepCount = 4});
        Require(Suppressed.Contacts.empty(), "joint with CollideConnected=false emitted sensor contact");
        Require(Provider.Rigid->DestroyConstraint(Weld.Constraint).Succeeded(),
            "connected collision weld release failed");
        const auto Restored = Provider.Rigid->Step({.DeltaTime = SoftBodyStepInterval, .SubStepCount = 4});
        const auto Began = std::ranges::any_of(Restored.Contacts, [](const auto &Contact) {
            return Contact.Phase == PhysicsContactPhase::Began;
        });
        Require(Began, "joint release did not restore connected collision event");
    }
    void DistanceSpringEquilibrium() {
        PhysicsWorldConfig Config;
        Config.Gravity = {};
        Config.EnableSleep = false;
        auto Provider = CreateUnifiedJoltProvider(Config);
        const auto Anchor = Provider.Rigid->CreateBody(Box({0, 0, 0}, {0.5f, 0.5f, 0.5f}, true));
        const auto Bob = Provider.Rigid->CreateBody(Box({3, 0, 0}, {0.5f, 0.5f, 0.5f}, false));
        Require(Anchor.IsValid() && Bob.IsValid(), "distance spring bodies failed admission");
        PhysicsConstraintDesc Joint;
        Joint.Kind = PhysicsConstraintKind::Distance;
        Joint.BodyA = Anchor;
        Joint.BodyB = Bob;
        Joint.RestLength = 1.5f;
        Joint.SpringFrequency = 8.0f;
        Joint.SpringDamping = 0.8f;
        const auto Spring = Provider.Rigid->TryCreateConstraint(Joint);
        Require(Spring.Succeeded(), "distance spring failed Jolt admission");
        Advance(Provider, 120);
        const auto State = Provider.Rigid->GetBodyState(Bob);
        Require(State && State->Transform.Position.x > 1.35f &&
            State->Transform.Position.x < 1.65f &&
            std::abs(State->LinearVelocity.x) < 0.1f,
            "distance spring failed to settle at authored rest length");
        Require(Provider.Rigid->DestroyConstraint(Spring.Constraint).Succeeded(),
            "distance spring release failed");
    }
    void ArticulatedChainUnderImpulse() {
        PhysicsWorldConfig Config;
        Config.Gravity = {0, -9.81f, 0};
        Config.EnableSleep = false;
        auto Provider = CreateUnifiedJoltProvider(Config);
        std::array<PhysicsBodyId, 4> Links;
        for (std::size_t Index = 0; Index < Links.size(); ++Index) {
            auto Description = Box({0, -2.0f * Index, 0}, {1, 2, 1}, Index == 0);
            if (Index != 0) Description.GravityScale = 1.0f;
            Links[Index] = Provider.Rigid->CreateBody(Description);
            Require(Links[Index].IsValid(), "articulated link failed admission");
        }
        for (std::size_t Index = 1; Index < Links.size(); ++Index) {
            PhysicsConstraintDesc Joint;
            Joint.Kind = PhysicsConstraintKind::BallSocket;
            Joint.BodyA = Links[Index - 1];
            Joint.BodyB = Links[Index];
            Joint.LocalFrameA.Position = {0, -1, 0};
            Joint.LocalFrameB.Position = {0, 1, 0};
            Joint.CollideConnected = false;
            Require(Provider.Rigid->TryCreateConstraint(Joint).Succeeded(),
                "articulated ball socket failed admission");
        }
        Require(Provider.Rigid->ApplyLinearImpulse(Links.back(), {12, 0, 0}).Succeeded(),
            "articulated end-link impulse failed");
        float MaximumSwing = 0;
        for (int Frame = 0; Frame < 180; ++Frame) {
            Advance(Provider);
            const auto End = Provider.Rigid->GetBodyState(Links.back());
            Require(End && std::isfinite(End->Transform.Position.x),
                "articulated chain produced nonfinite motion");
            MaximumSwing = std::max(MaximumSwing, std::abs(End->Transform.Position.x));
        }
        Require(MaximumSwing > 0.1f, "articulated chain did not respond to end-link impulse");
        for (std::size_t Index = 1; Index < Links.size(); ++Index) {
            const auto A = Provider.Rigid->GetBodyState(Links[Index - 1]);
            const auto B = Provider.Rigid->GetBodyState(Links[Index]);
            Require(A && B && glm::length((A->Transform * glm::vec3(0, -1, 0)) -
                (B->Transform * glm::vec3(0, 1, 0))) < 0.15f,
                "articulated chain lost a live joint pivot under gravity and impulse");
        }
    }
    void ForceAcrossHalfUpdates() {
        PhysicsWorldConfig Config;
        Config.Gravity = {};
        auto Provider = CreateUnifiedJoltProvider(Config);
        const auto Body = Provider.Rigid->CreateBody(Box({0, 0, 0}, {1, 1, 1}, false));
        Require(Body.IsValid(), "force fixture body admission failed");
        const auto Mass = Provider.Rigid->GetBodyState(Body)->Mass;
        Require(Provider.Rigid->ApplyForce(Body, {7, 0, 0}).Succeeded(),
            "neutral rigid force admission failed");
        Advance(Provider);
        const auto State = Provider.Rigid->GetBodyState(Body);
        const float Expected = 7.0f / Mass / 60.0f;
        Require(State && std::abs(State->LinearVelocity.x - Expected) < 0.02f,
            "two half-update schedule lost or duplicated neutral rigid force");
        Provider.Soft.reset();
        const auto AfterSoftRetirement = Provider.Rigid->Step({.DeltaTime = SoftBodyStepInterval,
            .SubStepCount = 4});
        Require(!AfterSoftRetirement.Motions.empty() && Provider.Rigid->IsBodyValid(Body),
            "rigid world retained a dangling soft pre-step callback after soft retirement");
    }
    void TransientSensorAcrossHalfUpdates() {
        PhysicsWorldConfig Config;
        Config.Gravity = {};
        auto Provider = CreateUnifiedJoltProvider(Config);
        auto SensorDescription = Box({0, 0, 0}, {1, 1, 1}, true);
        SensorDescription.CanCollide = false;
        SensorDescription.CanTouch = true;
        const auto Sensor = Provider.Rigid->CreateBody(SensorDescription);
        const auto Visitor = Provider.Rigid->CreateBody(Ball({-1, 0, 0}, 0.1f));
        Require(Sensor.IsValid() && Visitor.IsValid(), "transient sensor fixture admission failed");
        Require(Provider.Rigid->SetLinearVelocity(Visitor, {120, 0, 0}).Succeeded(),
            "transient sensor velocity failed");
        const auto Frame = Advance(Provider);
        bool Began = false, Ended = false;
        for (const auto &Event : Frame.Contacts) {
            if (!((Event.BodyA == Sensor && Event.BodyB == Visitor) ||
                  (Event.BodyA == Visitor && Event.BodyB == Sensor))) continue;
            Began |= Event.Phase == PhysicsContactPhase::Began;
            Ended |= Event.Phase == PhysicsContactPhase::Ended;
        }
        Require(Began && Ended,
            "transient half-frame sensor contact did not begin and end in one neutral step");
    }
    void ClothAndRubberCoupling() {
        PhysicsWorldConfig Config;
        Config.Gravity = {};
        auto Provider = CreateUnifiedJoltProvider(Config);
        SoftBodyDefinition Cloth;
        Cloth.Kind = SoftBodyKind::Cloth;
        Cloth.Position = {0, 0, 0};
        Cloth.Size = {4, 4, 0.2f};
        Cloth.ResolutionX = Cloth.ResolutionY = 9;
        Cloth.Material.StretchCompliance = 1.0e-6f;
        for (std::uint32_t X = 0; X < 9; ++X)
            Cloth.Attachments.push_back({X, {-2.0f + 0.5f * float(X), 2, 0}});
        auto HardCloth = Cloth;
        HardCloth.Material.StretchCompliance = 0.0f;
        Require(!Provider.Soft->CreateBody(HardCloth).IsValid(),
            "unqualified exact-hard cloth was admitted");
        const auto Fabric = Provider.Soft->CreateBody(Cloth);
        Require(Fabric.IsValid(), "cloth admission failed");
        const auto BeforeOverflow = Provider.Soft->GetBodyState(Fabric);
        Require(Provider.Soft->ApplyImpulseAtPosition(Fabric,
            {std::numeric_limits<float>::max(), 0, 0}, {0, 0, 0}).Status ==
                PhysicsOperationStatus::InvalidDescription,
            "overflowing point impulse was accepted");
        const auto AfterOverflow = Provider.Soft->GetBodyState(Fabric);
        Require(BeforeOverflow && AfterOverflow &&
            *BeforeOverflow->Positions == *AfterOverflow->Positions,
            "rejected point impulse mutated cloth positions");
        Advance(Provider);
        const auto AfterOverflowStep = Provider.Soft->GetBodyState(Fabric);
        Require(AfterOverflowStep &&
            std::abs((*AfterOverflowStep->Positions)[40].x) < 0.01f,
            "rejected point impulse altered the next physical step");
        const auto Sphere = Provider.Rigid->CreateBody(Ball({0, 0, -1.2f}));
        Require(Sphere.IsValid(), "cloth projectile admission failed");
        Require(Provider.Rigid->SetLinearVelocity(Sphere, {0, 0, 12}).Succeeded(),
            "cloth projectile velocity failed");
        Advance(Provider, 45);
        const auto State = Provider.Soft->GetBodyState(Fabric);
        const auto Rigid = Provider.Rigid->GetBodyState(Sphere);
        Require(State && State->Positions && Rigid, "cloth coupling state missing");
        float MaximumDeflection = 0;
        for (std::size_t Index = 9; Index < State->Positions->size(); ++Index)
            MaximumDeflection = std::max(MaximumDeflection, std::abs((*State->Positions)[Index].z));
        Require(MaximumDeflection > 0.02f, "cloth did not deform under rigid contact");
        Require(Rigid->LinearVelocity.z < 11.0f, "cloth did not feed back to rigid projectile");
        const auto PreviousRevision = State->VertexRevision;
        Cloth.Attachments[4].Position.z = 0.25f;
        Require(Provider.Soft->UpdateBody(Fabric, Cloth).Succeeded(), "moving attachment update failed");
        const auto Pinned = Provider.Soft->GetBodyState(Fabric);
        Require(Pinned && Pinned->VertexRevision > PreviousRevision &&
            std::abs((*Pinned->Positions)[4].z - 0.25f) < 0.01f,
            "moving attachment did not publish its new position");
        Require(Provider.Soft->ApplyForce(Fabric, {100, 0, 0}).Succeeded(),
            "pending force before cloth retirement failed");
        Require(Provider.Soft->DestroyBody(Fabric).Succeeded(), "cloth release failed");
        const auto Reused = Provider.Soft->CreateBody(Cloth);
        Require(Reused.IsValid() && Reused != Fabric && !Provider.Soft->IsBodyValid(Fabric),
            "stale cloth identity resolved after reuse");
        const auto BeforeUnstepped = Provider.Soft->GetBodyInspection(Reused);
        const auto ExtractedWithoutAdvance = Provider.Soft->Step({.DeltaTime = SoftBodyStepInterval,
            .Gravity = {0, 0, 0}});
        Require(BeforeUnstepped && !BeforeUnstepped->Simulated &&
            ExtractedWithoutAdvance.States.size() == 1 &&
            !ExtractedWithoutAdvance.States.front().Simulated &&
            Provider.Soft->GetBodyInspection(Reused)->VertexRevision == BeforeUnstepped->VertexRevision,
            "new cloth falsely reported a prior rigid advance as its own simulation");
        Advance(Provider);
        const auto ReusedState = Provider.Soft->GetBodyState(Reused);
        Require(ReusedState && std::abs((*ReusedState->Positions)[40].x) < 0.05f,
            "reused soft slot inherited the retired body's pending force");
        Require(Provider.Soft->DestroyBody(Reused).Succeeded(), "reused cloth release failed");
        Require(Provider.Rigid->DestroyBody(Sphere).Succeeded(), "cloth projectile release failed");

        SoftBodyDefinition Rubber;
        Rubber.Kind = SoftBodyKind::Rubber;
        Rubber.Size = {1, 1, 1};
        Rubber.ResolutionX = Rubber.ResolutionY = Rubber.ResolutionZ = 4;
        const auto Elastic = Provider.Soft->CreateBody(Rubber);
        const auto Target = Provider.Rigid->CreateBody(Ball({1.1f, 0, 0}, 0.35f));
        Require(Elastic.IsValid() && Target.IsValid(), "rubber scene admission failed");
        Require(Provider.Soft->ApplyImpulse(Elastic, {40, 0, 0}).Succeeded(), "rubber impulse failed");
        Advance(Provider, 60);
        const auto TargetState = Provider.Rigid->GetBodyState(Target);
        Require(TargetState && TargetState->Transform.Position.x > 1.15f,
            "rubber did not push the rigid target");
    }
    void NeutralSoftDampingRetention() {
        PhysicsWorldConfig Config;
        Config.Gravity = {};
        auto Provider = CreateUnifiedJoltProvider(Config);
        SoftBodyDefinition Rubber;
        Rubber.Kind = SoftBodyKind::Rubber;
        Rubber.Size = {1, 1, 1};
        Rubber.ResolutionX = Rubber.ResolutionY = Rubber.ResolutionZ = 2;
        Rubber.Material.Damping = 0.04f;
        const auto Body = Provider.Soft->CreateBody(Rubber);
        Require(Body.IsValid(), "free rubber damping fixture failed admission");
        const auto MeanX = [](const SoftBodyState &State) {
            float Sum = 0;
            for (const auto &Position : *State.Positions) Sum += Position.x;
            return Sum / float(State.Positions->size());
        };
        const auto Initial = Provider.Soft->GetBodyState(Body);
        Require(Initial && Provider.Soft->ApplyImpulse(Body, {8, 0, 0}).Succeeded(),
            "free rubber damping impulse failed");
        Advance(Provider);
        const auto First = Provider.Soft->GetBodyState(Body);
        Advance(Provider);
        const auto Second = Provider.Soft->GetBodyState(Body);
        Require(First && Second, "free rubber damping states missing");
        const float FirstDisplacement = MeanX(*First) - MeanX(*Initial);
        const float SecondDisplacement = MeanX(*Second) - MeanX(*First);
        Require(FirstDisplacement > 0.05f &&
            std::abs(SecondDisplacement / FirstDisplacement - 0.96f) < 0.01f,
            "native damping failed neutral per-frame velocity retention");
    }
    void NeutralPointImpulseDistribution() {
        PhysicsWorldConfig Config;
        Config.Gravity = {};
        auto Provider = CreateUnifiedJoltProvider(Config);
        SoftBodyDefinition Cloth;
        Cloth.Size = {2, 2, 0.1f};
        Cloth.ResolutionX = Cloth.ResolutionY = 3;
        Cloth.Material.StretchCompliance = 1.0f;
        Cloth.Material.BendCompliance = 1.0f;
        Cloth.Material.Damping = 0;
        const auto Body = Provider.Soft->CreateBody(Cloth);
        Require(Body.IsValid(), "point impulse cloth failed admission");
        Require(Provider.Soft->ApplyImpulseAtPosition(Body, {0, 0, 1}, {0.5f, 0, 0}).Succeeded(),
            "point impulse cloth rejected bounded input");
        Advance(Provider);
        const auto State = Provider.Soft->GetBodyState(Body);
        Require(State && State->Positions, "point impulse cloth state missing");
        const auto Moved = std::count_if(State->Positions->begin(), State->Positions->end(),
            [](const auto &Position) { return Position.z > 0.005f; });
        Require(Moved >= 3, "point impulse did not reach four nearest movable nodes");
    }
    void DataModelAdmissionAndStreaming() {
        auto Game = std::make_shared<DataModel>();
        auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
        Require(bool(World), "DataModel Workspace missing");
        PhysicsWorldConfig Config;
        Config.Gravity = {};
        WorldRootTestAccess::Inject(*World, CreateUnifiedJoltProvider(Config));
        World->SetGravity(0.0f);
        auto Wall = std::make_shared<Part>();
        Wall->SetName("JoltWall");
        Wall->SetAnchored(true);
        Wall->SetSize({1, 4, 4});
        Wall->SetCFrame(CFrame(2, 0, 0));
        Wall->SetParent(World);
        auto Mover = std::make_shared<Part>();
        Mover->SetName("JoltMover");
        Mover->SetSize({0.6f, 0.6f, 0.6f});
        Mover->SetCFrame(CFrame(0, 0, 0));
        Mover->SetParent(World);
        const auto FirstId = WorldRootTestAccess::Body(*World, *Mover);
        Require(FirstId.IsValid(), "WorldRoot did not admit Part to Jolt");
        Mover->SetLinearVelocity({10, 0, 0});
        for (int Step = 0; Step < 15; ++Step)
            World->StepPhysics(SoftBodyStepInterval, std::nullopt);
        Require(Mover->GetCFrame().Position.x < 1.25f,
            "WorldRoot Part publication crossed a Jolt wall");
        Mover->SetParent(nullptr);
        World->StepPhysics(0.0f, std::nullopt);
        Require(!WorldRootTestAccess::IsBodyValid(*World, FirstId),
            "streamed-out Part retained its native Jolt body");
        Mover->SetCFrame(CFrame(0, 0, 0));
        Mover->SetParent(World);
        const auto SecondId = WorldRootTestAccess::Body(*World, *Mover);
        Require(SecondId.IsValid() && SecondId != FirstId,
            "streamed-in Part resolved stale Jolt identity");

        auto Fabric = std::make_shared<Cloth>();
        Fabric->SetName("JoltFabric");
        Fabric->SetPosition({0, 3, 0});
        Fabric->SetSize({2, 2, 0.2f});
        Fabric->SetResolutionX(9);
        Fabric->SetResolutionY(9);
        Fabric->SetParent(World);
        World->StepPhysics(SoftBodyStepInterval, std::nullopt);
        const auto Vertex = World->GetDeformableVertex(Fabric->GetObjectId(), 40);
        Require(Vertex && std::isfinite(Vertex->x) && std::isfinite(Vertex->y),
            "WorldRoot did not publish Jolt cloth vertex state");
        auto Pin = std::make_shared<SoftBodyAttachment>();
        Pin->SetVertexIndex(0);
        Pin->SetPosition({-1, 4, 0});
        Pin->SetParent(Fabric);
        World->StepPhysics(0.0f, std::nullopt);
        const auto BeforePinMove = World->GetDeformableVertex(Fabric->GetObjectId(), 40);
        Pin->SetPosition({-1, 5, 0});
        World->StepPhysics(0.0f, std::nullopt);
        const auto AfterPinMove = World->GetDeformableVertex(Fabric->GetObjectId(), 40);
        const auto PinnedVertex = World->GetDeformableVertex(Fabric->GetObjectId(), 0);
        Require(BeforePinMove && AfterPinMove && PinnedVertex &&
            glm::length(*BeforePinMove - *AfterPinMove) < 0.01f &&
            std::abs(PinnedVertex->y - 5.0f) < 0.01f,
            "authored moving attachment reset cloth deformation or missed pin position");
        Fabric->Destroy();
        World->StepPhysics(0.0f, std::nullopt);
        Require(!World->GetDeformableVertex(Fabric->GetObjectId(), 40),
            "destroyed Cloth retained Jolt published vertices");
        Game->Destroy();

        auto SavedRoot = std::make_shared<Folder>();
        SavedRoot->SetName("SavedRoot");
        auto SavedPart = std::make_shared<Part>();
        SavedPart->SetName("SavedPart");
        SavedPart->SetSize({1, 1, 1});
        SavedPart->SetParent(SavedRoot);
        auto SavedCloth = std::make_shared<Cloth>();
        SavedCloth->SetName("SavedCloth");
        SavedCloth->SetResolutionX(9);
        SavedCloth->SetResolutionY(9);
        SavedCloth->SetParent(SavedRoot);
        SavedRoot->MarkPersistenceSubtreeArchivable();
        std::shared_ptr<Instance> Root = SavedRoot;
        const auto Snapshot = InstanceSerialization::Serialize(
            InstanceSerialization::InstanceFormat::Json, Root);
        Require(Snapshot.find("SoftBodyId") == std::string::npos &&
            Snapshot.find("PhysicsBodyId") == std::string::npos,
            "snapshot leaked Jolt runtime identity");
        std::istringstream Input(Snapshot);
        auto Loaded = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, Input);
        Require(Loaded.Ok && Loaded.Instance, "semantic fixture failed to reopen");
        auto ReopenedGame = std::make_shared<DataModel>();
        auto ReopenedWorld = std::dynamic_pointer_cast<Workspace>(ReopenedGame->GetService("Workspace"));
        WorldRootTestAccess::Inject(*ReopenedWorld, CreateUnifiedJoltProvider(Config));
        ReopenedWorld->SetGravity(0.0f);
        Loaded.Instance->SetParent(ReopenedWorld);
        auto ReopenedPart = std::dynamic_pointer_cast<Part>(
            Loaded.Instance->FindFirstChild("SavedPart", false));
        auto ReopenedCloth = std::dynamic_pointer_cast<Cloth>(
            Loaded.Instance->FindFirstChild("SavedCloth", false));
        ReopenedWorld->StepPhysics(SoftBodyStepInterval, std::nullopt);
        Require(ReopenedPart && WorldRootTestAccess::Body(*ReopenedWorld, *ReopenedPart).IsValid() &&
            ReopenedCloth && ReopenedWorld->GetDeformableVertex(ReopenedCloth->GetObjectId(), 40),
            "reopened semantic fixture failed Jolt readmission");
        ReopenedGame->Destroy();
    }
    void DataModelMotorsAndCcd() {
        auto Game = std::make_shared<DataModel>();
        auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
        PhysicsWorldConfig Config;
        Config.Gravity = {};
        WorldRootTestAccess::Inject(*World, CreateUnifiedJoltProvider(Config));
        World->SetGravity(0.0f);
        const auto PartAt = [&](glm::vec3 Position, bool Anchored) {
            auto Result = std::make_shared<Part>();
            Result->SetAnchored(Anchored);
            Result->SetCanCollide(false);
            Result->SetCFrame(CFrame(Position.x, Position.y, Position.z));
            Result->SetParent(World);
            return Result;
        };
        const auto MakeAttachment = [](const std::shared_ptr<Part> &Parent, glm::vec3 Local) {
            auto Result = std::make_shared<Attachment>();
            Result->SetCFrame(CFrame(Local.x, Local.y, Local.z));
            Result->SetParent(Parent);
            return Result;
        };
        auto Frame = PartAt({0, 0, 0}, true);
        auto Door = PartAt({2, 0, 0}, false);
        auto PivotA = MakeAttachment(Frame, {0.5f, 0, 0});
        auto PivotB = MakeAttachment(Door, {-1.5f, 0, 0});
        auto Hinge = std::make_shared<JointConstraint>();
        Hinge->SetJointType("Hinge");
        Hinge->SetAttachment0(PivotA);
        Hinge->SetAttachment1(PivotB);
        Hinge->SetEnableLimit(true);
        Hinge->SetLowerLimit(-0.4f);
        Hinge->SetUpperLimit(0.4f);
        Hinge->SetEnableMotor(true);
        Hinge->SetMotorSpeed(2.0f);
        Hinge->SetMaximumMotorForce(1000.0f);
        Hinge->SetParent(World);
        auto Rail = PartAt({10, 0, 0}, true);
        auto Slide = PartAt({10, 0, 0}, false);
        auto TrackA = MakeAttachment(Rail, {0, 0, 0});
        auto TrackB = MakeAttachment(Slide, {0, 0, 0});
        auto Slider = std::make_shared<JointConstraint>();
        Slider->SetJointType("Slider");
        Slider->SetAttachment0(TrackA);
        Slider->SetAttachment1(TrackB);
        Slider->SetEnableLimit(true);
        Slider->SetLowerLimit(-1.0f);
        Slider->SetUpperLimit(1.0f);
        Slider->SetEnableMotor(true);
        Slider->SetMotorSpeed(3.0f);
        Slider->SetMaximumMotorForce(1000.0f);
        Slider->SetParent(World);
        World->StepPhysics(0.0f, std::nullopt);
        Require(WorldRootTestAccess::Constraint(*World, *Hinge).IsValid() &&
            WorldRootTestAccess::Constraint(*World, *Slider).IsValid(),
            "authored hinge and slider failed Jolt admission");
        for (int FrameIndex = 0; FrameIndex < 90; ++FrameIndex)
            World->StepPhysics(SoftBodyStepInterval, std::nullopt);
        const auto Angle = std::atan2(Door->GetCFrame().Rotation[0][1],
            Door->GetCFrame().Rotation[0][0]);
        Require(std::abs(Angle) > 0.05f && std::abs(Angle) < 0.7f &&
            Slide->GetCFrame().Position.x > 10.1f &&
            Slide->GetCFrame().Position.x < 11.3f,
            "authored motors did not move within hinge/slider limits");
        auto Wall = std::make_shared<Part>();
        Wall->SetAnchored(true);
        Wall->SetSize({0.1f, 4, 4});
        Wall->SetCFrame(CFrame(30, 0, 0));
        Wall->SetParent(World);
        auto Bullet = std::make_shared<Part>();
        Bullet->SetShape(Enums::PartType::Ball);
        Bullet->SetSize({0.5f, 0.5f, 0.5f});
        Bullet->SetCFrame(CFrame(27, 0, 0));
        Bullet->SetMovingBodyCCD(true);
        Bullet->SetParent(World);
        Bullet->SetLinearVelocity({360, 0, 0});
        World->StepPhysics(SoftBodyStepInterval, std::nullopt);
        Require(Bullet->GetCFrame().Position.x < 30.5f,
            "authored high-speed CCD projectile tunneled through thin wall");
        Game->Destroy();
    }
    void DataModelMixedGameplay() {
        const auto ProjectRoot = std::filesystem::temp_directory_path() /
            ("gargantuan-jolt-gameplay-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        struct TemporaryProject {
            std::filesystem::path Root;
            ~TemporaryProject() {
                if (Root.parent_path() != std::filesystem::temp_directory_path()) return;
                std::error_code Error;
                std::filesystem::remove_all(Root, Error);
            }
        } Cleanup{ProjectRoot};
        const auto AssetPath = ProjectRoot / "assets" / "floor.obj";
        std::filesystem::create_directories(AssetPath.parent_path());
        {
            std::ofstream Output(AssetPath, std::ios::binary | std::ios::trunc);
            Output << "v -4 0 -4\nv 4 0 -4\nv -4 0 4\nv 4 0 4\n"
                "f 1 3 2\nf 2 3 4\n";
            Require(bool(Output), "bounded gameplay OBJ write failed");
        }
        DiskFilesystem Filesystem(ProjectRoot);
        SourceMount Mount(Filesystem);
        auto Game = std::make_shared<DataModel>();
        Game->Root = ProjectRoot;
        Game->Filesystem = &Filesystem;
        auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
        auto Assets = std::dynamic_pointer_cast<AssetService>(Game->GetService("AssetService"));
        Require(bool(World) && bool(Assets), "gameplay Workspace or AssetService missing");
        WorldRootTestAccess::Inject(*World, CreateUnifiedJoltProvider({}));
        World->SetGravity(9.81f);
        const auto Imported = Assets->ImportProjectAsset(Mount, "assets/floor.obj",
            AssetKind::Mesh, "Gameplay floor");
        Require(Imported.Ok && Imported.Record && Imported.Records.size() == 2 &&
            Assets->ResolveCollisionForMesh(Imported.Record->Reference.Value).has_value(),
            "real gameplay mesh import did not publish collision artifact");
        auto Floor = std::make_shared<MeshPart>();
        Floor->SetName("ImportedFloor");
        Floor->SetAnchored(true);
        Floor->SetCanCollide(true);
        Floor->SetCanTouch(true);
        Floor->SetSize({1, 1, 1});
        Floor->SetMesh(Imported.Record->Reference.Value);
        Floor->SetCollisionMode(Enums::MeshCollisionMode::StaticMesh);
        Floor->SetParent(World);
        World->StepPhysics(0.0f, std::nullopt);
        const auto FloorId = WorldRootTestAccess::Body(*World, *Floor);
        const auto Ray = WorldRootTestAccess::Raycast(*World, {0, 4, 0}, {0, -8, 0});
        Require(FloorId.IsValid() && WorldRootTestAccess::Collision(*World).ActiveResources == 1 &&
            Ray.Succeeded() && Ray.Instance == Floor && std::abs(Ray.Position.y) < 0.05f &&
            Ray.Normal.y > 0.9f,
            "authored imported mesh did not admit or answer floor query");
        int FloorContacts = 0;
        auto Touch = Floor->Touched->Connect([&](std::shared_ptr<BasePart> Other) {
            if (Other && Other != Floor) ++FloorContacts;
        });
        const auto MakeProbe = [&](const char *Name, Enums::PartType Shape,
                                   glm::vec3 Position) {
            auto Body = std::make_shared<Part>();
            Body->SetName(Name);
            Body->SetShape(Shape);
            Body->SetSize({1, 1, 1});
            Body->SetCFrame(CFrame(Position.x, Position.y, Position.z));
            Body->SetParent(World);
            return Body;
        };
        auto Sphere = MakeProbe("GameplaySphere", Enums::PartType::Ball, {-2, 2.5f, 0});
        auto Lower = MakeProbe("StackLower", Enums::PartType::Block, {1, 2.5f, 0});
        auto Upper = MakeProbe("StackUpper", Enums::PartType::Block, {1, 4, 0});
        Require(WorldRootTestAccess::Body(*World, *Sphere).IsValid() &&
            WorldRootTestAccess::Body(*World, *Lower).IsValid() &&
            WorldRootTestAccess::Body(*World, *Upper).IsValid(),
            "authored gameplay probes failed admission");
        float LowestLowerY = std::numeric_limits<float>::max();
        for (int Frame = 0; Frame < 120; ++Frame) {
            World->StepPhysics(SoftBodyStepInterval, std::nullopt);
            LowestLowerY = std::min(LowestLowerY, Lower->GetCFrame().Position.y);
        }
        const auto SphereY = Sphere->GetCFrame().Position.y;
        const auto LowerY = Lower->GetCFrame().Position.y;
        const auto UpperY = Upper->GetCFrame().Position.y;
        std::cout << "[Physics:JoltProvider] mixed stack contacts=" << FloorContacts
            << " sphere_y=" << SphereY << " lower_y=" << LowerY
            << " upper_y=" << UpperY << " lowest_lower_y=" << LowestLowerY
            << '\n' << std::flush;
        Require(FloorContacts >= 2 && std::isfinite(SphereY) &&
            SphereY > 0.25f && SphereY < 1.2f &&
            LowestLowerY > 0.1f && LowerY > 0.25f && LowerY < 1.2f &&
            UpperY > LowerY + 0.45f && UpperY < 2.5f,
            "authored sphere or box stack missed imported floor or penetrated deeply");
        auto Wall = std::make_shared<Part>();
        Wall->SetAnchored(true);
        Wall->SetSize({0.5f, 3, 2});
        Wall->SetCFrame(CFrame(2, 1, 3));
        Wall->SetParent(World);
        const auto Motion = WorldRootTestAccess::Move(*World, {.Position = {0, 1, 3},
            .Radius = 0.35f, .Height = 1.8f, .Translation = {4, 0, 0}, .Velocity = {4, 0, 0}});
        Require(Motion.Succeeded() && Motion.Collided && Motion.Position.x > 0.5f &&
            Motion.Position.x < 1.8f,
            "WorldRoot character capsule crossed authored Part wall");
        Touch->Disconnect();
        Sphere->Destroy(); Lower->Destroy(); Upper->Destroy(); Floor->Destroy();
        World->StepPhysics(0.0f, std::nullopt);
        Require(WorldRootTestAccess::Collision(*World).ActiveResources == 0,
            "authored imported mesh retained native residency after teardown");
        World->SetGravity(0.0f);
        auto Fabric = std::make_shared<Cloth>();
        Fabric->SetPosition({10, 3, 0});
        Fabric->SetSize({2, 2, 0.2f});
        Fabric->SetResolutionX(9);
        Fabric->SetResolutionY(9);
        for (std::uint32_t Vertex = 0; Vertex < 9; ++Vertex) {
            auto Pin = std::make_shared<SoftBodyAttachment>();
            Pin->SetVertexIndex(Vertex);
            Pin->SetPosition({9.0f + Vertex * 0.25f, 4, 0});
            Pin->SetParent(Fabric);
        }
        Fabric->SetParent(World);
        auto Striker = MakeProbe("ClothStriker", Enums::PartType::Ball, {10, 3, -2});
        Striker->SetGravityScale(0.0f);
        Striker->SetLinearVelocity({0, 0, 8});
        World->StepPhysics(0.0f, std::nullopt);
        const auto ClothBefore = World->GetDeformableVertex(Fabric->GetObjectId(), 40);
        float PeakClothDeflection = 0;
        for (int Frame = 0; Frame < 35; ++Frame) {
            World->StepPhysics(SoftBodyStepInterval, std::nullopt);
            const auto Vertex = World->GetDeformableVertex(Fabric->GetObjectId(), 40);
            if (ClothBefore && Vertex)
                PeakClothDeflection = std::max(PeakClothDeflection,
                    std::abs(Vertex->z - ClothBefore->z));
        }
        std::cout << "[Physics:JoltProvider] mixed cloth peak_deflection="
            << PeakClothDeflection << " striker_vz=" << Striker->GetLinearVelocity().z
            << '\n' << std::flush;
        Require(ClothBefore && PeakClothDeflection > 0.005f &&
            Striker->GetLinearVelocity().z < 7.9f,
            "authored dynamic Part strike did not deform cloth with reciprocal response");
        Striker->Destroy(); Fabric->Destroy();
        auto Rubber = std::make_shared<RubberBody>();
        Rubber->SetPosition({18, 3, 0});
        Rubber->SetSize({2, 2, 2});
        Rubber->SetResolutionX(5); Rubber->SetResolutionY(5); Rubber->SetResolutionZ(5);
        Rubber->SetParent(World);
        auto Target = MakeProbe("RubberPushTarget", Enums::PartType::Block, {20, 3, 0});
        Target->SetGravityScale(0.0f);
        World->StepPhysics(0.0f, std::nullopt);
        Require(WorldRootTestAccess::SoftBody(*World, *Rubber).IsValid() &&
            WorldRootTestAccess::Body(*World, *Target).IsValid(),
            "authored rubber push endpoints failed admission");
        Rubber->ApplyImpulse({25, 0, 0});
        for (int Frame = 0; Frame < 60; ++Frame)
            World->StepPhysics(SoftBodyStepInterval, std::nullopt);
        std::cout << "[Physics:JoltProvider] mixed rubber target_x="
            << Target->GetCFrame().Position.x << " target_vx="
            << Target->GetLinearVelocity().x << '\n' << std::flush;
        Require(Target->GetCFrame().Position.x > 20.03f,
            "authored rubber impact did not push dynamic Part");
        Game->Destroy();
    }
#ifdef PHYSICS_GANTRIA_ATTACHMENTS
    void DataModelDynamicSoftRigidAttachment() {
        auto Game = std::make_shared<DataModel>();
        auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
        PhysicsWorldConfig Config;
        Config.Gravity = {};
        auto Provider = CreateUnifiedJoltProvider(Config);
        auto *Rigid = Provider.Rigid.get();
        auto *Soft = Provider.Soft.get();
        WorldRootTestAccess::Inject(*World, std::move(Provider));
        World->SetGravity(0.0f);
        auto Load = std::make_shared<Part>();
        Load->SetSize({1, 1, 1});
        Load->SetCFrame(CFrame(1, 3, 0));
        Load->SetCanCollide(false);
        Load->SetCanTouch(false);
        Load->SetParent(World);
        auto Control = std::make_shared<Part>();
        Control->SetSize({1, 1, 1});
        Control->SetCFrame(CFrame(5, 3, 0));
        Control->SetCanCollide(false);
        Control->SetCanTouch(false);
        Control->SetParent(World);
        auto Fabric = std::make_shared<Cloth>();
        Fabric->SetPosition({0, 3, 0});
        Fabric->SetSize({2, 2, 0.2f});
        Fabric->SetResolutionX(9);
        Fabric->SetResolutionY(9);
        for (std::uint32_t Vertex = 0; Vertex < 9; ++Vertex) {
            auto Pin = std::make_shared<SoftBodyAttachment>();
            Pin->SetVertexIndex(Vertex);
            Pin->SetPosition({-1.0f + 0.25f * Vertex, 4, 0});
            Pin->SetParent(Fabric);
        }
        Fabric->SetParent(World);
        World->StepPhysics(0.0f, std::nullopt);
        const auto SoftId = WorldRootTestAccess::SoftBody(*World, *Fabric);
        const auto RigidId = WorldRootTestAccess::Body(*World, *Load);
        Require(SoftId.IsValid() && RigidId.IsValid() &&
            WorldRootTestAccess::Body(*World, *Control).IsValid(),
            "authored dynamic attachment endpoints failed WorldRoot admission");
        Require(!ConnectJoltRigidSoftAttachment(*Rigid, *Soft, SoftId, 0,
            RigidId, {-1, 0, 0}).Succeeded(),
            "pinned authored vertex was admitted to native dynamic attachment");
        Require(ConnectJoltRigidSoftAttachment(*Rigid, *Soft, SoftId, 40,
            RigidId, {-1, 0, 0}).Succeeded(),
            "authored Cloth/Part dynamic attachment failed native admission");
        Require(!ConnectJoltRigidSoftAttachment(*Rigid, *Soft, SoftId, 40,
            RigidId, {-1, 0, 0}).Succeeded(),
            "duplicate dynamic attachment was admitted");
        const auto InitialVertex = World->GetDeformableVertex(Fabric->GetObjectId(), 40);
        Load->SetLinearVelocity({4, 0, 0});
        Control->SetLinearVelocity({4, 0, 0});
        float PeakVertexDeflection = 0;
        float PeakImpulse = 0;
        for (int Frame = 0; Frame < 30; ++Frame) {
            World->StepPhysics(SoftBodyStepInterval, std::nullopt);
            const auto Vertex = World->GetDeformableVertex(Fabric->GetObjectId(), 40);
            if (InitialVertex && Vertex)
                PeakVertexDeflection = std::max(PeakVertexDeflection,
                    glm::length(*Vertex - *InitialVertex));
            const auto Link = GetJoltRigidSoftAttachmentState(*Rigid, SoftId, RigidId);
            if (Link) PeakImpulse = std::max(PeakImpulse,
                glm::length(Link->LastImpulseOnSoft));
        }
        const auto FinalVertex = World->GetDeformableVertex(Fabric->GetObjectId(), 40);
        const auto LinkState = GetJoltRigidSoftAttachmentState(*Rigid, SoftId, RigidId);
        std::cout << "[Physics:JoltProvider] dynamic attachment metrics initial_x="
            << (InitialVertex ? InitialVertex->x : std::numeric_limits<float>::quiet_NaN())
            << " final_x=" << (FinalVertex ? FinalVertex->x : std::numeric_limits<float>::quiet_NaN())
            << " rigid_x=" << Load->GetCFrame().Position.x
            << " rigid_velocity_x=" << Load->GetLinearVelocity().x
            << " control_x=" << Control->GetCFrame().Position.x
            << " control_velocity_x=" << Control->GetLinearVelocity().x
            << " peak_vertex_deflection=" << PeakVertexDeflection
            << " peak_impulse=" << PeakImpulse
            << " active=" << (LinkState && LinkState->Active)
            << " applied_steps=" << (LinkState ? LinkState->AppliedSteps : 0)
            << " last_impulse_x=" << (LinkState ? LinkState->LastImpulseOnSoft.x : 0)
            << " endpoint_gap_x=" << (FinalVertex ?
                Load->GetCFrame().Position.x - 1.0f - FinalVertex->x :
                std::numeric_limits<float>::quiet_NaN())
            << '\n' << std::flush;
        Require(InitialVertex && FinalVertex && LinkState && LinkState->Active &&
            LinkState->AppliedSteps > 0 && PeakImpulse > 0.000001f &&
            PeakVertexDeflection > 0.0f &&
            Control->GetCFrame().Position.x > 6.5f &&
            Load->GetCFrame().Position.x < 1.5f &&
            Load->GetLinearVelocity().x < 1.0f &&
            std::abs(Load->GetCFrame().Position.x - 1.0f - FinalVertex->x) < 0.05f,
            "authored linked Part did not resist motion relative to the unlinked control");
        Fabric->Destroy();
        World->StepPhysics(0.0f, std::nullopt);
        Require(!GetJoltRigidSoftAttachmentState(*Rigid, SoftId, RigidId) &&
            !Soft->IsBodyValid(SoftId),
            "destroyed authored Cloth retained native dynamic attachment");
        SoftBodyDefinition Tiny;
        Tiny.ResolutionX = Tiny.ResolutionY = 3;
        for (int Index = 0; Index < 260; ++Index) {
            const auto Reused = Soft->CreateBody(Tiny);
            Require(Reused.IsValid() && Soft->DestroyBody(Reused).Succeeded(),
                "native-generation churn failed");
        }
        Require(!ConnectJoltRigidSoftAttachment(*Rigid, *Soft, SoftId, 40,
            RigidId, {-1, 0, 0}).Succeeded(),
            "retired neutral soft generation reconnected after native sequence churn");
        World->StepPhysics(SoftBodyStepInterval, std::nullopt);
        Game->Destroy();
    }
#endif
}

int main() {
    PhysicsEvaluation::SuppressErrorDialogs();
    PhysicsEvaluation::InstallPhysicsAllocators();
    try {
        gargantuan::BootstrapNativeRuntimeSchema();
        std::cout << "[Physics:JoltProvider] rigid identity/events\n" << std::flush;
        RigidIdentityAndEvents();
        std::cout << "[Physics:JoltProvider] capsule/mesh residency\n" << std::flush;
        CapsuleAndMeshResidency();
        std::cout << "[Physics:JoltProvider] weld\n" << std::flush;
        WeldUnderImpulse();
        std::cout << "[Physics:JoltProvider] connected collision\n" << std::flush;
        ConnectedCollisionPolicy();
        std::cout << "[Physics:JoltProvider] distance spring\n" << std::flush;
        DistanceSpringEquilibrium();
        std::cout << "[Physics:JoltProvider] articulated chain\n" << std::flush;
        ArticulatedChainUnderImpulse();
        std::cout << "[Physics:JoltProvider] force\n" << std::flush;
        ForceAcrossHalfUpdates();
        std::cout << "[Physics:JoltProvider] transient contacts\n" << std::flush;
        TransientSensorAcrossHalfUpdates();
        std::cout << "[Physics:JoltProvider] cloth/rubber coupling\n" << std::flush;
        ClothAndRubberCoupling();
        std::cout << "[Physics:JoltProvider] soft damping\n" << std::flush;
        NeutralSoftDampingRetention();
        std::cout << "[Physics:JoltProvider] point impulse\n" << std::flush;
        NeutralPointImpulseDistribution();
        std::cout << "[Physics:JoltProvider] DataModel lifecycle\n" << std::flush;
        DataModelAdmissionAndStreaming();
        std::cout << "[Physics:JoltProvider] DataModel motors/CCD\n" << std::flush;
        DataModelMotorsAndCcd();
        std::cout << "[Physics:JoltProvider] DataModel mixed gameplay\n" << std::flush;
        DataModelMixedGameplay();
#ifdef PHYSICS_GANTRIA_ATTACHMENTS
        std::cout << "[Physics:JoltProvider] DataModel dynamic attachment\n" << std::flush;
        DataModelDynamicSoftRigidAttachment();
#endif
        std::cout << "[Physics:JoltProvider] qualification passed\n";
        return 0;
    } catch (const std::exception &Error) {
        std::cerr << "[Physics:JoltProvider] " << Error.what() << '\n';
        return 1;
    }
}
