#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/JointConstraint.hpp"
#include "gargantuan/classes/WeldConstraint.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/datatypes/RaycastParams.hpp"
#include "gargantuan/physics/PhysicsBackend.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "../src/runtime/RuntimeWorkDiagnostics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <Luau/Compiler.h>
#include <lua.h>
#include <luacode.h>

namespace gargantuan {
	struct WorldRootTestAccess {
		static std::size_t BodyCount(const WorldRoot &World) { return World.PartBodies.size(); }
		static std::size_t PartCount(const WorldRoot &World) { return World.Parts.size(); }
		static std::size_t PartMembershipCount(const WorldRoot &World) { return World.PartMembership.size(); }
		static const std::vector<std::shared_ptr<BasePart>> &OrderedParts(const WorldRoot &World) { return World.Parts; }
		static std::size_t ConstraintCount(const WorldRoot &World) { return World.ConstraintJoints.size(); }
		static PhysicsBodyId Body(const WorldRoot &World, const BasePart &Part) {
			auto Found = World.PartBodies.find(Part.GetObjectId());
			return Found == World.PartBodies.end() ? PhysicsBodyId{} : Found->second;
		}
		static PhysicsConstraintId Constraint(const WorldRoot &World, const gargantuan::Constraint &Constraint) {
			auto Found = World.ConstraintJoints.find(Constraint.GetObjectId());
			return Found == World.ConstraintJoints.end() ? PhysicsConstraintId{} : Found->second;
		}
		static std::optional<PhysicsBodyState> State(const WorldRoot &World, PhysicsBodyId Body) {
			return World.Physics.GetBodyState(Body);
		}
		static bool IsBodyValid(const WorldRoot &World, PhysicsBodyId Body) {
			return World.Physics.IsBodyValid(Body);
		}
		static bool IsConstraintValid(const WorldRoot &World, PhysicsConstraintId Constraint) {
			return World.Physics.IsConstraintValid(Constraint);
		}
		static PhysicsKinematicMotionResult DetachedKinematicMotion(const PhysicsKinematicMotionRequest &Request) {
			WorldRoot Detached;
			return Detached.ResolveKinematicMotion(Request);
		}
	};
}

namespace {
	using namespace gargantuan;

	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "FAIL: " << Message << '\n';
		++Failures;
	}

	bool Near(float A, float B, float Epsilon = 0.001f) {
		return std::abs(A - B) <= Epsilon;
	}

	class RecordingBackend final : public gargantuan::IPhysicsBackend {
	  public:
		int CreateCalls = 0;
		int UpdateCalls = 0;
		int StepCalls = 0;
		bool Live = true;
		gargantuan::PhysicsBodyDesc LastDescription{};

		bool IsValid() const override { return Live; }
		bool IsBodyValid(gargantuan::PhysicsBodyId Body) const override { return Body == gargantuan::PhysicsBodyId{1, 1}; }
		bool IsConstraintValid(gargantuan::PhysicsConstraintId) const override { return false; }
		gargantuan::PhysicsBodyId CreateBody(const gargantuan::PhysicsBodyDesc &Description) override {
			++CreateCalls;
			LastDescription = Description;
			return {1, 1};
		}
		gargantuan::PhysicsOperationResult UpdateBody(
			gargantuan::PhysicsBodyId,
			const gargantuan::PhysicsBodyDesc &Description
		) override {
			++UpdateCalls;
			LastDescription = Description;
			return {};
		}
		gargantuan::PhysicsOperationResult DestroyBody(gargantuan::PhysicsBodyId) override { return {}; }
		gargantuan::PhysicsConstraintId CreateConstraint(const gargantuan::PhysicsConstraintDesc &) override { return {}; }
		gargantuan::PhysicsOperationResult DestroyConstraint(gargantuan::PhysicsConstraintId) override { return {}; }
		gargantuan::PhysicsOperationResult ApplyLinearImpulse(gargantuan::PhysicsBodyId, glm::vec3) override { return {}; }
		gargantuan::PhysicsOperationResult SetGravity(glm::vec3) override { return {}; }
		std::optional<gargantuan::PhysicsBodyState> GetBodyState(gargantuan::PhysicsBodyId) const override {
			return std::nullopt;
		}
		gargantuan::PhysicsKinematicMotionResult
		MoveKinematicCapsule(const gargantuan::PhysicsKinematicMotionRequest &Request) const override {
			return {
				.Position = Request.Position + Request.Translation,
				.AppliedTranslation = Request.Translation,
				.Velocity = Request.Velocity,
			};
		}
		gargantuan::PhysicsRaycastResult Raycast(const gargantuan::PhysicsRaycastRequest &) const override {
			return {};
		}
		gargantuan::PhysicsStepResult Step(const gargantuan::PhysicsStepConfig &) override {
			++StepCalls;
			return {};
		}
	};

	void TestNeutralContract() {
		using namespace gargantuan;
		auto Backend = std::make_unique<RecordingBackend>();
		auto *Recorder = Backend.get();
		PhysicsWorld World(std::move(Backend));
		PhysicsBodyDesc Description;
		Description.Anchored = true;
		const auto Body = World.CreateBody(Description);
		Check(Body == PhysicsBodyId{1, 1} && Recorder->CreateCalls == 1, "neutral world delegates body creation");
		Description.CanCollide = false;
		Check(World.UpdateBody(Body, Description).Succeeded() && Recorder->UpdateCalls == 1,
			"neutral world delegates semantic body updates");
		(void)World.Step({});
		Check(Recorder->StepCalls == 1, "neutral world delegates explicit stepping");
	}

	void TestBackendIdentityAndLifecycle() {
		using namespace gargantuan;
		PhysicsWorld World(PhysicsWorldConfig{.Gravity = {0.0f, 0.0f, 0.0f}});
		PhysicsBodyDesc Description;
		Description.Anchored = true;
		const auto First = World.CreateBody(Description);
		Check(First.IsValid() && World.IsBodyValid(First), "backend creates a valid Gargantuan body identity");
		Check(World.DestroyBody(First).Succeeded(), "backend destroys a live body");
		Check(!World.IsBodyValid(First), "destroyed physics identity is invalidated");
		const auto Second = World.CreateBody(Description);
		Check(Second.IsValid() && Second != First, "reused backend storage receives a new generation");
		Check(!World.UpdateBody(First, Description).Succeeded(), "stale body update fails closed");
		Check(!World.DestroyBody(First).Succeeded(), "duplicate or stale body destroy is structured and safe");

		auto Invalid = Description;
		Invalid.Shape.Size = {0.0f, 1.0f, 1.0f};
		Check(!World.CreateBody(Invalid).IsValid(), "invalid shape dimensions fail body creation");

		const auto Third = World.CreateBody(Description);
		const auto Joint = World.CreateConstraint({
			.Kind = PhysicsConstraintKind::Weld,
			.BodyA = Second,
			.BodyB = Third,
		});
		Check(Joint.IsValid() && World.IsConstraintValid(Joint), "constraint uses neutral body identities");
		Check(World.DestroyBody(Second).Succeeded() && !World.IsConstraintValid(Joint),
			"body destruction invalidates attached constraints safely");
	}

	void TestAdvancedConstraintBackend() {
		using namespace gargantuan;
		PhysicsWorld World(PhysicsWorldConfig{.Gravity = {0.0f, 0.0f, 0.0f}});
		PhysicsBodyDesc AnchorDesc;
		AnchorDesc.Anchored = true;
		AnchorDesc.CanCollide = false;
		PhysicsBodyDesc MovingDesc = AnchorDesc;
		MovingDesc.Anchored = false;
		const auto Anchor = World.CreateBody(AnchorDesc);
		const auto Door = World.CreateBody(MovingDesc);
		const auto Hinge = World.CreateConstraint({
			.Kind = PhysicsConstraintKind::Hinge, .BodyA = Anchor, .BodyB = Door,
			.CollideConnected = false, .EnableLimit = true, .LowerLimit = -0.4f, .UpperLimit = 0.4f,
			.EnableMotor = true, .MotorSpeed = 2.0f, .MaximumMotorForce = 1000.0f,
		});
		Check(Hinge.IsValid(), "hinge admits angular limits and motor torque");
		for (int Index = 0; Index < 90; ++Index) (void)World.Step({});
		const auto DoorState = World.GetBodyState(Door);
		const auto DoorAngle = DoorState ? std::atan2(DoorState->Transform.Rotation[0][1],
			DoorState->Transform.Rotation[0][0]) : 0.0f;
		Check(DoorState && std::abs(DoorAngle) > 0.05f && std::abs(DoorAngle) < 0.7f,
			"hinge motor rotates door and angular limits bound it");
		Check(World.DestroyConstraint(Hinge).Succeeded() && !World.IsConstraintValid(Hinge),
			"hinge destruction retires its generation-safe ID");

		const auto Slide = World.CreateBody(MovingDesc);
		const auto Slider = World.CreateConstraint({
			.Kind = PhysicsConstraintKind::Slider, .BodyA = Anchor, .BodyB = Slide,
			.CollideConnected = false, .EnableLimit = true, .LowerLimit = -1.0f, .UpperLimit = 1.0f,
			.EnableMotor = true, .MotorSpeed = 3.0f, .MaximumMotorForce = 1000.0f,
		});
		Check(Slider.IsValid(), "slider admits linear limits and motor force");
		for (int Index = 0; Index < 90; ++Index) (void)World.Step({});
		const auto SlideState = World.GetBodyState(Slide);
		Check(SlideState && SlideState->Transform.Position.x > 0.1f &&
			SlideState->Transform.Position.x < 1.3f && std::abs(SlideState->Transform.Position.y) < 0.2f,
			"slider motor moves along frame A x-axis and respects travel limit");

		MovingDesc.Transform = CFrame(0.0f, -3.0f, 0.0f);
		const auto Weight = World.CreateBody(MovingDesc);
		const auto Spring = World.CreateConstraint({
			.Kind = PhysicsConstraintKind::Distance, .BodyA = Anchor, .BodyB = Weight,
			.CollideConnected = false, .RestLength = 1.0f,
			.SpringFrequency = 8.0f, .SpringDamping = 0.8f,
		});
		Check(Spring.IsValid(), "distance spring admits finite rest length and tuning");
		for (int Index = 0; Index < 120; ++Index) (void)World.Step({});
		const auto WeightState = World.GetBodyState(Weight);
		Check(WeightState && std::abs(WeightState->Transform.Position.y + 1.0f) < 0.8f,
			"distance spring pulls separated assembly toward its rest length");

		const auto Socket = World.CreateConstraint({
			.Kind = PhysicsConstraintKind::BallSocket, .BodyA = Anchor, .BodyB = Weight,
			.CollideConnected = false, .LocalFrameB = CFrame(0.0f, 1.0f, 0.0f),
		});
		Check(Socket.IsValid(), "ball socket accepts local attachment frames");
		Check(!World.CreateConstraint({.Kind = PhysicsConstraintKind::Hinge, .BodyA = Anchor,
			.BodyB = Door, .EnableMotor = true, .MaximumMotorForce = 0.0f}).IsValid(),
			"non-driving hinge motor fails closed");
		Check(!World.CreateConstraint({.Kind = PhysicsConstraintKind::Distance, .BodyA = Anchor,
			.BodyB = Door, .EnableLimit = true, .LowerLimit = 2.0f, .UpperLimit = 1.0f}).IsValid(),
			"reversed distance limits fail closed");
		Check(!World.CreateConstraint({.Kind = static_cast<PhysicsConstraintKind>(255),
			.BodyA = Anchor, .BodyB = Door}).IsValid(), "unsupported joint kind fails closed");
		Check(World.DestroyBody(Weight).Succeeded() && !World.IsConstraintValid(Spring) &&
			!World.IsConstraintValid(Socket), "body removal invalidates every attached joint type");
	}

	void TestBoundedConstraintAdmission() {
		using namespace gargantuan;
		PhysicsWorld World(PhysicsWorldConfig{.Gravity = {}, .ConstraintLimit = 2});
		PhysicsBodyDesc Description;
		Description.Anchored = true;
		const auto BodyA = World.CreateBody(Description);
		const auto BodyB = World.CreateBody(Description);
		const PhysicsConstraintDesc JointDesc{.BodyA = BodyA, .BodyB = BodyB};
		const auto First = World.TryCreateConstraint(JointDesc);
		const auto Second = World.TryCreateConstraint(JointDesc);
		const auto Denied = World.TryCreateConstraint(JointDesc);
		Check(First.Succeeded() && Second.Succeeded() &&
			Denied.Operation.Status == PhysicsOperationStatus::ResourceLimit && !Denied.Constraint.IsValid() &&
			World.IsConstraintValid(First.Constraint) && World.IsConstraintValid(Second.Constraint),
			"bounded joint admission reports ResourceLimit without disturbing live joints");
		Check(World.DestroyConstraint(First.Constraint).Succeeded() &&
			!World.IsConstraintValid(First.Constraint), "freeing a joint retires its identity and capacity");
		const auto Recovered = World.TryCreateConstraint(JointDesc);
		Check(Recovered.Succeeded() && Recovered.Constraint != First.Constraint &&
			World.IsConstraintValid(Second.Constraint), "capacity recovery preserves surviving and new generations");
		Check(World.DestroyBody(BodyA).Succeeded() && !World.IsConstraintValid(Recovered.Constraint) &&
			!World.IsConstraintValid(Second.Constraint), "body teardown releases every attached joint admission");
		PhysicsWorld Disabled(PhysicsWorldConfig{.ConstraintLimit = 0});
		const auto DisabledA = Disabled.CreateBody(Description);
		const auto DisabledB = Disabled.CreateBody(Description);
		Check(Disabled.TryCreateConstraint({.BodyA = DisabledA, .BodyB = DisabledB}).Operation.Status ==
			PhysicsOperationStatus::ResourceLimit, "zero configured joint capacity fails closed");
	}

	void TestStaticTriangleMeshBackend() {
		using namespace gargantuan;
		PhysicsWorld World(PhysicsWorldConfig{.Gravity = {0.0f, 0.0f, 0.0f}});
		auto Positions = std::make_shared<const std::vector<glm::vec3>>(std::vector<glm::vec3>{
			{-1.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 1.0f},
			{1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, -1.0f},
		});
		auto Indices = std::make_shared<const std::vector<std::uint32_t>>(
			std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3});
		auto Mesh = std::make_shared<const PhysicsTriangleMesh>(PhysicsTriangleMesh{
			.ResourceId = "asset://collision-fixture",
			.ContentRevision = 1,
			.Positions = Positions,
			.Indices = Indices,
		});
		PhysicsBodyDesc Description;
		Description.Anchored = true;
		Description.Shape = {.Kind = PhysicsShapeKind::StaticTriangleMesh, .Size = {2.0f, 1.0f, 3.0f}, .Mesh = Mesh};
		Description.Transform = CFrame(0.0f, 4.0f, 0.0f);
		const auto First = World.CreateBody(Description);
		if (!First.IsValid()) {
			const auto Failure = World.GetCollisionResidency();
			std::cerr << "[Physics:MeshAdmission] " << Failure.LastAdmissionFailure
				<< "; resources=" << Failure.ActiveResources << "; consumers=" << Failure.ActiveConsumers
				<< "; failures=" << Failure.FailedAdmissions << "; rotation=";
			for (int Column = 0; Column < 3; ++Column)
				for (int Row = 0; Row < 3; ++Row)
					std::cerr << Description.Transform.Rotation[Column][Row] << ',';
			std::cerr << '\n';
		}
		Check(First.IsValid(), "validated static triangle mesh creates a backend shape");
		const auto Second = World.CreateBody(Description);
		Check(Second.IsValid(), "a second body shares immutable collision mesh data");
		auto Residency = World.GetCollisionResidency();
		Check(Residency.ActiveResources == 1 && Residency.ActiveConsumers == 2 &&
			Residency.CanonicalBytes == Positions->size() * sizeof(glm::vec3) +
				Indices->size() * sizeof(std::uint32_t) && Residency.NativeMeshBytes > 0,
			"collision residency accounts one native resource and two live consumers");
		auto Cast = World.Raycast({.Origin = {0.0f, 8.0f, 0.0f}, .Direction = {0.0f, -8.0f, 0.0f}});
		Check(Cast.Succeeded() && Cast.HasHit() && Near(Cast.Candidates.front().Position.y, 4.0f),
			"raycast sees the scaled translated static mesh");
		const auto Mover = World.MoveKinematicCapsule({
			.Position = {0.0f, 6.0f, 0.0f}, .Radius = 0.5f, .Height = 2.0f,
			.Translation = {0.0f, -3.0f, 0.0f}, .Velocity = {0.0f, -3.0f, 0.0f},
		});
		Check(Mover.Succeeded() && Mover.Collided && Mover.Position.y >= 4.9f,
			"kinematic capsule motion collides with the active static triangle mesh");
		Description.Transform = CFrame(0.0f, 2.0f, 0.0f);
		Check(World.UpdateBody(First, Description).Succeeded(), "mesh transform update retains body identity");
		Check(World.IsBodyValid(First), "mesh update leaves neutral identity live");
		auto Invalid = Description;
		Invalid.Shape.Mesh = std::make_shared<const PhysicsTriangleMesh>(PhysicsTriangleMesh{
			.ResourceId = "asset://invalid-collision-fixture", .ContentRevision = 1,
			.Positions = Positions,
			.Indices = std::make_shared<const std::vector<std::uint32_t>>(std::vector<std::uint32_t>{0, 1, 99}),
		});
		Check(!World.UpdateBody(First, Invalid).Succeeded(), "invalid replacement rejects without deleting old shape");
		Check(World.IsBodyValid(First) && World.GetBodyState(First)->Description.Shape.Kind ==
			PhysicsShapeKind::StaticTriangleMesh, "failed mesh replacement retains previous neutral body state");
		auto Thin = Description;
		Thin.Shape.Size = {0.01f, 1.0f, 0.01f};
		Thin.Shape.Mesh = std::make_shared<const PhysicsTriangleMesh>(PhysicsTriangleMesh{
			.ResourceId = "asset://thin-collision-fixture", .ContentRevision = 1,
			.Positions = std::make_shared<const std::vector<glm::vec3>>(
				std::vector<glm::vec3>{{0.0f, 0.0f, 0.0f}, {0.02f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.02f}}),
			.Indices = std::make_shared<const std::vector<std::uint32_t>>(std::vector<std::uint32_t>{0, 2, 1}),
		});
		Check(!World.UpdateBody(First, Thin).Succeeded() && World.IsBodyValid(First),
			"anisotropic scale that collapses triangle area rejects before native shape creation");
		auto Far = Description;
		Far.Transform = CFrame(100'001.0f, 2.0f, 0.0f);
		Check(!World.UpdateBody(First, Far).Succeeded() && World.IsBodyValid(First),
			"mesh outside the Box3D float coordinate bound preserves prior shape");
		for (int Case = 0; Case < 3; ++Case) {
			auto InvalidRotation = Description;
			auto Rotation = glm::mat3(1.0f);
			if (Case == 0) Rotation[0].y = 0.25f; // shear
			if (Case == 1) Rotation[0].x = -1.0f; // reflection
			if (Case == 2) Rotation[0].x = 1.25f; // non-unit basis
			InvalidRotation.Transform = CFrame({0.0f, 6.0f, 0.0f}, Rotation);
			Check(World.UpdateBody(First, InvalidRotation).Status == PhysicsOperationStatus::InvalidDescription &&
				World.GetBodyState(First)->Description.Transform.FuzzyEq(Description.Transform),
				"non-rigid mesh CFrame rejects before changing the old body transform");
			const auto Prior = World.Raycast({
				.Origin = {0.0f, 5.0f, 0.0f}, .Direction = {0.0f, -5.0f, 0.0f},
				.Filter = {.Type = PhysicsQueryFilterType::Include, .Bodies = {First}},
			});
			Check(Prior.Succeeded() && Prior.HasHit() && Prior.Candidates.front().Body == First &&
				Near(Prior.Candidates.front().Position.y, 2.0f),
				"non-rigid mesh update leaves previous collider queryable at its old pose");
		}
		Description.Anchored = false;
		Check(World.UpdateBody(First, Description).Status == PhysicsOperationStatus::InvalidDescription,
			"dynamic triangle mesh request fails closed at neutral backend boundary");
		Description.Anchored = true;
		Description.Shape.Size.x = 0.001f;
		Check(World.UpdateBody(First, Description).Status == PhysicsOperationStatus::InvalidDescription,
			"mesh scale below Box3D's 0.01 limit is rejected before native clamping");
		Check(World.DestroyBody(First).Succeeded() && World.DestroyBody(Second).Succeeded(),
			"both mesh shapes retire before their shared native resource");
		Residency = World.GetCollisionResidency();
		Check(Residency.ActiveResources == 0 && Residency.ActiveConsumers == 0 && Residency.NativeMeshBytes == 0 &&
			Residency.FailedAdmissions >= 1, "collision resource retirement and failed admission are accounted");
		Check(!World.IsBodyValid(First), "destroyed mesh body identity is stale");
	}

	void TestDenseMeshContactCandidateOverflow() {
		// Box3D's mesh BVH visits lower-Y leaves first. These 300 triangles
		// overlap the Ball's expanded query AABB but sit beyond contact range.
		// The real support is last in the higher-Y leaf, after the old 256-slot
		// candidate cache would stop traversal.
		struct Observation {
			bool RayHitSupport = false;
			int BeganContacts = 0;
		};
		auto RunFixture = [](bool Dense) {
			std::vector<glm::vec3> Positions;
			std::vector<std::uint32_t> Indices;
			auto AddTriangle = [&](float Y, float OffsetX, float OffsetZ) {
				const auto Base = static_cast<std::uint32_t>(Positions.size());
				Positions.insert(Positions.end(), {
					{-1.0f + OffsetX, Y, -1.0f + OffsetZ},
					{OffsetX, Y, 1.0f + OffsetZ},
					{1.0f + OffsetX, Y, -1.0f + OffsetZ},
				});
				Indices.insert(Indices.end(), {Base, Base + 1, Base + 2});
			};
			if (Dense) for (int Index = 0; Index < 300; ++Index)
				AddTriangle(-0.05f, static_cast<float>(Index % 20) * 0.0001f,
					static_cast<float>(Index / 20) * 0.0001f);
			AddTriangle(0.0f, 0.0f, 0.0f);
			auto Mesh = std::make_shared<const PhysicsTriangleMesh>(PhysicsTriangleMesh{
				.ResourceId = Dense ? "asset://dense-contact-overflow" : "asset://sparse-contact-reference",
				.ContentRevision = 1,
				.Positions = std::make_shared<const std::vector<glm::vec3>>(std::move(Positions)),
				.Indices = std::make_shared<const std::vector<std::uint32_t>>(std::move(Indices)),
			});
			PhysicsWorld World(PhysicsWorldConfig{.Gravity = {0.0f, 0.0f, 0.0f}});
			PhysicsBodyDesc Floor;
			Floor.Anchored = true;
			Floor.Shape = {.Kind = PhysicsShapeKind::StaticTriangleMesh, .Size = {1.0f, 1.0f, 1.0f}, .Mesh = Mesh};
			const auto FloorId = World.CreateBody(Floor);
			Check(FloorId.IsValid(), "dense-contact fixture admits its static mesh");
			if (!FloorId.IsValid()) return Observation{};
			const auto Ray = World.Raycast({
				.Origin = {0.0f, 1.0f, 0.0f}, .Direction = {0.0f, -2.0f, 0.0f},
				.Filter = {.Type = PhysicsQueryFilterType::Include, .Bodies = {FloorId}},
			});
			Observation Result;
			Result.RayHitSupport = Ray.Succeeded() && Ray.HasHit() &&
				Ray.Candidates.front().Body == FloorId && Near(Ray.Candidates.front().Position.y, 0.0f);
			PhysicsBodyDesc Ball;
			Ball.Shape = {.Kind = PhysicsShapeKind::Ball, .Size = {1.0f, 1.0f, 1.0f}};
			Ball.Transform = CFrame(0.0f, 0.49f, 0.0f);
			const auto BallId = World.CreateBody(Ball);
			Check(BallId.IsValid(), "dense-contact fixture admits its overlapping dynamic Ball");
			if (!BallId.IsValid()) return Result;
			for (int Step = 0; Step < 8; ++Step) {
				for (const auto &Contact : World.Step({}).Contacts)
					if (Contact.Phase == PhysicsContactPhase::Began &&
						((Contact.BodyA == FloorId && Contact.BodyB == BallId) ||
						 (Contact.BodyA == BallId && Contact.BodyB == FloorId)))
						++Result.BeganContacts;
			}
			return Result;
		};
		const auto Sparse = RunFixture(false);
		const auto Dense = RunFixture(true);
		std::cout << "[Physics:DenseContactRegression] decoys=300 cache=256 sparse_ray=" << Sparse.RayHitSupport
			<< " dense_ray=" << Dense.RayHitSupport << " sparse_began=" << Sparse.BeganContacts
			<< " dense_began=" << Dense.BeganContacts << '\n';
		Check(Sparse.RayHitSupport && Dense.RayHitSupport,
			"sparse and dense meshes retain the support triangle for filtered raycasts");
		Check(Sparse.BeganContacts >= 1, "sparse support produces a dynamic Ball contact");
		Check(Dense.BeganContacts >= 1, "dense support remains contactable after 300 irrelevant AABB candidates");
	}

	void TestDetachedWorldRootKinematicMotion() {
		const auto Result = WorldRootTestAccess::DetachedKinematicMotion({
			.Position = {0.0f, 3.0f, 0.0f}, .Radius = 0.5f, .Height = 2.0f,
			.Translation = {0.0f, -1.0f, 0.0f}, .Velocity = {0.0f, -1.0f, 0.0f},
		});
		Check(Result.Succeeded() && Near(Result.Position.y, 2.0f) && !Result.Collided,
			"detached stack WorldRoot resolves kinematic motion without a shared owner");
	}

	void TestChunkedMeshShapeLifecycle() {
		auto Positions = std::make_shared<std::vector<glm::vec3>>();
		auto Indices = std::make_shared<std::vector<std::uint32_t>>();
		for (int Z = 0; Z <= 8; ++Z)
			for (int X = 0; X <= 16; ++X)
				Positions->push_back({-1.0f + static_cast<float>(X) / 8.0f, 0.0f,
					-1.0f + static_cast<float>(Z) / 4.0f});
		for (int Z = 0; Z < 8; ++Z)
			for (int X = 0; X < 16; ++X) {
				const auto A = static_cast<std::uint32_t>(Z * 17 + X);
				const auto B = A + 17;
				Indices->insert(Indices->end(), {A, B, B + 1, A, B + 1, A + 1});
			}
		Check(Indices->size() / 3 == 256, "chunk lifecycle fixture crosses the 255-triangle boundary");
		auto MeshAt = [&](std::uint64_t Revision) {
			return std::make_shared<const PhysicsTriangleMesh>(PhysicsTriangleMesh{
				.ResourceId = "asset://chunk-lifecycle", .ContentRevision = Revision,
				.Positions = Positions, .Indices = Indices,
			});
		};
		auto DescriptionAt = [&](std::uint64_t Revision) {
			PhysicsBodyDesc Description;
			Description.Anchored = true;
			Description.Shape = {.Kind = PhysicsShapeKind::StaticTriangleMesh,
				.Size = {1.0f, 1.0f, 1.0f}, .Mesh = MeshAt(Revision)};
			return Description;
		};
		const PhysicsRaycastRequest Ray{
			.Origin = {0.04f, 1.0f, 0.04f}, .Direction = {0.0f, -2.0f, 0.0f},
		};
		{
			PhysicsWorld World(PhysicsWorldConfig{.Gravity = {0.0f, 0.0f, 0.0f}});
			const auto First = World.CreateBody(DescriptionAt(1));
			const auto Second = World.CreateBody(DescriptionAt(1));
			Check(First.IsValid() && Second.IsValid(), "two chunked mesh consumers create successfully");
			if (!First.IsValid() || !Second.IsValid()) return;
			const auto Live = World.GetCollisionResidency();
			Check(Live.ActiveResources == 1 && Live.ActiveConsumers == 2 && Live.ActiveMeshShapes == 4 &&
				Live.NativeChunks == 2 && Live.ChunkTriangleLimit == 255 && Live.NativeMeshBytes > 0 &&
				Live.PeakBuildNativeBytes >= Live.NativeMeshBytes,
				"one resource shares two native chunks across four live shapes");
			Check(World.Raycast(Ray).HasHit(), "chunked mesh remains queryable");
			PhysicsQueryShape Query;
			Query.Transform=CFrame(glm::vec3(0,.1f,0)); Query.Radius=.5f;
			const auto Overlap=World.Overlap({Query,{}});
			Check(Overlap.Succeeded() && Overlap.Bodies==std::vector<PhysicsBodyId>{First,Second},"chunked mesh overlap deduplicates native chunks into two stable body owners");
			Query.Transform=CFrame(glm::vec3(0,2,0));
			const auto Cast=World.ShapeCast({Query,{0,-4,0},{}});
			Check(Cast.Succeeded() && Cast.Candidates.size()==2 && Cast.Candidates.front().Distance>1,"sphere cast against chunked static meshes returns owned finite body hits");
			Check(World.DestroyBody(First).Succeeded() && World.DestroyBody(Second).Succeeded(),
				"both chunked mesh consumers retire");
			const auto Retired = World.GetCollisionResidency();
			Check(Retired.ActiveResources == 0 && Retired.ActiveConsumers == 0 &&
				Retired.ActiveMeshShapes == 0 && Retired.NativeChunks == 0 && Retired.NativeMeshBytes == 0 &&
				Retired.CreatedMeshShapes == 4 && Retired.RetiredMeshShapes == 4,
				"last consumer retires both native chunks and all four shapes");
		}
		{
			PhysicsWorld World(PhysicsWorldConfig{.Gravity = {0.0f, 0.0f, 0.0f},
				.CollisionMeshShapeLimit = 2});
			const auto First = World.CreateBody(DescriptionAt(1));
			Check(First.IsValid(), "two-shape limit admits one chunked mesh consumer");
			if (!First.IsValid()) return;
			Check(!World.CreateBody(DescriptionAt(1)).IsValid(),
				"two-shape limit denies a second shared chunked mesh consumer");
			Check(!World.UpdateBody(First, DescriptionAt(2)).Succeeded(),
				"replacement needs shape headroom while old chunks remain live");
			const auto State = World.GetBodyState(First);
			const auto Denied = World.GetCollisionResidency();
			Check(State && State->Description.Shape.Mesh && State->Description.Shape.Mesh->ContentRevision == 1 &&
				Denied.ActiveResources == 1 && Denied.ActiveConsumers == 1 && Denied.ActiveMeshShapes == 2 &&
				Denied.FailedAdmissions >= 2 && World.Raycast(Ray).HasHit(),
				"shape-budget denial preserves the prior revision and queryable body");
			Check(World.DestroyBody(First).Succeeded(), "old chunked consumer frees shape headroom");
			const auto Replacement = World.CreateBody(DescriptionAt(2));
			Check(Replacement.IsValid() && World.Raycast(Ray).HasHit(),
				"shape budget recovers for a new revision after retirement");
			if (Replacement.IsValid()) Check(World.DestroyBody(Replacement).Succeeded(),
				"recovered chunked consumer retires");
			const auto Final = World.GetCollisionResidency();
			Check(Final.ActiveResources == 0 && Final.ActiveConsumers == 0 && Final.ActiveMeshShapes == 0 &&
				Final.CreatedMeshShapes == Final.RetiredMeshShapes,
				"denied and recovered chunked shapes leave balanced accounting");
		}
		PhysicsWorld Clamped(PhysicsWorldConfig{
			.CollisionMeshShapeLimit = MaximumPhysicsCollisionMeshShapes + 1,
		});
		Check(Clamped.GetCollisionResidency().MeshShapeLimit == MaximumPhysicsCollisionMeshShapes,
			"world configuration cannot raise the hard native mesh-shape ceiling");
	}

	void TestChunkSeamContactEvents() {
		std::vector<glm::vec3> Positions;
		std::vector<std::uint32_t> Indices;
		auto AddTriangle = [&](glm::vec3 A, glm::vec3 B, glm::vec3 C) {
			const auto Base = static_cast<std::uint32_t>(Positions.size());
			Positions.insert(Positions.end(), {A, B, C});
			Indices.insert(Indices.end(), {Base, Base + 1, Base + 2});
		};
		// Global Box3D DFS orders these distant negative-X filler leaves first.
		// The two touching support triangles straddle the 255/256 chunk boundary.
		for (int Index = 0; Index < 254; ++Index) {
			const float X = -20.0f + static_cast<float>(Index) * 0.00001f;
			AddTriangle({X, 0.0f, -0.5f}, {X, 0.0f, 0.5f}, {X + 1.0f, 0.0f, 0.0f});
		}
		const auto SupportBase = static_cast<std::uint32_t>(Positions.size());
		Positions.insert(Positions.end(), {
			{-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f},
			{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f},
		});
		Indices.insert(Indices.end(), {
			SupportBase, SupportBase + 2, SupportBase + 1,
			SupportBase + 1, SupportBase + 2, SupportBase + 3,
		});
		auto Mesh = std::make_shared<const PhysicsTriangleMesh>(PhysicsTriangleMesh{
			.ResourceId = "asset://chunk-contact-seam", .ContentRevision = 1,
			.Positions = std::make_shared<const std::vector<glm::vec3>>(std::move(Positions)),
			.Indices = std::make_shared<const std::vector<std::uint32_t>>(std::move(Indices)),
		});
		PhysicsWorld World(PhysicsWorldConfig{.Gravity = {0.0f, 0.0f, 0.0f}});
		PhysicsBodyDesc Floor;
		Floor.Anchored = true;
		Floor.Shape = {.Kind = PhysicsShapeKind::StaticTriangleMesh, .Size = {1.0f, 1.0f, 1.0f}, .Mesh = Mesh};
		const auto FloorId = World.CreateBody(Floor);
		Check(FloorId.IsValid() && World.GetCollisionResidency().NativeChunks == 2,
			"seam fixture builds two native mesh chunks");
		if (!FloorId.IsValid()) return;
		for (const float X : {-0.3f, 0.3f}) {
			const auto Ray = World.Raycast({
				.Origin = {X, 1.0f, 0.0f}, .Direction = {0.0f, -2.0f, 0.0f},
				.Filter = {.Type = PhysicsQueryFilterType::Include, .Bodies = {FloorId}},
			});
			Check(Ray.Succeeded() && Ray.HasHit() && Near(Ray.Candidates.front().Position.y, 0.0f),
				"both sides of the native chunk seam remain ray-queryable");
		}
		PhysicsBodyDesc Ball;
		Ball.Shape = {.Kind = PhysicsShapeKind::Ball, .Size = {0.5f, 0.5f, 0.5f}};
		Ball.Transform = CFrame(-0.3f, 0.24f, 0.0f);
		const auto BallId = World.CreateBody(Ball);
		Check(BallId.IsValid(), "seam fixture creates a moving dynamic Ball");
		if (!BallId.IsValid()) return;
		auto CountEvents = [&](float X) {
			Ball.Transform = CFrame(X, 0.24f, 0.0f);
			Check(World.UpdateBody(BallId, Ball).Succeeded(), "seam fixture moves the Ball");
			std::array<int, 2> Counts{};
			for (int Step = 0; Step < 2; ++Step)
				for (const auto &Contact : World.Step({}).Contacts) {
					if (!((Contact.BodyA == FloorId && Contact.BodyB == BallId) ||
						(Contact.BodyA == BallId && Contact.BodyB == FloorId))) continue;
					++Counts[Contact.Phase == PhysicsContactPhase::Began ? 0 : 1];
				}
			return Counts;
		};
		const auto Left = CountEvents(-0.3f);
		const auto Across = CountEvents(0.0f);
		const auto Right = CountEvents(0.3f);
		const auto Away = CountEvents(2.0f);
		std::cout << "[Physics:ChunkSeam] left=" << Left[0] << '/' << Left[1]
			<< " across=" << Across[0] << '/' << Across[1]
			<< " right=" << Right[0] << '/' << Right[1]
			<< " away=" << Away[0] << '/' << Away[1] << '\n';
		Check(Left == std::array{1, 0} && Across == std::array{0, 0} &&
			Right == std::array{0, 0} && Away == std::array{0, 1},
			"neutral contact begins once, persists across a native chunk seam, and ends once after departure");
	}

	void TestMeshCollisionResidencyLimits() {
		using namespace gargantuan;
		PhysicsWorld Clamped(PhysicsWorldConfig{
			.CollisionResourceLimit = MaximumPhysicsCollisionResources + 1,
			.CollisionConsumerLimit = MaximumPhysicsCollisionConsumers + 1,
			.CollisionCanonicalByteLimit = MaximumPhysicsCollisionCanonicalBytes + 1,
			.CollisionNativeByteLimit = MaximumPhysicsCollisionNativeBytes + 1,
		});
		const auto ClampedStats = Clamped.GetCollisionResidency();
		Check(ClampedStats.ResourceLimit == MaximumPhysicsCollisionResources &&
			ClampedStats.ConsumerLimit == MaximumPhysicsCollisionConsumers &&
			ClampedStats.CanonicalByteLimit == MaximumPhysicsCollisionCanonicalBytes &&
			ClampedStats.NativeByteLimit == MaximumPhysicsCollisionNativeBytes,
			"world configuration cannot raise the hard collision residency ceilings");
		const auto Positions = std::make_shared<const std::vector<glm::vec3>>(std::vector<glm::vec3>{
			{-1.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f},
		});
		const auto Indices = std::make_shared<const std::vector<std::uint32_t>>(std::vector<std::uint32_t>{0, 1, 2});
		const auto CanonicalBytes = Positions->size() * sizeof(glm::vec3) + Indices->size() * sizeof(std::uint32_t);
		auto MakeDescription = [&](std::uint64_t Revision) {
			PhysicsBodyDesc Value;
			Value.Anchored = true;
			Value.Shape.Kind = PhysicsShapeKind::StaticTriangleMesh;
			Value.Shape.Mesh = std::make_shared<const PhysicsTriangleMesh>(PhysicsTriangleMesh{
				.ResourceId = "asset://residency-fixture", .ContentRevision = Revision,
				.Positions = Positions, .Indices = Indices,
			});
			return Value;
		};
		PhysicsWorld IdentifierWorld(PhysicsWorldConfig{});
		auto LongIdentifier = MakeDescription(1);
		LongIdentifier.Shape.Mesh = std::make_shared<const PhysicsTriangleMesh>(PhysicsTriangleMesh{
			.ResourceId = std::string(MaximumPhysicsCollisionResourceIdBytes + 1, 'x'),
			.ContentRevision = 1, .Positions = Positions, .Indices = Indices,
		});
		Check(!IdentifierWorld.CreateBody(LongIdentifier).IsValid() &&
			IdentifierWorld.GetCollisionResidency().FailedAdmissions == 1,
			"oversized collision resource identity fails closed before native admission");
		PhysicsWorld World(PhysicsWorldConfig{
			.CollisionResourceLimit = 1, .CollisionConsumerLimit = 2,
			.CollisionCanonicalByteLimit = CanonicalBytes * 2,
		});
		const auto First = World.CreateBody(MakeDescription(1));
		const auto Second = World.CreateBody(MakeDescription(1));
		Check(First.IsValid() && Second.IsValid(), "shared mesh consumers fit a one-resource limit");
		auto Stats = World.GetCollisionResidency();
		Check(Stats.ActiveResources == 1 && Stats.ActiveConsumers == 2 && Stats.CanonicalBytes == CanonicalBytes &&
			Stats.CreatedResources == 1 && Stats.CreatedConsumers == 2,
			"one immutable mesh is charged once while each shape is charged as a consumer");
		Check(!World.CreateBody(MakeDescription(1)).IsValid(), "consumer limit rejects a third shared shape");
		Check(World.DestroyBody(Second).Succeeded(), "one shared consumer can retire independently");
		Check(!World.UpdateBody(First, MakeDescription(2)).Succeeded() && World.IsBodyValid(First),
			"resource limit rejects replacement while the prior revision remains live");
		const auto State = World.GetBodyState(First);
		Check(State && State->Description.Shape.Mesh->ContentRevision == 1,
			"denied revision replacement retains the old coherent description");
		const auto Ray = World.Raycast({.Origin = {-0.5f, 2.0f, 0.0f}, .Direction = {0.0f, -4.0f, 0.0f}});
		Check(Ray.Succeeded() && Ray.HasHit() && Ray.Candidates.front().Body == First,
			"denied replacement leaves the prior collider queryable");
		Check(World.DestroyBody(First).Succeeded(), "last old-revision consumer retires");
		Stats = World.GetCollisionResidency();
		Check(Stats.ActiveResources == 0 && Stats.ActiveConsumers == 0 && Stats.CanonicalBytes == 0 &&
			Stats.NativeMeshBytes == 0 && Stats.RetiredResources == 1 && Stats.RetiredConsumers == 2,
			"last shape retirement releases all native and canonical resource budget");
		const auto Recovered = World.CreateBody(MakeDescription(2));
		Check(Recovered.IsValid() && Recovered != First && !World.IsBodyValid(First),
			"retired budget admits a new revision with a fresh generation");
		Check(World.DestroyBody(Recovered).Succeeded(), "recovered resource retires");
		Stats = World.GetCollisionResidency();
		Check(Stats.CreatedResources == 2 && Stats.RetiredResources == 2 &&
			Stats.CreatedConsumers == 3 && Stats.RetiredConsumers == 3 && Stats.FailedAdmissions >= 2,
			"admission, creation, and retirement counters reconcile after recovery");

		PhysicsWorld CanonicalWorld(PhysicsWorldConfig{
			.CollisionResourceLimit = 2, .CollisionConsumerLimit = 2,
			.CollisionCanonicalByteLimit = CanonicalBytes,
		});
		const auto CanonicalBody = CanonicalWorld.CreateBody(MakeDescription(1));
		Check(CanonicalBody.IsValid() && !CanonicalWorld.UpdateBody(CanonicalBody, MakeDescription(2)).Succeeded(),
			"canonical-byte ceiling counts old and candidate revisions together");
		Check(CanonicalWorld.GetCollisionResidency().ActiveResources == 1,
			"canonical-byte denial does not retire prior resource");
		Check(CanonicalWorld.DestroyBody(CanonicalBody).Succeeded(), "canonical-budget fixture retires");

		PhysicsWorld ConsumerWorld(PhysicsWorldConfig{.CollisionConsumerLimit = 1});
		const auto ConsumerBody = ConsumerWorld.CreateBody(MakeDescription(1));
		auto Resized = MakeDescription(1);
		Resized.Shape.Size.x = 2.0f;
		Check(ConsumerBody.IsValid() && !ConsumerWorld.UpdateBody(ConsumerBody, Resized).Succeeded() &&
			ConsumerWorld.GetCollisionResidency().ActiveConsumers == 1 &&
			ConsumerWorld.GetCollisionResidency().LastAdmissionFailure == "Mesh collision consumer limit reached",
			"consumer ceiling includes both old and candidate shapes during replacement");
		Check(ConsumerWorld.DestroyBody(ConsumerBody).Succeeded(), "consumer-budget fixture retires");

		PhysicsWorld MeasureWorld(PhysicsWorldConfig{});
		const auto MeasureBody = MeasureWorld.CreateBody(MakeDescription(1));
		const auto NativeBytes = MeasureWorld.GetCollisionResidency().NativeMeshBytes;
		Check(MeasureBody.IsValid() && NativeBytes > 1, "native-byte fixture measures exact Box3D mesh size");
		Check(MeasureWorld.DestroyBody(MeasureBody).Succeeded(), "native-byte measurement retires");
		PhysicsWorld NativeAggregateWorld(PhysicsWorldConfig{
			.CollisionResourceLimit = 2, .CollisionConsumerLimit = 2,
			.CollisionNativeByteLimit = NativeBytes,
		});
		const auto NativeAggregateBody = NativeAggregateWorld.CreateBody(MakeDescription(1));
		Check(NativeAggregateBody.IsValid() &&
			!NativeAggregateWorld.UpdateBody(NativeAggregateBody, MakeDescription(2)).Succeeded() &&
			NativeAggregateWorld.GetCollisionResidency().NativeMeshBytes == NativeBytes,
			"native-byte ceiling counts old and candidate revision data together");
		Check(NativeAggregateWorld.DestroyBody(NativeAggregateBody).Succeeded(),
			"native aggregate-budget fixture retires");

		PhysicsWorld NativeWorld(PhysicsWorldConfig{.CollisionNativeByteLimit = 1});
		Check(!NativeWorld.CreateBody(MakeDescription(1)).IsValid(),
			"native-byte ceiling rejects a mesh after bounded candidate construction");
		Stats = NativeWorld.GetCollisionResidency();
		Check(Stats.ActiveResources == 0 && Stats.CreatedResources == 0 && Stats.RetiredResources == 0 &&
			Stats.FailedAdmissions == 1 &&
			Stats.LastAdmissionFailure == "Mesh collision native-byte limit reached",
			"native admission failure publishes no resource ownership and identifies the ceiling");
	}

	void TestKinematicWorldQueries() {
		using namespace gargantuan;
		PhysicsWorld World(PhysicsWorldConfig{.Gravity = {0.0f, 0.0f, 0.0f}});
		PhysicsBodyDesc Floor;
		Floor.Anchored = true;
		Floor.Transform = CFrame(0.0f, -1.0f, 0.0f);
		Floor.Shape.Size = {40.0f, 2.0f, 40.0f};
		Check(World.CreateBody(Floor).IsValid(), "kinematic query fixture creates real floor geometry");

		auto Ground = World.MoveKinematicCapsule({
			.Position = {0.0f, 2.25f, 0.0f},
			.Radius = 1.0f,
			.Height = 4.0f,
			.Translation = {0.0f, -1.0f, 0.0f},
			.Velocity = {0.0f, -10.0f, 0.0f},
		});
		Check(
			Ground.Succeeded() && Ground.Collided && Ground.HasFloor,
			"capsule grounding comes from world collision instead of a constant height"
		);
		Check(
			Ground.Position.y >= 1.99f && Ground.FloorNormal.y > 0.9f && Ground.Velocity.y >= -0.001f,
			"ground query returns bounded position, floor normal, and clipped velocity"
		);
		auto GroundSlide = World.MoveKinematicCapsule({
			.Position = {0.0f, 2.0f, 0.0f},
			.Radius = 1.0f,
			.Height = 4.0f,
			.Translation = {2.0f, -0.25f, 0.0f},
			.Velocity = {8.0f, -1.0f, 0.0f},
		});
		Check(
			GroundSlide.Succeeded() && GroundSlide.HasFloor && GroundSlide.Position.x > 1.9f,
			"bounded mover iterations preserve tangential motion while grounding clips gravity"
		);

		PhysicsBodyDesc Wall;
		Wall.Anchored = true;
		Wall.Transform = CFrame(4.0f, 2.0f, 0.0f);
		Wall.Shape.Size = {2.0f, 4.0f, 8.0f};
		Check(World.CreateBody(Wall).IsValid(), "kinematic query fixture creates obstacle geometry");
		auto Blocked = World.MoveKinematicCapsule({
			.Position = {0.0f, 2.01f, 0.0f},
			.Radius = 1.0f,
			.Height = 4.0f,
			.Translation = {8.0f, 0.0f, 0.0f},
			.Velocity = {16.0f, 0.0f, 0.0f},
		});
		Check(
			Blocked.Succeeded() && Blocked.Collided && Blocked.Position.x < 3.0f,
			"horizontal capsule motion collides with world obstacles"
		);
		Check(
			Blocked.AppliedTranslation.x < 8.0f && Blocked.Velocity.x < 0.01f,
			"obstacle result bounds translation and removes velocity into the contact plane"
		);

		auto Invalid = World.MoveKinematicCapsule({.Radius = 0.0f, .Height = 4.0f});
		Check(
			!Invalid.Succeeded() && Invalid.Status == PhysicsOperationStatus::InvalidDescription,
			"invalid capsule descriptions fail closed"
		);
	}

	std::shared_ptr<gargantuan::Workspace> MakeWorkspace() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		return std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
	}

	void TestCommittedBodyUpdates() {
		using namespace gargantuan;
		auto Workspace = MakeWorkspace();
		Check(Workspace != nullptr, "physics update fixture obtains Workspace");
		if (!Workspace) return;
		auto Part = std::make_shared<gargantuan::Part>();
		Part->SetAnchored(true);
		Part->SetParent(Workspace);
		const auto Body = WorldRootTestAccess::Body(*Workspace, *Part);
		Check(Body.IsValid(), "parented Part receives neutral body identity");

		Part->SetCFrame(CFrame(3.0f, 4.0f, 5.0f));
		Part->SetSize({2.0f, 4.0f, 6.0f});
		Part->SetAnchored(false);
		Part->SetCanCollide(false);
		Part->SetCanTouch(false);
		Part->SetShape(Enums::PartType::Ball);
		Workspace->StepPhysics(0.0, std::nullopt);
		auto State = WorldRootTestAccess::State(*Workspace, Body);
		Check(State.has_value(), "updated body remains live");
		if (State) {
			Check(State->Description.Transform.FuzzyEq(Part->GetCFrame()), "committed CFrame reaches backend at safe point");
			Check(Near(State->Description.Shape.Size.x, 2.0f) && Near(State->Description.Shape.Size.y, 4.0f) &&
				Near(State->Description.Shape.Size.z, 6.0f), "committed Size reaches backend shape state");
			Check(State->Description.Shape.Kind == PhysicsShapeKind::Ball, "Part shape is expressed with neutral semantics");
			Check(!State->Description.Anchored, "committed Anchored reaches backend motion state");
			Check(!State->Description.CanCollide, "committed CanCollide reaches backend collision state");
			Check(!State->Description.CanTouch, "committed CanTouch reaches backend event state");
		}
		Check(WorldRootTestAccess::Body(*Workspace, *Part) == Body,
			"private shape reconstruction preserves Gargantuan body identity");

		for (int Index = 0; Index < 8; ++Index) {
			Part->SetSize({2.0f + Index, 4.0f, 6.0f});
			Part->SetCanCollide(Index % 2 == 0);
			Workspace->StepPhysics(0.0, std::nullopt);
		}
		Check(WorldRootTestAccess::Body(*Workspace, *Part) == Body,
			"repeated geometry and collision edits preserve body identity");
		Part->SetAnchored(true);
		Part->SetCanCollide(true);
		Part->SetCanTouch(true);
		Workspace->StepPhysics(0.0, std::nullopt);
		State = WorldRootTestAccess::State(*Workspace, Body);
		Check(State && State->Description.Anchored && State->Description.CanCollide && State->Description.CanTouch,
			"repeated toggles restore anchored collision and touch semantics");
	}

	void TestPartAdmissionMembership() {
		auto Workspace = MakeWorkspace();
		Check(Workspace != nullptr, "part membership fixture obtains Workspace");
		if (!Workspace) return;
		auto First = std::make_shared<Part>();
		auto Second = std::make_shared<Part>();
		First->SetParent(Workspace);
		Second->SetParent(Workspace);
		Check(WorldRootTestAccess::PartCount(*Workspace) == 2 &&
			WorldRootTestAccess::PartMembershipCount(*Workspace) == 2 &&
			WorldRootTestAccess::OrderedParts(*Workspace) == std::vector<std::shared_ptr<BasePart>>{First, Second},
			"successful admissions preserve unique ordered Part traversal");
		First->SetSize({2.0f, 2.0f, 2.0f});
		Workspace->StepPhysics(0.0, std::nullopt);
		Check(WorldRootTestAccess::PartCount(*Workspace) == 2 &&
			WorldRootTestAccess::PartMembershipCount(*Workspace) == 2,
			"safe-point updates do not duplicate Part admission");
		First->SetParent(nullptr);
		Check(WorldRootTestAccess::PartCount(*Workspace) == 1 &&
			WorldRootTestAccess::PartMembershipCount(*Workspace) == 1 &&
			WorldRootTestAccess::OrderedParts(*Workspace).front() == Second,
			"removal retires ordered and indexed Part membership");
		First->SetParent(Workspace);
		Check(WorldRootTestAccess::PartCount(*Workspace) == 2 &&
			WorldRootTestAccess::PartMembershipCount(*Workspace) == 2 &&
			WorldRootTestAccess::OrderedParts(*Workspace) == std::vector<std::shared_ptr<BasePart>>{Second, First},
			"readmission appends once in established traversal order");
		Second->Destroy();
		Check(WorldRootTestAccess::PartCount(*Workspace) == 1 &&
			WorldRootTestAccess::PartMembershipCount(*Workspace) == 1,
			"destruction retires Part membership before slot reuse");
	}

	void TestMeshPartCollisionFallback() {
		using namespace gargantuan;
		auto Workspace = MakeWorkspace();
		if (!Workspace) return;
		auto Part = std::make_shared<MeshPart>();
		Part->SetCollisionMode(Enums::MeshCollisionMode::StaticMesh);
		Part->SetAnchored(false);
		Part->SetParent(Workspace);
		const auto Body = WorldRootTestAccess::Body(*Workspace, *Part);
		auto State = WorldRootTestAccess::State(*Workspace, Body);
		Check(State && State->Description.Shape.Kind == PhysicsShapeKind::Box,
			"unanchored StaticMesh request explicitly projects inherited Box collision");
		Part->SetAnchored(true);
		Workspace->StepPhysics(0.0, std::nullopt);
		State = WorldRootTestAccess::State(*Workspace, Body);
		Check(State && State->Description.Shape.Kind == PhysicsShapeKind::Box && !State->Description.Anchored &&
			WorldRootTestAccess::Body(*Workspace, *Part) == Body,
			"unavailable collision asset retains the previous complete Box body state and identity");
		const auto PriorHit = Workspace->ResolveRaycast({-2.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 0.0f});
		Check(PriorHit.Succeeded() && PriorHit.Instance == Part,
			"failed StaticMesh update preserves previous queryable collision");
		Part->SetCollisionMode(Enums::MeshCollisionMode::Box);
		Workspace->StepPhysics(0.0, std::nullopt);
		State = WorldRootTestAccess::State(*Workspace, Body);
		Check(WorldRootTestAccess::Body(*Workspace, *Part) == Body && State && State->Description.Anchored,
			"collision mode changes project at the normal safe point");
		auto Missing = std::make_shared<MeshPart>();
		Missing->SetCollisionMode(Enums::MeshCollisionMode::StaticMesh);
		Missing->SetAnchored(true);
		Missing->SetParent(Workspace);
		Check(!WorldRootTestAccess::Body(*Workspace, *Missing).IsValid(),
			"a new anchored StaticMesh body waits for valid collision content");
		Missing->SetCollisionMode(Enums::MeshCollisionMode::Box);
		Workspace->StepPhysics(0.0, std::nullopt);
		Check(WorldRootTestAccess::Body(*Workspace, *Missing).IsValid(),
			"a previously unavailable MeshPart can recover through a later safe point");
	}

	void TestConstraintAndPendingDestroy() {
		using namespace gargantuan;
		auto Workspace = MakeWorkspace();
		if (!Workspace) return;
		auto PartA = std::make_shared<Part>();
		auto PartB = std::make_shared<Part>();
		PartA->SetAnchored(true);
		PartB->SetAnchored(true);
		PartA->SetParent(Workspace);
		PartB->SetParent(Workspace);
		auto Weld = std::make_shared<WeldConstraint>();
		Weld->SetPart0(PartA);
		Weld->SetPart1(PartB);
		Weld->SetParent(Workspace);
		const auto BodyA = WorldRootTestAccess::Body(*Workspace, *PartA);
		const auto Joint = WorldRootTestAccess::Constraint(*Workspace, *Weld);
		Check(Joint.IsValid(), "valid WeldConstraint receives neutral constraint identity");

		PartA->SetSize({3.0f, 2.0f, 1.0f});
		Workspace->StepPhysics(0.0, std::nullopt);
		Check(WorldRootTestAccess::Body(*Workspace, *PartA) == BodyA &&
			WorldRootTestAccess::IsConstraintValid(*Workspace, Joint),
			"body shape update preserves attached constraint");

		Weld->SetEnabled(false);
		Workspace->StepPhysics(0.0, std::nullopt);
		Check(WorldRootTestAccess::ConstraintCount(*Workspace) == 0, "disabled constraint is removed at safe point");
		Weld->SetEnabled(true);
		Workspace->StepPhysics(0.0, std::nullopt);
		Check(WorldRootTestAccess::ConstraintCount(*Workspace) == 1, "re-enabled constraint is reconciled");

		PartA->SetCFrame(CFrame(9.0f, 9.0f, 9.0f));
		PartA->SetParent(nullptr);
		Workspace->StepPhysics(0.0, std::nullopt);
		Check(!WorldRootTestAccess::IsBodyValid(*Workspace, BodyA), "destroyed or removed Part invalidates body before pending update");
		Check(WorldRootTestAccess::ConstraintCount(*Workspace) == 0, "body removal clears attached constraint identity");
	}

	void TestAuthoredJointLifecycle() {
		using namespace gargantuan;
		auto Workspace = MakeWorkspace();
		if (!Workspace) return;
		Workspace->SetGravity(0.0f);
		auto Frame = std::make_shared<Part>();
		auto Door = std::make_shared<Part>();
		Frame->SetAnchored(true);
		Door->SetAnchored(true);
		Frame->SetParent(Workspace);
		Door->SetParent(Workspace);
		auto PivotA = std::make_shared<Attachment>();
		auto PivotB = std::make_shared<Attachment>();
		PivotA->SetParent(Frame);
		PivotB->SetParent(Door);
		auto Hinge = std::make_shared<JointConstraint>();
		const auto *MotorSpeedProperty = Hinge->FindProperty("MotorSpeed");
		Check(MotorSpeedProperty &&
			MotorSpeedProperty->PersistencePolicy == InstanceProperty::Persistence::Saved &&
			MotorSpeedProperty->ReplicationPolicy == InstanceProperty::Replication::FutureReplicated,
			"joint parameters participate in generated persistence and replication schema");
		Hinge->SetJointType("Hinge");
		Hinge->SetAttachment0(PivotA);
		Hinge->SetAttachment1(PivotB);
		Hinge->SetEnableLimit(true);
		Hinge->SetLowerLimit(-0.5f);
		Hinge->SetUpperLimit(0.5f);
		Hinge->SetParent(Workspace);
		const auto FirstJoint = WorldRootTestAccess::Constraint(*Workspace, *Hinge);
		Check(FirstJoint.IsValid(), "WorldRoot creates reflected attachment hinge");

		PivotA->SetCFrame(CFrame(0.5f, 0.0f, 0.0f));
		Workspace->StepPhysics(0.0, std::nullopt);
		const auto ReframedJoint = WorldRootTestAccess::Constraint(*Workspace, *Hinge);
		Check(ReframedJoint.IsValid() && ReframedJoint != FirstJoint &&
			!WorldRootTestAccess::IsConstraintValid(*Workspace, FirstJoint),
			"attachment frame edit replaces joint at physics safe point");

		bool RejectedType = false;
		try { Hinge->SetJointType("Unsupported"); }
		catch (const std::invalid_argument &) { RejectedType = true; }
		Workspace->StepPhysics(0.0, std::nullopt);
		Check(RejectedType && Hinge->GetJointType() == "Hinge" &&
			WorldRootTestAccess::Constraint(*Workspace, *Hinge) == ReframedJoint &&
			WorldRootTestAccess::IsConstraintValid(*Workspace, ReframedJoint),
			"invalid authored joint type rejects before changing the live joint");
		Hinge->SetLowerLimit(1.0f);
		Workspace->StepPhysics(0.0, std::nullopt);
		Check(WorldRootTestAccess::Constraint(*Workspace, *Hinge) == ReframedJoint &&
			WorldRootTestAccess::IsConstraintValid(*Workspace, ReframedJoint),
			"invalid limit update preserves the prior coherent joint");
		Hinge->SetJointType("BallSocket");
		Hinge->SetEnableLimit(false);
		Hinge->SetLowerLimit(0.0f);
		Hinge->SetUpperLimit(0.0f);
		Workspace->StepPhysics(0.0, std::nullopt);
		Check(WorldRootTestAccess::Constraint(*Workspace, *Hinge).IsValid() &&
			WorldRootTestAccess::Constraint(*Workspace, *Hinge) != ReframedJoint,
			"corrected joint type replaces stale configuration");

		const auto ActiveJoint = WorldRootTestAccess::Constraint(*Workspace, *Hinge);
		Door->SetParent(nullptr);
		Workspace->StepPhysics(0.0, std::nullopt);
		Check(!WorldRootTestAccess::IsConstraintValid(*Workspace, ActiveJoint) &&
			WorldRootTestAccess::ConstraintCount(*Workspace) == 0,
			"detached endpoint invalidates joint before slot reuse");
		Door->SetParent(Workspace);
		Workspace->StepPhysics(0.0, std::nullopt);
		Check(WorldRootTestAccess::Constraint(*Workspace, *Hinge).IsValid(),
			"returning endpoint reconciles a new joint generation");
	}

	void TestGameplayLuauJointAssembly() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		Game->SetArchivable(true);
		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		if (!WorkspaceValue) return;
		WorkspaceValue->SetGravity(0.0f);
		ScriptEngine Engine(Game);
		const std::string Source = R"(
			local Workspace = game:GetService("Workspace")
			local function MakePart(Name, Position, Anchored)
				local Part = Instance.new("Part")
				Part.Archivable = true
				Part.Name = Name
				Part.CFrame = CFrame.new(Position)
				Part.CanCollide = false
				Part.CanTouch = false
				Part.Anchored = Anchored
				Part.Parent = Workspace
				return Part
			end
			local function MakeJoint(Name, Kind, PartA, PartB, FrameA, FrameB)
				local AttachmentA = Instance.new("Attachment")
				AttachmentA.Archivable = true
				AttachmentA.Name = Name .. "A"
				AttachmentA.CFrame = FrameA
				AttachmentA.Parent = PartA
				local AttachmentB = Instance.new("Attachment")
				AttachmentB.Archivable = true
				AttachmentB.Name = Name .. "B"
				AttachmentB.CFrame = FrameB
				AttachmentB.Parent = PartB
				local Joint = Instance.new("JointConstraint")
				Joint.Archivable = true
				Joint.Name = Name
				Joint.JointType = Kind
				Joint.Parent = Workspace
				Joint.Attachment0 = AttachmentA
				Joint.Attachment1 = AttachmentB
				Joint.CollideConnected = false
				return Joint
			end
			local Frame = MakePart("DoorFrame", Vector3.new(0, 0, 0), true)
			local Door = MakePart("DoorBody", Vector3.new(2, 0, 0), false)
			local Hinge = MakeJoint("DoorHinge", "Hinge", Frame, Door,
				CFrame.new(0.5, 0, 0), CFrame.new(-1.5, 0, 0))
			Hinge.EnableLimit = true
			Hinge.LowerLimit = -0.4
			Hinge.UpperLimit = 0.4
			Hinge.EnableMotor = true
			Hinge.MotorSpeed = 2
			Hinge.MaximumMotorForce = 1000
			local Rail = MakePart("Rail", Vector3.new(10, 0, 0), true)
			local Slide = MakePart("SlideBody", Vector3.new(10, 0, 0), false)
			local Slider = MakeJoint("DrivenSlider", "Slider", Rail, Slide,
				CFrame.new(0, 0, 0), CFrame.new(0, 0, 0))
			Slider.EnableLimit = true
			Slider.LowerLimit = -1
			Slider.UpperLimit = 1
			Slider.EnableMotor = true
			Slider.MotorSpeed = 3
			Slider.MaximumMotorForce = 1000
			local Mount = MakePart("SpringMount", Vector3.new(20, 5, 0), true)
			local Weight = MakePart("SpringWeight", Vector3.new(20, 2, 0), false)
			local Spring = MakeJoint("SuspendedWeight", "Distance", Mount, Weight,
				CFrame.new(0, 0, 0), CFrame.new(0, 0, 0))
			Spring.RestLength = 1
			Spring.SpringFrequency = 8
			Spring.SpringDamping = 0.8
			assert(not pcall(function() Hinge.JointType = "Unknown" end))
		)";
		auto Bytecode = Luau::compile(Source);
		const int Loaded = luau_load(Engine.L, "physics-joint-assembly", Bytecode.data(), Bytecode.size(), 0);
		ScriptSecurityScope Security(ScriptSecurityContext::ClientRuntime());
		const int Status = Loaded == LUA_OK ? lua_pcall(Engine.L, 0, 0, 0) : Loaded;
		if (Status != LUA_OK)
			std::cerr << "LUAU ERROR: " << (lua_tostring(Engine.L, -1) ? lua_tostring(Engine.L, -1) : "unknown")
				<< '\n';
		Check(Status == LUA_OK, "normal gameplay Luau authors three live constraint mechanisms");
		if (Status != LUA_OK) return;
		WorkspaceValue->StepPhysics(0.0, std::nullopt);
		auto Door = std::dynamic_pointer_cast<Part>(WorkspaceValue->FindFirstDescendant("DoorBody"));
		auto Slide = std::dynamic_pointer_cast<Part>(WorkspaceValue->FindFirstDescendant("SlideBody"));
		auto Weight = std::dynamic_pointer_cast<Part>(WorkspaceValue->FindFirstDescendant("SpringWeight"));
		auto Hinge = std::dynamic_pointer_cast<JointConstraint>(WorkspaceValue->FindFirstDescendant("DoorHinge"));
		auto Slider = std::dynamic_pointer_cast<JointConstraint>(WorkspaceValue->FindFirstDescendant("DrivenSlider"));
		auto Spring = std::dynamic_pointer_cast<JointConstraint>(WorkspaceValue->FindFirstDescendant("SuspendedWeight"));
		Check(Door && Slide && Weight && Hinge && Slider && Spring &&
			WorldRootTestAccess::Constraint(*WorkspaceValue, *Hinge).IsValid() &&
			WorldRootTestAccess::Constraint(*WorkspaceValue, *Slider).IsValid() &&
			WorldRootTestAccess::Constraint(*WorkspaceValue, *Spring).IsValid(),
			"Luau-created attachments resolve to active WorldRoot joints");
		if (!Door || !Slide || !Weight || !Hinge || !Slider || !Spring) return;
		for (int Index = 0; Index < 120; ++Index) WorkspaceValue->StepPhysics(1.0 / 60.0, std::nullopt);
		const auto &DoorPose = Door->GetCFrame();
		const auto DoorAngle = std::atan2(DoorPose.Rotation[0][1], DoorPose.Rotation[0][0]);
		Check(std::abs(DoorAngle) > 0.05f && std::abs(DoorAngle) < 0.7f,
			"WorldRoot hinge motor moves a door within its authored angular limit");
		Check(Slide->GetCFrame().Position.x > 10.1f && Slide->GetCFrame().Position.x < 11.3f &&
			std::abs(Slide->GetCFrame().Position.y) < 0.2f,
			"WorldRoot slider motor moves a platform within its authored travel limit");
		Check(Weight->GetCFrame().Position.y > 3.2f && Weight->GetCFrame().Position.y < 4.8f,
			"WorldRoot spring pulls a suspended weight toward its rest length");

		std::shared_ptr<Instance> Root = Game;
		const auto Encoded = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Root);
		std::istringstream Input(Encoded);
		auto Decoded = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, Input);
		auto LoadedHinge = Decoded.Instance ? std::dynamic_pointer_cast<JointConstraint>(
			Decoded.Instance->FindFirstDescendant("DoorHinge")) : nullptr;
		auto LoadedSlider = Decoded.Instance ? std::dynamic_pointer_cast<JointConstraint>(
			Decoded.Instance->FindFirstDescendant("DrivenSlider")) : nullptr;
		auto LoadedSpring = Decoded.Instance ? std::dynamic_pointer_cast<JointConstraint>(
			Decoded.Instance->FindFirstDescendant("SuspendedWeight")) : nullptr;
		std::cout << "[Physics:JointPersistence] Decoded=" << Decoded.Ok << " Hinge=" << bool(LoadedHinge) << " Slider=" << bool(LoadedSlider) << " Spring=" << bool(LoadedSpring) << '\n';
		for(const auto &Error:Decoded.Errors) std::cerr << "[Physics:JointPersistence] " << Error << '\n';
		if(LoadedHinge && LoadedSlider && LoadedSpring)
			std::cout << "[Physics:JointPersistence] A0=" << bool(LoadedHinge->GetAttachment0()) << " A1=" << bool(LoadedHinge->GetAttachment1()) << " Enabled=" << LoadedHinge->GetEnabled() << " Type=" << LoadedHinge->GetJointType() << " Limit=" << LoadedHinge->GetEnableLimit() << " Motor=" << LoadedHinge->GetEnableMotor() << " Upper=" << LoadedHinge->GetUpperLimit() << " SliderForce=" << LoadedSlider->GetMaximumMotorForce() << " SpringHz=" << LoadedSpring->GetSpringFrequency() << '\n';
		Check(Decoded.Ok && LoadedHinge && LoadedSlider && LoadedSpring &&
			LoadedHinge->GetAttachment0() && *LoadedHinge->GetAttachment0() &&
			LoadedHinge->GetAttachment1() && *LoadedHinge->GetAttachment1() &&
			LoadedHinge->GetEnabled() && LoadedHinge->GetJointType() == "Hinge" &&
			LoadedHinge->GetEnableLimit() && LoadedHinge->GetEnableMotor() &&
			Near(LoadedHinge->GetUpperLimit(), 0.4f) &&
			LoadedSlider->GetJointType() == "Slider" && Near(LoadedSlider->GetMaximumMotorForce(), 1000.0f) &&
			LoadedSpring->GetJointType() == "Distance" && Near(LoadedSpring->GetSpringFrequency(), 8.0f),
			"gameplay joint frames, references, limits, motors, and springs survive save and reopen");
	}

	void TestSimulationPublicationAndImpulse() {
		using namespace gargantuan;
		auto Workspace = MakeWorkspace();
		if (!Workspace) return;
		Workspace->SetGravity(0.0f);
		auto Part = std::make_shared<gargantuan::Part>();
		Part->SetCFrame(CFrame(0.0f, 5.0f, 0.0f));
		Part->SetParent(Workspace);
		Part->ApplyImpulse({10.0f, 0.0f, 0.0f});
		Workspace->StepPhysics(1.0 / 60.0, std::nullopt);
		Check(Part->GetCFrame().Position.x > 0.0f, "backend motion publishes to authoritative CFrame after impulse");
		Workspace->StepPhysics(0.0, std::nullopt);
		const auto Body = WorldRootTestAccess::Body(*Workspace, *Part);
		auto State = WorldRootTestAccess::State(*Workspace, Body);
		Check(State && State->Description.Transform.FuzzyEq(Part->GetCFrame()),
			"physics publication does not create a transform feedback update");
	}

	void TestPhysicsPublicationNestedMutation() {
		auto Workspace = MakeWorkspace();
		if (!Workspace) return;
		Workspace->SetGravity(0.0f);
		auto Moving = std::make_shared<Part>();
		auto Other = std::make_shared<Part>();
		Moving->SetCFrame(CFrame(0.0f, 5.0f, 0.0f));
		Moving->SetParent(Workspace);
		Other->SetAnchored(true);
		Other->SetParent(Workspace);
		bool Mutated = false;
		auto Connection = Moving->GetPropertyChangedSignal("CFrame")->Connect([&](std::monostate) {
			if (Mutated) return;
			Mutated = true;
			Moving->SetCanCollide(false);
			Other->SetSize({3.0f, 3.0f, 3.0f});
			Moving->SetCFrame(CFrame(8.0f, 5.0f, 0.0f));
		});
		Moving->ApplyImpulse({10.0f, 0.0f, 0.0f});
		Workspace->StepPhysics(1.0 / 60.0, std::nullopt);
		Check(Mutated, "physics publication invokes synchronous CFrame observers");
		Workspace->StepPhysics(0.0, std::nullopt);
		const auto MovingState = WorldRootTestAccess::State(*Workspace, WorldRootTestAccess::Body(*Workspace, *Moving));
		const auto OtherState = WorldRootTestAccess::State(*Workspace, WorldRootTestAccess::Body(*Workspace, *Other));
		Check(MovingState && !MovingState->Description.CanCollide &&
			MovingState->Description.Transform.FuzzyEq(Moving->GetCFrame()),
			"same-Part nested collision and CFrame edits reach the next physics safe point");
		Check(OtherState && Near(OtherState->Description.Shape.Size.x, 3.0f) &&
			Near(OtherState->Description.Shape.Size.y, 3.0f) && Near(OtherState->Description.Shape.Size.z, 3.0f),
			"other-Part nested shape edit reaches the next physics safe point");
		Connection->Disconnect();
		auto DestroyedDuringPublication = std::make_shared<Part>();
		DestroyedDuringPublication->SetCFrame(CFrame(0.0f, 8.0f, 0.0f));
		DestroyedDuringPublication->SetParent(Workspace);
		bool Destroyed = false;
		auto DestroyConnection = DestroyedDuringPublication->GetPropertyChangedSignal("CFrame")->Connect(
			[&](std::monostate) {
				if (Destroyed) return;
				Destroyed = true;
				DestroyedDuringPublication->Destroy();
			});
		DestroyedDuringPublication->ApplyImpulse({10.0f, 0.0f, 0.0f});
		Workspace->StepPhysics(1.0 / 60.0, std::nullopt);
		Check(Destroyed && !WorldRootTestAccess::Body(*Workspace, *DestroyedDuringPublication).IsValid() &&
			WorldRootTestAccess::PartCount(*Workspace) == 2 &&
			WorldRootTestAccess::PartMembershipCount(*Workspace) == 2,
			"destruction in physics publication retires body and indexed membership");
		Workspace->StepPhysics(0.0, std::nullopt);
		DestroyConnection->Disconnect();
	}

	void TestBodyDynamicsAndCollisionGroups() {
		using namespace gargantuan;
		PhysicsWorld World(PhysicsWorldConfig{.Gravity = {0.0f, 0.0f, 0.0f}});
		PhysicsBodyDesc Description;
		Description.CollisionGroup = 2;
		Description.CollisionMask = std::uint16_t{1} << 3;
		Description.LinearDamping = 0.0f;
		Description.AngularDamping = 0.0f;
		const auto Dynamic = World.CreateBody(Description);
		Check(Dynamic.IsValid(), "dynamic body accepts bounded collision and damping description");
		Check(World.SetLinearVelocity(Dynamic, {2.0f, 0.0f, 0.0f}).Succeeded(), "linear velocity accepts a dynamic body");
		Check(World.SetAngularVelocity(Dynamic, {0.0f, 1.0f, 0.0f}).Succeeded(), "angular velocity accepts a dynamic body");
		auto State = World.GetBodyState(Dynamic);
		Check(State && Near(State->LinearVelocity.x, 2.0f) && Near(State->AngularVelocity.y, 1.0f) &&
			State->Mass > 0.0f && State->LocalInertia.x > 0.0f,
			"state exposes velocity, mass, and local inertia without native handles");
		Check(World.ApplyAngularImpulse(Dynamic, {0.0f, 2.0f, 0.0f}).Succeeded(), "angular impulse is applied");
		Check(World.GetBodyState(Dynamic)->AngularVelocity.y > 1.0f,"angular impulse changes angular velocity before an opposing point impulse");
		Check(World.ApplyImpulseAtPosition(Dynamic, {0.0f, 0.0f, 5.0f}, {1.0f, 0.0f, 0.0f}).Succeeded(),
			"off-center impulse is applied");
		Check(World.ApplyForce(Dynamic, {4.0f, 0.0f, 0.0f}).Succeeded() &&
			World.ApplyTorque(Dynamic, {0.0f, 0.0f, 2.0f}).Succeeded(),
			"force and torque are accepted for the next solver step");
		(void)World.Step({});
		State = World.GetBodyState(Dynamic);
		Check(State && State->LinearVelocity.x > 2.0f && State->LinearVelocity.z > 0.0f && State->AngularVelocity.z > 0.0f,
			"impulses and step force change velocity");
		Check(World.SetAwake(Dynamic, false).Succeeded() && !World.GetBodyState(Dynamic)->Awake,
			"sleep request is observable");
		Check(World.SetAwake(Dynamic, true).Succeeded() && World.GetBodyState(Dynamic)->Awake,
			"wake request is observable");
		Check(!World.ApplyForce(Dynamic, {NAN, 0.0f, 0.0f}).Succeeded(), "nonfinite force rejects without mutation");
		Check(!World.SetLinearVelocity({Dynamic.Slot, Dynamic.Generation + 1}, {}).Succeeded(),
			"stale body velocity mutation rejects");

		PhysicsBodyDesc Target;
		Target.Anchored = true;
		Target.Transform = CFrame(8.0f, 0.0f, 0.0f);
		Target.CollisionGroup = 3;
		Target.CollisionMask = (std::uint16_t{1} << 2) | (std::uint16_t{1} << 0);
		const auto Static = World.CreateBody(Target);
		const auto Ray = PhysicsRaycastRequest{.Origin = {5.0f, 0.0f, 0.0f}, .Direction = {5.0f, 0.0f, 0.0f}};
		Check(World.Raycast(Ray).HasHit(), "compatible default query sees collision group");
		auto RejectedRay = Ray;
		RejectedRay.Filter.CollisionMask = std::uint16_t{1} << 1;
		Check(!World.Raycast(RejectedRay).HasHit(), "query mask excludes incompatible group");
		Target.CollisionMask = 0;
		Check(World.UpdateBody(Static, Target).Succeeded() && !World.Raycast(Ray).HasHit(),
			"runtime collision mask update immediately changes queries");
		Target.CollisionMask = std::uint16_t{1} << 0;
		Check(World.UpdateBody(Static, Target).Succeeded() && World.Raycast(Ray).HasHit(),
			"runtime collision mask update restores compatible queries");
		Target.Kinematic = true;
		Check(!World.UpdateBody(Static, Target).Succeeded(), "anchored kinematic conflict rejects atomically");
		Target.Anchored = false;
		Check(World.UpdateBody(Static, Target).Succeeded() && World.SetLinearVelocity(Static, {-1.0f, 0.0f, 0.0f}).Succeeded(),
			"static body can transition to velocity-driven kinematic body");
		Check(!World.ApplyForce(Static, {1.0f, 0.0f, 0.0f}).Succeeded(), "kinematic body rejects dynamic force");
		Target.Kinematic = false;
		Check(World.UpdateBody(Static, Target).Succeeded() && World.ApplyForce(Static, {1.0f, 0.0f, 0.0f}).Succeeded(),
			"kinematic body can transition to dynamic body");
	}

	void TestWorldRootDynamicsProjection() {
		using namespace gargantuan;
		const auto *Schema = InstanceClassRegistry::GetDefinitionByName("BasePart");
		Check(Schema != nullptr, "BasePart schema is published");
		if (Schema) {
			for (const auto *Name : {"Kinematic", "CollisionGroup", "CollisionMask", "LinearDamping",
				"AngularDamping", "GravityScale", "SleepEnabled"}) {
				auto Found = Schema->AllProperties.find(Name);
				Check(Found != Schema->AllProperties.end() &&
					Found->second->PersistencePolicy == InstanceProperty::Persistence::Saved &&
					Found->second->ReplicationPolicy == InstanceProperty::Replication::FutureReplicated,
					"authored rigid controls retain generated persistence and replication metadata");
			}
		}
		auto Workspace = MakeWorkspace();
		if (!Workspace) return;
		Workspace->SetGravity(0.0f);
		auto Part = std::make_shared<gargantuan::Part>();
		bool RejectedGroup = false;
		try { Part->SetCollisionGroup(16); } catch (const std::exception &) { RejectedGroup = true; }
		Check(RejectedGroup && Part->GetCollisionGroup() == 0, "out-of-range group leaves committed property unchanged");
		Part->SetCollisionGroup(4);
		Part->SetCollisionMask(std::uint16_t{1} << 5);
		Part->SetGravityScale(0.5f);
		Part->SetLinearDamping(0.0f);
		Part->SetParent(Workspace);
		const auto Body = WorldRootTestAccess::Body(*Workspace, *Part);
		Check(Body.IsValid(), "WorldRoot admits authored body controls");
		Part->SetLinearVelocity({3.0f, 0.0f, 0.0f});
		Part->ApplyForce({4.0f, 0.0f, 0.0f});
		Workspace->StepPhysics(1.0 / 60.0, std::nullopt);
		Check(Part->GetLinearVelocity().x > 3.0f && Part->GetMass() > 0.0f && Part->GetLocalInertia().x > 0.0f,
			"WorldRoot safe point applies controls and publishes read-only mass and velocity");
		Part->SetCollisionMask(0);
		Workspace->StepPhysics(0.0, std::nullopt);
		auto State = WorldRootTestAccess::State(*Workspace, Body);
		Check(State && State->Description.CollisionMask == 0 && State->Description.CollisionGroup == 4 &&
			State->Description.GravityScale == 0.5f,
			"committed group, mask, and gravity changes reach the same backend body");
		Part->SetParent(nullptr);
		Check(Part->GetMass() == 0.0f, "removed Part retires published body diagnostics");
	}

	void TestBodyControlCapabilityBoundary() {
		using namespace gargantuan;
		auto Workspace = MakeWorkspace();
		if (!Workspace) return;
		Workspace->SetGravity(0.0f);
		auto Part = std::make_shared<gargantuan::Part>();
		Part->SetName("CapabilityProbe");
		Part->SetParent(Workspace);
		const auto Initial = Part->GetCFrame();
		ScriptEngine Engine(Workspace->GetDataModel());
		StackValue<std::shared_ptr<gargantuan::Part>>::Push(Engine.L, Part);
		lua_setglobal(Engine.L, "ControlPart");
		auto Run = [&](const char *Name, const std::string &Source, ScriptSecurityContext Context) {
			const auto Bytecode = Luau::compile(Source);
			const int Loaded = luau_load(Engine.L, Name, Bytecode.data(), Bytecode.size(), 0);
			int Status = Loaded;
			if (Loaded == LUA_OK) {
				ScriptSecurityScope Security(Context);
				Status = lua_pcall(Engine.L, 0, 0, 0);
			}
			if (Status != LUA_OK) {
				std::cerr << "[Physics:Authority] " << Name << ": " <<
					(lua_tostring(Engine.L, -1) ? lua_tostring(Engine.L, -1) : "unknown") << '\n';
				lua_settop(Engine.L, 0);
			}
			return Status == LUA_OK;
		};
		const ScriptSecurityContext StudioReadOnly{
			ScriptExecutionDomain::Studio, {ScriptCapability::ReadDataModel}};
		Check(Run("physics-denied-writes", R"(
			local Part = ControlPart
			local Force = Vector3.new(3, 0, 0)
			assert(Part:GetMass() > 0)
			assert(not pcall(function() Part:ApplyImpulse(Force) end))
			assert(not pcall(function() Part:ApplyAngularImpulse(Force) end))
			assert(not pcall(function() Part:ApplyImpulseAtPosition(Force, Vector3.new(0, 0, 1)) end))
			assert(not pcall(function() Part:ApplyForce(Force) end))
			assert(not pcall(function() Part:ApplyTorque(Force) end))
			assert(not pcall(function() Part:SetLinearVelocity(Force) end))
			assert(not pcall(function() Part:SetAngularVelocity(Force) end))
			assert(not pcall(function() Part:SetAwake(false) end))
		)", StudioReadOnly), "read-only Studio context denies every BasePart runtime write");
		const ScriptSecurityContext NoCaps{ScriptExecutionDomain::Core, {}};
		Check(Run("physics-denied-reads", R"(
			local Part = ControlPart
			assert(not pcall(function() return Part:GetLinearVelocity() end))
			assert(not pcall(function() return Part:GetAngularVelocity() end))
			assert(not pcall(function() return Part:GetMass() end))
			assert(not pcall(function() return Part:GetLocalInertia() end))
			assert(not pcall(function() return Part:GetAwake() end))
		)", NoCaps), "no-capability context denies every BasePart runtime read");
		Workspace->StepPhysics(1.0f / 60.0f, std::nullopt);
		Check(Part->GetCFrame().FuzzyEq(Initial) &&
			Near(glm::length(Part->GetLinearVelocity()), 0.0f) &&
			Near(glm::length(Part->GetAngularVelocity()), 0.0f) && Part->GetAwake(),
			"denied BasePart controls do not enqueue velocity, impulses, forces, torque, or sleep");
		Check(Run("physics-capable-controls", R"(
			local Part = ControlPart
			local Linear = Vector3.new(2, 0, 0)
			Part:SetLinearVelocity(Linear)
			Part:SetAngularVelocity(Vector3.new(0, 1, 0))
			Part:ApplyImpulse(Vector3.new(1, 0, 0))
			Part:ApplyAngularImpulse(Vector3.new(0, 1, 0))
			Part:ApplyImpulseAtPosition(Vector3.new(1, 0, 0), Vector3.new(0, 0, 1))
			Part:ApplyForce(Vector3.new(1, 0, 0))
			Part:ApplyTorque(Vector3.new(0, 1, 0))
			Part:SetAwake(true)
			assert(Part:GetLinearVelocity().X == 2)
			assert(Part:GetAngularVelocity().Y == 1)
			assert(Part:GetMass() > 0)
			assert(Part:GetLocalInertia().X > 0)
			assert(Part:GetAwake())
		)", ScriptSecurityContext::ClientRuntime()),
			"capable gameplay Luau retains all BasePart runtime controls and observations");
		Workspace->StepPhysics(1.0f / 60.0f, std::nullopt);
		Check(Part->GetCFrame().Position.x > Initial.Position.x &&
			Part->GetLinearVelocity().x > 2.0f && Part->GetAwake(),
			"capable gameplay controls still reach the physics safe point");
	}

	void TestForceDurationAndKinematicMotion() {
		using namespace gargantuan;
		PhysicsWorld Backend(PhysicsWorldConfig{.Gravity={0,0,0}});
		PhysicsBodyDesc Description;
		Description.LinearDamping=0; Description.AngularDamping=0;
		const auto Forced=Backend.CreateBody(Description);
		Description.Transform=CFrame(10,0,0);
		const auto Impulsed=Backend.CreateBody(Description);
		Check(Backend.ApplyForce(Forced,{60,0,0}).Succeeded() && Backend.ApplyLinearImpulse(Impulsed,{1,0,0}).Succeeded(),"equal-step force and impulse fixture accepts controls");
		(void)Backend.Step({});
		const float ForcedSpeed=Backend.GetBodyState(Forced)->LinearVelocity.x;
		const float ImpulseSpeed=Backend.GetBodyState(Impulsed)->LinearVelocity.x;
		Check(ForcedSpeed>0 && std::abs(ForcedSpeed-ImpulseSpeed)<.001f,"force integrates over one fixed step while impulse applies momentum immediately");
		(void)Backend.Step({});
		Check(std::abs(Backend.GetBodyState(Forced)->LinearVelocity.x-ForcedSpeed)<.001f,"one force request is not replayed on later fixed steps");
		auto World=MakeWorkspace(); World->SetGravity(196.2f);
		auto Part=std::make_shared<gargantuan::Part>();
		Part->SetKinematic(true); Part->SetParent(World); Part->SetLinearVelocity({6,0,0});
		World->StepPhysics(1.0/60.0,std::nullopt);
		Check(std::abs(Part->GetCFrame().Position.x-.1f)<.002f && std::abs(Part->GetCFrame().Position.y)<.002f,"WorldRoot publishes velocity-driven kinematic translation without gravity");
		Part->SetAnchored(true); World->StepPhysics(0.0,std::nullopt);
		const auto Fixed=Part->GetCFrame();
		Part->SetLinearVelocity({100,0,0}); World->StepPhysics(1.0/60.0,std::nullopt);
		Check(Part->GetCFrame().FuzzyEq(Fixed),"anchored safe-point transition discards pending moving-body controls");
		Part->Destroy();
	}

	void TestBodyControlPersistence() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		Game->SetArchivable(true);
		auto Workspace = std::dynamic_pointer_cast<gargantuan::Workspace>(Game->GetService("Workspace"));
		if (!Workspace) return;
		auto Part = std::make_shared<gargantuan::Part>();
		Part->SetName("PersistedRigidControl");
		Part->SetArchivable(true);
		Part->SetKinematic(true);
		Part->SetCollisionGroup(7);
		Part->SetCollisionMask(129);
		Part->SetLinearDamping(0.25f);
		Part->SetAngularDamping(0.5f);
		Part->SetGravityScale(1.5f);
		Part->SetSleepEnabled(false);
		Part->SetMovingBodyCCD(true);
		Part->SetParent(Workspace);
		std::shared_ptr<Instance> Root = Game;
		const auto Encoded = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Root);
		std::istringstream Input(Encoded);
		auto Decoded = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, Input);
		auto Loaded = Decoded.Instance ?
			std::dynamic_pointer_cast<gargantuan::Part>(Decoded.Instance->FindFirstDescendant("PersistedRigidControl")) : nullptr;
		if (!Decoded.Ok || !Loaded) {
			std::cerr << "[Physics:Persistence] Decoded=" << Decoded.Ok << " Part=" << bool(Loaded) << '\n';
			for(const auto &Error:Decoded.Errors) std::cerr << "[Physics:Persistence] " << Error << '\n';
		} else {
			std::cout << "[Physics:Persistence] Kinematic=" << Loaded->GetKinematic() << " Group=" << Loaded->GetCollisionGroup()
				<< " Mask=" << Loaded->GetCollisionMask() << " LinearDamping=" << Loaded->GetLinearDamping()
				<< " AngularDamping=" << Loaded->GetAngularDamping() << " GravityScale=" << Loaded->GetGravityScale()
				<< " Sleep=" << Loaded->GetSleepEnabled() << " CCD=" << Loaded->GetMovingBodyCCD() << '\n';
		}
		Check(Decoded.Ok && Loaded && Loaded->GetKinematic() && Loaded->GetCollisionGroup() == 7 &&
			Loaded->GetCollisionMask() == 129 && Near(Loaded->GetLinearDamping(), 0.25f) &&
			Near(Loaded->GetAngularDamping(), 0.5f) && Near(Loaded->GetGravityScale(), 1.5f) &&
			!Loaded->GetSleepEnabled() && Loaded->GetMovingBodyCCD(),
			"authored rigid controls and CCD policy survive project serialization and reopen");
	}

	void TestContinuousCollisionPolicy() {
		using namespace gargantuan;
		PhysicsBodyDesc Projectile;
		Projectile.Shape.Kind = PhysicsShapeKind::Ball;
		Projectile.Shape.Size = {0.5f, 0.5f, 0.5f};
		Projectile.Transform = CFrame(-3.0f, 0.0f, 0.0f);
		Projectile.MovingBodyCCD = true;
		PhysicsBodyDesc ThinWall;
		ThinWall.Anchored = true;
		ThinWall.Shape.Size = {0.1f, 4.0f, 4.0f};
		ThinWall.Transform = CFrame(0.0f, 0.0f, 0.0f);

		PhysicsWorld StaticWorld(PhysicsWorldConfig{.Gravity = {0.0f, 0.0f, 0.0f}});
		const auto Wall = StaticWorld.CreateBody(ThinWall);
		Check(Wall.IsValid(), "thin static obstacle is admitted");
		for (int Attempt = 0; Attempt < 8; ++Attempt) {
			const auto Ball = StaticWorld.CreateBody(Projectile);
			Check(Ball.IsValid() && StaticWorld.SetLinearVelocity(Ball, {360.0f, 0.0f, 0.0f}).Succeeded(),
				"high-speed projectile admits bounded velocity");
			(void)StaticWorld.Step({});
			auto State = StaticWorld.GetBodyState(Ball);
			Check(State && State->Transform.Position.x < 0.5f,
				"repeated high-speed ball crossing is stopped by a thin static wall");
			Check(StaticWorld.DestroyBody(Ball).Succeeded(), "repeated projectile retires before the next pass");
		}
		Projectile.MovingBodyCCD = false;
		const auto DefaultBall = StaticWorld.CreateBody(Projectile);
		Check(DefaultBall.IsValid() && StaticWorld.SetLinearVelocity(DefaultBall, {360.0f, 0.0f, 0.0f}).Succeeded(),
			"default body retains static CCD without the moving-body option");
		(void)StaticWorld.Step({});
		auto DefaultState = StaticWorld.GetBodyState(DefaultBall);
		Check(DefaultState && DefaultState->Transform.Position.x < 0.5f,
			"default dynamic body still receives pinned Box3D static CCD");

		PhysicsWorld MovingWorld(PhysicsWorldConfig{.Gravity = {0.0f, 0.0f, 0.0f}});
		ThinWall.Anchored = false;
		const auto MovingTarget = MovingWorld.CreateBody(ThinWall);
		Check(MovingTarget.IsValid() && MovingWorld.SetLinearVelocity(MovingTarget, {-2.0f, 0.0f, 0.0f}).Succeeded(),
			"moving rigid obstacle is admitted");
		Projectile.MovingBodyCCD = true;
		const auto MovingBall = MovingWorld.CreateBody(Projectile);
		Check(MovingBall.IsValid() && MovingWorld.SetLinearVelocity(MovingBall, {360.0f, 0.0f, 0.0f}).Succeeded(),
			"moving-body CCD projectile is admitted");
		(void)MovingWorld.Step({});
		auto MovingState = MovingWorld.GetBodyState(MovingBall);
		Check(MovingState && MovingState->Transform.Position.x < 1.0f,
			"opt-in sweep catches a fast ball against a moving dynamic obstacle");
		PhysicsWorld KinematicWorld(PhysicsWorldConfig{.Gravity = {0.0f, 0.0f, 0.0f}});
		ThinWall.Kinematic = true;
		const auto KinematicTarget = KinematicWorld.CreateBody(ThinWall);
		const auto KinematicBall = KinematicWorld.CreateBody(Projectile);
		Check(KinematicTarget.IsValid() && KinematicBall.IsValid() &&
			KinematicWorld.SetLinearVelocity(KinematicTarget, {-2.0f, 0.0f, 0.0f}).Succeeded() &&
			KinematicWorld.SetLinearVelocity(KinematicBall, {360.0f, 0.0f, 0.0f}).Succeeded(),
			"moving kinematic obstacle and fast dynamic ball are admitted");
		(void)KinematicWorld.Step({});
		auto KinematicState = KinematicWorld.GetBodyState(KinematicBall);
		Check(KinematicState && KinematicState->Transform.Position.x < 1.0f,
			"opt-in sweep catches a fast ball against a moving kinematic obstacle");
		Projectile.CanCollide = false;
		Check(!MovingWorld.CreateBody(Projectile).IsValid(), "direct CCD sensor description rejects before native admission");
		Projectile.CanCollide = true;
		Projectile.Kinematic = true;
		Check(!MovingWorld.CreateBody(Projectile).IsValid(), "direct kinematic CCD description rejects before native admission");
		Projectile.Kinematic = false;
		Projectile.Anchored = true;
		Check(!MovingWorld.CreateBody(Projectile).IsValid(), "direct static CCD description rejects before native admission");

		auto Positions = std::make_shared<const std::vector<glm::vec3>>(std::vector<glm::vec3>{
			{0.0f, -2.0f, -2.0f}, {0.0f, -2.0f, 2.0f},
			{0.0f, 2.0f, 2.0f}, {0.0f, 2.0f, -2.0f},
		});
		auto Indices = std::make_shared<const std::vector<std::uint32_t>>(
			std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3});
		auto Mesh = std::make_shared<const PhysicsTriangleMesh>(PhysicsTriangleMesh{
			.ResourceId = "asset://ccd-wall", .ContentRevision = 1,
			.Positions = Positions, .Indices = Indices,
		});
		PhysicsBodyDesc MeshWall;
		MeshWall.Anchored = true;
		MeshWall.Shape = {.Kind = PhysicsShapeKind::StaticTriangleMesh, .Size = {1.0f, 1.0f, 1.0f}, .Mesh = Mesh};
		PhysicsWorld MeshWorld(PhysicsWorldConfig{.Gravity = {0.0f, 0.0f, 0.0f}});
		const auto MeshBody = MeshWorld.CreateBody(MeshWall);
		Projectile.Anchored = false;
		const auto MeshBall = MeshWorld.CreateBody(Projectile);
		Check(MeshBody.IsValid() && MeshBall.IsValid() &&
			MeshWorld.SetLinearVelocity(MeshBall, {360.0f, 0.0f, 0.0f}).Succeeded(),
			"static triangle mesh wall and fast ball are admitted");
		(void)MeshWorld.Step({});
		auto MeshState = MeshWorld.GetBodyState(MeshBall);
		Check(MeshState && MeshState->Transform.Position.x < 0.5f,
			"fast convex body uses static triangle mesh CCD target");
	}

	void TestWorldRootContinuousCollisionProjection() {
		using namespace gargantuan;
		auto Workspace = MakeWorkspace();
		if (!Workspace) return;
		Workspace->SetGravity(0.0f);
		auto Wall = std::make_shared<Part>();
		Wall->SetAnchored(true);
		Wall->SetSize({0.1f, 4.0f, 4.0f});
		Wall->SetParent(Workspace);
		auto Ball = std::make_shared<Part>();
		Ball->SetName("CCDBall");
		Ball->SetShape(Enums::PartType::Ball);
		Ball->SetSize({0.5f, 0.5f, 0.5f});
		Ball->SetCFrame(CFrame(-3.0f, 0.0f, 0.0f));
		Ball->SetMovingBodyCCD(true);
		Ball->SetParent(Workspace);
		const auto Id = WorldRootTestAccess::Body(*Workspace, *Ball);
		auto State = WorldRootTestAccess::State(*Workspace, Id);
		Check(State && State->Description.MovingBodyCCD,
			"authored moving-body CCD reaches the neutral body without changing identity");
		Ball->SetLinearVelocity({360.0f, 0.0f, 0.0f});
		Workspace->StepPhysics(1.0 / 60.0, std::nullopt);
		Check(Ball->GetCFrame().Position.x < 0.5f, "CCD result publishes through WorldRoot CFrame motion");
		if (auto Game = Workspace->GetDataModel()) {
			ScriptEngine Engine(Game);
			const auto Bytecode = Luau::compile(R"(
				local Ball = game:GetService("Workspace").CCDBall
				assert(Ball.MovingBodyCCD)
				assert(Ball.Position.X < 0.5)
			)");
			const int Loaded = luau_load(Engine.L, "physics-ccd-observation", Bytecode.data(), Bytecode.size(), 0);
			ScriptSecurityScope Security(ScriptSecurityContext::ClientRuntime());
			const int Status = Loaded == LUA_OK ? lua_pcall(Engine.L, 0, 0, 0) : Loaded;
			if (Status != LUA_OK)
				std::cerr << "[Physics:CCD] LUAU ERROR: " <<
					(lua_tostring(Engine.L, -1) ? lua_tostring(Engine.L, -1) : "unknown") << '\n';
			Check(Status == LUA_OK, "Luau observes persisted CCD policy and published projectile motion");
		}
		Ball->SetAnchored(true);
		Workspace->StepPhysics(0.0, std::nullopt);
		State = WorldRootTestAccess::State(*Workspace, Id);
		Check(State && !State->Description.MovingBodyCCD && Ball->GetMovingBodyCCD(),
			"anchoring suppresses effective CCD while retaining authored policy");
		Ball->SetAnchored(false);
		Workspace->StepPhysics(0.0, std::nullopt);
		State = WorldRootTestAccess::State(*Workspace, Id);
		Check(State && State->Description.MovingBodyCCD && WorldRootTestAccess::Body(*Workspace, *Ball) == Id,
			"unanchoring restores CCD at a safe point without replacing body identity");
		Ball->SetKinematic(true);
		Workspace->StepPhysics(0.0, std::nullopt);
		State = WorldRootTestAccess::State(*Workspace, Id);
		Check(State && !State->Description.MovingBodyCCD,
			"kinematic transition suppresses the dynamic-only CCD request");
		Ball->SetKinematic(false);
		Workspace->StepPhysics(0.0, std::nullopt);
		State = WorldRootTestAccess::State(*Workspace, Id);
		Check(State && State->Description.MovingBodyCCD,
			"dynamic transition restores authored CCD request");
	}

	void TestTouchAndSensorUpdates() {
		using namespace gargantuan;
		auto Workspace = MakeWorkspace();
		if (!Workspace) return;
		Workspace->SetGravity(0.0f);
		auto Sensor = std::make_shared<Part>();
		Sensor->SetAnchored(true);
		Sensor->SetCanCollide(false);
		Sensor->SetCanTouch(true);
		Sensor->SetCollisionGroup(2);
		Sensor->SetCollisionMask(0);
		Sensor->SetSize({4.0f, 4.0f, 4.0f});
		auto Visitor = std::make_shared<Part>();
		Visitor->SetCollisionGroup(3);
		Visitor->SetCollisionMask(std::uint16_t{1} << 2);
		Visitor->SetCFrame(CFrame(0.0f, 0.0f, 0.0f));
		Sensor->SetParent(Workspace);
		Visitor->SetParent(Workspace);
		int Touches = 0;
		Sensor->Touched->Connect([&](std::shared_ptr<BasePart>) { ++Touches; });
		Workspace->StepPhysics(1.0 / 60.0, std::nullopt);
		Check(Touches == 0, "sensor mask rejects a disallowed group before event publication");
		Sensor->SetCollisionMask(std::uint16_t{1} << 3);
		Visitor->SetCFrame(CFrame(20.0f, 0.0f, 0.0f));
		Workspace->StepPhysics(1.0 / 60.0, std::nullopt);
		Visitor->SetCFrame(CFrame(0.0f, 0.0f, 0.0f));
		Workspace->StepPhysics(1.0 / 60.0, std::nullopt);
		Check(Touches == 1, "non-colliding CanTouch body receives normalized sensor begin event");

		Visitor->SetCFrame(CFrame(20.0f, 0.0f, 0.0f));
		Workspace->StepPhysics(1.0 / 60.0, std::nullopt);
		Sensor->SetCanTouch(false);
		Visitor->SetCFrame(CFrame(0.0f, 0.0f, 0.0f));
		Workspace->StepPhysics(1.0 / 60.0, std::nullopt);
		Check(Touches == 1, "committed CanTouch false disables later sensor events");
	}

	void TestStaticWorldStepReference() {
		PhysicsWorld World(PhysicsWorldConfig{.Gravity = {0.0f, 0.0f, 0.0f}});
		PhysicsWorld Reference(PhysicsWorldConfig{.Gravity = {0.0f, 0.0f, 0.0f}});
		std::array<PhysicsBodyDesc, 3> Descriptions{};
		Descriptions[0].Anchored = true;
		Descriptions[0].CanCollide = false;
		Descriptions[0].Shape.Size = {4.0f, 4.0f, 4.0f};
		Descriptions[1].Anchored = true;
		Descriptions[2].Anchored = true;
		Descriptions[2].CanCollide = false;
		Descriptions[2].CanTouch = false;
		std::array<PhysicsBodyId, 3> Ids{};
		for (std::size_t Index = 0; Index < Ids.size(); ++Index) {
			Ids[Index] = World.CreateBody(Descriptions[Index]);
			Check(Ids[Index].IsValid() && Reference.CreateBody(Descriptions[Index]) == Ids[Index],
				"static reference fixture has matching generation-safe identities");
		}
		// Force the reference through ordinary Box3D stepping, without a new
		// test-only engine switch. This distant dynamic body cannot interact.
		auto DynamicDescription = Descriptions[2];
		DynamicDescription.Anchored = false;
		DynamicDescription.Transform = CFrame(10000.0f, 10000.0f, 10000.0f);
		const auto ReferenceDynamic = Reference.CreateBody(DynamicDescription);
		Check(ReferenceDynamic.IsValid(), "reference always has a dynamic body");
		auto Update = [&](std::size_t Index) {
			Check(World.UpdateBody(Ids[Index], Descriptions[Index]).Succeeded() &&
				Reference.UpdateBody(Ids[Index], Descriptions[Index]).Succeeded(), "paired body mutation succeeds");
		};
		auto ContactKeys = [](const PhysicsStepResult &Result) {
			std::vector<std::tuple<PhysicsBodyId, PhysicsBodyId, PhysicsContactPhase>> Keys;
			for (const auto &Contact : Result.Contacts)
				Keys.emplace_back(Contact.BodyA, Contact.BodyB, Contact.Phase);
			std::sort(Keys.begin(), Keys.end());
			return Keys;
		};
		std::uint64_t ActualSteps = 0, EmptySteps = 0;
		auto CompareStep = [&](bool ExpectBackendStep) {
			runtime_detail::WorkSample Sample{};
			PhysicsStepResult Actual;
			{
				runtime_detail::WorkCapture Capture(&Sample);
				Actual = World.Step({});
			}
			const auto Expected = Reference.Step({});
			const auto &Work = Sample[static_cast<std::size_t>(runtime_detail::WorkPhase::PhysicsBackendStep)];
			Check(Work.Calls == (ExpectBackendStep ? 1u : 0u) && Work.Units == 1 &&
				Work.Retained == (ExpectBackendStep ? 1u : 0u) && Work.Rejected == (ExpectBackendStep ? 0u : 1u),
				"only settled static worlds elide backend work");
			ActualSteps += Work.Retained;
			EmptySteps += Work.Rejected;
			Check(ContactKeys(Actual) == ContactKeys(Expected) && Actual.EventsTruncated == Expected.EventsTruncated,
				"static fast path equals full-step reference contact events without replay");
			for (const auto Id : Ids) {
				const auto A = World.GetBodyState(Id), B = Reference.GetBodyState(Id);
				Check(A && B && glm::length(A->Transform.Position - B->Transform.Position) < 0.0001f &&
					glm::length(A->LinearVelocity - B->LinearVelocity) < 0.0001f,
					"static fast path preserves current body states");
			}
			if (!ExpectBackendStep)
				Check(Actual.Motions.empty() && Actual.Contacts.empty(), "skipped steps return no stale native events");
			return Actual;
		};
		const auto Initial = CompareStep(true);
		Check(Initial.Contacts.size() == 1 && Initial.Contacts[0].Phase == PhysicsContactPhase::Began,
			"static sensor overlap begins once");
		for (int Index = 0; Index < 128; ++Index) {
			Descriptions[2].Transform = CFrame(static_cast<float>(Index % 9), 0.0f, 0.0f);
			Update(2);
			CompareStep(false);
		}
		Descriptions[1].Transform = CFrame(20.0f, 0.0f, 0.0f);
		Update(1);
		const auto End = CompareStep(true);
		Check(End.Contacts.size() == 1 && End.Contacts[0].Phase == PhysicsContactPhase::Ended,
			"static transform changes end existing overlaps promptly");
		CompareStep(false);

		std::uint32_t Seed = 0x3F17u;
		for (int Iteration = 0; Iteration < 96; ++Iteration) {
			Seed = Seed * 1664525u + 1013904223u;
			switch ((Seed >> 16) % 6) {
			case 0:
				Descriptions[1].Transform = CFrame((Seed & 1) ? 0.0f : 20.0f, 0.0f, 0.0f);
				Update(1);
				break;
			case 1:
				Descriptions[0].CanTouch = !Descriptions[0].CanTouch;
				Update(0);
				break;
			case 2:
				Descriptions[1].Shape.Size = glm::vec3((Seed & 1) ? 2.0f : 8.0f);
				Descriptions[1].Shape.Kind = (Seed & 2) ? PhysicsShapeKind::Ball : PhysicsShapeKind::Box;
				Update(1);
				break;
			case 3:
				Descriptions[1].CanCollide = !Descriptions[1].CanCollide;
				Update(1);
				break;
			case 4:
				Descriptions[2].CanTouch = !Descriptions[2].CanTouch;
				Update(2);
				break;
			case 5: {
				const auto Old = Ids[1];
				Check(World.DestroyBody(Old).Succeeded() && Reference.DestroyBody(Old).Succeeded(),
					"destroy during settled static lifetime succeeds");
				Ids[1] = World.CreateBody(Descriptions[1]);
				Check(Ids[1] != Old && Reference.CreateBody(Descriptions[1]) == Ids[1],
					"static replacement receives fresh generation in both paths");
				Check(!World.UpdateBody(Old, Descriptions[1]).Succeeded() && !World.DestroyBody(Old).Succeeded(),
					"stale updates and destroys cannot corrupt static step eligibility");
				break;
			}
			}
			CompareStep(true);
			CompareStep(false);
			CompareStep(false);
		}
		Descriptions[1].CanCollide = true;
		Descriptions[1].Transform = CFrame(5.0f, 0.0f, 0.0f);
		Update(1);
		const auto Query = World.Raycast({.Origin = {}, .Direction = {12.0f, 0.0f, 0.0f}});
		Check(Query.Succeeded() && std::any_of(Query.Candidates.begin(), Query.Candidates.end(),
			[&](const auto &Hit) { return Hit.Body == Ids[1]; }), "query sees mutation before any backend step");
		CompareStep(true);
		Descriptions[1].Anchored = false;
		Update(1);
		for (int Index = 0; Index < 12; ++Index) CompareStep(true);
		Descriptions[1].Anchored = true;
		Update(1);
		CompareStep(true);
		CompareStep(false);
		Check(World.SetGravity({}).Succeeded() && Reference.SetGravity({}).Succeeded(), "gravity mutation succeeds");
		CompareStep(true);
		Check(World.ApplyLinearImpulse(Ids[1], {1.0f, 0.0f, 0.0f}).Succeeded() &&
			Reference.ApplyLinearImpulse(Ids[1], {1.0f, 0.0f, 0.0f}).Succeeded(), "anchored impulse remains well-defined");
		CompareStep(true);
		const PhysicsConstraintDesc Constraint{.BodyA = Ids[0], .BodyB = Ids[1]};
		const auto Joint = World.CreateConstraint(Constraint);
		Check(Joint.IsValid() && Reference.CreateConstraint(Constraint) == Joint, "static constraint admission matches");
		CompareStep(true);
		CompareStep(false);
		Check(World.DestroyConstraint(Joint).Succeeded() && Reference.DestroyConstraint(Joint).Succeeded(),
			"static constraint removal matches");
		CompareStep(true);
		CompareStep(false);
		const auto Dynamic = World.CreateBody(DynamicDescription);
		Check(Dynamic.IsValid(), "new dynamic body disables static no-work path");
		CompareStep(true);
		CompareStep(true);
		Check(World.DestroyBody(Dynamic).Succeeded() && !World.DestroyBody(Dynamic).Succeeded(),
			"dynamic removal and stale duplicate do not corrupt the live count");
		CompareStep(true);
		CompareStep(false);
		Check(ActualSteps > 100 && EmptySteps > 300, "reference covers both active and no-work paths repeatedly");
	}

	std::shared_ptr<Part> MakeRayPart(
		const std::shared_ptr<Workspace> &WorkspaceValue,
		std::string Name,
		glm::vec3 Position,
		glm::vec3 Size = {2.0f, 2.0f, 2.0f},
		Enums::PartType Shape = Enums::PartType::Block
	) {
		auto Value = std::make_shared<Part>();
		Value->SetName(std::move(Name));
		Value->SetAnchored(true);
		Value->SetCFrame(CFrame(Position));
		Value->SetSize(Size);
		Value->SetShape(Shape);
		Value->SetParent(WorkspaceValue);
		return Value;
	}

	void TestSemanticRaycastContract() {
		using namespace gargantuan;
		auto WorkspaceValue = MakeWorkspace();
		auto NearPart = MakeRayPart(WorkspaceValue, "Near", {5.0f, 0.0f, 0.0f});
		auto FarPart = MakeRayPart(WorkspaceValue, "Far", {9.0f, 0.0f, 0.0f});
		auto Hit = WorkspaceValue->ResolveRaycast({0.0f, 0.0f, 0.0f}, {12.0f, 0.0f, 0.0f});
		Check(Hit.Succeeded() && Hit.Instance == NearPart, "semantic raycast returns the closest authoritative Part");
		Check(
			Hit.HasHit() && Near(Hit.Distance, 4.0f) && Near(Hit.Position.x, 4.0f) && Hit.Normal.x < -0.99f,
			"semantic raycast returns world impact position, normalized normal, and direction-derived distance"
		);
		auto Miss = WorkspaceValue->ResolveRaycast({0.0f, 10.0f, 0.0f}, {12.0f, 0.0f, 0.0f});
		Check(Miss.Succeeded() && !Miss.HasHit(), "valid ray miss returns no semantic result");
		auto Exact = WorkspaceValue->ResolveRaycast({0.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 0.0f});
		Check(
			Exact.Succeeded() && Exact.Instance == NearPart && Near(Exact.Distance, 4.0f),
			"raycast includes a hit exactly at Direction magnitude"
		);
		auto Grazing = WorkspaceValue->ResolveRaycast({0.0f, 0.999f, 0.0f}, {12.0f, 0.0f, 0.0f});
		Check(
			Grazing.Succeeded() && Grazing.Instance == NearPart,
			"raycast uses the real collider for a finite grazing hit"
		);
		NearPart->SetCanCollide(false);
		auto NonColliding = WorkspaceValue->ResolveRaycast({}, {12.0f, 0.0f, 0.0f});
		Check(
			NonColliding.Succeeded() && NonColliding.Instance == FarPart,
			"non-colliding rigid bodies do not participate in raycasts"
		);
		NearPart->SetCanCollide(true);

		Check(
			WorkspaceValue->ResolveRaycast({}, {}).Status == PhysicsOperationStatus::InvalidDescription,
			"zero direction fails the bounded native contract"
		);
		Check(
			WorkspaceValue->ResolveRaycast({}, {MinimumRaycastDistance * 0.5f, 0.0f, 0.0f}).Status ==
				PhysicsOperationStatus::InvalidDescription,
			"near-zero direction fails the bounded native contract"
		);
		Check(
			WorkspaceValue->ResolveRaycast({std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f})
					.Status == PhysicsOperationStatus::InvalidDescription,
			"non-finite origin fails the bounded native contract"
		);
		Check(
			WorkspaceValue->ResolveRaycast({}, {std::numeric_limits<float>::infinity(), 0.0f, 0.0f}).Status ==
				PhysicsOperationStatus::InvalidDescription,
			"non-finite direction fails the bounded native contract"
		);
		Check(
			WorkspaceValue->ResolveRaycast({}, {MaximumRaycastDistance + 1.0f, 0.0f, 0.0f}).Status ==
				PhysicsOperationStatus::InvalidDescription,
			"oversized ray distance fails the bounded native contract"
		);

		auto FolderValue = std::make_shared<Folder>();
		FolderValue->SetName("FilterRoot");
		FolderValue->SetParent(WorkspaceValue);
		NearPart->SetParent(FolderValue);
		RaycastParams Exclude;
		Exclude.FilterDescendantsInstances.Values = {FolderValue, FolderValue};
		auto Excluded = WorkspaceValue->ResolveRaycast({}, {12.0f, 0.0f, 0.0f}, Exclude);
		Check(
			Excluded.Succeeded() && Excluded.Instance == FarPart,
			"exclude filtering deduplicates roots and applies descendant semantics"
		);
		RaycastParams Include;
		Include.FilterType = Enums::RaycastFilterType::Include;
		Include.FilterDescendantsInstances.Values = {FolderValue};
		auto Included = WorkspaceValue->ResolveRaycast({}, {12.0f, 0.0f, 0.0f}, Include);
		Check(
			Included.Succeeded() && Included.Instance == NearPart,
			"include filtering admits only colliders under semantic roots"
		);

		auto ForeignWorkspace = MakeWorkspace();
		auto Foreign = MakeRayPart(ForeignWorkspace, "Foreign", {3.0f, 0.0f, 0.0f});
		RaycastParams CrossWorld;
		CrossWorld.FilterDescendantsInstances.Values = {Foreign};
		Check(
			WorkspaceValue->ResolveRaycast({}, {12.0f, 0.0f, 0.0f}, CrossWorld).Status ==
				PhysicsOperationStatus::InvalidDescription,
			"cross-world filter roots fail closed"
		);
		RaycastParams Destroyed;
		Destroyed.FilterDescendantsInstances.Values = {NearPart};
		NearPart->Destroy();
		Check(
			WorkspaceValue->ResolveRaycast({}, {12.0f, 0.0f, 0.0f}, Destroyed).Status ==
				PhysicsOperationStatus::InvalidDescription,
			"destroyed filter roots fail without dereferencing stale identity"
		);
		auto Replacement = MakeRayPart(WorkspaceValue, "Replacement", {5.0f, 0.0f, 0.0f});
		auto ReplacementHit = WorkspaceValue->ResolveRaycast({}, {12.0f, 0.0f, 0.0f});
		Check(
			ReplacementHit.Succeeded() && ReplacementHit.Instance == Replacement && ReplacementHit.Instance != NearPart,
			"destroyed collider identity cannot resolve to replacement storage"
		);

		Replacement->SetCFrame(CFrame(glm::vec3{20.0f, 0.0f, 0.0f}) * CFrame::Angles(0.0f, 0.785398163f, 0.0f));
		Replacement->SetSize({8.0f, 2.0f, 2.0f});
		auto MovedMiss = WorkspaceValue->ResolveRaycast({}, {12.0f, 0.0f, 0.0f});
		Check(
			MovedMiss.Succeeded() && MovedMiss.Instance == FarPart,
			"query safe point observes committed CFrame and Size together before casting"
		);
		auto RotatedHit = WorkspaceValue->ResolveRaycast({}, {30.0f, 0.0f, 0.0f});
		Check(
			RotatedHit.Succeeded() && RotatedHit.HasHit() && std::isfinite(RotatedHit.Distance) &&
				Near(glm::length(RotatedHit.Normal), 1.0f),
			"rotated Box3D collider produces a finite normalized semantic hit"
		);
	}

	void TestPrimitiveRaycastsAndDeterministicTie() {
		using namespace gargantuan;
		for (const auto [Shape, Name] : std::array{
				 std::pair{Enums::PartType::Block, "box"},
				 std::pair{Enums::PartType::Ball, "ball"},
				 std::pair{Enums::PartType::Cylinder, "cylinder"},
				 std::pair{Enums::PartType::Wedge, "wedge"},
				 std::pair{Enums::PartType::CornerWedge, "corner wedge"}
			 }) {
			auto WorkspaceValue = MakeWorkspace();
			auto PartValue = MakeRayPart(WorkspaceValue, Name, {5.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, Shape);
			auto Hit = WorkspaceValue->ResolveRaycast({}, {10.0f, 0.0f, 0.0f});
			Check(
				Hit.Succeeded() && Hit.Instance == PartValue && std::isfinite(Hit.Distance),
				"canonical rigid primitive participates in the real Box3D raycast"
			);
		}

		auto WorkspaceValue = MakeWorkspace();
		auto First = MakeRayPart(WorkspaceValue, "TieFirst", {5.0f, 0.0f, 0.0f});
		auto Second = MakeRayPart(WorkspaceValue, "TieSecond", {5.0f, 0.0f, 0.0f});
		auto Expected = First->GetObjectId() < Second->GetObjectId() ? First : Second;
		for (int Iteration = 0; Iteration < 64; ++Iteration) {
			auto Hit = WorkspaceValue->ResolveRaycast({}, {10.0f, 0.0f, 0.0f});
			Check(Hit.Instance == Expected, "equal-distance ray hits use stable generation-safe identity ordering");
		}
	}

	void TestGameplayLuauRaycastStress() {
		using namespace gargantuan;
		auto Game = std::make_shared<DataModel>();
		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		auto Target = MakeRayPart(WorkspaceValue, "RayTarget", {5.0f, 0.0f, 0.0f});
		ScriptEngine Engine(Game);
		const std::string Source = R"(
			local Workspace = game:GetService("Workspace")
			local Params = RaycastParams.new()
			assert(Params.CollisionGroup == 0 and Params.CollisionMask == 65535)
			assert(not pcall(function() Params.CollisionGroup = 16 end))
			assert(not pcall(function() Params.CollisionMask = 65536 end))
			assert(Workspace.RayTarget:GetMass() >= 0)
			assert(Workspace.RayTarget:GetLinearVelocity() == Vector3.zero)
			assert(Workspace.RayTarget:GetAngularVelocity() == Vector3.zero)
			assert(Workspace.RayTarget:GetLocalInertia().X >= 0)
			assert(type(Workspace.RayTarget:GetAwake()) == "boolean")
			Params.FilterType = Enum.RaycastFilterType.Exclude
			Params.FilterDescendantsInstances = {}
			local TooMany = {}
			for Index = 1, 129 do
				TooMany[Index] = Workspace.RayTarget
			end
			assert(not pcall(function() Params.FilterDescendantsInstances = TooMany end))
			assert(not pcall(function() Params.FilterDescendantsInstances = { [1] = Workspace.RayTarget, [3] = Workspace.RayTarget } end))
			assert(not pcall(function() Params.FilterDescendantsInstances = { ["1"] = Workspace.RayTarget } end))
			assert(not pcall(function() Params.FilterDescendantsInstances = { [129] = Workspace.RayTarget } end))
			assert(not pcall(function() Params.FilterDescendantsInstances = { [1.5] = Workspace.RayTarget } end))
			assert(not pcall(function() Params.FilterType = Enum.PartType.Block end))
			Params.FilterDescendantsInstances = { Workspace.RayTarget, Workspace.RayTarget }
			assert(Workspace:Raycast(Vector3.zero, Vector3.new(10, 0, 0), Params) == nil)
			Params.FilterType = Enum.RaycastFilterType.Include
			assert(Workspace:Raycast(Vector3.zero, Vector3.new(10, 0, 0), Params).Instance == Workspace.RayTarget)
			Params.FilterType = Enum.RaycastFilterType.Exclude
			Params.FilterDescendantsInstances = {}
			for Index = 1, 2000 do
				local Result = Workspace:Raycast(Vector3.zero, Vector3.new(10, 0, 0), Params)
				assert(Result and Result.Instance == Workspace.RayTarget)
				assert(math.abs(Result.Distance - 4) < 0.01)
				if Index == 1 then
					assert(not pcall(function() Result.Distance = 2 end))
				end
			end
			assert(Workspace:Raycast(Vector3.new(0, 10, 0), Vector3.new(10, 0, 0), Params) == nil)
		)";
		auto Bytecode = Luau::compile(Source);
		const int Loaded = luau_load(Engine.L, "physics-query-stress", Bytecode.data(), Bytecode.size(), 0);
		ScriptSecurityScope Security(ScriptSecurityContext::ClientRuntime());
		const int Status = Loaded == LUA_OK ? lua_pcall(Engine.L, 0, 0, 0) : Loaded;
		if (Status != LUA_OK)
			std::cerr << "LUAU ERROR: " << (lua_tostring(Engine.L, -1) ? lua_tostring(Engine.L, -1) : "unknown")
					  << '\n';
		Check(Status == LUA_OK, "normal gameplay Luau can issue repeated bounded immutable semantic raycasts");
	}
}

namespace {
	void TestAdvancedQueryBoundsAndRotation() {
		using namespace gargantuan;
		auto World=MakeWorkspace();
		auto Target=MakeRayPart(World,"RotatedTarget",{5,0,0});
		for(auto Kind:{PhysicsQueryShapeKind::Box,PhysicsQueryShapeKind::Capsule}) {
			PhysicsQueryShape Query; Query.Kind=Kind; Query.Size={.2f,12,.2f}; Query.Radius=.2f; Query.Height=12;
			Check(World->ResolveOverlap(Query).Parts.empty(),"unrotated narrow proxy misses off-axis target");
			Query.Transform=CFrame::Angles(0,0,1.57079632679f);
			Check(World->ResolveOverlap(Query).Parts==std::vector<std::shared_ptr<BasePart>>{Target},"oriented box/capsule proxy rotates local points into world axes");
			Query.Transform.Position={0,-5,0};
			Check(World->ResolveShapeCast(Query,{0,10,0}).Instance==Target,"oriented box/capsule sweeps hit off-axis target");
		}
		Target->Destroy();
		std::vector<std::shared_ptr<BasePart>> Parts;
		for(int Index=0;Index<300;++Index) Parts.push_back(MakeRayPart(World,"OverlapBound",{}));
		auto Expected=Parts;
		std::ranges::sort(Expected,{},[](const auto &Part){return Part->GetObjectId();}); Expected.resize(256);
		auto Overlap=World->ResolveOverlap(PhysicsQueryShape{});
		Check(Overlap.Succeeded() && Overlap.Truncated && Overlap.Parts==Expected,"300-owner overlap returns lowest 256 semantic identities with explicit truncation");
		for(auto &Part:Parts) Part->Destroy();
		PhysicsWorld Backend(PhysicsWorldConfig{});
		PhysicsBodyDesc Description; Description.Anchored=true;
		std::vector<PhysicsBodyId> Ids;
		for(std::size_t Index=0;Index<=MaximumRaycastCandidates;++Index) Ids.push_back(Backend.CreateBody(Description));
		Check(std::ranges::all_of(Ids,[](auto Id){return Id.IsValid();}),"candidate overflow fixture admits 4097 distinct body owners");
		auto Overflow=Backend.Overlap({PhysicsQueryShape{},{}});
		Check(!Overflow.Succeeded() && Overflow.Truncated && Overflow.Bodies.empty(),"4097-owner overlap fails closed without arbitrary partial subset");
		PhysicsQueryShape Shape; Shape.Transform=CFrame(glm::vec3(-5,0,0));
		auto CastOverflow=Backend.ShapeCast({Shape,{10,0,0},{}});
		Check(!CastOverflow.Succeeded() && CastOverflow.CandidatesTruncated && CastOverflow.Candidates.empty(),"4097-owner cast fails closed at candidate cap");
		for(auto Id:Ids) Check(Backend.DestroyBody(Id).Succeeded(),"overflow query fixture retires all native owners");
	}
	void TestAdvancedWorldQueries() {
		using namespace gargantuan;
		auto Game=std::make_shared<DataModel>();
		auto World=std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		auto First=MakeRayPart(World,"QueryFirst",{5,0,0});
		auto Second=MakeRayPart(World,"QuerySecond",{5,0,0});
		for (auto Kind:{PhysicsQueryShapeKind::Sphere,PhysicsQueryShapeKind::Capsule,PhysicsQueryShapeKind::Box}) {
			PhysicsQueryShape Shape;
			Shape.Kind=Kind;
			Shape.Radius=.5f; Shape.Height=2; Shape.Size={1,2,1};
			auto Hit=World->ResolveShapeCast(Shape,{10,0,0});
			auto Expected=First->GetObjectId()<Second->GetObjectId()?First:Second;
			Check(Hit.Succeeded() && Hit.Instance==Expected && std::abs(Hit.Distance-3.5f)<.05f && std::abs(glm::length(Hit.Normal)-1)<.001f,"sphere/capsule/box casts return semantic nearest stable hit and unit normal");
			Shape.Transform=CFrame(glm::vec3(5,0,0));
			auto Overlap=World->ResolveOverlap(Shape,{},1);
			Check(Overlap.Succeeded() && Overlap.Parts.size()==1 && Overlap.Parts.front()==Expected && Overlap.Truncated,"overlap sorts semantic identities before explicit deterministic truncation");
			RaycastParams Params;
			Params.FilterType=Enums::RaycastFilterType::Include;
			Params.FilterDescendantsInstances.Values={Second};
			auto Included=World->ResolveOverlap(Shape,Params);
			Check(Included.Succeeded() && Included.Parts.size()==1 && Included.Parts.front()==Second,"advanced query honors descendant inclusion");
			Params.CollisionMask=0;
			Check(World->ResolveOverlap(Shape,Params).Parts.empty(),"advanced query honors native and neutral collision masks");
			Check(!World->ResolveOverlap(Shape,{},0).Succeeded(),"overlap rejects zero result limit");
			Shape.Radius=-1; Shape.Size={-1,1,1};
			Check(!World->ResolveShapeCast(Shape,{10,0,0}).Succeeded(),"invalid query shape fails before native dispatch");
		}
		ScriptEngine Engine(Game);
		const auto Bytecode=Luau::compile(R"(
			local W=game:GetService("Workspace")
			assert(W:SphereCast(Vector3.zero,.5,Vector3.new(10,0,0)).Distance>3)
			assert(W:CapsuleCast(CFrame.new(),.5,2,Vector3.new(10,0,0)).Instance)
			assert(W:BoxCast(CFrame.new(),Vector3.new(1,2,1),Vector3.new(10,0,0)).Instance)
			for _, R in { W:GetPartsInSphere(Vector3.new(5,0,0),1,nil,1), W:GetPartsInCapsule(CFrame.new(5,0,0),.5,2,nil,1), W:GetPartsInBox(CFrame.new(5,0,0),Vector3.new(2,2,2),nil,1) } do
				assert(#R.Parts==1 and R.Truncated)
				assert(not pcall(function() R.Truncated=false end))
				assert(not pcall(function() R.Parts[1]=nil end))
			end
			assert(not pcall(function() W:GetPartsInSphere(Vector3.zero,1,nil,257) end))
			assert(not pcall(function() W:SphereCast(Vector3.zero,-1,Vector3.new(10,0,0)) end))
		)");
		const int Loaded=luau_load(Engine.L,"advanced-physics-queries",Bytecode.data(),Bytecode.size(),0);
		ScriptSecurityScope Security(ScriptSecurityContext::ClientRuntime());
		const int Status=Loaded==LUA_OK?lua_pcall(Engine.L,0,0,0):Loaded;
		if (Status!=LUA_OK) std::cerr << "[Physics:Query] " << lua_tostring(Engine.L,-1) << '\n';
		Check(Status==LUA_OK,"Luau authored advanced queries expose bounded immutable results");
		First->Destroy(); Second->Destroy();
		PhysicsQueryShape Shape; Shape.Transform=CFrame(glm::vec3(5,0,0));
		Check(World->ResolveOverlap(Shape).Parts.empty(),"destroyed query owners retire at safe point");
	}
}

int main() {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
	} catch (const std::exception &Exception) {
		std::cerr << "Runtime schema bootstrap failed: " << Exception.what() << '\n';
		return 1;
	}
	if (const auto *DenseOnly = std::getenv("GARGANTUAN_DENSE_CONTACT_REGRESSION");
		DenseOnly && DenseOnly[0] == '1') {
		TestDenseMeshContactCandidateOverflow();
		return Failures == 0 ? 0 : 1;
	}
	TestNeutralContract();
	TestAdvancedWorldQueries();
	TestAdvancedQueryBoundsAndRotation();
	TestBackendIdentityAndLifecycle();
	TestAdvancedConstraintBackend();
	TestBoundedConstraintAdmission();
	TestStaticTriangleMeshBackend();
	TestDenseMeshContactCandidateOverflow();
	TestDetachedWorldRootKinematicMotion();
	TestChunkedMeshShapeLifecycle();
	TestChunkSeamContactEvents();
	TestMeshCollisionResidencyLimits();
	TestKinematicWorldQueries();
	TestCommittedBodyUpdates();
	TestPartAdmissionMembership();
	TestMeshPartCollisionFallback();
	TestConstraintAndPendingDestroy();
	TestAuthoredJointLifecycle();
	TestGameplayLuauJointAssembly();
	TestSimulationPublicationAndImpulse();
	TestPhysicsPublicationNestedMutation();
	TestBodyDynamicsAndCollisionGroups();
	TestWorldRootDynamicsProjection();
	TestBodyControlCapabilityBoundary();
	TestForceDurationAndKinematicMotion();
	TestBodyControlPersistence();
	TestContinuousCollisionPolicy();
	TestWorldRootContinuousCollisionProjection();
	TestTouchAndSensorUpdates();
	TestStaticWorldStepReference();
	TestSemanticRaycastContract();
	TestPrimitiveRaycastsAndDeterministicTie();
	TestGameplayLuauRaycastStress();
	if (Failures == 0) std::cout << "All physics backend tests passed\n";
	return Failures == 0 ? 0 : 1;
}
