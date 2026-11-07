#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/JointConstraint.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/physics/PhysicsQueryTypes.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "Psapi.lib")
#elif defined(__linux__)
#include <fstream>
#include <unistd.h>
#endif

namespace gargantuan {
	struct WorldRootTestAccess {
		static std::size_t Bodies(const WorldRoot &World) { return World.PartBodies.size(); }
		static std::size_t Constraints(const WorldRoot &World) { return World.ConstraintJoints.size(); }
		static std::optional<PhysicsBodyState> State(const WorldRoot &World, const BasePart &Part) {
			const auto Found = World.PartBodies.find(Part.GetObjectId());
			if (Found == World.PartBodies.end()) return std::nullopt;
			return World.Physics.GetBodyState(Found->second);
		}
	};
}

namespace {
	using namespace gargantuan;
	using Clock = std::chrono::steady_clock;
	using Json = nlohmann::ordered_json;

	void Require(bool Condition, std::string_view Message) {
		if (!Condition) throw std::runtime_error("[Physics:ExpansionBenchmark] " + std::string(Message));
	}

	double Milliseconds(Clock::duration Duration) {
		return std::chrono::duration<double, std::milli>(Duration).count();
	}

	Json Summary(std::vector<double> Values) {
		Require(!Values.empty(), "empty timing series");
		std::ranges::sort(Values);
		auto At = [&](double Fraction) {
			const auto Position = static_cast<std::size_t>(std::ceil(Fraction * Values.size())) - 1;
			return Values[std::min(Position, Values.size() - 1)];
		};
		return {{"P50Ms", At(0.50)}, {"P95Ms", At(0.95)}, {"P99Ms", At(0.99)},
			{"MaximumMs", Values.back()}};
	}

	Json ResidentBytes() {
#if defined(_WIN32)
		PROCESS_MEMORY_COUNTERS Counters{};
		Counters.cb = sizeof(Counters);
		if (GetProcessMemoryInfo(GetCurrentProcess(), &Counters, sizeof(Counters)))
			return static_cast<std::uint64_t>(Counters.WorkingSetSize);
#elif defined(__linux__)
		std::ifstream Input("/proc/self/statm");
		std::uint64_t Pages = 0, Resident = 0;
		if (Input >> Pages >> Resident) return Resident * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
#endif
		return nullptr;
	}

	std::shared_ptr<Workspace> GetWorkspace(const std::shared_ptr<DataModel> &Game) {
		auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		Require(World != nullptr, "Workspace unavailable");
		return World;
	}

	std::shared_ptr<Part> MakePart(glm::vec3 Position, glm::vec3 Size, bool Anchored) {
		auto Body = std::make_shared<Part>();
		Body->SetCFrame(CFrame(Position));
		Body->SetSize(Size);
		Body->SetAnchored(Anchored);
		return Body;
	}

	Json RunQueries(int Iterations) {
		auto Game = std::make_shared<DataModel>();
		auto World = GetWorkspace(Game);
		auto First = MakePart({5.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, true);
		auto Second = MakePart({5.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, true);
		First->SetParent(World); Second->SetParent(World);
		World->StepPhysics(0.0, std::nullopt);
		Require(WorldRootTestAccess::Bodies(*World) == 2, "query bodies not admitted");
		const auto Expected = First->GetObjectId() < Second->GetObjectId() ? First : Second;
		const auto ResidentBefore = ResidentBytes();
		Json Rows = Json::array();
		for (const auto Kind : {PhysicsQueryShapeKind::Sphere, PhysicsQueryShapeKind::Capsule,
			PhysicsQueryShapeKind::Box}) {
			PhysicsQueryShape Shape;
			Shape.Kind = Kind;
			Shape.Radius = 0.5f;
			Shape.Height = 2.0f;
			Shape.Size = {1.0f, 2.0f, 1.0f};
			std::vector<double> Cast, Overlap;
			Cast.reserve(Iterations); Overlap.reserve(Iterations);
			for (int Index = 0; Index < Iterations; ++Index) {
				const auto CastStart = Clock::now();
				const auto Hit = World->ResolveShapeCast(Shape, {10.0f, 0.0f, 0.0f});
				Cast.push_back(Milliseconds(Clock::now() - CastStart));
				Require(Hit.Succeeded() && Hit.Instance == Expected &&
					std::abs(Hit.Distance - 3.5f) < 0.05f &&
					std::abs(glm::length(Hit.Normal) - 1.0f) < 0.001f,
					"cast did not return stable nearest semantic hit");
				Shape.Transform = CFrame(5.0f, 0.0f, 0.0f);
				const auto OverlapStart = Clock::now();
				const auto Result = World->ResolveOverlap(Shape, {}, 1);
				Overlap.push_back(Milliseconds(Clock::now() - OverlapStart));
				Require(Result.Succeeded() && Result.Parts.size() == 1 &&
					Result.Parts.front() == Expected && Result.Truncated,
					"overlap did not enforce stable result cap");
				Shape.Transform = CFrame();
			}
			Rows.push_back({{"Shape", Kind == PhysicsQueryShapeKind::Sphere ? "Sphere"
				: Kind == PhysicsQueryShapeKind::Capsule ? "Capsule" : "Box"},
				{"Iterations", Iterations}, {"Cast", Summary(Cast)}, {"OverlapLimit1", Summary(Overlap)}});
		}
		PhysicsQueryShape LimitShape;
		LimitShape.Transform = CFrame(5.0f, 0.0f, 0.0f);
		Require(!World->ResolveOverlap(LimitShape, {}, MaximumPhysicsOverlapResults + 1).Succeeded(),
			"overlap accepted a limit above its cap");
		const auto ResidentAfter = ResidentBytes();
		First->Destroy(); Second->Destroy();
		Require(WorldRootTestAccess::Bodies(*World) == 0 && World->ResolveOverlap(LimitShape).Parts.empty(),
			"query bodies did not retire");
		First.reset(); Second.reset();
		const auto ResidentAfterRetire = ResidentBytes();
		Game->Destroy();
		return {{"Case", "advanced-queries"}, {"Status", "measured-after-only"}, {"Bodies", 2},
			{"Rows", Rows}, {"ResidentBytesBefore", ResidentBefore}, {"ResidentBytesAfter", ResidentAfter},
			{"ResidentBytesAfterRetire", ResidentAfterRetire}, {"RetiredBodies", 2},
			{"AllocationCount", nullptr}, {"Validation", "stable cast hit, overlap truncation and cap, retirement"}};
	}

	Json RunCcd(bool Enabled, int Trials) {
		auto Game = std::make_shared<DataModel>();
		auto World = GetWorkspace(Game);
		World->SetGravity(0.0f);
		const auto ResidentBefore = ResidentBytes();
		std::vector<double> Step;
		Step.reserve(Trials);
		int Stopped = 0, TouchEvents = 0;
		double PositionSum = 0.0;
		for (int Trial = 0; Trial < Trials; ++Trial) {
			auto Wall = MakePart({0.0f, 0.0f, 0.0f}, {0.1f, 4.0f, 4.0f}, false);
			auto Ball = MakePart({-3.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f}, false);
			Ball->SetShape(Enums::PartType::Ball);
			Ball->SetMovingBodyCCD(Enabled);
			Wall->SetParent(World); Ball->SetParent(World);
			World->StepPhysics(0.0, std::nullopt);
			Require(WorldRootTestAccess::Bodies(*World) == 2, "CCD scene not admitted");
			const auto State = WorldRootTestAccess::State(*World, *Ball);
			Require(State && State->Description.MovingBodyCCD == Enabled, "CCD policy not projected to backend");
			int TrialTouches = 0;
			Ball->Touched->Connect([&](std::shared_ptr<BasePart>) { ++TrialTouches; });
			Wall->SetLinearVelocity({-2.0f, 0.0f, 0.0f});
			Ball->SetLinearVelocity({360.0f, 0.0f, 0.0f});
			const auto Started = Clock::now();
			World->StepPhysics(1.0 / 60.0, std::nullopt);
			Step.push_back(Milliseconds(Clock::now() - Started));
			const float FinalX = Ball->GetCFrame().Position.x;
			Require(std::isfinite(FinalX), "CCD scene published nonfinite position");
			if (FinalX < 1.0f) ++Stopped;
			TouchEvents += TrialTouches;
			PositionSum += FinalX;
			Wall->Destroy(); Ball->Destroy();
			Require(WorldRootTestAccess::Bodies(*World) == 0, "CCD bodies did not retire");
		}
		Require(!Enabled || Stopped == Trials, "enabled CCD missed a moving thin obstacle");
		const auto ResidentAfter = ResidentBytes();
		Game->Destroy();
		return {{"Case", Enabled ? "moving-body-ccd-on" : "moving-body-ccd-off"},
			{"Status", "measured-after-only"}, {"Trials", Trials}, {"ProjectileSpeed", 360.0},
			{"TargetSpeed", -2.0}, {"Step", Summary(Step)}, {"StoppedBeforeX1", Stopped},
			{"TouchEvents", TouchEvents}, {"MeanFinalX", PositionSum / Trials},
			{"RetiredBodies", 2 * Trials},
			{"ResidentBytesBefore", ResidentBefore}, {"ResidentBytesAfter", ResidentAfter},
			{"AllocationCount", nullptr},
			{"Validation", Enabled ? "effective CCD and every projectile stopped" : "finite pose; outcome reported without quality equivalence"}};
	}

	std::shared_ptr<JointConstraint> MakeJoint(const std::shared_ptr<Workspace> &World,
		std::string_view Kind, const std::shared_ptr<Part> &A, const std::shared_ptr<Part> &B,
		glm::vec3 FrameA, glm::vec3 FrameB, float Lower, float Upper, float Speed) {
		auto AttachmentA = std::make_shared<Attachment>();
		auto AttachmentB = std::make_shared<Attachment>();
		AttachmentA->SetCFrame(CFrame(FrameA)); AttachmentB->SetCFrame(CFrame(FrameB));
		AttachmentA->SetParent(A); AttachmentB->SetParent(B);
		auto Joint = std::make_shared<JointConstraint>();
		Joint->SetJointType(std::string(Kind));
		Joint->SetAttachment0(AttachmentA); Joint->SetAttachment1(AttachmentB);
		Joint->SetCollideConnected(false);
		Joint->SetEnableLimit(true); Joint->SetLowerLimit(Lower); Joint->SetUpperLimit(Upper);
		Joint->SetEnableMotor(true); Joint->SetMotorSpeed(Speed); Joint->SetMaximumMotorForce(1000.0f);
		Joint->SetParent(World);
		return Joint;
	}

	Json RunMotors(int Frames) {
		auto Game = std::make_shared<DataModel>();
		auto World = GetWorkspace(Game);
		World->SetGravity(0.0f);
		auto Frame = MakePart({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, true);
		auto Door = MakePart({2.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, false);
		auto Rail = MakePart({10.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, true);
		auto Slide = MakePart({10.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, false);
		Frame->SetParent(World); Door->SetParent(World);
		Rail->SetParent(World); Slide->SetParent(World);
		auto Hinge = MakeJoint(World, "Hinge", Frame, Door,
			{0.5f, 0.0f, 0.0f}, {-1.5f, 0.0f, 0.0f}, -0.4f, 0.4f, 2.0f);
		auto Slider = MakeJoint(World, "Slider", Rail, Slide,
			{}, {}, -1.0f, 1.0f, 3.0f);
		World->StepPhysics(0.0, std::nullopt);
		Require(WorldRootTestAccess::Bodies(*World) == 4 && WorldRootTestAccess::Constraints(*World) == 2,
			"motorized joints not admitted");
		const auto ResidentBefore = ResidentBytes();
		std::vector<double> Step;
		Step.reserve(Frames);
		for (int Index = 0; Index < Frames; ++Index) {
			const auto Started = Clock::now();
			World->StepPhysics(1.0 / 60.0, std::nullopt);
			Step.push_back(Milliseconds(Clock::now() - Started));
		}
		const auto &Pose = Door->GetCFrame();
		const float Angle = std::atan2(Pose.Rotation[0][1], Pose.Rotation[0][0]);
		const float SlideX = Slide->GetCFrame().Position.x;
		Require(std::isfinite(Angle) && std::abs(Angle) > 0.05f && std::abs(Angle) < 0.7f,
			"hinge motor failed to move within limit");
		Require(std::isfinite(SlideX) && SlideX > 10.1f && SlideX < 11.3f &&
			std::abs(Slide->GetCFrame().Position.y) < 0.2f,
			"slider motor failed to move within limit");
		const auto ResidentAfter = ResidentBytes();
		Hinge->Destroy(); Slider->Destroy();
		Frame->Destroy(); Door->Destroy(); Rail->Destroy(); Slide->Destroy();
		Require(WorldRootTestAccess::Bodies(*World) == 0 && WorldRootTestAccess::Constraints(*World) == 0,
			"motor scene retained body or joint");
		Frame.reset(); Door.reset(); Rail.reset(); Slide.reset(); Hinge.reset(); Slider.reset();
		const auto ResidentAfterRetire = ResidentBytes();
		Game->Destroy();
		return {{"Case", "hinge-slider-motors"}, {"Status", "measured-after-only"},
			{"Bodies", 4}, {"Constraints", 2}, {"Frames", Frames}, {"Step", Summary(Step)},
			{"FinalHingeAngle", Angle}, {"FinalSliderX", SlideX},
			{"ResidentBytesBefore", ResidentBefore}, {"ResidentBytesAfter", ResidentAfter},
			{"ResidentBytesAfterRetire", ResidentAfterRetire}, {"RetiredBodies", 4}, {"RetiredConstraints", 2},
			{"AllocationCount", nullptr}, {"Validation", "motor motion within limits; complete retirement"}};
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		bool Quick = false;
		std::string CaseFilter;
		for (int Index = 1; Index < ArgumentCount; ++Index) {
			const std::string_view Argument(Arguments[Index]);
			if (Argument == "--quick") Quick = true;
			else if (Argument.starts_with("--case=")) CaseFilter = Argument.substr(7);
			else throw std::invalid_argument("unknown argument: " + std::string(Argument));
		}
		Require(CaseFilter.empty() || CaseFilter == "advanced-queries" || CaseFilter == "moving-body-ccd" ||
			CaseFilter == "hinge-slider-motors", "unknown case filter");
		std::cout << Json{{"Type", "PhysicsExpansionBenchmark"}, {"Scope", "EngineWorldRoot"},
			{"SourceCapability", "advanced APIs only; pre-feature status is unsupported"},
			{"FixedDeltaSeconds", 1.0 / 60.0}, {"AllocationCount", nullptr}}.dump() << '\n';
		if (CaseFilter.empty() || CaseFilter == "advanced-queries")
			std::cout << RunQueries(Quick ? 8 : 128).dump() << '\n';
		if (CaseFilter.empty() || CaseFilter == "moving-body-ccd") {
			std::cout << RunCcd(false, Quick ? 2 : 32).dump() << '\n';
			std::cout << RunCcd(true, Quick ? 2 : 32).dump() << '\n';
		}
		if (CaseFilter.empty() || CaseFilter == "hinge-slider-motors")
			std::cout << RunMotors(Quick ? 20 : 120).dump() << '\n';
		return 0;
	} catch (const std::exception &Failure) {
		std::cerr << "[Physics:ExpansionBenchmark] " << Failure.what() << '\n';
		return 1;
	}
}
