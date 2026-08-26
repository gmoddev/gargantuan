#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/datatypes/RaycastParams.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace gargantuan {
	struct WorldRootTestAccess {
		static void Flush(WorldRoot &World) {
			World.ApplyPendingPhysicsChanges();
		}
		static PhysicsOperationResult
		Prepare(const WorldRoot &World, const RaycastParams &Params, PhysicsQueryFilter &Filter) {
			return World.BuildRaycastFilter(Params, Filter);
		}
		static PhysicsRaycastResult BackendRaycast(WorldRoot &World, const PhysicsRaycastRequest &Request) {
			return World.Physics.Raycast(Request);
		}
		static std::shared_ptr<BasePart> Resolve(const WorldRoot &World, PhysicsBodyId Body) {
			return World.ResolvePart(Body);
		}
	};
}

namespace {
	using namespace gargantuan;
	using Clock = std::chrono::steady_clock;
	using Json = nlohmann::json;

	struct Percentiles {
		double P50 = 0.0;
		double P95 = 0.0;
		double P99 = 0.0;
	};

	Percentiles Summarize(std::vector<double> Samples) {
		if (Samples.empty()) return {};
		std::ranges::sort(Samples);
		auto At = [&](double Fraction) {
			return Samples[std::min(
				Samples.size() - 1,
				static_cast<std::size_t>(std::ceil(Fraction * static_cast<double>(Samples.size())) - 1.0)
			)];
		};
		return {At(0.50), At(0.95), At(0.99)};
	}

	Json Encode(const Percentiles &Value) {
		return {{"p50", Value.P50}, {"p95", Value.P95}, {"p99", Value.P99}};
	}

	glm::vec3 PositionFor(std::string_view Layout, std::size_t Index) {
		if (Index == 0) return {5.0f, 0.0f, 0.0f};
		if (Layout == "dense") {
			return {
				5.0f + static_cast<float>(Index % 40) * 0.4f,
				static_cast<float>(static_cast<int>((Index / 40) % 20) - 10) * 0.4f,
				static_cast<float>(static_cast<int>((Index / 800) % 20) - 10) * 0.4f,
			};
		}
		if (Layout == "corridor") {
			return {
				5.0f + static_cast<float>(Index % 100) * 3.0f,
				0.0f,
				static_cast<float>(Index / 100) * 4.0f,
			};
		}
		if (Layout == "misses") {
			return {
				5.0f + static_cast<float>(Index % 100) * 5.0f,
				100.0f,
				100.0f + static_cast<float>(Index / 100) * 5.0f,
			};
		}
		return {
			5.0f + static_cast<float>(Index % 100) * 12.0f,
			static_cast<float>((Index / 100) % 10) * 12.0f,
			static_cast<float>(Index / 1000) * 12.0f,
		};
	}

	struct Case {
		std::string Name;
		glm::vec3 Origin{0.0f};
		glm::vec3 Direction{0.0f};
		RaycastParams Params;
	};

	Json RunScenario(std::size_t Count, std::string_view Layout, bool Quick) {
		auto Game = std::make_shared<DataModel>();
		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		auto FilterFolder = std::make_shared<Folder>();
		FilterFolder->SetName("LargeFilterSubtree");
		FilterFolder->SetParent(WorkspaceValue);
		std::vector<std::shared_ptr<Part>> Parts;
		Parts.reserve(Count);
		for (std::size_t Index = 0; Index < Count; ++Index) {
			auto Value = std::make_shared<Part>();
			Value->SetAnchored(true);
			Value->SetCFrame(CFrame(PositionFor(Layout, Index)));
			Value->SetSize({1.0f, 1.0f, 1.0f});
			std::shared_ptr<Instance> ParentValue = Index < MaximumRaycastFilterBodies
														? std::static_pointer_cast<Instance>(FilterFolder)
														: std::static_pointer_cast<Instance>(WorkspaceValue);
			Value->SetParent(std::move(ParentValue));
			Parts.push_back(std::move(Value));
		}
		WorldRootTestAccess::Flush(*WorkspaceValue);

		RaycastParams Include;
		Include.FilterType = Enums::RaycastFilterType::Include;
		Include.FilterDescendantsInstances.Values = {Parts.front()};
		RaycastParams Exclude;
		Exclude.FilterDescendantsInstances.Values = {Parts.front()};
		RaycastParams LargeExclude;
		LargeExclude.FilterDescendantsInstances.Values = {FilterFolder};
		std::vector<Case> Cases{
			{"nearest-hit", {}, {500.0f, 0.0f, 0.0f}, {}},
			{"miss", {0.0f, 50.0f, 0.0f}, {500.0f, 0.0f, 0.0f}, {}},
			{"include-filter", {}, {500.0f, 0.0f, 0.0f}, Include},
			{"exclude-filter", {}, {500.0f, 0.0f, 0.0f}, Exclude},
			{"large-subtree-exclusion", {}, {500.0f, 0.0f, 0.0f}, LargeExclude},
			{"short-ray", {}, {8.0f, 0.0f, 0.0f}, {}},
			{"maximum-distance-miss", {0.0f, -100.0f, 0.0f}, {0.0f, 0.0f, MaximumRaycastDistance}, {}},
		};

		Json CaseRows = Json::array();
		const int Iterations = Quick ? 64 : 512;
		for (const auto &BenchmarkCase : Cases) {
			std::vector<double> FilterSamples;
			std::vector<double> BackendSamples;
			std::vector<double> ResolutionSamples;
			std::vector<double> TotalSamples;
			FilterSamples.reserve(Iterations);
			BackendSamples.reserve(Iterations);
			ResolutionSamples.reserve(Iterations);
			TotalSamples.reserve(Iterations);
			for (int Iteration = 0; Iteration < Iterations; ++Iteration) {
				PhysicsQueryFilter Filter;
				auto Start = Clock::now();
				auto Prepared = WorldRootTestAccess::Prepare(*WorkspaceValue, BenchmarkCase.Params, Filter);
				FilterSamples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - Start).count());
				if (!Prepared.Succeeded()) throw std::runtime_error(Prepared.Message);

				Start = Clock::now();
				auto Backend = WorldRootTestAccess::BackendRaycast(
					*WorkspaceValue,
					{
						.Origin = BenchmarkCase.Origin,
						.Direction = BenchmarkCase.Direction,
						.Filter = Filter,
					}
				);
				BackendSamples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - Start).count());
				if (!Backend.Succeeded()) throw std::runtime_error(Backend.Message);

				Start = Clock::now();
				if (Backend.HasHit())
					(void)WorldRootTestAccess::Resolve(*WorkspaceValue, Backend.Candidates.front().Body);
				ResolutionSamples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - Start).count());

				Start = Clock::now();
				auto Total = WorkspaceValue->ResolveRaycast(
					BenchmarkCase.Origin, BenchmarkCase.Direction, BenchmarkCase.Params
				);
				TotalSamples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - Start).count());
				if (!Total.Succeeded()) throw std::runtime_error(Total.Message);
			}
			CaseRows.push_back({
				{"case", BenchmarkCase.Name},
				{"filterPreparationUs", Encode(Summarize(std::move(FilterSamples)))},
				{"backendQueryUs", Encode(Summarize(std::move(BackendSamples)))},
				{"semanticResolutionUs", Encode(Summarize(std::move(ResolutionSamples)))},
				{"totalUs", Encode(Summarize(std::move(TotalSamples)))},
			});
		}
		Game->Destroy();
		return {{"colliders", Count}, {"layout", Layout}, {"cases", std::move(CaseRows)}};
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		const bool Quick = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--quick";
		const auto Counts = Quick ? std::vector<std::size_t>{1, 100} : std::vector<std::size_t>{1, 100, 1'000, 10'000};
		nlohmann::json Results = nlohmann::json::array();
		for (const auto Count : Counts)
			for (const auto Layout : {"sparse", "dense", "corridor", "misses"})
				Results.push_back(RunScenario(Count, Layout, Quick));
		std::cout << nlohmann::json{{"quick", Quick}, {"results", std::move(Results)}}.dump(2) << '\n';
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Physics:QueryBenchmark] FAIL: " << Error.what() << '\n';
		return 1;
	}
}
