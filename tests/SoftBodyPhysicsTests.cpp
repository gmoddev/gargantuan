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
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <glm/gtc/quaternion.hpp>

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
		Check(
			World.DestroyBody(First).Succeeded() && !World.IsBodyValid(First), "destroy invalidates deformable identity"
		);
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
				std::ranges::all_of(
					*InvalidColliderResult.States.front().Positions,
					[](const glm::vec3 &Position) {
						return std::isfinite(Position.x) && std::isfinite(Position.y) && std::isfinite(Position.z);
					}
				),
			"invalid collider snapshots are rejected without contaminating solver state"
		);

		SoftBodyWorld Left;
		SoftBodyWorld Right;
		const auto LeftId = Left.CreateBody(Definition);
		const auto RightId = Right.CreateBody(Definition);
		for (int Step = 0; Step < 120; ++Step) {
			auto LeftResult = Left.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -20.0f, 0.0f}});
			auto RightResult = Right.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -20.0f, 0.0f}});
			Check(
				LeftResult.States.size() == 1 && RightResult.States.size() == 1,
				"deterministic worlds publish one state"
			);
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
		Check(
			Initial && Initial->Positions->size() == 64 && Initial->Indices->size() == 294,
			"cloth builds stable grid topology"
		);
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
			Hanging && Hanging->TopologyRevision == InitialTopology &&
				Hanging->VertexRevision > Initial->VertexRevision,
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
		const auto MinimumY = Resting ? std::ranges::min(
											*Resting->Positions, {}, [](const glm::vec3 &Value) { return Value.y; }
										).y
									  : -100.0f;
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
			if (State)
				for (const auto &Position : *State->Positions)
					AverageX += Position.x;
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
		Check(
			Rest && Rest->Positions->size() == 64 && !Rest->Indices->empty(),
			"rubber builds a surface-rendered elastic lattice"
		);
		Check(World.ApplyImpulse(Body, {30.0f, 8.0f, 0.0f}).Succeeded(), "rubber accepts a semantic impulse");
		(void)World.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, 0.0f, 0.0f}});
		const auto Deformed = World.GetBodyState(Body);
		const auto Probe = std::size_t{63};
		const auto RestDistance = Distance((*Rest->Positions)[Probe], (*Rest->Positions)[0]);
		const auto DeformedError = std::abs(
			Distance((*Deformed->Positions)[Probe], (*Deformed->Positions)[0]) - RestDistance
		);
		for (int Step = 0; Step < 180; ++Step)
			(void)World.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, 0.0f, 0.0f}});
		const auto Recovered = World.GetBodyState(Body);
		const auto RecoveredError = std::abs(
			Distance((*Recovered->Positions)[Probe], (*Recovered->Positions)[0]) - RestDistance
		);
		if (!(RecoveredError < DeformedError))
			std::cerr << "[Physics:SoftBodyTest] rubber errors deformed=" << DeformedError
					  << " recovered=" << RecoveredError << '\n';
		Check(
			RecoveredError < DeformedError,
			"rubber shape constraints recover toward the translated rest shape after impulse"
		);

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

	void TestJobifiedDeterminismAndLifecycle() {
		using namespace gargantuan;
		SoftBodyWorld Jobified;
		SoftBodyWorld Reference;
		auto Small = ClothDefinition(16, 16);
		Small.Attachments.clear();
		const auto JobifiedBody = Jobified.CreateBody(Small);
		const auto ReferenceBody = Reference.CreateBody(Small);
		for (int Step = 0; Step < 30; ++Step) {
			const auto JobResult = Jobified.Step({
				.DeltaTime = SoftBodyStepInterval,
				.Gravity = {0.0f, -20.0f, 0.0f},
				.ExecutionMode = SoftBodyExecutionMode::Jobified,
			});
			const auto ReferenceResult = Reference.Step({
				.DeltaTime = SoftBodyStepInterval,
				.Gravity = {0.0f, -20.0f, 0.0f},
				.ExecutionMode = SoftBodyExecutionMode::SynchronousReference,
			});
			Check(JobResult.Profile.JobsDispatched == 1, "one body dispatches one bounded worker job");
			Check(ReferenceResult.Profile.JobsDispatched == 0, "synchronous reference bypasses worker dispatch");
		}
		Check(
			*Jobified.GetBodyState(JobifiedBody)->Positions == *Reference.GetBodyState(ReferenceBody)->Positions,
			"jobified same-body solve is byte-equivalent to the synchronous reference"
		);

		SoftBodyWorld Multiple;
		const auto First = Multiple.CreateBody(ClothDefinition(8, 8));
		const auto Second = Multiple.CreateBody(ClothDefinition(16, 16));
		const auto Third = Multiple.CreateBody(ClothDefinition(4, 4));
		auto MultipleResult = Multiple.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -10.0f, 0.0f}});
		Check(
			MultipleResult.Profile.JobsDispatched == 3 && MultipleResult.States.size() == 3 &&
				MultipleResult.States[0].Body == First && MultipleResult.States[1].Body == Second &&
				MultipleResult.States[2].Body == Third,
			"multiple independently completed bodies merge in deterministic identity order"
		);

		SoftBodyWorld Lifecycle;
		auto Large = ClothDefinition(256, 256);
		Large.Attachments.clear();
		Large.Quality = SoftBodyQuality::High;
		const auto Old = Lifecycle.CreateBody(Large);
		SoftBodyStepResult InFlightResult;
		std::thread Worker([&] {
			InFlightResult = Lifecycle.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -20.0f, 0.0f}});
		});
		for (int Attempt = 0; Attempt < 1000 && !Lifecycle.HasInFlightStep(); ++Attempt)
			std::this_thread::yield();
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		auto Dropped = Lifecycle.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -20.0f, 0.0f}});
		Check(Dropped.Profile.BacklogDrops == 1, "a second in-flight batch is rejected instead of queued");
		auto Disabled = Large;
		Disabled.Enabled = false;
		Disabled.ResolutionX = 8;
		Disabled.ResolutionY = 8;
		Check(
			Lifecycle.UpdateBody(Old, Disabled).Succeeded(),
			"disable and topology reconfiguration can supersede in-flight work"
		);
		Worker.join();
		const auto DisabledState = Lifecycle.GetBodyState(Old);
		Check(
			DisabledState && DisabledState->Positions->size() == 64 && !DisabledState->Simulated &&
				InFlightResult.Profile.StaleResults >= 1,
			"stale in-flight work cannot overwrite a disabled, topology-reconfigured body"
		);

		SoftBodyWorld ReconfigureWorld;
		const auto ReconfiguredBody = ReconfigureWorld.CreateBody(Large);
		SoftBodyStepResult ReconfigureResult;
		std::thread ReconfigureWorker([&] {
			ReconfigureResult =
				ReconfigureWorld.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -20.0f, 0.0f}});
		});
		for (int Attempt = 0; Attempt < 1000 && !ReconfigureWorld.HasInFlightStep(); ++Attempt)
			std::this_thread::yield();
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		auto Reconfigured = Large;
		Reconfigured.ResolutionX = 8;
		Reconfigured.ResolutionY = 8;
		Reconfigured.Material.Damping = 0.25f;
		Check(
			ReconfigureWorld.UpdateBody(ReconfiguredBody, Reconfigured).Succeeded(),
			"enabled topology and material reconfiguration can supersede in-flight work"
		);
		ReconfigureWorker.join();
		const auto ReconfiguredState = ReconfigureWorld.GetBodyState(ReconfiguredBody);
		Check(
			ReconfiguredState && ReconfiguredState->Positions->size() == 64 && !ReconfiguredState->Simulated &&
				ReconfigureResult.Profile.StaleResults >= 1,
			"stale in-flight work cannot overwrite a newer enabled topology or material"
		);

		SoftBodyWorld DestroyWorld;
		const auto DestroyedBody = DestroyWorld.CreateBody(Large);
		SoftBodyStepResult DestroyResult;
		std::thread DestroyWorker([&] {
			DestroyResult = DestroyWorld.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -20.0f, 0.0f}});
		});
		for (int Attempt = 0; Attempt < 1000 && !DestroyWorld.HasInFlightStep(); ++Attempt)
			std::this_thread::yield();
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		Check(
			DestroyWorld.DestroyBody(DestroyedBody).Succeeded(),
			"destroy while a deformable worker is in flight succeeds safely"
		);
		const auto Replacement = DestroyWorld.CreateBody(ClothDefinition(8, 8));
		DestroyWorker.join();
		const auto ReplacementState = DestroyWorld.GetBodyState(Replacement);
		Check(
			Replacement.Slot == DestroyedBody.Slot && Replacement.Generation != DestroyedBody.Generation &&
				ReplacementState && ReplacementState->Positions->size() == 64 &&
				!DestroyWorld.IsBodyValid(DestroyedBody) && DestroyResult.Profile.StaleResults >= 1,
			"stale in-flight work cannot resurrect a destroyed body or overwrite a reused slot generation"
		);

		SoftBodyWorld ShutdownWorld;
		auto ShutdownDefinition = ClothDefinition(128, 128);
		ShutdownDefinition.Attachments.clear();
		ShutdownDefinition.Quality = SoftBodyQuality::High;
		(void)ShutdownWorld.CreateBody(ShutdownDefinition);
		std::thread ShutdownWorker([&] {
			(void)ShutdownWorld.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -20.0f, 0.0f}});
		});
		for (int Attempt = 0; Attempt < 1000 && !ShutdownWorld.HasInFlightStep(); ++Attempt)
			std::this_thread::yield();
		ShutdownWorld.Shutdown();
		ShutdownWorker.join();
		Check(!ShutdownWorld.IsValid(), "world shutdown drains active worker jobs and rejects future work");
	}

	void TestBroadphaseAndPrimitiveCoverage() {
		using namespace gargantuan;
		auto Definition = ClothDefinition(4, 4);
		Definition.Attachments.clear();
		Definition.Position = {0.0f, 0.0f, 0.0f};
		Definition.Size = {0.2f, 0.2f, 0.1f};
		Definition.Material.Damping = 0.0f;
		std::vector<SoftBodyCollider> Colliders;
		Colliders.push_back({
			.Shape = {.Kind = PhysicsShapeKind::Box, .Size = {2.0f, 2.0f, 2.0f}},
			.Transform = CFrame(0.0f, 0.0f, 0.0f),
		});
		for (int Index = 1; Index < 256; ++Index) {
			Colliders.push_back({
				.Shape = {.Kind = PhysicsShapeKind::Box, .Size = {1.0f, 1.0f, 1.0f}},
				.Transform = CFrame(100.0f + static_cast<float>(Index) * 4.0f, 0.0f, 0.0f),
			});
		}
		SoftBodyWorld Broadphase;
		SoftBodyWorld BruteForce;
		const auto BroadphaseBody = Broadphase.CreateBody(Definition);
		const auto BruteForceBody = BruteForce.CreateBody(Definition);
		auto BroadphaseResult = Broadphase.Step({
			.DeltaTime = SoftBodyStepInterval,
			.Gravity = {0.0f, 0.0f, 0.0f},
			.Colliders = Colliders,
			.BroadphaseMode = SoftBodyBroadphaseMode::DeterministicSweep,
		});
		auto BruteForceResult = BruteForce.Step({
			.DeltaTime = SoftBodyStepInterval,
			.Gravity = {0.0f, 0.0f, 0.0f},
			.Colliders = Colliders,
			.BroadphaseMode = SoftBodyBroadphaseMode::BruteForceReference,
		});
		Check(
			*Broadphase.GetBodyState(BroadphaseBody)->Positions == *BruteForce.GetBodyState(BruteForceBody)->Positions,
			"deterministic broadphase produces collision-equivalent results to brute force"
		);
		Check(
			BroadphaseResult.Profile.CandidateColliders < BruteForceResult.Profile.CandidateColliders / 16,
			"sparse broadphase materially reduces candidate colliders without misses"
		);

		const auto RotatedBounds = glm::mat3_cast(glm::angleAxis(0.785398163f, glm::vec3(0.0f, 0.0f, 1.0f)));
		const SoftBodyCollider RotatedCollider{
			.Shape = {.Kind = PhysicsShapeKind::Box, .Size = {4.0f, 0.25f, 0.25f}},
			.Transform = CFrame({1.2f, 1.2f, 0.0f}, RotatedBounds),
		};
		SoftBodyWorld RotatedBroadphase;
		SoftBodyWorld RotatedBruteForce;
		const auto RotatedBroadphaseBody = RotatedBroadphase.CreateBody(Definition);
		const auto RotatedBruteForceBody = RotatedBruteForce.CreateBody(Definition);
		const auto RotatedBroadphaseResult = RotatedBroadphase.Step({
			.DeltaTime = SoftBodyStepInterval,
			.Gravity = {0.0f, 0.0f, 0.0f},
			.Colliders = {RotatedCollider},
			.BroadphaseMode = SoftBodyBroadphaseMode::DeterministicSweep,
		});
		(void)RotatedBruteForce.Step({
			.DeltaTime = SoftBodyStepInterval,
			.Gravity = {0.0f, 0.0f, 0.0f},
			.Colliders = {RotatedCollider},
			.BroadphaseMode = SoftBodyBroadphaseMode::BruteForceReference,
		});
		Check(
			RotatedBroadphaseResult.Profile.CandidateColliders > 0 &&
				*RotatedBroadphase.GetBodyState(RotatedBroadphaseBody)->Positions ==
					*RotatedBruteForce.GetBodyState(RotatedBruteForceBody)->Positions,
			"rotated conservative collider bounds preserve brute-force collision results"
		);

		std::vector<SoftBodyCollider> DenseColliders(64, Colliders.front());
		SoftBodyWorld DenseBroadphase;
		SoftBodyWorld DenseBruteForce;
		const auto DenseBroadphaseBody = DenseBroadphase.CreateBody(Definition);
		const auto DenseBruteForceBody = DenseBruteForce.CreateBody(Definition);
		const auto DenseBroadphaseResult = DenseBroadphase.Step({
			.DeltaTime = SoftBodyStepInterval,
			.Gravity = {0.0f, 0.0f, 0.0f},
			.Colliders = DenseColliders,
			.BroadphaseMode = SoftBodyBroadphaseMode::DeterministicSweep,
		});
		const auto DenseBruteForceResult = DenseBruteForce.Step({
			.DeltaTime = SoftBodyStepInterval,
			.Gravity = {0.0f, 0.0f, 0.0f},
			.Colliders = DenseColliders,
			.BroadphaseMode = SoftBodyBroadphaseMode::BruteForceReference,
		});
		Check(
			DenseBroadphaseResult.Profile.CandidateColliders == DenseBruteForceResult.Profile.CandidateColliders &&
				*DenseBroadphase.GetBodyState(DenseBroadphaseBody)->Positions ==
					*DenseBruteForce.GetBodyState(DenseBruteForceBody)->Positions,
			"dense broadphase retains every plausible candidate and remains reference-equivalent"
		);

		std::vector<SoftBodyCollider> OverLimitColliders(MaximumSoftBodyColliders + 1);
		for (std::size_t Index = 0; Index < OverLimitColliders.size(); ++Index) {
			OverLimitColliders[Index].Shape = {.Kind = PhysicsShapeKind::Box, .Size = {1.0f, 1.0f, 1.0f}};
			OverLimitColliders[Index].Transform = CFrame(100.0f + static_cast<float>(Index), 0.0f, 0.0f);
		}
		SoftBodyWorld LimitWorld;
		(void)LimitWorld.CreateBody(Definition);
		const auto LimitResult = LimitWorld.Step({
			.DeltaTime = SoftBodyStepInterval,
			.Gravity = {0.0f, 0.0f, 0.0f},
			.Colliders = std::move(OverLimitColliders),
		});
		Check(
			LimitResult.CollidersTruncated && LimitResult.Profile.CandidateColliders == 0,
			"collider broadphase construction enforces the 4,096-snapshot hard limit"
		);

		for (const auto Kind : {
				 PhysicsShapeKind::Ball,
				 PhysicsShapeKind::Box,
				 PhysicsShapeKind::Cylinder,
				 PhysicsShapeKind::Wedge,
				 PhysicsShapeKind::CornerWedge,
			 }) {
			SoftBodyWorld PrimitiveWorld;
			const auto Body = PrimitiveWorld.CreateBody(Definition);
			const auto Before = PrimitiveWorld.GetBodyState(Body);
			const auto Rotation = glm::mat3_cast(glm::angleAxis(0.47f, glm::normalize(glm::vec3(1.0f, 2.0f, 0.5f))));
			SoftBodyCollider Primitive{
				.Shape = {.Kind = Kind, .Size = {2.0f, 2.0f, 2.0f}},
				.Transform = CFrame({0.0f, 0.0f, 0.0f}, Rotation),
			};
			(void)PrimitiveWorld.Step({
				.DeltaTime = SoftBodyStepInterval,
				.Gravity = {0.0f, 0.0f, 0.0f},
				.Colliders = {Primitive},
			});
			const auto After = PrimitiveWorld.GetBodyState(Body);
			bool Moved = false;
			for (std::size_t Index = 0; Index < After->Positions->size(); ++Index)
				Moved = Moved || Distance((*Before->Positions)[Index], (*After->Positions)[Index]) > 0.1f;
			Check(Moved, "rotated semantic primitive resolves an intersecting thin cloth body");
		}

		auto ProbeDefinition = Definition;
		ProbeDefinition.ResolutionX = 2;
		ProbeDefinition.ResolutionY = 2;
		ProbeDefinition.Size = {0.001f, 0.001f, 0.001f};
		ProbeDefinition.Material.Thickness = 0.05f;
		auto ContactProbe = [&](glm::vec3 Position, const char *Message) {
			ProbeDefinition.Position = Position;
			SoftBodyWorld ProbeWorld;
			const auto Probe = ProbeWorld.CreateBody(ProbeDefinition);
			const auto Before = ProbeWorld.GetBodyState(Probe);
			(void)ProbeWorld.Step({
				.DeltaTime = SoftBodyStepInterval,
				.Gravity = {0.0f, 0.0f, 0.0f},
				.Colliders = {{.Shape = {.Kind = PhysicsShapeKind::Box, .Size = {2.0f, 2.0f, 2.0f}}}},
			});
			const auto After = ProbeWorld.GetBodyState(Probe);
			Check(Distance((*Before->Positions)[0], (*After->Positions)[0]) > 0.001f, Message);
		};
		ContactProbe({1.03f, 1.03f, 0.0f}, "box edge contact includes thin-cloth thickness");
		ContactProbe({1.025f, 1.025f, 1.025f}, "box corner contact includes thin-cloth thickness");
		ContactProbe({1.049f, 0.0f, 0.0f}, "grazing box contact remains inclusive and stable");

		ProbeDefinition.Position = {0.9f, 0.9f, 0.9f};
		SoftBodyWorld FalsePositiveWorld;
		SoftBodyWorld EmptyWorld;
		const auto FalsePositiveBody = FalsePositiveWorld.CreateBody(ProbeDefinition);
		const auto EmptyBody = EmptyWorld.CreateBody(ProbeDefinition);
		const auto FalsePositiveResult = FalsePositiveWorld.Step({
			.DeltaTime = SoftBodyStepInterval,
			.Gravity = {0.0f, 0.0f, 0.0f},
			.Colliders = {{.Shape = {.Kind = PhysicsShapeKind::Ball, .Size = {2.0f, 2.0f, 2.0f}}}},
		});
		(void)EmptyWorld.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, 0.0f, 0.0f}});
		Check(
			FalsePositiveResult.Profile.CandidateColliders > 0,
			"broadphase conservatively admits sphere-AABB false positives"
		);
		Check(
			*FalsePositiveWorld.GetBodyState(FalsePositiveBody)->Positions ==
				*EmptyWorld.GetBodyState(EmptyBody)->Positions,
			"narrow phase safely rejects a broadphase false positive"
		);

		ProbeDefinition.Position = {0.0f, 4.0f, 0.0f};
		SoftBodyWorld HighSpeedWorld;
		const auto HighSpeedBody = HighSpeedWorld.CreateBody(ProbeDefinition);
		Check(
			HighSpeedWorld.ApplyImpulse(HighSpeedBody, {0.0f, -1000.0f, 0.0f}).Succeeded(),
			"high-speed non-CCD probe accepts a bounded finite impulse"
		);
		(void)HighSpeedWorld.Step({
			.DeltaTime = SoftBodyStepInterval,
			.Gravity = {0.0f, 0.0f, 0.0f},
			.Colliders = {{.Shape = {.Kind = PhysicsShapeKind::Box, .Size = {2.0f, 2.0f, 2.0f}}}},
		});
		Check(
			std::ranges::all_of(
				*HighSpeedWorld.GetBodyState(HighSpeedBody)->Positions,
				[](const glm::vec3 &Position) {
					return std::isfinite(Position.x) && std::isfinite(Position.y) && std::isfinite(Position.z);
				}
			),
			"high-speed displacement remains finite within the documented non-CCD contract"
		);
	}

	void TestRotationAwareRubberAndVolume() {
		using namespace gargantuan;
		auto MeasureVolume = [](const std::vector<glm::vec3> &Positions) {
			constexpr std::array<std::array<std::uint32_t, 4>, 5> CellTetrahedra{{
				{0, 1, 2, 4},
				{1, 3, 2, 7},
				{1, 2, 4, 7},
				{1, 4, 5, 7},
				{2, 6, 4, 7},
			}};
			auto Vertex = [](std::uint32_t X, std::uint32_t Y, std::uint32_t Z) { return (Z * 4U + Y) * 4U + X; };
			double Volume = 0.0;
			for (std::uint32_t Z = 0; Z < 3; ++Z) {
				for (std::uint32_t Y = 0; Y < 3; ++Y) {
					for (std::uint32_t X = 0; X < 3; ++X) {
						const std::array Cell{
							Vertex(X, Y, Z),
							Vertex(X + 1, Y, Z),
							Vertex(X, Y + 1, Z),
							Vertex(X + 1, Y + 1, Z),
							Vertex(X, Y, Z + 1),
							Vertex(X + 1, Y, Z + 1),
							Vertex(X, Y + 1, Z + 1),
							Vertex(X + 1, Y + 1, Z + 1),
						};
						for (const auto &Tetrahedron : CellTetrahedra) {
							const auto &A = Positions[Cell[Tetrahedron[0]]];
							const auto &B = Positions[Cell[Tetrahedron[1]]];
							const auto &C = Positions[Cell[Tetrahedron[2]]];
							const auto &D = Positions[Cell[Tetrahedron[3]]];
							Volume += std::abs(static_cast<double>(glm::dot(B - A, glm::cross(C - A, D - A)))) / 6.0;
						}
					}
				}
			}
			return Volume;
		};
		SoftBodyDefinition Definition;
		Definition.Kind = SoftBodyKind::Rubber;
		Definition.Position = {0.0f, 0.0f, 0.0f};
		Definition.Size = {4.0f, 2.0f, 2.0f};
		Definition.ResolutionX = 4;
		Definition.ResolutionY = 4;
		Definition.ResolutionZ = 4;
		Definition.Material.Damping = 0.01f;
		Definition.Material.ShapeCompliance = 0.00001f;
		Definition.Material.VolumeCompliance = 0.000001f;
		SoftBodyWorld World;
		const auto Body = World.CreateBody(Definition);
		const auto Rest = World.GetBodyState(Body);
		const auto RestAxis = glm::normalize((*Rest->Positions)[3] - (*Rest->Positions)[0]);
		const auto RestSpan = Distance((*Rest->Positions)[3], (*Rest->Positions)[0]);
		Check(
			World.ApplyImpulseAtPosition(Body, {0.0f, 0.0f, 12.0f}, {2.0f, 1.0f, 0.0f}).Succeeded(),
			"rubber accepts a bounded off-center semantic impulse"
		);
		for (int Step = 0; Step < 90; ++Step)
			(void)World.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, 0.0f, 0.0f}});
		const auto Rotated = World.GetBodyState(Body);
		const auto RotatedAxis = glm::normalize((*Rotated->Positions)[3] - (*Rotated->Positions)[0]);
		const auto RotatedSpan = Distance((*Rotated->Positions)[3], (*Rotated->Positions)[0]);
		Check(glm::dot(RestAxis, RotatedAxis) < 0.995f, "off-center impulse produces retained rubber rotation");
		Check(
			std::abs(RotatedSpan - RestSpan) < RestSpan * 0.15f,
			"rotation-aware shape matching restores local shape without restoring world orientation"
		);
		const auto RestVolume = MeasureVolume(*Rest->Positions);
		const auto RotatedVolume = MeasureVolume(*Rotated->Positions);
		Check(
			std::isfinite(RotatedVolume) && std::abs(RotatedVolume - RestVolume) < RestVolume * 0.2,
			"tetrahedral XPBD volume preservation bounds rubber volume loss during rotational deformation"
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
		Check(
			RuntimeState && RuntimeState->Positions->size() == 64, "WorldRoot owns post-step semantic deformation state"
		);

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
		Check(
			FirstChanges.MeshesCreated == 1 && Projection.GetMeshCount() == 1,
			"headless projection applies deformable topology"
		);
		WorkspaceValue->StepPhysics(SoftBodyStepInterval, std::nullopt);
		auto Second = Publisher.Publish(*WorkspaceValue, Camera, 640, 360);
		Check(
			!Second->FullResync && Second->MeshCreates.empty() && Second->MeshVertexUpdates.size() == 1,
			"steady simulation publishes vertex-only deformable updates"
		);
		auto SecondChanges = Projection.Apply(*Second);
		Check(
			SecondChanges.MeshesUpdated == 1 && Projection.GetMeshCount() == 1,
			"headless projection keeps persistent mesh residency"
		);
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
		const auto PlaySnapshot = InstanceSerialization::Serialize(
			InstanceSerialization::InstanceFormat::Json, PlayRoot
		);
		PlaySession FirstPlay(
			{9101},
			PlaySnapshot,
			InstanceSerialization::InstanceFormat::Json,
			std::filesystem::temp_directory_path(),
			64,
			64,
			1
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
			{9102},
			PlaySnapshot,
			InstanceSerialization::InstanceFormat::Json,
			std::filesystem::temp_directory_path(),
			64,
			64,
			2
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
		PersistedMaterial->SetVolumeCompliance(0.00025f);
		PersistedMaterial->SetParent(PersistedRoot);
		PersistedBody->SetMaterial(PersistedMaterial);
		auto PersistedPin = std::make_shared<SoftBodyAttachment>();
		PersistedPin->SetVertexIndex(3);
		PersistedPin->SetPosition({1.0f, 2.0f, 3.0f});
		PersistedPin->SetParent(PersistedBody);
		PersistedBody->SetParent(PersistedRoot);
		PersistedRoot->MarkPersistenceSubtreeArchivable();
		const auto Serialized = InstanceSerialization::Serialize(
			InstanceSerialization::InstanceFormat::Json, PersistedRoot
		);
		if (!(Serialized.find("PersistedCloth") != std::string::npos &&
			  Serialized.find("ResolutionX") != std::string::npos &&
			  Serialized.find("VertexIndex") != std::string::npos && Serialized.find("Damping") != std::string::npos &&
			  Serialized.find("VolumeCompliance") != std::string::npos))
			std::cerr << "[Physics:SoftBodyTest] persisted document=" << Serialized << '\n';
		Check(
			Serialized.find("PersistedCloth") != std::string::npos &&
				Serialized.find("ResolutionX") != std::string::npos &&
				Serialized.find("VertexIndex") != std::string::npos && Serialized.find("Damping") != std::string::npos &&
				Serialized.find("VolumeCompliance") != std::string::npos,
			"persistence contains semantic body, topology, material, and attachment settings"
		);
		Check(
			Serialized.find("Velocity") == std::string::npos && Serialized.find("Lambda") == std::string::npos &&
				Serialized.find("VertexRevision") == std::string::npos &&
				Serialized.find("SoftBodyId") == std::string::npos,
			"persistence excludes solver, identity, and render runtime state"
		);
		std::istringstream Input(Serialized);
		auto Loaded = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, Input);
		if (!Loaded.Ok)
			for (const auto &Error : Loaded.Errors)
				std::cerr << "[Physics:SoftBodyTest] load error: " << Error << '\n';
		Check(
			Loaded.Ok && Loaded.Instance && Loaded.Instance->FindFirstChild("PersistedCloth", false),
			"semantic deformable state round-trips"
		);
		auto LoadedMaterial = Loaded.Instance ? std::dynamic_pointer_cast<SoftBodyMaterial>(
			Loaded.Instance->FindFirstChild("PersistedMaterial", false)
		) : nullptr;
		Check(
			LoadedMaterial && Near(LoadedMaterial->GetVolumeCompliance(), 0.00025f),
			"semantic volume compliance survives persistence reload"
		);

		Body->Destroy();
		auto Removed = Publisher.Publish(*WorkspaceValue, Camera, 640, 360);
		Check(
			Removed->Removes.size() == 1 && Removed->MeshRemoves.size() == 1,
			"destruction removes render object and mesh residency"
		);
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
		TestJobifiedDeterminismAndLifecycle();
		TestBroadphaseAndPrimitiveCoverage();
		TestRotationAwareRubberAndVolume();
		TestRuntimeRenderAndPersistenceLifecycle();
	} catch (const std::exception &Exception) {
		std::cerr << "[Physics:SoftBodyTest] unexpected exception: " << Exception.what() << '\n';
		return 1;
	}
	if (Failures != 0) return 1;
	std::cout << "[Physics:SoftBodyTest] All soft-body foundation tests passed\n";
	return 0;
}
