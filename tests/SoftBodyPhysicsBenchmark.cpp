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
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

// clang-format off: Windows.h must define the Win32 API types before psapi.h.
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
// clang-format on

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
		std::vector<double> Snapshot;
		std::vector<double> Dispatch;
		std::vector<double> WorkerWait;
		std::vector<double> Solver;
		std::vector<double> Integration;
		std::vector<double> Constraints;
		std::vector<double> BroadphaseConstruction;
		std::vector<double> BroadphaseQuery;
		std::vector<double> Collision;
		std::vector<double> Merge;
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
		return GetProcessMemoryInfo(GetCurrentProcess(), &Counters, sizeof(Counters))
				   ? static_cast<std::uint64_t>(Counters.WorkingSetSize)
				   : 0;
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
			Output.Snapshot.push_back(NanosecondsToMilliseconds(Profile.SnapshotNanoseconds));
			Output.Dispatch.push_back(NanosecondsToMilliseconds(Profile.DispatchNanoseconds));
			Output.WorkerWait.push_back(NanosecondsToMilliseconds(Profile.WorkerWaitNanoseconds));
			Output.Integration.push_back(NanosecondsToMilliseconds(Profile.IntegrationNanoseconds));
			Output.Constraints.push_back(NanosecondsToMilliseconds(Profile.ConstraintNanoseconds));
			Output.BroadphaseConstruction.push_back(
				NanosecondsToMilliseconds(Profile.BroadphaseConstructionNanoseconds)
			);
			Output.BroadphaseQuery.push_back(NanosecondsToMilliseconds(Profile.BroadphaseQueryNanoseconds));
			Output.Collision.push_back(NanosecondsToMilliseconds(Profile.CollisionNanoseconds));
			Output.Merge.push_back(NanosecondsToMilliseconds(Profile.ResultMergeNanoseconds));
			Output.Extraction.push_back(NanosecondsToMilliseconds(Profile.ExtractionNanoseconds));
			Output.Solver.push_back(NanosecondsToMilliseconds(
				Profile.IntegrationNanoseconds + Profile.ConstraintNanoseconds + Profile.CollisionNanoseconds
			));
			Output.Publication.push_back(PublicationMilliseconds);
			Output.Application.push_back(ApplicationMilliseconds);
		};

		Samples Output;
		for (std::size_t Frame = 0; Frame < Warmup; ++Frame)
			StepFrame(false, Output);
		for (std::size_t Frame = 0; Frame < Frames; ++Frame)
			StepFrame(true, Output);
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
			{"WorkerCount", Profile.WorkerCount},
			{"JobsPerFrame", Profile.JobsDispatched},
			{"WorkerUtilization", Profile.WorkerUtilization},
			{"ColliderQueries", Profile.ColliderQueries},
			{"CandidateColliders", Profile.CandidateColliders},
			{"CandidatesPerBody",
			 Profile.ColliderQueries == 0
				 ? 0.0
				 : static_cast<double>(Profile.CandidateColliders) / static_cast<double>(Profile.ColliderQueries)},
			{"QueuedResultBytes", Profile.QueuedResultBytes},
			{"BacklogDrops", Profile.BacklogDrops},
			{"FrozenBodies", Profile.FrozenBodies},
			{"StaleResults", Profile.StaleResults},
			{"Step", Summarize(Output.Step)},
			{"MainSnapshot", Summarize(Output.Snapshot)},
			{"JobDispatch", Summarize(Output.Dispatch)},
			{"WorkerWait", Summarize(Output.WorkerWait)},
			{"Solver", Summarize(Output.Solver)},
			{"Integration", Summarize(Output.Integration)},
			{"Constraints", Summarize(Output.Constraints)},
			{"BroadphaseConstruction", Summarize(Output.BroadphaseConstruction)},
			{"BroadphaseQuery", Summarize(Output.BroadphaseQuery)},
			{"Collision", Summarize(Output.Collision)},
			{"ResultMerge", Summarize(Output.Merge)},
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
			Result.push_back(
				{"Cloth-" + std::to_string(Vertices) + "-HangingPinned", false, Resolution, "HangingPinned", 240}
			);
			Result.push_back({"Cloth-" + std::to_string(Vertices) + "-Collision", false, Resolution, "Collision", 240});
			Result.push_back({"Cloth-" + std::to_string(Vertices) + "-Sustained", false, Resolution, "Sustained", 600});
		}
		Result.push_back({"Rubber-Small", true, 4, "Sustained", 600});
		Result.push_back({"Rubber-Medium", true, 8, "Sustained", 600});
		Result.push_back({"Rubber-Stress", true, 16, "Sustained", 600});
		Result.push_back({"Rubber-LargeStress", true, 32, "Sustained", 120});
		return Result;
	}

	gargantuan::SoftBodyDefinition BackendCloth(std::uint32_t Resolution, float Offset) {
		gargantuan::SoftBodyDefinition Definition;
		Definition.Kind = gargantuan::SoftBodyKind::Cloth;
		Definition.Position = {Offset, 4.0f, 0.0f};
		Definition.Size = {8.0f, 8.0f, 0.5f};
		Definition.ResolutionX = Resolution;
		Definition.ResolutionY = Resolution;
		Definition.Material.Damping = 0.03f;
		return Definition;
	}

	gargantuan::SoftBodyDefinition BackendRubber(std::uint32_t Resolution, float Offset) {
		auto Definition = BackendCloth(Resolution, Offset);
		Definition.Kind = gargantuan::SoftBodyKind::Rubber;
		Definition.Size = {4.0f, 4.0f, 4.0f};
		Definition.ResolutionZ = Resolution;
		Definition.Material.ShapeCompliance = 0.001f;
		Definition.Material.VolumeCompliance = 0.0001f;
		return Definition;
	}

	nlohmann::ordered_json RunBackendCase(
		std::string Name,
		const std::vector<gargantuan::SoftBodyDefinition> &Definitions,
		std::size_t ColliderCount,
		gargantuan::SoftBodyExecutionMode ExecutionMode,
		gargantuan::SoftBodyBroadphaseMode BroadphaseMode,
		std::size_t Warmup,
		std::size_t Frames
	) {
		using namespace gargantuan;
		SoftBodyWorld World;
		std::size_t Vertices = 0;
		for (const auto &Definition : Definitions) {
			const auto Body = World.CreateBody(Definition);
			if (!Body.IsValid()) throw std::runtime_error("[Physics:SoftBodyBenchmark] backend case exceeded bounds");
			Vertices += static_cast<std::size_t>(Definition.ResolutionX) * Definition.ResolutionY *
						(Definition.Kind == SoftBodyKind::Rubber ? Definition.ResolutionZ : 1);
		}
		std::vector<SoftBodyCollider> Colliders;
		Colliders.reserve(ColliderCount);
		if (ColliderCount != 0)
			Colliders.push_back({
				.Shape = {.Kind = PhysicsShapeKind::Box, .Size = {100.0f, 2.0f, 100.0f}},
				.Transform = CFrame(0.0f, -1.0f, 0.0f),
			});
		for (std::size_t Index = Colliders.size(); Index < ColliderCount; ++Index) {
			const auto X = 200.0f + static_cast<float>(Index % 64) * 5.0f;
			const auto Z = 200.0f + static_cast<float>(Index / 64) * 5.0f;
			Colliders.push_back({
				.Shape = {.Kind = static_cast<PhysicsShapeKind>(Index % 5), .Size = {2.0f, 2.0f, 2.0f}},
				.Transform = CFrame(X, 0.0f, Z),
			});
		}
		Samples Output;
		SoftBodyStepProfile LastProfile;
		auto Run = [&](bool Record) {
			const auto Start = Clock::now();
			auto Result = World.Step({
				.DeltaTime = SoftBodyStepInterval,
				.Gravity = {0.0f, -40.0f, 0.0f},
				.Colliders = Colliders,
				.ExecutionMode = ExecutionMode,
				.BroadphaseMode = BroadphaseMode,
			});
			LastProfile = Result.Profile;
			if (!Record) return;
			Output.Step.push_back(Milliseconds(Clock::now() - Start));
			Output.Snapshot.push_back(NanosecondsToMilliseconds(LastProfile.SnapshotNanoseconds));
			Output.Dispatch.push_back(NanosecondsToMilliseconds(LastProfile.DispatchNanoseconds));
			Output.WorkerWait.push_back(NanosecondsToMilliseconds(LastProfile.WorkerWaitNanoseconds));
			Output.Integration.push_back(NanosecondsToMilliseconds(LastProfile.IntegrationNanoseconds));
			Output.Constraints.push_back(NanosecondsToMilliseconds(LastProfile.ConstraintNanoseconds));
			Output.BroadphaseConstruction.push_back(
				NanosecondsToMilliseconds(LastProfile.BroadphaseConstructionNanoseconds)
			);
			Output.BroadphaseQuery.push_back(NanosecondsToMilliseconds(LastProfile.BroadphaseQueryNanoseconds));
			Output.Collision.push_back(NanosecondsToMilliseconds(LastProfile.CollisionNanoseconds));
			Output.Merge.push_back(NanosecondsToMilliseconds(LastProfile.ResultMergeNanoseconds));
			Output.Extraction.push_back(NanosecondsToMilliseconds(LastProfile.ExtractionNanoseconds));
		};
		for (std::size_t Frame = 0; Frame < Warmup; ++Frame)
			Run(false);
		for (std::size_t Frame = 0; Frame < Frames; ++Frame)
			Run(true);
		return nlohmann::ordered_json{
			{"Case", std::move(Name)},
			{"Kind", "BackendScaling"},
			{"Bodies", Definitions.size()},
			{"Vertices", Vertices},
			{"Colliders", ColliderCount},
			{"ExecutionMode", ExecutionMode == SoftBodyExecutionMode::Jobified ? "Jobified" : "SynchronousReference"},
			{"BroadphaseMode",
			 BroadphaseMode == SoftBodyBroadphaseMode::DeterministicSweep ? "DeterministicSweep"
																		  : "BruteForceReference"},
			{"MeasuredFrames", Frames},
			{"WorkerCount", LastProfile.WorkerCount},
			{"JobsPerFrame", LastProfile.JobsDispatched},
			{"WorkerUtilization", LastProfile.WorkerUtilization},
			{"CandidateColliders", LastProfile.CandidateColliders},
			{"CandidatesPerBody",
			 LastProfile.ColliderQueries == 0 ? 0.0
											  : static_cast<double>(LastProfile.CandidateColliders) /
													static_cast<double>(LastProfile.ColliderQueries)},
			{"EstimatedSolverBytes", LastProfile.EstimatedBytes},
			{"QueuedResultBytes", LastProfile.QueuedResultBytes},
			{"BacklogDrops", LastProfile.BacklogDrops},
			{"FrozenBodies", LastProfile.FrozenBodies},
			{"Step", Summarize(Output.Step)},
			{"MainSnapshot", Summarize(Output.Snapshot)},
			{"JobDispatch", Summarize(Output.Dispatch)},
			{"WorkerWait", Summarize(Output.WorkerWait)},
			{"Integration", Summarize(Output.Integration)},
			{"ConstraintSolving", Summarize(Output.Constraints)},
			{"BroadphaseConstruction", Summarize(Output.BroadphaseConstruction)},
			{"BroadphaseQuery", Summarize(Output.BroadphaseQuery)},
			{"NarrowCollision", Summarize(Output.Collision)},
			{"ResultMerge", Summarize(Output.Merge)},
			{"StateExtraction", Summarize(Output.Extraction)},
		};
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
			if (Argument == "--quick")
				Quick = true;
			else if (Argument.starts_with("--warmup="))
				WarmupOverride = std::stoull(std::string(Argument.substr(9)));
			else if (Argument.starts_with("--frames="))
				FrameOverride = std::stoull(std::string(Argument.substr(9)));
			else if (Argument.starts_with("--filter="))
				Filter = Argument.substr(9);
			else
				throw std::invalid_argument("Unknown benchmark argument: " + std::string(Argument));
		}

		nlohmann::ordered_json Header{
			{"Type", "SoftBodyPhysicsFoundationBenchmark"},
			{"Cpu", CpuBrand()},
			{"Build", "Release"},
			{"SimulationThreading", "BoundedIndependentBodyJobs"},
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
		if (!Quick) {
			auto Emit = [&](const std::string &Name, auto &&Run) {
				if (Filter.empty() || Name.find(Filter) != std::string::npos) std::cout << Run().dump() << '\n';
			};
			const auto ScalingWarmup = WarmupOverride == 0 ? 5 : WarmupOverride;
			const auto ScalingFrames = FrameOverride == 0 ? 60 : FrameOverride;
			auto ClothBodies = [](std::size_t Count, std::uint32_t Resolution) {
				std::vector<gargantuan::SoftBodyDefinition> Definitions;
				for (std::size_t Index = 0; Index < Count; ++Index)
					Definitions.push_back(BackendCloth(Resolution, static_cast<float>(Index) * 12.0f));
				return Definitions;
			};
			for (const auto &[Name, Count, Resolution] : std::array{
					 std::tuple{"Multi-4x4K-Cloth", std::size_t{4}, std::uint32_t{64}},
					 std::tuple{"Multi-8x4K-Cloth", std::size_t{8}, std::uint32_t{64}},
					 std::tuple{"Multi-4x16K-Cloth", std::size_t{4}, std::uint32_t{128}},
					 std::tuple{"Multi-32x64-Cloth", std::size_t{32}, std::uint32_t{8}},
				 }) {
				Emit(Name, [&] {
					return RunBackendCase(
						Name,
						ClothBodies(Count, Resolution),
						32,
						gargantuan::SoftBodyExecutionMode::Jobified,
						gargantuan::SoftBodyBroadphaseMode::DeterministicSweep,
						ScalingWarmup,
						ScalingFrames
					);
				});
			}
			std::vector<gargantuan::SoftBodyDefinition> Mixed = ClothBodies(4, 64);
			for (std::size_t Index = 0; Index < 4; ++Index)
				Mixed.push_back(BackendRubber(8, 60.0f + static_cast<float>(Index) * 8.0f));
			Emit("Multi-Mixed-Cloth-Rubber", [&] {
				return RunBackendCase(
					"Multi-Mixed-Cloth-Rubber",
					Mixed,
					32,
					gargantuan::SoftBodyExecutionMode::Jobified,
					gargantuan::SoftBodyBroadphaseMode::DeterministicSweep,
					ScalingWarmup,
					ScalingFrames
				);
			});
			for (const auto Mode : {
					 gargantuan::SoftBodyExecutionMode::SynchronousReference,
					 gargantuan::SoftBodyExecutionMode::Jobified,
				 }) {
				const std::string Name = Mode == gargantuan::SoftBodyExecutionMode::Jobified
											 ? "Compare-4x4K-Jobified"
											 : "Compare-4x4K-Synchronous";
				Emit(Name, [&] {
					return RunBackendCase(
						Name,
						ClothBodies(4, 64),
						32,
						Mode,
						gargantuan::SoftBodyBroadphaseMode::DeterministicSweep,
						ScalingWarmup,
						ScalingFrames
					);
				});
			}
			for (const auto ColliderCount : {32u, 256u, 1024u, 4096u}) {
				for (const auto Mode : {
						 gargantuan::SoftBodyBroadphaseMode::DeterministicSweep,
						 gargantuan::SoftBodyBroadphaseMode::BruteForceReference,
					 }) {
					const std::string Name = "Colliders-" + std::to_string(ColliderCount) +
											 (Mode == gargantuan::SoftBodyBroadphaseMode::DeterministicSweep
												  ? "-Broadphase"
												  : "-BruteForce");
					Emit(Name, [&] {
						return RunBackendCase(
							Name,
							ClothBodies(1, 32),
							ColliderCount,
							gargantuan::SoftBodyExecutionMode::Jobified,
							Mode,
							ScalingWarmup,
							FrameOverride == 0 ? 20 : FrameOverride
						);
					});
				}
			}
		}
	} catch (const std::exception &Exception) {
		std::cerr << "[Physics:SoftBodyBenchmark] " << Exception.what() << '\n';
		return 1;
	}
	return 0;
}
