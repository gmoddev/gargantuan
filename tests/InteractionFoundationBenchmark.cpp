#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/ProximityPrompt.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/services/InteractionService.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace gargantuan {
	struct InteractionServiceTestAccess {
		static void Step(InteractionService &Service, InteractionService::Clock::time_point Now) {
			Service.Step(Now);
		}
		static std::tuple<ObjectId, std::size_t, std::size_t>
		Query(InteractionService &Service, const glm::vec3 &Origin, const std::shared_ptr<Player> &PlayerValue) {
			Service.ProcessDirtyPrompts();
			auto Result = Service.QueryNearest(Origin, PlayerValue);
			return {Result.Prompt, Result.Considered, Result.Raycasts};
		}
		static void ProcessDirty(InteractionService &Service) {
			Service.ProcessDirtyPrompts();
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
				Samples.size() - 1, static_cast<std::size_t>(std::ceil(Fraction * Samples.size()) - 1)
			)];
		};
		return {At(0.50), At(0.95), At(0.99)};
	}

	glm::vec3 PositionFor(std::string_view Distribution, std::size_t Index) {
		if (Distribution == "occluded")
			return {2.0f + static_cast<float>(Index % 100) * 0.1f, 0.0f, static_cast<float>(Index / 100) * 0.1f};
		if (Distribution == "dense") {
			const float X = static_cast<float>(static_cast<int>(Index % 31) - 15) * 0.2f;
			const float Y = static_cast<float>(static_cast<int>((Index / 31) % 7) - 3) * 0.2f;
			const float Z = static_cast<float>(static_cast<int>((Index / 217) % 31) - 15) * 0.2f;
			return {X, Y, Z};
		}
		if (Distribution == "far") return {1000.0f + static_cast<float>(Index % 100), 0.0f, 1000.0f};
		const float X = 2.0f + static_cast<float>(Index % 100) * 24.0f;
		const float Z = static_cast<float>(Index / 100) * 24.0f;
		return {X, 0.0f, Z};
	}

	Json RunScenario(std::size_t Count, std::string_view Distribution, bool RequiresLineOfSight, bool Quick) {
		auto Game = std::make_shared<DataModel>();
		auto PlayersValue = std::dynamic_pointer_cast<Players>(Game->GetService("Players"));
		PlayersValue->SetDefaultControllerEnabled(false);
		PlayersValue->SetDefaultCameraEnabled(false);
		HeadlessRenderer Renderer(Vector2(320.0f, 180.0f));
		Engine Runtime(Game, &Renderer);
		auto LocalPlayer = *Runtime.Players->GetLocalPlayer();
		auto Character = std::make_shared<KinematicCharacter>();
		Character->SetParent(Runtime.Workspace);
		Character->SetPosition({0.0f, 0.0f, 0.0f});
		LocalPlayer->SetCharacter(Character);

		std::vector<std::shared_ptr<Part>> Parts;
		std::vector<std::shared_ptr<ProximityPrompt>> Prompts;
		Parts.reserve(Count);
		Prompts.reserve(Count);
		const auto RegistrationStart = Clock::now();
		for (std::size_t Index = 0; Index < Count; ++Index) {
			auto PartValue = std::make_shared<Part>();
			PartValue->SetAnchored(true);
			PartValue->SetCFrame(CFrame(PositionFor(Distribution, Index)));
			PartValue->SetParent(Runtime.Workspace);
			auto Prompt = std::make_shared<ProximityPrompt>();
			Prompt->SetMaxActivationDistance(64.0f);
			Prompt->SetRequiresLineOfSight(RequiresLineOfSight);
			if (Distribution == "disabled") Prompt->SetEnabled(false);
			Prompt->SetParent(PartValue);
			Parts.push_back(std::move(PartValue));
			Prompts.push_back(std::move(Prompt));
		}
		InteractionServiceTestAccess::ProcessDirty(*Runtime.Interaction);
		if (Distribution == "occluded") {
			auto Wall = std::make_shared<Part>();
			Wall->SetAnchored(true);
			Wall->SetCFrame(CFrame(1.0f, 0.0f, 0.0f));
			Wall->SetSize({0.2f, 8.0f, 8.0f});
			Wall->SetParent(Runtime.Workspace);
			Parts.push_back(std::move(Wall));
		}
		const auto RegistrationUs = std::chrono::duration<double, std::micro>(Clock::now() - RegistrationStart).count();

		const auto UpdateCount = std::min<std::size_t>(Count, Quick ? 8 : 128);
		const auto UpdateStart = Clock::now();
		for (std::size_t Index = 0; Index < UpdateCount; ++Index)
			Parts[Index]->SetCFrame(CFrame(PositionFor(Distribution, Index) + glm::vec3(0.25f, 0.0f, 0.0f)));
		InteractionServiceTestAccess::ProcessDirty(*Runtime.Interaction);
		const auto PropertyUpdateUs = std::chrono::duration<double, std::micro>(Clock::now() - UpdateStart).count();

		Json PlayerRows = Json::array();
		for (const std::size_t PlayerCount : {1u, 4u, 16u}) {
			std::vector<double> Samples;
			std::size_t Considered = 0;
			std::size_t Raycasts = 0;
			const auto Iterations = Quick ? 32 : 256;
			Samples.reserve(Iterations);
			for (int Iteration = 0; Iteration < Iterations; ++Iteration) {
				const auto Start = Clock::now();
				for (std::size_t PlayerIndex = 0; PlayerIndex < PlayerCount; ++PlayerIndex) {
					auto [PromptId, QueryConsidered, QueryRaycasts] = InteractionServiceTestAccess::Query(
						*Runtime.Interaction, {static_cast<float>(PlayerIndex) * 0.1f, 0.0f, 0.0f}, LocalPlayer
					);
					(void)PromptId;
					Considered += QueryConsidered;
					Raycasts += QueryRaycasts;
				}
				Samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - Start).count());
			}
			const auto Result = Summarize(std::move(Samples));
			PlayerRows.push_back({
				{"players", PlayerCount},
				{"queryBatchUs", {{"p50", Result.P50}, {"p95", Result.P95}, {"p99", Result.P99}}},
				{"meanPromptsConsideredPerPlayer", static_cast<double>(Considered) / (Iterations * PlayerCount)},
				{"meanRaycastsPerPlayer", static_cast<double>(Raycasts) / (Iterations * PlayerCount)},
			});
		}

		Json HoldRow = nullptr;
		if (Distribution == "dense" && !Prompts.empty()) {
			Prompts.front()->SetHoldDuration(0.25f);
			InteractionServiceTestAccess::ProcessDirty(*Runtime.Interaction);
			auto Now = InteractionService::Clock::time_point(std::chrono::seconds(1));
			InteractionServiceTestAccess::Step(*Runtime.Interaction, Now);
			Runtime.Interaction->BeginActivation();
			std::vector<double> Samples;
			for (int Index = 0; Index < (Quick ? 16 : 128); ++Index) {
				Now += std::chrono::milliseconds(8);
				const auto Start = Clock::now();
				InteractionServiceTestAccess::Step(*Runtime.Interaction, Now);
				Samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - Start).count());
			}
			Runtime.Interaction->EndActivation();
			InteractionServiceTestAccess::Step(*Runtime.Interaction, Now + std::chrono::milliseconds(40));
			const auto Result = Summarize(std::move(Samples));
			HoldRow = {{"p50", Result.P50}, {"p95", Result.P95}, {"p99", Result.P99}};
		}

		Runtime.Destroy();
		Game->Destroy();
		return {
			{"prompts", Count},
			{"distribution", Distribution},
			{"requiresLineOfSight", RequiresLineOfSight},
			{"registrationUs", RegistrationUs},
			{"propertyUpdateCount", UpdateCount},
			{"propertyUpdateUs", PropertyUpdateUs},
			{"queries", std::move(PlayerRows)},
			{"holdAndPresentationUpdateUs", std::move(HoldRow)},
		};
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		const bool Quick = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--quick";
		const std::vector<std::size_t> Counts = Quick ? std::vector<std::size_t>{1, 100}
													  : std::vector<std::size_t>{1, 100, 1'000, 10'000};
		Json Results = Json::array();
		for (const auto Count : Counts)
			for (const auto Distribution : {"sparse", "dense", "occluded", "far", "disabled"})
				for (const bool RequiresLineOfSight : {false, true})
					Results.push_back(RunScenario(Count, Distribution, RequiresLineOfSight, Quick));
		std::cout << Json{{"quick", Quick}, {"results", std::move(Results)}}.dump(2) << '\n';
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Interaction:Benchmark] FAIL: " << Error.what() << '\n';
		return 1;
	}
}
