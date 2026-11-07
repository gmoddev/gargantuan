#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/WeldConstraint.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "../src/runtime/RuntimeWorkDiagnostics.hpp"

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
	};
}

namespace {
	using namespace gargantuan;
	using Clock = std::chrono::steady_clock;
	using Json = nlohmann::ordered_json;

	double Milliseconds(Clock::duration Duration) {
		return std::chrono::duration<double, std::milli>(Duration).count();
	}

	Json Summarize(std::vector<double> Values) {
		if (Values.empty()) throw std::runtime_error("[Physics:FoundationBenchmark] no timing samples");
		std::ranges::sort(Values);
		auto At = [&](double Fraction) {
			const auto Position = static_cast<std::size_t>(std::ceil(Fraction * Values.size())) - 1;
			return Values[std::min(Position, Values.size() - 1)];
		};
		return {{"P50Ms", At(0.50)}, {"P95Ms", At(0.95)}, {"P99Ms", At(0.99)},
			{"MaximumMs", Values.back()}};
	}

	std::optional<std::uint64_t> ResidentBytes() {
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
		return std::nullopt;
	}

	Json EncodeBytes(std::optional<std::uint64_t> Bytes) {
		return Bytes ? Json(*Bytes) : Json(nullptr);
	}

	void Require(bool Condition, std::string_view Message) {
		if (!Condition) throw std::runtime_error("[Physics:FoundationBenchmark] " + std::string(Message));
	}

	std::shared_ptr<Workspace> GetWorkspace(const std::shared_ptr<DataModel> &Game) {
		auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		Require(World != nullptr, "Workspace unavailable");
		return World;
	}

	std::shared_ptr<Part> MakePart(glm::vec3 Position, bool Anchored, glm::vec3 Size = {1.0f}) {
		auto Body = std::make_shared<Part>();
		Body->SetAnchored(Anchored);
		Body->SetSize(Size);
		Body->SetCFrame(CFrame(Position));
		return Body;
	}

	void CheckFinite(const Part &Body) {
		const auto Position = Body.GetCFrame().Position;
		Require(std::isfinite(Position.x) && std::isfinite(Position.y) && std::isfinite(Position.z),
			"simulation published a nonfinite position");
	}

	Json RunStepCase(std::string_view Name, int Warmup, int Frames, bool Constrained) {
		auto Game = std::make_shared<DataModel>();
		auto World = GetWorkspace(Game);
		World->SetGravity(Constrained ? 20.0f : 196.2f);
		auto Group = std::make_shared<Folder>();
		auto Floor = MakePart({0.0f, -1.0f, 0.0f}, true, {64.0f, 2.0f, 64.0f});
		Floor->SetParent(Group);
		std::vector<std::shared_ptr<Part>> Bodies;
		const std::size_t Count = Constrained ? 32 : 128;
		Bodies.reserve(Count);
		for (std::size_t Index = 0; Index < Count; ++Index) {
			const auto Position = Constrained
				? glm::vec3{static_cast<float>(Index % 8) * 2.0f - 7.0f, 5.0f + static_cast<float>(Index / 8) * 2.0f, 0.0f}
				: glm::vec3{static_cast<float>(Index % 8) * 1.02f - 3.5f,
					2.0f + static_cast<float>(Index / 64) * 1.02f,
					static_cast<float>((Index / 8) % 8) * 1.02f - 3.5f};
			auto Body = MakePart(Position, Constrained && Index == 0);
			Body->SetParent(Group);
			Bodies.push_back(std::move(Body));
		}
		if (Constrained) {
			for (std::size_t Index = 1; Index < Count; ++Index) {
				auto Weld = std::make_shared<WeldConstraint>();
				Weld->SetPart0(Bodies[Index - 1]);
				Weld->SetPart1(Bodies[Index]);
				Weld->SetParent(Group);
			}
		}
		Group->SetParent(World);
		World->StepPhysics(0.0, std::nullopt);
		Require(WorldRootTestAccess::Bodies(*World) == Count + 1, "body admission mismatch");
		Require(WorldRootTestAccess::Constraints(*World) == (Constrained ? Count - 1 : 0),
			"constraint admission mismatch");
		const auto InitialY = Bodies.back()->GetCFrame().Position.y;
		const auto ResidentAtStart = ResidentBytes();
		std::vector<double> WorldStep, RigidStep, BackendStep;
		WorldStep.reserve(Frames); RigidStep.reserve(Frames); BackendStep.reserve(Frames);
		for (int Frame = 0; Frame < Warmup + Frames; ++Frame) {
			runtime_detail::WorkSample Sample{};
			const auto Started = Clock::now();
			{
				runtime_detail::WorkCapture Capture(&Sample);
				World->StepPhysics(1.0 / 60.0, std::nullopt);
			}
			if (Frame < Warmup) continue;
			const auto Rigid = Sample[static_cast<std::size_t>(runtime_detail::WorkPhase::PhysicsRigidStep)];
			const auto Backend = Sample[static_cast<std::size_t>(runtime_detail::WorkPhase::PhysicsBackendStep)];
			Require(Rigid.Calls > 0 && Backend.Calls > 0, "rigid/backend step was not executed");
			WorldStep.push_back(Milliseconds(Clock::now() - Started));
			RigidStep.push_back(static_cast<double>(Rigid.Nanoseconds) / 1e6);
			BackendStep.push_back(static_cast<double>(Backend.Nanoseconds) / 1e6);
		}
		for (const auto &Body : Bodies) {
			CheckFinite(*Body);
			Require(Body->GetCFrame().Position.y > (Constrained ? -0.25f : 0.4f),
				"body penetrated floor");
		}
		if (Constrained) {
			Require(std::abs(Bodies.front()->GetCFrame().Position.y - 5.0f) < 0.01f,
				"anchored constraint endpoint moved");
			const auto Offset = Bodies[1]->GetCFrame().Position - Bodies[0]->GetCFrame().Position;
			Require(std::abs(glm::length(Offset) - 2.0f) < 0.5f,
				"weld failed to retain its endpoint separation");
			Require(WorldRootTestAccess::Constraints(*World) == Count - 1,
				"constraint disappeared during simulation");
		} else Require(Bodies.back()->GetCFrame().Position.y < InitialY - 0.01f,
			"dynamic body did not respond to gravity");
		const auto ResidentAtEnd = ResidentBytes();
		Group->Destroy();
		Require(WorldRootTestAccess::Bodies(*World) == 0 && WorldRootTestAccess::Constraints(*World) == 0,
			"world retained body or constraint after destruction");
		Bodies.clear();
		Floor.reset();
		Group.reset();
		const auto ResidentAfterDestroy = ResidentBytes();
		Game->Destroy();
		return {{"Case", std::string(Name)}, {"Bodies", Count + 1}, {"Constraints", Constrained ? Count - 1 : 0},
			{"WarmupFrames", Warmup}, {"MeasuredFrames", Frames}, {"WorldStep", Summarize(WorldStep)},
			{"RigidStep", Summarize(RigidStep)}, {"BackendStep", Summarize(BackendStep)},
			{"ResidentBytesBefore", EncodeBytes(ResidentAtStart)}, {"ResidentBytesAfter", EncodeBytes(ResidentAtEnd)},
			{"ResidentBytesAfterDestroy", EncodeBytes(ResidentAfterDestroy)}, {"AllocationCount", nullptr},
			{"Validation", Constrained ? "finite positions, weld separation, joint retention, identity retirement"
				: "finite positions, floor separation, gravity response, identity retirement"}};
	}

	Json RunChurnCase(int Cycles) {
		auto Game = std::make_shared<DataModel>();
		auto World = GetWorkspace(Game);
		World->SetGravity(0.0f);
		const auto ResidentAtStart = ResidentBytes();
		std::vector<double> Attach, Step, Destroy;
		Attach.reserve(Cycles); Step.reserve(Cycles); Destroy.reserve(Cycles);
		for (int Cycle = 0; Cycle < Cycles; ++Cycle) {
			auto Group = std::make_shared<Folder>();
			for (int Index = 0; Index < 32; ++Index) {
				auto Body = MakePart({static_cast<float>(Index * 3), 5.0f, 0.0f}, false);
				Body->SetParent(Group);
			}
			const auto AttachStart = Clock::now();
			Group->SetParent(World);
			World->StepPhysics(0.0, std::nullopt);
			Attach.push_back(Milliseconds(Clock::now() - AttachStart));
			Require(WorldRootTestAccess::Bodies(*World) == 32, "churn body admission mismatch");
			const auto StepStart = Clock::now();
			World->StepPhysics(1.0 / 60.0, std::nullopt);
			Step.push_back(Milliseconds(Clock::now() - StepStart));
			const auto DestroyStart = Clock::now();
			Group->Destroy();
			Destroy.push_back(Milliseconds(Clock::now() - DestroyStart));
			Require(WorldRootTestAccess::Bodies(*World) == 0, "churn retained retired bodies");
		}
		const auto ResidentAtEnd = ResidentBytes();
		Game->Destroy();
		return {{"Case", "body-churn"}, {"BodiesPerCycle", 32}, {"Cycles", Cycles},
			{"AttachAndAdmission", Summarize(Attach)}, {"Step", Summarize(Step)},
			{"DestroyAndRetire", Summarize(Destroy)}, {"ResidentBytesBefore", EncodeBytes(ResidentAtStart)},
			{"ResidentBytesAfter", EncodeBytes(ResidentAtEnd)}, {"AllocationCount", nullptr},
			{"Validation", "body count returns to zero every cycle"}};
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
		Require(CaseFilter.empty() || CaseFilter == "dense-contacts" || CaseFilter == "weld-chain" ||
			CaseFilter == "body-churn", "unknown case filter");
		std::cout << nlohmann::ordered_json{{"Type", "PhysicsFoundationBenchmark"},
			{"FixedDeltaSeconds", 1.0 / 60.0}, {"Scope", "EngineWorldRoot"},
			{"ResidentMemoryMetric", "process working set or RSS; includes allocator and unrelated process state"},
			{"AllocationCountMetric", "unavailable"}}.dump() << '\n';
		if (CaseFilter.empty() || CaseFilter == "dense-contacts")
			std::cout << RunStepCase("dense-contacts", Quick ? 2 : 30, Quick ? 8 : 120, false).dump() << '\n';
		if (CaseFilter.empty() || CaseFilter == "weld-chain")
			std::cout << RunStepCase("weld-chain", Quick ? 2 : 30, Quick ? 8 : 120, true).dump() << '\n';
		if (CaseFilter.empty() || CaseFilter == "body-churn")
			std::cout << RunChurnCase(Quick ? 2 : 30).dump() << '\n';
		return 0;
	} catch (const std::exception &Failure) {
		std::cerr << "[Physics:FoundationBenchmark] " << Failure.what() << '\n';
		return 1;
	}
}
