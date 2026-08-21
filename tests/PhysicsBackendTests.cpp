#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/WeldConstraint.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/physics/PhysicsBackend.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace gargantuan {
	struct WorldRootTestAccess {
		static std::size_t BodyCount(const WorldRoot &World) { return World.PartBodies.size(); }
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
	};
}

namespace {
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

	void TestTouchAndSensorUpdates() {
		using namespace gargantuan;
		auto Workspace = MakeWorkspace();
		if (!Workspace) return;
		Workspace->SetGravity(0.0f);
		auto Sensor = std::make_shared<Part>();
		Sensor->SetAnchored(true);
		Sensor->SetCanCollide(false);
		Sensor->SetCanTouch(true);
		Sensor->SetSize({4.0f, 4.0f, 4.0f});
		auto Visitor = std::make_shared<Part>();
		Visitor->SetCFrame(CFrame(0.0f, 0.0f, 0.0f));
		Sensor->SetParent(Workspace);
		Visitor->SetParent(Workspace);
		int Touches = 0;
		Sensor->Touched->Connect([&](std::shared_ptr<BasePart>) { ++Touches; });
		Workspace->StepPhysics(1.0 / 60.0, std::nullopt);
		Check(Touches == 1, "non-colliding CanTouch body receives normalized sensor begin event");

		Visitor->SetCFrame(CFrame(20.0f, 0.0f, 0.0f));
		Workspace->StepPhysics(1.0 / 60.0, std::nullopt);
		Sensor->SetCanTouch(false);
		Visitor->SetCFrame(CFrame(0.0f, 0.0f, 0.0f));
		Workspace->StepPhysics(1.0 / 60.0, std::nullopt);
		Check(Touches == 1, "committed CanTouch false disables later sensor events");
	}
}

int main() {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
	} catch (const std::exception &Exception) {
		std::cerr << "Runtime schema bootstrap failed: " << Exception.what() << '\n';
		return 1;
	}
	TestNeutralContract();
	TestBackendIdentityAndLifecycle();
	TestKinematicWorldQueries();
	TestCommittedBodyUpdates();
	TestConstraintAndPendingDestroy();
	TestSimulationPublicationAndImpulse();
	TestTouchAndSensorUpdates();
	if (Failures == 0) std::cout << "All physics backend tests passed\n";
	return Failures == 0 ? 0 : 1;
}
