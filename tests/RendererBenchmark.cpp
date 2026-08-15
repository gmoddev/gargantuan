// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#if defined(GARGANTUAN_WITH_FILAMENT)
#include "gargantuan/render/FilamentRenderer.hpp"
#endif
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
	using Clock = std::chrono::steady_clock;

	struct Options {
		std::string Backend = "projection";
		std::string Scenario = "all";
		std::optional<std::size_t> Count;
		std::size_t Frames = 30;
		std::size_t WarmupFrames = 3;
		std::uint32_t Width = 1280;
		std::uint32_t Height = 720;
		bool ShadowsEnabled = false;
	};

	struct ScenarioCase {
		std::string Name;
		std::size_t Count = 0;
	};

	struct Fixture {
		std::shared_ptr<gargantuan::DataModel> Game;
		std::shared_ptr<gargantuan::Workspace> World;
		std::vector<std::shared_ptr<gargantuan::Part>> Parts;
	};

	double Milliseconds(Clock::duration Duration) {
		return std::chrono::duration<double, std::milli>(Duration).count();
	}

	double Mean(const std::vector<double> &Samples) {
		return Samples.empty() ? 0.0 : std::accumulate(Samples.begin(), Samples.end(), 0.0) / Samples.size();
	}

	double Percentile(std::vector<double> Samples, double PercentileValue) {
		if (Samples.empty()) return 0.0;
		std::ranges::sort(Samples);
		const auto Index = static_cast<std::size_t>(std::ceil(PercentileValue * Samples.size())) - 1;
		return Samples[std::min(Index, Samples.size() - 1)];
	}

	std::size_t ParseSize(std::string_view Value, std::string_view Name) {
		std::size_t ParsedCharacters = 0;
		const auto Parsed = std::stoull(std::string(Value), &ParsedCharacters);
		if (ParsedCharacters != Value.size() || Parsed == 0)
			throw std::invalid_argument(std::string(Name) + " must be a positive integer");
		return static_cast<std::size_t>(Parsed);
	}

	Options ParseOptions(int ArgumentCount, char **Arguments) {
		Options ParsedOptions;
		for (int Index = 1; Index < ArgumentCount; ++Index) {
			const std::string_view Argument(Arguments[Index]);
			auto Value = [&](std::string_view Prefix) -> std::optional<std::string_view> {
				return Argument.starts_with(Prefix) ? std::optional(Argument.substr(Prefix.size())) : std::nullopt;
			};
			if (Argument == "--help") {
				std::cout <<
					"Usage: gargantuan_renderer_benchmark [--backend=projection|headless|sdl|filament|filament-headless] "
					"[--scenario=all|static|dynamic|mostly-static|mixed] [--count=N] "
					"[--frames=N] [--warmup=N] [--width=N] [--height=N] [--shadows=on|off]\n";
				std::exit(0);
			} else if (auto ParsedValue = Value("--backend=")) ParsedOptions.Backend = *ParsedValue;
			else if (auto ParsedValue = Value("--scenario=")) ParsedOptions.Scenario = *ParsedValue;
			else if (auto ParsedValue = Value("--count=")) ParsedOptions.Count = ParseSize(*ParsedValue, "count");
			else if (auto ParsedValue = Value("--frames=")) ParsedOptions.Frames = ParseSize(*ParsedValue, "frames");
			else if (auto ParsedValue = Value("--warmup=")) ParsedOptions.WarmupFrames = ParseSize(*ParsedValue, "warmup");
			else if (auto ParsedValue = Value("--width=")) ParsedOptions.Width = static_cast<std::uint32_t>(ParseSize(*ParsedValue, "width"));
			else if (auto ParsedValue = Value("--height=")) ParsedOptions.Height = static_cast<std::uint32_t>(ParseSize(*ParsedValue, "height"));
			else if (auto ParsedValue = Value("--shadows=")) {
				if (*ParsedValue == "on") ParsedOptions.ShadowsEnabled = true;
				else if (*ParsedValue == "off") ParsedOptions.ShadowsEnabled = false;
				else throw std::invalid_argument("shadows must be on or off");
			}
			else throw std::invalid_argument("Unknown benchmark argument: " + std::string(Argument));
		}
		if (ParsedOptions.Backend != "projection" && ParsedOptions.Backend != "headless" && ParsedOptions.Backend != "sdl" &&
			ParsedOptions.Backend != "filament" && ParsedOptions.Backend != "filament-headless")
			throw std::invalid_argument("unknown benchmark backend");
#if !defined(GARGANTUAN_WITH_FILAMENT)
		if (ParsedOptions.Backend == "filament" || ParsedOptions.Backend == "filament-headless")
			throw std::invalid_argument("this benchmark was built without GARGANTUAN_FILAMENT_ROOT");
#endif
		if (ParsedOptions.Scenario != "all" && ParsedOptions.Scenario != "static" && ParsedOptions.Scenario != "dynamic" &&
			ParsedOptions.Scenario != "mostly-static" && ParsedOptions.Scenario != "mixed")
			throw std::invalid_argument("unknown renderer benchmark scenario");
		return ParsedOptions;
	}

	std::vector<ScenarioCase> BuildCases(const Options &BenchmarkOptions) {
		std::vector<ScenarioCase> Cases;
		auto Add = [&](std::string Name, std::initializer_list<std::size_t> Counts) {
			if (BenchmarkOptions.Scenario != "all" && BenchmarkOptions.Scenario != Name) return;
			if (BenchmarkOptions.Count) {
				Cases.push_back({std::move(Name), *BenchmarkOptions.Count});
				return;
			}
			for (const auto Count : Counts) Cases.push_back({Name, Count});
		};
		Add("static", {1000, 10000, 50000});
		Add("dynamic", {1000, 10000, 50000});
		Add("mostly-static", {50000});
		Add("mixed", {50000});
		return Cases;
	}

	Fixture BuildFixture(const ScenarioCase &Scenario, bool ShadowsEnabled) {
		using namespace gargantuan;
		Fixture Result;
		Result.Game = std::make_shared<DataModel>();
		Result.World = std::dynamic_pointer_cast<Workspace>(Result.Game->GetService("Workspace"));
		if (!Result.World) throw std::runtime_error("Renderer benchmark could not construct Workspace");
		Result.Parts.reserve(Scenario.Count);
		Result.World->Parts.reserve(Scenario.Count);
		const auto Side = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(Scenario.Count))));
		for (std::size_t Index = 0; Index < Scenario.Count; ++Index) {
			auto PartValue = std::make_shared<Part>();
			PartValue->SetCastShadow(ShadowsEnabled);
			PartValue->SetCFrame(CFrame(glm::vec3{
				static_cast<float>(Index % Side) * 2.0f,
				static_cast<float>(Index / Side) * 2.0f,
				0.0f,
			}));
			if (Scenario.Name == "mixed") {
				switch (Index % 3) {
					case 0: PartValue->SetShape(Enums::PartType::Block); break;
					case 1: PartValue->SetShape(Enums::PartType::Ball); break;
					default: PartValue->SetShape(Enums::PartType::Cylinder); break;
				}
			}
			Result.World->Parts.push_back(PartValue);
			Result.Parts.push_back(std::move(PartValue));
		}
		return Result;
	}

	void Mutate(Fixture &BenchmarkFixture, const ScenarioCase &Scenario, std::size_t Frame) {
		if (Scenario.Name == "static" || Scenario.Name == "mixed") return;
		const auto Changed = Scenario.Name == "mostly-static"
			? std::max<std::size_t>(1, BenchmarkFixture.Parts.size() / 100)
			: BenchmarkFixture.Parts.size();
		for (std::size_t Index = 0; Index < Changed; ++Index) {
			const auto ObjectIndex = Scenario.Name == "mostly-static"
				? (Index + Frame * Changed) % BenchmarkFixture.Parts.size()
				: Index;
			const auto X = static_cast<float>(ObjectIndex % 256) * 2.0f;
			const auto Y = static_cast<float>(ObjectIndex / 256) * 2.0f;
			BenchmarkFixture.Parts[ObjectIndex]->SetCFrame(gargantuan::CFrame(
				glm::vec3{X, Y, static_cast<float>(Frame % 120) * 0.01f}
			));
		}
	}

	std::size_t CountNominalDrawCalls(const gargantuan::RenderSnapshot &Snapshot) {
		return Snapshot.Items.size() + static_cast<std::size_t>(std::ranges::count(Snapshot.Items, true, &gargantuan::RenderItem::CastShadow));
	}

	void RunCase(const Options &BenchmarkOptions, const ScenarioCase &Scenario) {
		using namespace gargantuan;
		const auto SetupStart = Clock::now();
		auto BenchmarkFixture = BuildFixture(Scenario, BenchmarkOptions.ShadowsEnabled);
		const auto SetupMilliseconds = Milliseconds(Clock::now() - SetupStart);

		std::unique_ptr<BaseRenderer> Renderer;
		RenderProjection Projection;
		const Vector2 RequestedViewport(
			static_cast<float>(BenchmarkOptions.Width), static_cast<float>(BenchmarkOptions.Height)
		);
		const auto StartupStart = Clock::now();
		if (BenchmarkOptions.Backend == "sdl" || BenchmarkOptions.Backend == "filament") {
			if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
				throw std::runtime_error(std::string("SDL video initialization failed: ") + SDL_GetError());
		}
		if (BenchmarkOptions.Backend == "sdl") {
			Renderer = std::make_unique<SDLRenderer>(RequestedViewport);
		} else if (BenchmarkOptions.Backend == "headless") {
			Renderer = std::make_unique<HeadlessRenderer>(RequestedViewport);
		}
#if defined(GARGANTUAN_WITH_FILAMENT)
		else if (BenchmarkOptions.Backend == "filament" || BenchmarkOptions.Backend == "filament-headless") {
			Renderer = std::make_unique<FilamentRenderer>(
				RequestedViewport,
				BenchmarkOptions.Backend == "filament-headless",
				BenchmarkOptions.ShadowsEnabled
			);
		}
#endif
		const auto StartupMilliseconds = Milliseconds(Clock::now() - StartupStart);

		const auto [ViewportWidth, ViewportHeight] = Renderer
			? Renderer->GetViewportSize()
			: std::pair<std::uint32_t, std::uint32_t>{BenchmarkOptions.Width, BenchmarkOptions.Height};
		if (ViewportWidth == 0 || ViewportHeight == 0) throw std::runtime_error("Benchmark backend has no viewport");

		double ResizeMilliseconds = 0.0;
		if (Renderer) {
			const auto ResizeStart = Clock::now();
			Renderer->Resize(static_cast<int>(ViewportWidth + 1), static_cast<int>(ViewportHeight + 1));
			Renderer->Resize(static_cast<int>(ViewportWidth), static_cast<int>(ViewportHeight));
			ResizeMilliseconds = Milliseconds(Clock::now() - ResizeStart);
		}

		RenderExtractor Extractor;
		const auto Camera = MakeLookAtRenderCameraInput(
			{0.0f, 0.0f, 1000.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 70.0f
		);
		auto Submit = [&](const RenderSnapshotPtr &Snapshot) {
			RenderProjectionChanges Changes;
			if (BenchmarkOptions.Backend == "projection") Changes = Projection.Apply(*Snapshot);
			else {
				Renderer->Draw(Snapshot);
#if defined(GARGANTUAN_WITH_FILAMENT)
				if (auto *Filament = dynamic_cast<FilamentRenderer *>(Renderer.get()))
					Changes = Filament->GetLastMetrics().Changes;
#endif
			}
			return Changes;
		};

		const auto FirstExtractionStart = Clock::now();
		auto FirstSnapshot = Extractor.Extract(*BenchmarkFixture.World, Camera, ViewportWidth, ViewportHeight);
		const auto FirstExtractionMilliseconds = Milliseconds(Clock::now() - FirstExtractionStart);
		const auto FirstSubmissionStart = Clock::now();
		const auto FirstChanges = Submit(FirstSnapshot);
		const auto FirstSubmissionMilliseconds = Milliseconds(Clock::now() - FirstSubmissionStart);

		for (std::size_t Frame = 0; Frame < BenchmarkOptions.WarmupFrames; ++Frame) {
			Mutate(BenchmarkFixture, Scenario, Frame + 1);
			auto Snapshot = Extractor.Extract(*BenchmarkFixture.World, Camera, ViewportWidth, ViewportHeight);
			Submit(Snapshot);
		}

		std::vector<double> ExtractionSamples;
		std::vector<double> SubmissionSamples;
		std::vector<double> ProjectionScanSamples;
		std::vector<double> ChangedApplySamples;
		std::vector<double> RenderSubmitSamples;
		std::vector<double> GpuFrameSamples;
		ExtractionSamples.reserve(BenchmarkOptions.Frames);
		SubmissionSamples.reserve(BenchmarkOptions.Frames);
		std::size_t Created = 0;
		std::size_t Updated = 0;
		std::size_t Removed = 0;
		std::size_t Unchanged = 0;
		RenderSnapshotPtr LastSnapshot = FirstSnapshot;
		for (std::size_t Frame = 0; Frame < BenchmarkOptions.Frames; ++Frame) {
			Mutate(BenchmarkFixture, Scenario, BenchmarkOptions.WarmupFrames + Frame + 1);
			const auto ExtractionStart = Clock::now();
			auto Snapshot = Extractor.Extract(*BenchmarkFixture.World, Camera, ViewportWidth, ViewportHeight);
			ExtractionSamples.push_back(Milliseconds(Clock::now() - ExtractionStart));

			const auto SubmissionStart = Clock::now();
			const auto Changes = Submit(Snapshot);
			SubmissionSamples.push_back(Milliseconds(Clock::now() - SubmissionStart));
#if defined(GARGANTUAN_WITH_FILAMENT)
			if (auto *Filament = dynamic_cast<FilamentRenderer *>(Renderer.get())) {
				const auto Metrics = Filament->GetLastMetrics();
				ProjectionScanSamples.push_back(Metrics.ProjectionScanMilliseconds);
				ChangedApplySamples.push_back(Metrics.ChangedObjectApplyMilliseconds);
				RenderSubmitSamples.push_back(Metrics.RendererSubmissionMilliseconds);
				if (Metrics.GpuFrameMilliseconds) GpuFrameSamples.push_back(*Metrics.GpuFrameMilliseconds);
			}
#endif
			Created += Changes.Created;
			Updated += Changes.Updated;
			Removed += Changes.Removed;
			Unchanged += Changes.Unchanged;
			LastSnapshot = std::move(Snapshot);
		}

		std::string BackendDriver = "NA";
		std::string AutomaticInstancing = "NA";
		std::string MaxAutomaticInstances = "NA";
		std::string RendererMemory = "NA";
		if (auto *Sdl = dynamic_cast<SDLRenderer *>(Renderer.get())) BackendDriver = Sdl->GetDriverName();
#if defined(GARGANTUAN_WITH_FILAMENT)
		if (auto *Filament = dynamic_cast<FilamentRenderer *>(Renderer.get())) {
			GpuFrameSamples = Filament->GetGpuFrameHistoryMilliseconds(BenchmarkOptions.Frames);
			BackendDriver = Filament->GetBackendName();
			AutomaticInstancing = Filament->IsAutomaticInstancingEnabled() ? "true" : "false";
			MaxAutomaticInstances = std::to_string(Filament->GetMaxAutomaticInstances());
			if (const auto Memory = Filament->GetRendererOwnedBytes()) RendererMemory = std::to_string(*Memory);
		}
#endif
		auto MeanOrNa = [](const std::vector<double> &Samples) {
			if (Samples.empty()) return std::string("NA");
			std::ostringstream Output;
			Output << std::fixed << std::setprecision(3) << Mean(Samples);
			return Output.str();
		};

		std::cout << Scenario.Name << ',' << Scenario.Count << ',' << BenchmarkOptions.Backend << ',' << BenchmarkOptions.Frames << ','
			<< std::fixed << std::setprecision(3)
			<< SetupMilliseconds << ',' << StartupMilliseconds << ',' << ResizeMilliseconds << ','
			<< FirstExtractionMilliseconds << ',' << Mean(ExtractionSamples) << ',' << Percentile(ExtractionSamples, 0.95) << ','
			<< FirstSubmissionMilliseconds << ',' << Mean(SubmissionSamples) << ',' << Percentile(SubmissionSamples, 0.95) << ','
			<< (BenchmarkOptions.Backend == "sdl" ? std::to_string(CountNominalDrawCalls(*LastSnapshot)) : "NA") << ','
			<< LastSnapshot->Items.size() << ',' << FirstChanges.Created << ','
			<< Created << ',' << Updated << ',' << Removed << ',' << Unchanged << ','
			<< MeanOrNa(ProjectionScanSamples) << ',' << MeanOrNa(ChangedApplySamples) << ','
			<< MeanOrNa(RenderSubmitSamples) << ',' << MeanOrNa(GpuFrameSamples) << ',' << GpuFrameSamples.size() << ','
			<< RendererMemory << ',' << "NA," << BackendDriver << ',' << AutomaticInstancing << ',' << MaxAutomaticInstances << ','
			<< (BenchmarkOptions.ShadowsEnabled ? "on" : "off") << "\n";

		if (Renderer) {
			Renderer->Destroy();
			Renderer.reset();
		}
		if (BenchmarkOptions.Backend == "sdl" || BenchmarkOptions.Backend == "filament")
			SDL_QuitSubSystem(SDL_INIT_VIDEO);
		BenchmarkFixture.Game->Destroy();
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		const auto BenchmarkOptions = ParseOptions(ArgumentCount, Arguments);
		std::cout << "Scenario,Objects,Backend,Frames,SetupMs,BackendStartupMs,ResizeRoundTripMs,"
			"FirstExtractionMs,ExtractionMeanMs,ExtractionP95Ms,FirstSubmissionMs,SubmissionMeanMs,"
			"SubmissionP95Ms,NominalDrawCalls,PublishedObjects,InitialProjectionCreated,ProjectionCreated,"
			"ProjectionUpdated,ProjectionRemoved,ProjectionUnchanged,ProjectionScanMeanMs,ChangedApplyMeanMs,"
			"RenderSubmitMeanMs,GpuFrameMeanMs,GpuFrameSamples,RendererMemoryBytes,ActualDrawCalls,BackendDriver,AutomaticInstancing,"
			"MaxAutomaticInstances,Shadows\n";
		for (const auto &Scenario : BuildCases(BenchmarkOptions)) RunCase(BenchmarkOptions, Scenario);
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Render:Benchmark] " << Error.what() << '\n';
		return 1;
	}
}
