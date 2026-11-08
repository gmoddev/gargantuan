#include "UnifiedJoltProvider.hpp"
#include "gargantuan/classes/Cloth.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
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
        const auto Hit = Provider.Rigid->Raycast({.Origin = {0, 2, 0}, .Direction = {0, -5, 0}});
        Require(Hit.Succeeded() && std::any_of(Hit.Candidates.begin(), Hit.Candidates.end(),
            [&](const auto &Candidate) { return Candidate.Body == Imported; }),
            "canonical mesh did not serve a ray");
        Require(Provider.Rigid->DestroyBody(Imported).Succeeded(), "mesh release failed");
        Require(!Provider.Rigid->IsBodyValid(Imported), "retired mesh body is still live");
        Require(Provider.Rigid->DestroyBody(Wall).Succeeded(), "wall release failed");
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
        const auto Fabric = Provider.Soft->CreateBody(Cloth);
        Require(Fabric.IsValid(), "cloth admission failed");
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
        Require(Provider.Soft->DestroyBody(Fabric).Succeeded(), "cloth release failed");
        const auto Reused = Provider.Soft->CreateBody(Cloth);
        Require(Reused.IsValid() && Reused != Fabric && !Provider.Soft->IsBodyValid(Fabric),
            "stale cloth identity resolved after reuse");
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
    try {
        RigidIdentityAndEvents();
        CapsuleAndMeshResidency();
        ClothAndRubberCoupling();
        DataModelAdmissionAndStreaming();
        std::cout << "[Physics:JoltProvider] qualification passed\n";
        return 0;
    } catch (const std::exception &Error) {
        std::cerr << "[Physics:JoltProvider] " << Error.what() << '\n';
        return 1;
    }
}
