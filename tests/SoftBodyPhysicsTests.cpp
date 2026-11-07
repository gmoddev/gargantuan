#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/Cloth.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/RubberBody.hpp"
#include "gargantuan/classes/SoftBodyAttachment.hpp"
#include "gargantuan/classes/SoftBodyMaterial.hpp"
#include "gargantuan/editor/PlaySession.hpp"
#include "gargantuan/physics/SoftBodyBackend.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "physics/SoftBodyTestHooks.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <glm/gtc/quaternion.hpp>

namespace {
	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "FAIL: " << Message << '\n';
		++Failures;
	}

	class StepGate final {
	  public:
		void Arm() { Armed.store(true); }

		void Pause(const gargantuan::SoftBodyStepProfile &ProfileValue = {}) {
			if (!Armed.load()) return;
			std::unique_lock Lock(Mutex);
			Profile = ProfileValue;
			Arrived = true;
			Changed.notify_all();
			if (!Changed.wait_for(Lock, std::chrono::seconds(5), [this] { return Released; }))
				throw std::runtime_error("[Physics:SoftBodyTest] Lifecycle barrier was not released");
		}

		[[nodiscard]] bool Wait() {
			std::unique_lock Lock(Mutex);
			return Changed.wait_for(Lock, std::chrono::seconds(5), [this] { return Arrived; });
		}

		void Release() {
			std::scoped_lock Lock(Mutex);
			Released = true;
			Changed.notify_all();
		}

		[[nodiscard]] gargantuan::SoftBodyStepProfile GetProfile() {
			std::scoped_lock Lock(Mutex);
			return Profile;
		}

	  private:
		std::atomic<bool> Armed = false;
		std::mutex Mutex;
		std::condition_variable Changed;
		bool Arrived = false;
		bool Released = false;
		gargantuan::SoftBodyStepProfile Profile;
	};

	class HeldSoftBodyStep final {
	  public:
		explicit HeldSoftBodyStep(
			bool AtAdmission = false,
			std::function<void()> BeforeAttachmentCommit = {},
			bool ThrowAfterMergeBarrier = false
		)
			: World(gargantuan::detail::CreateSoftBodyBackendForTesting(
				MakeHooks(Gate, AtAdmission, std::move(BeforeAttachmentCommit), ThrowAfterMergeBarrier))) {}

		~HeldSoftBodyStep() {
			Gate.Release();
			if (Worker.joinable()) Worker.join();
		}

		void Start() {
			Gate.Arm();
			Worker = std::thread([this] {
				try {
					Result = World.Step({.DeltaTime = gargantuan::SoftBodyStepInterval, .Gravity = {0.0f, -20.0f, 0.0f}});
				} catch (...) {
					Failure = std::current_exception();
				}
			});
		}

		void Finish() {
			Gate.Release();
			if (Worker.joinable()) Worker.join();
			if (Failure) std::rethrow_exception(Failure);
		}

		StepGate Gate;
		gargantuan::SoftBodyWorld World;
		gargantuan::SoftBodyStepResult Result;

	  private:
		static gargantuan::detail::SoftBodyStepTestHooks MakeHooks(
			StepGate &GateValue,
			bool AtAdmission,
			std::function<void()> BeforeAttachmentCommit,
			bool ThrowAfterMergeBarrier
		) {
			gargantuan::detail::SoftBodyStepTestHooks Hooks;
			Hooks.BeforeAttachmentCommit = std::move(BeforeAttachmentCommit);
			if (AtAdmission)
				Hooks.StepAdmitted = [&GateValue] { GateValue.Pause(); };
			else
				Hooks.BeforeMerge = [&GateValue, ThrowAfterMergeBarrier](const auto &Profile) mutable {
					GateValue.Pause(Profile);
					if (ThrowAfterMergeBarrier) {
						ThrowAfterMergeBarrier = false;
						throw std::runtime_error("synthetic pre-merge failure");
					}
				};
			return Hooks;
		}

		std::exception_ptr Failure;
		std::thread Worker;
	};

	bool AwaitMerge(HeldSoftBodyStep &Step, std::size_t Vertices) {
		if (!Step.Gate.Wait()) {
			Check(false, "the real solver must reach the bounded pre-merge barrier");
			Step.Finish();
			return false;
		}
		const auto Profile = Step.Gate.GetProfile();
		const bool Captured = Step.World.HasInFlightStep() && Profile.JobsDispatched == 1 &&
			Profile.QueuedResultBytes == Vertices * sizeof(glm::vec3);
		Check(Captured, "the old body snapshot and completed real solver job must be held before mutation");
		std::cout << "[Physics:SoftBodyTest] BeforeMerge SnapshotVertices=" << Vertices
			<< " JobsDispatched=" << Profile.JobsDispatched << " QueuedResultBytes=" << Profile.QueuedResultBytes
			<< " WorkerWaitNs=" << Profile.WorkerWaitNanoseconds << " Captured=" << Captured << '\n';
		return Captured;
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

	void TestMovingAttachmentAndBoundedInspection() {
		using namespace gargantuan;
		SoftBodyWorld World;
		auto Definition = ClothDefinition();
		const auto Body = World.CreateBody(Definition);
		Check(Body.IsValid(), "moving-attachment fixture creates a cloth body");
		for (int Step = 0; Step < 12; ++Step)
			(void)World.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -196.2f, 0.0f}});
		auto Before = World.GetBodyState(Body);
		Check(Before && Before->Positions && Before->Positions->size() == 64,
			"moving-attachment fixture retains accepted cloth vertices");
		if (!Before || !Before->Positions) return;
		const auto Center = (*Before->Positions)[36];
		Definition.Attachments[0].Position.y += 2.0f;
		Check(World.UpdateBody(Body, Definition).Succeeded(), "moving an existing pin is accepted");
		auto After = World.GetBodyState(Body);
		Check(After && After->Positions && Distance((*After->Positions)[36], Center) < 0.0001f,
			"moving a pin preserves the deformation of unpinned vertices");
		Check(After && After->TopologyRevision == Before->TopologyRevision &&
			After->VertexRevision > Before->VertexRevision &&
			Near((*After->Positions)[0].y, Definition.Attachments[0].Position.y),
			"pin movement changes only vertex state and keeps stable topology");
		auto Info = World.GetBodyInspection(Body);
		Check(Info && Info->VertexCount == 64 && Info->ConstraintCount > 0 && Info->AttachmentCount == 2 &&
			Info->Enabled && Info->TopologyRevision == After->TopologyRevision,
			"read-only inspection returns bounded counts and revisions");
		auto Invalid = Definition;
		Invalid.Attachments.push_back(Invalid.Attachments.front());
		Check(World.UpdateBody(Body, Invalid).Status == PhysicsOperationStatus::InvalidDescription &&
			World.GetBodyState(Body)->VertexRevision == After->VertexRevision,
			"duplicate pin update rejects atomically");
		Invalid = Definition;
		Invalid.Attachments[0].Position.x = std::numeric_limits<float>::infinity();
		Check(World.UpdateBody(Body, Invalid).Status == PhysicsOperationStatus::InvalidDescription &&
			World.GetBodyState(Body)->VertexRevision == After->VertexRevision,
			"non-finite pin update rejects atomically");
		Definition.Attachments.erase(Definition.Attachments.begin());
		Check(World.UpdateBody(Body, Definition).Succeeded() && World.GetBodyInspection(Body)->AttachmentCount == 1,
			"removing a pin releases it without rebuilding the cloth");
		(void)World.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -196.2f, 0.0f}});
		Check(World.DestroyBody(Body).Succeeded() && !World.GetBodyInspection(Body),
			"destroy invalidates bounded inspection");
	}

	void TestAttachmentCommitFailureIsAtomic() {
		using namespace gargantuan;
		detail::SoftBodyStepTestHooks Hooks;
		Hooks.BeforeAttachmentCommit = [] { throw std::bad_alloc(); };
		SoftBodyWorld World(detail::CreateSoftBodyBackendForTesting(std::move(Hooks)));
		auto Definition = ClothDefinition();
		const auto Body = World.CreateBody(Definition);
		(void)World.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -20.0f, 0.0f}});
		const auto Before = World.GetBodyState(Body);
		Definition.Attachments[0].Position.y += 3.0f;
		const auto Failure = World.UpdateBody(Body, Definition);
		const auto After = World.GetBodyState(Body);
		Check(Failure.Status == PhysicsOperationStatus::BackendFailure && Before && After &&
			Before->VertexRevision == After->VertexRevision &&
			*Before->Positions == *After->Positions && World.GetBodyInspection(Body)->AttachmentCount == 2,
			"failure after attachment allocation staging leaves live nodes and publication unchanged");
		(void)World.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -20.0f, 0.0f}});
		Check(World.GetBodyState(Body)->VertexRevision > Before->VertexRevision,
			"a failed attachment update leaves the original body operational");
	}

	void TestMovingRigidColliderThroughWorldRoot() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		World->SetGravity(0.0f);
		auto ClothBody = std::make_shared<Cloth>();
		ClothBody->SetResolutionX(8);
		ClothBody->SetResolutionY(8);
		ClothBody->SetPosition({0.0f, 5.0f, 0.0f});
		ClothBody->SetSize({2.0f, 2.0f, 0.5f});
		ClothBody->SetParent(World);
		auto Collider = std::make_shared<Part>();
		Collider->SetAnchored(true);
		Collider->SetSize({1.0f, 4.0f, 1.0f});
		Collider->SetCFrame(CFrame(0.0f, 5.0f, 2.0f));
		Collider->SetParent(World);
		World->StepPhysics(SoftBodyStepInterval, std::nullopt);
		const auto Before = World->GetDeformableVertex(ClothBody->GetObjectId(), 27);
		Collider->SetCFrame(CFrame(0.0f, 5.0f, 0.3f));
		World->StepPhysics(SoftBodyStepInterval, std::nullopt);
		const auto After = World->GetDeformableVertex(ClothBody->GetObjectId(), 27);
		Check(Before && After && std::isfinite(After->z) && Distance(*Before, *After) > 0.05f &&
			World->GetLastSoftBodyProfile().CandidateColliders > 0,
			"WorldRoot feeds a moved rigid primitive to the cloth contact step");
		Game->Destroy();
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
		Check(TierWorld.GetBodyInspection(TierBody) && !TierWorld.GetBodyInspection(TierBody)->Simulated,
			"read-only status reports a body frozen by the quality budget");
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

		HeldSoftBodyStep MovingPin;
		SoftBodyWorld MovingReference;
		auto MovingDefinition = ClothDefinition(16, 16);
		const auto MovingBody = MovingPin.World.CreateBody(MovingDefinition);
		const auto ReferenceBody = MovingReference.CreateBody(MovingDefinition);
		for (int Step = 0; Step < 12; ++Step) {
			(void)MovingPin.World.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -20.0f, 0.0f}});
			(void)MovingReference.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -20.0f, 0.0f}});
		}
		const auto BeforeMovingStep = MovingPin.World.GetBodyState(MovingBody);
		MovingPin.Start();
		if (!AwaitMerge(MovingPin, 16 * 16)) return;
		(void)MovingReference.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -20.0f, 0.0f}});
		const auto ExpectedSolved = MovingReference.GetBodyState(ReferenceBody);
		MovingDefinition.Attachments[0].Position.y += 1.0f;
		Check(MovingPin.World.UpdateBody(MovingBody, MovingDefinition).Succeeded(),
			"first attachment update while a solve is in flight is accepted");
		MovingDefinition.Attachments[0].Position.y += 1.0f;
		Check(MovingPin.World.UpdateBody(MovingBody, MovingDefinition).Succeeded(),
			"latest of two in-flight attachment targets is accepted");
		const auto PendingState = MovingPin.World.GetBodyState(MovingBody);
		Check(PendingState && BeforeMovingStep &&
			Distance((*PendingState->Positions)[136], (*BeforeMovingStep->Positions)[136]) < 0.0001f,
			"pending pin update preserves the last published unpinned deformation");
		MovingPin.Finish();
		const auto Merged = MovingPin.World.GetBodyState(MovingBody);
		Check(MovingPin.Result.Profile.StaleResults == 0 && Merged && ExpectedSolved && Merged->Simulated &&
			Distance((*Merged->Positions)[136], (*ExpectedSolved->Positions)[136]) < 0.0001f &&
			Near((*Merged->Positions)[0].y, MovingDefinition.Attachments[0].Position.y),
			"completed solve preserves unpinned deformation while publishing the latest pin target");
		Check(MovingReference.UpdateBody(ReferenceBody, MovingDefinition).Succeeded(),
			"reference applies the same pin movement after its step");
		(void)MovingPin.World.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -20.0f, 0.0f}});
		(void)MovingReference.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -20.0f, 0.0f}});
		Check(*MovingPin.World.GetBodyState(MovingBody)->Positions ==
			*MovingReference.GetBodyState(ReferenceBody)->Positions,
			"post-merge motion matches a reference with preserved unpinned velocities");

		HeldSoftBodyStep PendingFault(false, [] { throw std::bad_alloc(); });
		auto PendingFaultDefinition = ClothDefinition(16, 16);
		const auto PendingFaultBody = PendingFault.World.CreateBody(PendingFaultDefinition);
		PendingFault.Start();
		if (!AwaitMerge(PendingFault, 16 * 16)) return;
		const auto BeforePendingFault = PendingFault.World.GetBodyState(PendingFaultBody);
		PendingFaultDefinition.Attachments[0].Position.y += 1.0f;
		const auto PendingFailure = PendingFault.World.UpdateBody(PendingFaultBody, PendingFaultDefinition);
		const auto AfterPendingFault = PendingFault.World.GetBodyState(PendingFaultBody);
		Check(PendingFailure.Status == PhysicsOperationStatus::BackendFailure && BeforePendingFault &&
			AfterPendingFault && BeforePendingFault->VertexRevision == AfterPendingFault->VertexRevision &&
			*BeforePendingFault->Positions == *AfterPendingFault->Positions,
			"failed in-flight attachment staging leaves the previous publication unchanged");
		PendingFault.Finish();
		Check(PendingFault.Result.Profile.StaleResults == 0 &&
			PendingFault.World.GetBodyState(PendingFaultBody)->Simulated,
			"failed in-flight attachment update still accepts the original completed solve");

		HeldSoftBodyStep FailedMerge(false, {}, true);
		auto FailedMergeDefinition = ClothDefinition(16, 16);
		const auto FailedMergeBody = FailedMerge.World.CreateBody(FailedMergeDefinition);
		FailedMerge.Start();
		if (!AwaitMerge(FailedMerge, 16 * 16)) return;
		FailedMergeDefinition.Attachments[0].Position.y += 1.0f;
		Check(FailedMerge.World.UpdateBody(FailedMergeBody, FailedMergeDefinition).Succeeded(),
			"pending pin target is staged before a pre-merge failure");
		bool MergeFailed = false;
		try {
			FailedMerge.Finish();
		} catch (const std::runtime_error &) {
			MergeFailed = true;
		}
		Check(MergeFailed && !FailedMerge.World.HasInFlightStep() &&
			Near((*FailedMerge.World.GetBodyState(FailedMergeBody)->Positions)[0].y,
				FailedMergeDefinition.Attachments[0].Position.y),
			"pre-merge failure restores a current record with the pending pin target");
		(void)FailedMerge.World.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -20.0f, 0.0f}});
		Check(FailedMerge.World.GetBodyState(FailedMergeBody)->Simulated,
			"restored record continues simulating after the pre-merge failure");

		for (int Cancellation = 0; Cancellation < 3; ++Cancellation) {
			HeldSoftBodyStep Held;
			auto Definition = ClothDefinition(16, 16);
			const auto Body = Held.World.CreateBody(Definition);
			Held.Start();
			if (!AwaitMerge(Held, 16 * 16)) return;
			Definition.Attachments[0].Position.y += 1.0f;
			Check(Held.World.UpdateBody(Body, Definition).Succeeded(),
				"pending attachment target is staged before a superseding change");
			if (Cancellation == 0) {
				Definition.Enabled = false;
				Check(Held.World.UpdateBody(Body, Definition).Succeeded(),
					"disable supersedes a pending attachment update");
			} else if (Cancellation == 1) {
				Definition.ResolutionX = 8;
				Definition.Attachments.clear();
				Check(Held.World.UpdateBody(Body, Definition).Succeeded(),
					"topology change supersedes a pending attachment update");
			} else {
				Check(Held.World.DestroyBody(Body).Succeeded(),
					"destroy cancels a pending attachment update");
			}
			Held.Finish();
			const auto State = Held.World.GetBodyState(Body);
			Check(Held.Result.Profile.StaleResults == 1 && Held.Result.States.empty() &&
				(Cancellation == 2 ? !State :
					State && State->Positions->size() == (Cancellation == 1 ? 128u : 256u) && !State->Simulated),
				"superseding lifecycle change rejects the old worker result and pending pin target");
		}

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

		auto Large = ClothDefinition(256, 256);
		Large.Attachments.clear();
		Large.Quality = SoftBodyQuality::High;
		HeldSoftBodyStep Lifecycle;
		const auto Old = Lifecycle.World.CreateBody(Large);
		Lifecycle.Start();
		if (!AwaitMerge(Lifecycle, 256 * 256)) return;
		auto Dropped = Lifecycle.World.Step({.DeltaTime = SoftBodyStepInterval, .Gravity = {0.0f, -20.0f, 0.0f}});
		Check(Dropped.Profile.BacklogDrops == 1, "a second in-flight batch is rejected instead of queued");
		auto Disabled = Large;
		Disabled.Enabled = false;
		Disabled.ResolutionX = 8;
		Disabled.ResolutionY = 8;
		Check(
			Lifecycle.World.UpdateBody(Old, Disabled).Succeeded(),
			"disable and topology reconfiguration can supersede in-flight work"
		);
		const auto ExpectedDisabled = Lifecycle.World.GetBodyState(Old);
		Lifecycle.Finish();
		const auto DisabledState = Lifecycle.World.GetBodyState(Old);
		Check(
			DisabledState && DisabledState->Positions->size() == 64 && !DisabledState->Simulated &&
				Lifecycle.Result.Profile.StaleResults >= 1 && Lifecycle.Result.States.empty() &&
				DisabledState->VertexRevision == ExpectedDisabled->VertexRevision &&
				*DisabledState->Positions == *ExpectedDisabled->Positions,
			"stale in-flight work cannot overwrite a disabled, topology-reconfigured body"
		);

		HeldSoftBodyStep Reconfigure;
		const auto ReconfiguredBody = Reconfigure.World.CreateBody(Large);
		Reconfigure.Start();
		if (!AwaitMerge(Reconfigure, 256 * 256)) return;
		auto Reconfigured = Large;
		Reconfigured.ResolutionX = 8;
		Reconfigured.ResolutionY = 8;
		Reconfigured.Material.Damping = 0.25f;
		Check(
			Reconfigure.World.UpdateBody(ReconfiguredBody, Reconfigured).Succeeded(),
			"enabled topology and material reconfiguration can supersede in-flight work"
		);
		const auto ExpectedReconfigured = Reconfigure.World.GetBodyState(ReconfiguredBody);
		Reconfigure.Finish();
		const auto ReconfiguredState = Reconfigure.World.GetBodyState(ReconfiguredBody);
		Check(
			ReconfiguredState && ReconfiguredState->Positions->size() == 64 && !ReconfiguredState->Simulated &&
				Reconfigure.Result.Profile.StaleResults >= 1 && Reconfigure.Result.States.empty() &&
				ReconfiguredState->VertexRevision == ExpectedReconfigured->VertexRevision &&
				*ReconfiguredState->Positions == *ExpectedReconfigured->Positions,
			"stale in-flight work cannot overwrite a newer enabled topology or material"
		);

		HeldSoftBodyStep Destroy;
		const auto DestroyedBody = Destroy.World.CreateBody(Large);
		Destroy.Start();
		if (!AwaitMerge(Destroy, 256 * 256)) return;
		Check(
			Destroy.World.DestroyBody(DestroyedBody).Succeeded(),
			"destroy while a deformable worker is in flight succeeds safely"
		);
		const auto Replacement = Destroy.World.CreateBody(ClothDefinition(8, 8));
		const auto ExpectedReplacement = Destroy.World.GetBodyState(Replacement);
		Destroy.Finish();
		const auto ReplacementState = Destroy.World.GetBodyState(Replacement);
		Check(
			Replacement.Slot == DestroyedBody.Slot && Replacement.Generation != DestroyedBody.Generation &&
				ReplacementState && ReplacementState->Positions->size() == 64 &&
				!Destroy.World.IsBodyValid(DestroyedBody) && Destroy.Result.Profile.StaleResults >= 1 &&
				Destroy.Result.States.empty() && ReplacementState->VertexRevision == ExpectedReplacement->VertexRevision &&
				*ReplacementState->Positions == *ExpectedReplacement->Positions,
			"stale in-flight work cannot resurrect a destroyed body or overwrite a reused slot generation"
		);

		HeldSoftBodyStep Shutdown;
		auto ShutdownDefinition = ClothDefinition(128, 128);
		ShutdownDefinition.Attachments.clear();
		ShutdownDefinition.Quality = SoftBodyQuality::High;
		(void)Shutdown.World.CreateBody(ShutdownDefinition);
		Shutdown.Start();
		if (!AwaitMerge(Shutdown, 128 * 128)) return;
		std::jthread ShutdownWorker([&] { Shutdown.World.Shutdown(); });
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (Shutdown.World.IsValid() && std::chrono::steady_clock::now() < Deadline)
			std::this_thread::yield();
		Check(!Shutdown.World.IsValid(), "shutdown must start before the held completed batch can merge");
		Shutdown.Finish();
		ShutdownWorker.join();
		Check(
			!Shutdown.World.IsValid() && Shutdown.Result.Profile.StaleResults >= 1 && Shutdown.Result.States.empty(),
			"world shutdown drains active worker jobs and rejects future work"
		);
		std::cout << "[Physics:SoftBodyTest] BeforeMerge CompletedCases=Disable,Reconfigure,DestroyReuse,Shutdown\n";
	}

	void TestAdmissionDoesNotProveCapturedWork() {
		using namespace gargantuan;
		HeldSoftBodyStep Admitted(true);
		auto Large = ClothDefinition(256, 256);
		Large.Attachments.clear();
		Large.Quality = SoftBodyQuality::High;
		const auto Old = Admitted.World.CreateBody(Large);
		Admitted.Start();
		if (!Admitted.Gate.Wait()) {
			Check(false, "the admitted-only premise reproduction must reach its bounded barrier");
			Admitted.Finish();
			return;
		}
		const bool InFlightObserved = Admitted.World.HasInFlightStep();
		Check(InFlightObserved, "Step admission is observable before the old body snapshot exists");
		Check(Admitted.World.DestroyBody(Old).Succeeded(), "admitted-only reproduction destroys the old body");
		const auto Replacement = Admitted.World.CreateBody(ClothDefinition(8, 8));
		Admitted.Finish();
		const bool CapturedReplacement = Replacement.Slot == Old.Slot && Replacement.Generation != Old.Generation &&
				!Admitted.World.IsBodyValid(Old) && Admitted.World.IsBodyValid(Replacement) &&
				Admitted.Result.States.size() == 1 && Admitted.Result.States.front().Body == Replacement &&
				Admitted.Result.States.front().Positions->size() == 64 && Admitted.Result.Profile.StaleResults == 0;
		Check(
			CapturedReplacement,
			"an admitted-only step correctly captures the replacement: zero stale results does not imply resurrection"
		);
		std::cout << "[Physics:SoftBodyTest] AdmittedOnly InFlightObserved=" << InFlightObserved << " StaleResults="
			<< Admitted.Result.Profile.StaleResults << " CapturedReplacement=" << CapturedReplacement
			<< " OldStaleAssertionWouldFail=" << (CapturedReplacement && InFlightObserved) << '\n';
	}

	void TestPrivateHookFailureAndUnhookedEquivalence() {
		using namespace gargantuan;
		detail::SoftBodyStepTestHooks Throwing;
		Throwing.StepAdmitted = [] { throw std::runtime_error("synthetic lifecycle hook failure"); };
		SoftBodyWorld Failed(detail::CreateSoftBodyBackendForTesting(std::move(Throwing)));
		const auto FailedBody = Failed.CreateBody(ClothDefinition());
		bool Rejected = false;
		try { (void)Failed.Step({}); } catch (const std::runtime_error &) { Rejected = true; }
		Check(Rejected && !Failed.HasInFlightStep() && Failed.IsBodyValid(FailedBody),
			"an admitted hook failure clears the in-flight flag without corrupting the body");

		std::size_t Visits = 0;
		detail::SoftBodyStepTestHooks Observed;
		Observed.BeforeMerge = [&](const SoftBodyStepProfile &) { ++Visits; };
		SoftBodyWorld Hooked(detail::CreateSoftBodyBackendForTesting(std::move(Observed)));
		SoftBodyWorld Ordinary;
		const auto HookedBody = Hooked.CreateBody(ClothDefinition());
		const auto OrdinaryBody = Ordinary.CreateBody(ClothDefinition());
		for (int Step = 0; Step < 3; ++Step) {
			(void)Hooked.Step({});
			(void)Ordinary.Step({});
		}
		Check(Visits == 3 && *Hooked.GetBodyState(HookedBody)->Positions == *Ordinary.GetBodyState(OrdinaryBody)->Positions,
			"the private observation seam preserves byte-exact ordinary solver output");
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
		const auto RuntimeInfo = WorkspaceValue->GetDeformableInspection(Body->GetObjectId());
		Check(RuntimeInfo && RuntimeInfo->VertexCount == 64 && RuntimeInfo->AttachmentCount == 2 &&
			WorkspaceValue->GetDeformableVertex(Body->GetObjectId(), 64) == std::nullopt &&
			WorkspaceValue->GetDeformableVertex(Body->GetObjectId(), 0).has_value(),
			"WorldRoot exposes one bounded vertex and read-only body counts");
		for (int Step = 0; Step < 8; ++Step)
			WorkspaceValue->StepPhysics(SoftBodyStepInterval, std::nullopt);
		const auto BeforePinMove = WorkspaceValue->GetDeformableVertex(Body->GetObjectId(), 36);
		LeftPin->SetPosition({-4.0f, 14.0f, 0.0f});
		WorkspaceValue->StepPhysics(0.0, std::nullopt);
		const auto AfterPinMove = WorkspaceValue->GetDeformableVertex(Body->GetObjectId(), 36);
		Check(BeforePinMove && AfterPinMove && Distance(*BeforePinMove, *AfterPinMove) < 0.0001f &&
			Near(WorkspaceValue->GetDeformableVertex(Body->GetObjectId(), 0)->y, 14.0f),
			"reflected attachment Position update moves its target without resetting cloth motion");

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
		TestMovingAttachmentAndBoundedInspection();
		TestAttachmentCommitFailureIsAtomic();
		TestMovingRigidColliderThroughWorldRoot();
		TestPinnedClothAndRigidCollision();
		TestRubberRecoveryAndQualityTier();
		TestJobifiedDeterminismAndLifecycle();
		TestAdmissionDoesNotProveCapturedWork();
		TestPrivateHookFailureAndUnhookedEquivalence();
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
