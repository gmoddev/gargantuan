#include "gargantuan/classes/Cloth.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/RubberBody.hpp"
#include "gargantuan/classes/SoftBodyAttachment.hpp"
#include "gargantuan/classes/SoftBodyMaterial.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <intrin.h>
	#include <windows.h>
	#include <psapi.h>
	#pragma comment(lib, "Psapi.lib")
#elif defined(__x86_64__) || defined(__i386__)
	#include <cpuid.h>
	#include <fstream>
	#include <unistd.h>
#endif

namespace {
	using Clock = std::chrono::steady_clock;

	struct BenchmarkCase {
		std::string Name;
		bool Rubber = false;
		std::uint32_t Resolution = 0;
		std::string Scenario;
		std::size_t DefaultFrames = 120;
	};

	struct Samples {
		std::vector<double> Step;
		std::vector<double> Solver;
		std::vector<double> Integration;
		std::vector<double> Constraints;
		std::vector<double> Collision;
		std::vector<double> Extraction;
		std::vector<double> Publication;
		std::vector<double> Application;
	};

	double Milliseconds(Clock::duration Duration) {
		return std::chrono::duration<double, std::milli>(Duration).count();
	}

	double NanosecondsToMilliseconds(std::uint64_t Value) {
		return static_cast<double>(Value) / 1000000.0;
	}

	nlohmann::ordered_json Summarize(std::vector<double> Values) {
		if (Values.empty()) return nlohmann::ordered_json::object();
		std::ranges::sort(Values);
		auto Percentile = [&](double Value) {
			const auto Position = static_cast<std::size_t>(std::ceil(Value * static_cast<double>(Values.size()))) - 1;
			return Values[std::min(Position, Values.size() - 1)];
		};
		const auto Sum = std::accumulate(Values.begin(), Values.end(), 0.0);
		return {
			{"MeanMs", Sum / static_cast<double>(Values.size())},
			{"P50Ms", Percentile(0.50)},
			{"P95Ms", Percentile(0.95)},
			{"P99Ms", Percentile(0.99)},
			{"MaximumMs", Values.back()},
		};
	}

	std::string CpuBrand() {
		std::array<char, 49> Brand{};
	#if defined(_MSC_VER)
		std::array<int, 4> Registers{};
		for (int Leaf = 0; Leaf < 3; ++Leaf) {
			__cpuid(Registers.data(), 0x80000002 + Leaf);
			std::memcpy(Brand.data() + Leaf * 16, Registers.data(), 16);
		}
	#elif defined(__x86_64__) || defined(__i386__)
		std::array<unsigned int, 4> Registers{};
		for (unsigned int Leaf = 0; Leaf < 3; ++Leaf) {
			__get_cpuid(0x80000002 + Leaf, &Registers[0], &Registers[1], &Registers[2], &Registers[3]);
			std::memcpy(Brand.data() + Leaf * 16, Registers.data(), 16);
		}
	#else
		return "Unknown";
	#endif
		std::string Result(Brand.data());
		const auto First = Result.find_first_not_of(' ');
		const auto Last = Result.find_last_not_of(' ');
		return First == std::string::npos ? "Unknown" : Result.substr(First, Last - First + 1);
	}

	std::uint64_t ProcessWorkingSetBytes() {
	#if defined(_WIN32)
		PROCESS_MEMORY_COUNTERS Counters{};
		Counters.cb = sizeof(Counters);
		return GetProcessMemoryInfo(GetCurrentProcess(), &Counters, sizeof(Counters)) ?
			static_cast<std::uint64_t>(Counters.WorkingSetSize) : 0;
	#elif defined(__linux__)
		std::ifstream Input("/proc/self/statm");
		std::uint64_t Pages = 0;
		std::uint64_t Resident = 0;
		Input >> Pages >> Resident;
		return Resident * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
	#else
		return 0;
	#endif
	}

	std::shared_ptr<gargantuan::SoftBodyMaterial> MakeMaterial(bool Rubber) {
		using namespace gargantuan;
		auto Material = std::make_shared<SoftBodyMaterial>();
		Material->SetParticleMass(Rubber ? 0.2f : 0.1f);
		Material->SetDamping(Rubber ? 0.08f : 0.03f);
		Material->SetStretchCompliance(0.000001f);
		Material->SetBendCompliance(0.0001f);
		Material->SetShapeCompliance(Rubber ? 0.001f : 0.00001f);
		Material->SetThickness(0.05f);
		return Material;
	}

	nlohmann::ordered_json RunCase(const BenchmarkCase &Case, std::size_t Warmup, std::size_t Frames) {
		using namespace gargantuan;
		const auto AllocationStart = Clock::now();
		auto Game = std::make_shared<DataModel>();
		auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		World->SetGravity(40.0f);

		std::shared_ptr<DeformableBody> Body;
		std::size_t VertexCount = 0;
		if (!Case.Rubber) {
			auto ClothBody = std::make_shared<Cloth>();
			ClothBody->SetResolutionX(static_cast<int>(Case.Resolution));
			ClothBody->SetResolutionY(static_cast<int>(Case.Resolution));
			ClothBody->SetPosition({0.0f, 12.0f, 0.0f});
			ClothBody->SetSize({12.0f, 12.0f, 0.5f});
			Body = ClothBody;
			VertexCount = static_cast<std::size_t>(Case.Resolution) * Case.Resolution;
		} else {
			auto Rubber = std::make_shared<RubberBody>();
			Rubber->SetResolutionX(static_cast<int>(Case.Resolution));
			Rubber->SetResolutionY(static_cast<int>(Case.Resolution));
			Rubber->SetResolutionZ(static_cast<int>(Case.Resolution));
			Rubber->SetPosition({0.0f, 6.0f, 0.0f});
			Rubber->SetSize({4.0f, 4.0f, 4.0f});
			Body = Rubber;
			VertexCount = static_cast<std::size_t>(Case.Resolution) * Case.Resolution * Case.Resolution;
		}
		Body->SetName(Case.Name);
		auto Material = MakeMaterial(Case.Rubber);
		Material->SetParent(Body);
		Body->SetMaterial(Material);

		if (Case.Scenario == "HangingPinned" || Case.Scenario == "Sustained" || Case.Rubber) {
			auto Pin = std::make_shared<SoftBodyAttachment>();
			Pin->SetVertexIndex(0);
			Pin->SetPosition(Case.Rubber ? glm::vec3(-2.0f, 4.0f, -2.0f) : glm::vec3(-6.0f, 18.0f, 0.0f));
			Pin->SetParent(Body);
			if (!Case.Rubber) {
				auto RightPin = std::make_shared<SoftBodyAttachment>();
				RightPin->SetVertexIndex(static_cast<int>(Case.Resolution - 1));
				RightPin->SetPosition({6.0f, 18.0f, 0.0f});
				RightPin->SetParent(Body);
			}
		}

		if (Case.Scenario == "Collision" || Case.Rubber) {
			auto Floor = std::make_shared<Part>();
			Floor->SetAnchored(true);
			Floor->SetSize({100.0f, 2.0f, 100.0f});
			Floor->SetCFrame(CFrame(0.0f, -1.0f, 0.0f));
			Floor->SetParent(World);
		}
		Body->SetParent(World);
		World->StepPhysics(SoftBodyStepInterval, std::nullopt);

		RenderPublisher Publisher;
		RenderProjection Projection;
		const auto Camera = MakeLookAtRenderCameraInput({0.0f, 10.0f, 30.0f}, {0.0f, 8.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
		auto InitialPublication = Publisher.Publish(*World, Camera, 1280, 720);
		(void)Projection.Apply(*InitialPublication);
		const auto AllocationMilliseconds = Milliseconds(Clock::now() - AllocationStart);

		auto StepFrame = [&](bool Record, Samples &Output) {
			if (Case.Scenario == "Sustained") Body->ApplyForce({80.0f, 15.0f, -35.0f});
			if (Case.Rubber) Body->ApplyForce({30.0f, 0.0f, 15.0f});
			const auto StepStart = Clock::now();
			World->StepPhysics(SoftBodyStepInterval, std::nullopt);
			const auto StepMilliseconds = Milliseconds(Clock::now() - StepStart);
			const auto Profile = World->GetLastSoftBodyProfile();
			const auto PublicationStart = Clock::now();
			auto Publication = Publisher.Publish(*World, Camera, 1280, 720);
			const auto PublicationMilliseconds = Milliseconds(Clock::now() - PublicationStart);
			const auto ApplicationStart = Clock::now();
			(void)Projection.Apply(*Publication);
			const auto ApplicationMilliseconds = Milliseconds(Clock::now() - ApplicationStart);
			if (!Record) return;
			Output.Step.push_back(StepMilliseconds);
			Output.Integration.push_back(NanosecondsToMilliseconds(Profile.IntegrationNanoseconds));
			Output.Constraints.push_back(NanosecondsToMilliseconds(Profile.ConstraintNanoseconds));
			Output.Collision.push_back(NanosecondsToMilliseconds(Profile.CollisionNanoseconds));
			Output.Extraction.push_back(NanosecondsToMilliseconds(Profile.ExtractionNanoseconds));
			Output.Solver.push_back(NanosecondsToMilliseconds(
				Profile.IntegrationNanoseconds + Profile.ConstraintNanoseconds + Profile.CollisionNanoseconds
			));
			Output.Publication.push_back(PublicationMilliseconds);
			Output.Application.push_back(ApplicationMilliseconds);
		};

		Samples Output;
		for (std::size_t Frame = 0; Frame < Warmup; ++Frame) StepFrame(false, Output);
		for (std::size_t Frame = 0; Frame < Frames; ++Frame) StepFrame(true, Output);
		const auto FinalState = World->GetDeformableState(Body->GetObjectId());
		if (!FinalState || !FinalState->Positions || FinalState->Positions->size() != VertexCount)
			throw std::runtime_error("[Physics:SoftBodyBenchmark] final topology was not stable");

		const auto Profile = World->GetLastSoftBodyProfile();
		nlohmann::ordered_json Result{
			{"Case", Case.Name},
			{"Kind", Case.Rubber ? "Rubber" : "Cloth"},
			{"Scenario", Case.Scenario},
			{"Vertices", VertexCount},
			{"WarmupFrames", Warmup},
			{"MeasuredFrames", Frames},
			{"FixedDeltaSeconds", SoftBodyStepInterval},
			{"AllocationMilliseconds", AllocationMilliseconds},
			{"EstimatedSolverBytes", Profile.EstimatedBytes},
			{"ProcessWorkingSetBytes", ProcessWorkingSetBytes()},
			{"Step", Summarize(Output.Step)},
			{"Solver", Summarize(Output.Solver)},
			{"Integration", Summarize(Output.Integration)},
			{"Constraints", Summarize(Output.Constraints)},
			{"Collision", Summarize(Output.Collision)},
			{"StateExtraction", Summarize(Output.Extraction)},
			{"RenderPublication", Summarize(Output.Publication)},
			{"ProjectionApplication", Summarize(Output.Application)},
		};
		Game->Destroy();
		return Result;
	}

	std::vector<BenchmarkCase> Cases() {
		std::vector<BenchmarkCase> Result;
		for (const auto Resolution : {32u, 64u, 128u, 256u}) {
			const auto Vertices = Resolution * Resolution;
			Result.push_back({"Cloth-" + std::to_string(Vertices) + "-HangingPinned", false, Resolution, "HangingPinned", 240});
			Result.push_back({"Cloth-" + std::to_string(Vertices) + "-Collision", false, Resolution, "Collision", 240});
			Result.push_back({"Cloth-" + std::to_string(Vertices) + "-Sustained", false, Resolution, "Sustained", 600});
		}
		Result.push_back({"Rubber-Small", true, 4, "Sustained", 600});
		Result.push_back({"Rubber-Medium", true, 8, "Sustained", 600});
		Result.push_back({"Rubber-Stress", true, 16, "Sustained", 600});
		return Result;
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		bool Quick = false;
		std::size_t WarmupOverride = 0;
		std::size_t FrameOverride = 0;
		std::string Filter;
		for (int Index = 1; Index < ArgumentCount; ++Index) {
			const std::string_view Argument(Arguments[Index]);
			if (Argument == "--quick") Quick = true;
			else if (Argument.starts_with("--warmup=")) WarmupOverride = std::stoull(std::string(Argument.substr(9)));
			else if (Argument.starts_with("--frames=")) FrameOverride = std::stoull(std::string(Argument.substr(9)));
			else if (Argument.starts_with("--filter=")) Filter = Argument.substr(9);
			else throw std::invalid_argument("Unknown benchmark argument: " + std::string(Argument));
		}

		nlohmann::ordered_json Header{
			{"Type", "SoftBodyPhysicsFoundationBenchmark"},
			{"Cpu", CpuBrand()},
			{"Build", "Release"},
			{"SimulationThreading", "SingleMainThread"},
			{"RendererApplication", "HeadlessRenderProjection"},
		};
		std::cout << Header.dump() << '\n';
		for (const auto &Case : Cases()) {
			if (!Filter.empty() && Case.Name.find(Filter) == std::string::npos) continue;
			if (Quick && Case.Name != "Cloth-1024-HangingPinned") continue;
			const auto Warmup = Quick ? 1 : (WarmupOverride == 0 ? 30 : WarmupOverride);
			const auto Frames = Quick ? 3 : (FrameOverride == 0 ? Case.DefaultFrames : FrameOverride);
			std::cout << RunCase(Case, Warmup, Frames).dump() << '\n';
		}
	} catch (const std::exception &Exception) {
		std::cerr << "[Physics:SoftBodyBenchmark] " << Exception.what() << '\n';
		return 1;
	}
	return 0;
}
