#include "UnifiedJoltProvider.hpp"
#include "../ExperimentSupport.hpp"
#include "gargantuan/classes/Cloth.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"

#include <algorithm>
#include <cmath>
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
        static bool IsBodyValid(const WorldRoot &World, PhysicsBodyId Id) {
            return World.Physics.IsBodyValid(Id);
        }
    };
}

namespace {
    using namespace gargantuan;
    using PhysicsEvaluation::CreateUnifiedJoltProvider;

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
        std::cout << "[Physics:JoltProvider] force\n" << std::flush;
        ForceAcrossHalfUpdates();
        std::cout << "[Physics:JoltProvider] transient contacts\n" << std::flush;
        TransientSensorAcrossHalfUpdates();
        std::cout << "[Physics:JoltProvider] cloth/rubber coupling\n" << std::flush;
        ClothAndRubberCoupling();
        std::cout << "[Physics:JoltProvider] DataModel lifecycle\n" << std::flush;
        DataModelAdmissionAndStreaming();
        std::cout << "[Physics:JoltProvider] qualification passed\n";
        return 0;
    } catch (const std::exception &Error) {
        std::cerr << "[Physics:JoltProvider] " << Error.what() << '\n';
        return 1;
    }
}
