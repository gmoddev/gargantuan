#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/Cloth.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/RubberBody.hpp"
#include "gargantuan/classes/SoftBodyAttachment.hpp"
#include "gargantuan/classes/SoftBodyMaterial.hpp"
#include "gargantuan/editor/PlaySession.hpp"
#include "gargantuan/physics/SoftBodyBackend.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "FAIL: " << Message << '\n';
		++Failures;
	}

	bool Near(float Left, float Right, float Epsilon = 0.001f) {
		return std::abs(Left - Right) <= Epsilon;
	}

	float Distance(const glm::vec3 &Left, const glm::vec3 &Right) {
		return glm::length(Left - Right);
	}

	gargantuan::SoftBodyDefinition ClothDefinition(std::uint32_t X = 8, std::uint32_t Y = 8) {
		using namespace gargantuan;
		SoftBodyDefinition Definition;
		Definition.Kind = SoftBodyKind::Cloth;
		Definition.Position = {0.0f, 8.0f, 0.0f};
		Definition.Size = {8.0f, 8.0f, 0.5f};
		Definition.ResolutionX = X;
		Definition.ResolutionY = Y;
		Definition.Material.Damping = 0.02f;
		Definition.Attachments = {
			{0, {-4.0f, 12.0f, 0.0f}},
			{X - 1, {4.0f, 12.0f, 0.0f}},
		};
		return Definition;
	}

	void TestIdentityBoundsAndDeterminism() {
		using namespace gargantuan;
		SoftBodyWorld World;
		auto Definition = ClothDefinition();
		const auto First = World.CreateBody(Definition);
		Check(First.IsValid() && World.IsBodyValid(First), "neutral deformable body obtains a live generation-safe ID");
		Check(World.DestroyBody(First).Succeeded() && !World.IsBodyValid(First), "destroy invalidates deformable identity");
		Check(
			World.DestroyBody(First).Status == PhysicsOperationStatus::InvalidId,
			"duplicate deformable destruction fails closed"
		);
		const auto Replacement = World.CreateBody(Definition);
		Check(
			Replacement.IsValid() && Replacement.Slot == First.Slot && Replacement.Generation != First.Generation,
			"reused deformable slots advance generation"
		);

		auto Oversized = ClothDefinition(256, 256);
		Oversized.ResolutionX = 257;
		Check(!World.CreateBody(Oversized).IsValid(), "per-body vertex bounds reject oversized topology atomically");
		auto Invalid = Definition;
		Invalid.Material.ParticleMass = 0.0f;
		Check(!World.CreateBody(Invalid).IsValid(), "invalid material definitions fail closed");
		Check(
			World.ApplyForce(Replacement, {std::numeric_limits<float>::infinity(), 0.0f, 0.0f}).Status ==
				PhysicsOperationStatus::InvalidDescription,
			"non-finite force input is rejected"
		);
		SoftBodyCollider InvalidCollider;
		InvalidCollider.Shape.Size.x = std::numeric_limits<float>::quiet_NaN();
		auto InvalidColliderResult = World.Step({
			.DeltaTime = SoftBodyStepInterval,
			.Gravity = {0.0f, 0.0f, 0.0f},
			.Colliders = {InvalidCollider},
		});
		Check(
			InvalidColliderResult.CollidersTruncated && InvalidColliderResult.States.size() == 1 &&
				std::ranges::all_of(*InvalidColliderResult.States.front().Positions, [](const glm::vec3 &Position) {
					return std::isfinite(Position.x) && std::isfinite(Position.y) && std::isfinite(Position.z);
				}),
			"invalid collider snapshots are rejected without contaminating solver state"
		);

		SoftBodyWorld Left;
		SoftBodyWorld Right;
		const auto LeftId = Left.CreateBody(Definition);
		const auto RightId = Right.CreateBody(Definition);
		for (int Step = 0; Step < 120; ++Step) {
			auto LeftResult = Left.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -20.0f, 0.0f}});
			auto RightResult = Right.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -20.0f, 0.0f}});
			Check(LeftResult.States.size() == 1 && RightResult.States.size() == 1, "deterministic worlds publish one state");
		}
		const auto LeftState = Left.GetBodyState(LeftId);
		const auto RightState = Right.GetBodyState(RightId);
		Check(
			LeftState && RightState && *LeftState->Positions == *RightState->Positions,
			"identical fixed-step inputs produce byte-equivalent CPU positions"
		);
	}

	void TestPinnedClothAndRigidCollision() {
		using namespace gargantuan;
		SoftBodyWorld World;
		auto Definition = ClothDefinition();
		const auto Body = World.CreateBody(Definition);
		const auto Initial = World.GetBodyState(Body);
		Check(Initial && Initial->Positions->size() == 64 && Initial->Indices->size() == 294, "cloth builds stable grid topology");
		const auto InitialTopology = Initial ? Initial->TopologyRevision : 0;
		for (int Step = 0; Step < 120; ++Step)
			(void)World.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -40.0f, 0.0f}});
		const auto Hanging = World.GetBodyState(Body);
		Check(
			Hanging && Near((*Hanging->Positions)[0].y, 12.0f) && Near((*Hanging->Positions)[7].y, 12.0f),
			"cloth attachments remain exact under sustained gravity"
		);
		Check(
			Hanging && (*Hanging->Positions)[35].y < (*Initial->Positions)[35].y,
			"unpinned cloth vertices sag under gravity"
		);
		Check(
			Hanging && Hanging->TopologyRevision == InitialTopology && Hanging->VertexRevision > Initial->VertexRevision,
			"cloth simulation changes vertices without changing topology"
		);

		SoftBodyWorld CollisionWorld;
		auto FallingDefinition = ClothDefinition();
		FallingDefinition.Attachments.clear();
		FallingDefinition.Position = {0.0f, 5.0f, 0.0f};
		FallingDefinition.Size = {4.0f, 4.0f, 0.5f};
		const auto Falling = CollisionWorld.CreateBody(FallingDefinition);
		SoftBodyCollider Floor{
			.Shape = {.Kind = PhysicsShapeKind::Box, .Size = {100.0f, 2.0f, 100.0f}},
			.Transform = CFrame(0.0f, -1.0f, 0.0f),
		};
		for (int Step = 0; Step < 240; ++Step)
			(void)CollisionWorld.Step({
				.DeltaTime = SoftBodyStepInterval,
				.Gravity = {0.0f, -40.0f, 0.0f},
				.Colliders = {Floor},
			});
		const auto Resting = CollisionWorld.GetBodyState(Falling);
		const auto MinimumY = Resting ? std::ranges::min(*Resting->Positions, {}, [](const glm::vec3 &Value) {
			return Value.y;
		}).y : -100.0f;
		Check(MinimumY >= 0.045f, "cloth collision keeps vertices outside a neutral rigid box collider");

		auto SlideDistance = [&Floor](float Friction) {
			SoftBodyWorld SlideWorld;
			auto SlideDefinition = ClothDefinition(2, 2);
			SlideDefinition.Attachments.clear();
			SlideDefinition.Position = {0.0f, 0.1f, 0.0f};
			SlideDefinition.Size = {2.0f, 0.1f, 0.5f};
			SlideDefinition.Material.Damping = 0.0f;
			SlideDefinition.Material.Friction = Friction;
			const auto SlideBody = SlideWorld.CreateBody(SlideDefinition);
			(void)SlideWorld.ApplyImpulse(SlideBody, {4.0f, 0.0f, 0.0f});
			for (int Step = 0; Step < 30; ++Step)
				(void)SlideWorld.Step({
					.DeltaTime = SoftBodyStepInterval,
					.Gravity = {0.0f, -20.0f, 0.0f},
					.Colliders = {Floor},
				});
			const auto State = SlideWorld.GetBodyState(SlideBody);
			float AverageX = 0.0f;
			if (State) for (const auto &Position : *State->Positions) AverageX += Position.x;
			return State ? AverageX / static_cast<float>(State->Positions->size()) : 0.0f;
		};
		const auto FreeSlide = SlideDistance(0.0f);
		const auto FrictionSlide = SlideDistance(1.0f);
		Check(
			FrictionSlide < FreeSlide * 0.5f,
			"material friction removes tangential motion after rigid-primitive contact"
		);
	}

	void TestRubberRecoveryAndQualityTier() {
		using namespace gargantuan;
		SoftBodyWorld World;
		SoftBodyDefinition Definition;
		Definition.Kind = SoftBodyKind::Rubber;
		Definition.Position = {0.0f, 4.0f, 0.0f};
		Definition.Size = {3.0f, 3.0f, 3.0f};
		Definition.ResolutionX = 4;
		Definition.ResolutionY = 4;
		Definition.ResolutionZ = 4;
		Definition.Material.ShapeCompliance = 0.01f;
		Definition.Material.Damping = 0.08f;
		Definition.Attachments = {{0, {-1.5f, 2.5f, -1.5f}}};
		const auto Body = World.CreateBody(Definition);
		const auto Rest = World.GetBodyState(Body);
		Check(Rest && Rest->Positions->size() == 64 && !Rest->Indices->empty(), "rubber builds a surface-rendered elastic lattice");
		Check(World.ApplyImpulse(Body, {30.0f, 8.0f, 0.0f}).Succeeded(), "rubber accepts a semantic impulse");
		(void)World.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, 0.0f, 0.0f}});
		const auto Deformed = World.GetBodyState(Body);
		const auto Probe = std::size_t{63};
		const auto RestDistance = Distance((*Rest->Positions)[Probe], (*Rest->Positions)[0]);
		const auto DeformedError = std::abs(Distance((*Deformed->Positions)[Probe], (*Deformed->Positions)[0]) - RestDistance);
		for (int Step = 0; Step < 180; ++Step)
			(void)World.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, 0.0f, 0.0f}});
		const auto Recovered = World.GetBodyState(Body);
		const auto RecoveredError = std::abs(Distance((*Recovered->Positions)[Probe], (*Recovered->Positions)[0]) - RestDistance);
		if (!(RecoveredError < DeformedError))
			std::cerr << "[Physics:SoftBodyTest] rubber errors deformed=" << DeformedError << " recovered=" << RecoveredError << '\n';
		Check(RecoveredError < DeformedError, "rubber shape constraints recover toward the translated rest shape after impulse");

		SoftBodyWorld TierWorld;
		auto LowQuality = ClothDefinition(65, 65);
		LowQuality.Attachments.clear();
		LowQuality.Quality = SoftBodyQuality::Low;
		const auto TierBody = TierWorld.CreateBody(LowQuality);
		auto TierResult = TierWorld.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -20.0f, 0.0f}});
		Check(
			TierBody.IsValid() && TierResult.VerticesTruncated && TierResult.States.size() == 1 &&
				!TierResult.States.front().Simulated,
			"low quality freezes topology that cannot fit its hard active-vertex budget"
		);
	}

	void TestRuntimeRenderAndPersistenceLifecycle() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		WorkspaceValue->SetGravity(20.0f);
		auto Body = std::make_shared<Cloth>();
		Body->SetResolutionX(8);
		Body->SetResolutionY(8);
		Body->SetPosition({0.0f, 8.0f, 0.0f});
		Body->SetSize({8.0f, 8.0f, 0.5f});
		auto Material = std::make_shared<SoftBodyMaterial>();
		Material->SetName("Fabric");
		Material->SetParent(Body);
		Body->SetMaterial(Material);
		auto LeftPin = std::make_shared<SoftBodyAttachment>();
		LeftPin->SetVertexIndex(0);
		LeftPin->SetPosition({-4.0f, 12.0f, 0.0f});
		LeftPin->SetParent(Body);
		auto RightPin = std::make_shared<SoftBodyAttachment>();
		RightPin->SetVertexIndex(7);
		RightPin->SetPosition({4.0f, 12.0f, 0.0f});
		RightPin->SetParent(Body);
		Body->SetParent(WorkspaceValue);
		WorkspaceValue->StepPhysics(SoftBodyStepInterval, std::nullopt);
		const auto RuntimeState = WorkspaceValue->GetDeformableState(Body->GetObjectId());
		Check(RuntimeState && RuntimeState->Positions->size() == 64, "WorldRoot owns post-step semantic deformation state");

		RenderPublisher Publisher;
		RenderProjection Projection;
		const auto Camera = MakeLookAtRenderCameraInput({0.0f, 8.0f, 24.0f}, {0.0f, 8.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
		auto First = Publisher.Publish(*WorkspaceValue, Camera, 640, 360);
		Check(
			First->FullResync && First->MeshCreates.size() == 1 && First->Creates.size() == 1 &&
				First->Creates.front().Mesh.has_value(),
			"full publication creates one stable deformable mesh and render object"
		);
		auto FirstChanges = Projection.Apply(*First);
		Check(FirstChanges.MeshesCreated == 1 && Projection.GetMeshCount() == 1, "headless projection applies deformable topology");
		WorkspaceValue->StepPhysics(SoftBodyStepInterval, std::nullopt);
		auto Second = Publisher.Publish(*WorkspaceValue, Camera, 640, 360);
		Check(
			!Second->FullResync && Second->MeshCreates.empty() && Second->MeshVertexUpdates.size() == 1,
			"steady simulation publishes vertex-only deformable updates"
		);
		auto SecondChanges = Projection.Apply(*Second);
		Check(SecondChanges.MeshesUpdated == 1 && Projection.GetMeshCount() == 1, "headless projection keeps persistent mesh residency");
		Publisher.RequestFullResync();
		auto RestartPublication = Publisher.Publish(*WorkspaceValue, Camera, 640, 360);
		Check(
			RestartPublication->FullResync && RestartPublication->MeshCreates.size() == 1 &&
				RestartPublication->MeshVertexUpdates.empty(),
			"renderer restart republishes complete deformable topology instead of relying on stale residency"
		);
		(void)Projection.Apply(*RestartPublication);

		Game->MarkPersistenceSubtreeArchivable();
		Body->SetName("PlayCloth");
		std::shared_ptr<Instance> PlayRoot = Game;
		const auto PlaySnapshot = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, PlayRoot);
		PlaySession FirstPlay(
			{9101}, PlaySnapshot, InstanceSerialization::InstanceFormat::Json,
			std::filesystem::temp_directory_path(), 64, 64, 1
		);
		auto FirstPlayWorld = FirstPlay.GetWorld();
		auto FirstPlayWorkspace = std::dynamic_pointer_cast<Workspace>(FirstPlayWorld->GetService("Workspace"));
		auto FirstPlayBody = FirstPlayWorld->FindFirstChild("PlayCloth", true);
		FirstPlayWorkspace->StepPhysics(SoftBodyStepInterval, std::nullopt);
		Check(
			FirstPlay.GetState() == PlaySessionState::Running && FirstPlayBody &&
				FirstPlayWorkspace->GetDeformableState(FirstPlayBody->GetObjectId()).has_value(),
			"play snapshot constructs and simulates a fresh deformable runtime"
		);
		FirstPlay.Stop();
		Check(
			FirstPlay.GetState() == PlaySessionState::Stopped && !FirstPlay.GetWorld(),
			"stopping play releases its deformable runtime world"
		);
		PlaySession SecondPlay(
			{9102}, PlaySnapshot, InstanceSerialization::InstanceFormat::Json,
			std::filesystem::temp_directory_path(), 64, 64, 2
		);
		auto SecondPlayWorld = SecondPlay.GetWorld();
		auto SecondPlayWorkspace = std::dynamic_pointer_cast<Workspace>(SecondPlayWorld->GetService("Workspace"));
		auto SecondPlayBody = SecondPlayWorld->FindFirstChild("PlayCloth", true);
		SecondPlayWorkspace->StepPhysics(SoftBodyStepInterval, std::nullopt);
		Check(
			SecondPlay.GetState() == PlaySessionState::Running && SecondPlayBody &&
				SecondPlayWorkspace->GetDeformableState(SecondPlayBody->GetObjectId()).has_value(),
			"restarting play reconstructs deformable solver identity from semantics"
		);
		SecondPlay.Stop();

		std::shared_ptr<Instance> PersistedRoot = std::make_shared<Folder>();
		auto PersistedBody = std::make_shared<Cloth>();
		PersistedBody->SetName("PersistedCloth");
		PersistedBody->SetResolutionX(12);
		PersistedBody->SetResolutionY(10);
		auto PersistedMaterial = std::make_shared<SoftBodyMaterial>();
		PersistedMaterial->SetName("PersistedMaterial");
		PersistedMaterial->SetDamping(0.2f);
		PersistedMaterial->SetParent(PersistedRoot);
		PersistedBody->SetMaterial(PersistedMaterial);
		auto PersistedPin = std::make_shared<SoftBodyAttachment>();
		PersistedPin->SetVertexIndex(3);
		PersistedPin->SetPosition({1.0f, 2.0f, 3.0f});
		PersistedPin->SetParent(PersistedBody);
		PersistedBody->SetParent(PersistedRoot);
		PersistedRoot->MarkPersistenceSubtreeArchivable();
		const auto Serialized = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, PersistedRoot);
		if (!(Serialized.find("PersistedCloth") != std::string::npos && Serialized.find("ResolutionX") != std::string::npos &&
			Serialized.find("VertexIndex") != std::string::npos && Serialized.find("Damping") != std::string::npos))
			std::cerr << "[Physics:SoftBodyTest] persisted document=" << Serialized << '\n';
		Check(
			Serialized.find("PersistedCloth") != std::string::npos && Serialized.find("ResolutionX") != std::string::npos &&
				Serialized.find("VertexIndex") != std::string::npos && Serialized.find("Damping") != std::string::npos,
			"persistence contains semantic body, topology, material, and attachment settings"
		);
		Check(
			Serialized.find("Velocity") == std::string::npos && Serialized.find("Lambda") == std::string::npos &&
				Serialized.find("VertexRevision") == std::string::npos && Serialized.find("SoftBodyId") == std::string::npos,
			"persistence excludes solver, identity, and render runtime state"
		);
		std::istringstream Input(Serialized);
		auto Loaded = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, Input);
		if (!Loaded.Ok) for (const auto &Error : Loaded.Errors) std::cerr << "[Physics:SoftBodyTest] load error: " << Error << '\n';
		Check(Loaded.Ok && Loaded.Instance && Loaded.Instance->FindFirstChild("PersistedCloth", false), "semantic deformable state round-trips");

		Body->Destroy();
		auto Removed = Publisher.Publish(*WorkspaceValue, Camera, 640, 360);
		Check(Removed->Removes.size() == 1 && Removed->MeshRemoves.size() == 1, "destruction removes render object and mesh residency");
		(void)Projection.Apply(*Removed);
		Check(Projection.GetMeshCount() == 0, "deformable mesh residency is released after destruction");
		Game->Destroy();
	}
}

int main() {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		TestIdentityBoundsAndDeterminism();
		TestPinnedClothAndRigidCollision();
		TestRubberRecoveryAndQualityTier();
		TestRuntimeRenderAndPersistenceLifecycle();
	} catch (const std::exception &Exception) {
		std::cerr << "[Physics:SoftBodyTest] unexpected exception: " << Exception.what() << '\n';
		return 1;
	}
	if (Failures != 0) return 1;
	std::cout << "[Physics:SoftBodyTest] All soft-body foundation tests passed\n";
	return 0;
}
