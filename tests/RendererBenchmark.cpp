// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/editor/EditorViewport.hpp"
#include "gargantuan/render/PrimitiveMeshes.hpp"
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
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
	using Clock = std::chrono::steady_clock;

	enum class FrameSynchronization { Completion, Queued, BoundedPair };

	struct Options {
		std::string Backend = "projection";
		std::string Scenario = "all";
		std::optional<std::size_t> Count;
		std::size_t Frames = 30;
		std::size_t WarmupFrames = 3;
		std::uint32_t Width = 1280;
		std::uint32_t Height = 720;
		bool ShadowsEnabled = false;
		bool Incremental = false;
		FrameSynchronization Synchronization = FrameSynchronization::Completion;
		std::optional<std::filesystem::path> CapturePrefix;
		std::optional<std::filesystem::path> SamplesPath;
		std::optional<bool> SdlHdr;
		bool SdlOpaqueGrouping = true;
		bool SdlProfileDetailed = false;
		bool SdlCompactTraversal = false;
		bool SdlInstancing = false;
		bool SdlSceneEnvelope = false;
		std::uint32_t CulledPercent = 50;
		bool CameraAway = false;
		std::optional<std::size_t> Lights;
		bool StaticLights = false;
	};
	constexpr std::array<std::string_view, 18> DetailedPhaseNames{
		"ShadowClassification", "ProjectionValidation", "ProjectionMutation", "MeshApplication",
		"PaletteApplication", "ShadowRecording", "SkyRecording", "OpaqueRecording",
		"ResolveRecording", "GuiRecording", "GpuSubmit", "CommandAcquire",
		"CameraTraversal", "CameraCull", "CameraResidency", "OpaqueGrouping",
		"WorldDrawRecording", "Transparent"
	};
	constexpr std::array<std::string_view, 5> DetailedCounterNames{
		"CameraVisitedObjects", "CameraHiddenObjects", "CameraFrustumTests",
		"CameraMeshLookups", "WorldPrimitiveDraws"
	};
	constexpr std::array<std::string_view, 4> InstanceCounterNames{
		"InstancedDrawCalls", "InstancedObjects", "InstanceUploadBytes", "VisibleIndexUploadBytes"
	};
	constexpr std::array<std::string_view, 3> InstancePhaseNames{
		"InstanceApplication", "InstanceUpload", "InstancePreparation"
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

	template <typename Integer = std::size_t>
	Integer ParseSize(std::string_view Value, std::string_view Name) {
		Integer Parsed = 0;
		const auto [End, Error] = std::from_chars(Value.data(), Value.data() + Value.size(), Parsed);
		if (Error != std::errc{} || End != Value.data() + Value.size() || Parsed == 0)
			throw std::invalid_argument(std::string(Name) + " must be a positive integer");
		return Parsed;
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
					"[--scenario=all|static|dynamic|mostly-static|mixed|culled|material-check|material-extensions] [--count=N] "
					"[--frames=N] [--warmup=N] [--width=N] [--height=N] [--shadows=on|off] "
					"[--incremental] [--synchronize=on|off|bounded-2] [--capture=path-prefix] [--samples=csv-path] "
					"[--sdl-hdr=on|off] [--sdl-opaque-grouping=off] [--sdl-profile=on|off] "
					"[--sdl-compact-traversal=on|off] [--sdl-instancing=on|off] "
					"[--sdl-scene-envelope=on|off] [--scenario=culled --culled-percent=0|50|100 [--camera-away]] "
					"[--scenario=local-lights --lights=N [--static-lights]]\n";
				std::exit(0);
			} else if (auto ParsedValue = Value("--backend=")) ParsedOptions.Backend = *ParsedValue;
			else if (auto ParsedValue = Value("--scenario=")) ParsedOptions.Scenario = *ParsedValue;
			else if (auto ParsedValue = Value("--count=")) ParsedOptions.Count = ParseSize(*ParsedValue, "count");
			else if (auto ParsedValue = Value("--frames=")) ParsedOptions.Frames = ParseSize(*ParsedValue, "frames");
			else if (auto ParsedValue = Value("--warmup=")) ParsedOptions.WarmupFrames = ParseSize(*ParsedValue, "warmup");
			else if (auto ParsedValue = Value("--width=")) ParsedOptions.Width = ParseSize<std::uint32_t>(*ParsedValue, "width");
			else if (auto ParsedValue = Value("--height=")) ParsedOptions.Height = ParseSize<std::uint32_t>(*ParsedValue, "height");
			else if (auto ParsedValue = Value("--lights=")) ParsedOptions.Lights = ParseSize(*ParsedValue, "lights");
			else if (Argument == "--static-lights") ParsedOptions.StaticLights = true;
			else if (Argument == "--incremental") ParsedOptions.Incremental = true;
			else if (Argument == "--sdl-opaque-grouping=off") ParsedOptions.SdlOpaqueGrouping = false;
			else if (Argument == "--sdl-profile=on") ParsedOptions.SdlProfileDetailed = true;
			else if (Argument == "--sdl-profile=off") ParsedOptions.SdlProfileDetailed = false;
			else if (Argument == "--sdl-compact-traversal=on") ParsedOptions.SdlCompactTraversal = true;
			else if (Argument == "--sdl-compact-traversal=off") ParsedOptions.SdlCompactTraversal = false;
			else if (Argument == "--sdl-instancing=on") ParsedOptions.SdlInstancing = true;
			else if (Argument == "--sdl-instancing=off") ParsedOptions.SdlInstancing = false;
			else if (Argument == "--sdl-scene-envelope=on") ParsedOptions.SdlSceneEnvelope = true;
			else if (Argument == "--sdl-scene-envelope=off") ParsedOptions.SdlSceneEnvelope = false;
			else if (Argument == "--camera-away") ParsedOptions.CameraAway = true;
			else if (auto ParsedValue = Value("--culled-percent=")) {
				if (*ParsedValue == "0") ParsedOptions.CulledPercent = 0;
				else if (*ParsedValue == "50") ParsedOptions.CulledPercent = 50;
				else if (*ParsedValue == "100") ParsedOptions.CulledPercent = 100;
				else throw std::invalid_argument("culled percent must be 0, 50 or 100");
			}
			else if (auto ParsedValue = Value("--capture=")) {
				if (ParsedValue->empty()) throw std::invalid_argument("capture prefix must not be empty");
				ParsedOptions.CapturePrefix = std::filesystem::path(*ParsedValue);
			} else if (auto ParsedValue = Value("--samples=")) {
				if (ParsedValue->empty()) throw std::invalid_argument("sample CSV path must not be empty");
				ParsedOptions.SamplesPath = std::filesystem::path(*ParsedValue);
			} else if (auto ParsedValue = Value("--sdl-hdr=")) {
				if (*ParsedValue == "on") ParsedOptions.SdlHdr = true;
				else if (*ParsedValue == "off") ParsedOptions.SdlHdr = false;
				else throw std::invalid_argument("SDL HDR must be on or off");
			} else if (auto ParsedValue = Value("--synchronize=")) {
				if (*ParsedValue == "on") ParsedOptions.Synchronization = FrameSynchronization::Completion;
				else if (*ParsedValue == "off") ParsedOptions.Synchronization = FrameSynchronization::Queued;
				else if (*ParsedValue == "bounded-2") ParsedOptions.Synchronization = FrameSynchronization::BoundedPair;
				else throw std::invalid_argument("synchronize must be on, off or bounded-2");
			}
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
		if (ParsedOptions.SdlHdr && ParsedOptions.Backend != "sdl")
			throw std::invalid_argument("--sdl-hdr requires --backend=sdl");
		if (ParsedOptions.SdlProfileDetailed && (!ParsedOptions.Incremental || ParsedOptions.Backend != "sdl"))
			throw std::invalid_argument("--sdl-profile requires an incremental SDL timing workload");
		if (ParsedOptions.SdlCompactTraversal && (!ParsedOptions.Incremental || ParsedOptions.Backend != "sdl"))
			throw std::invalid_argument("--sdl-compact-traversal requires an incremental SDL timing workload");
		if (ParsedOptions.SdlInstancing && (!ParsedOptions.Incremental || ParsedOptions.Backend != "sdl"))
			throw std::invalid_argument("--sdl-instancing requires an incremental SDL timing workload");
		if (ParsedOptions.SdlSceneEnvelope && (!ParsedOptions.Incremental || ParsedOptions.Backend != "sdl"))
			throw std::invalid_argument("--sdl-scene-envelope requires an incremental SDL timing workload");
		if (ParsedOptions.CulledPercent != 50 && ParsedOptions.Scenario != "culled")
			throw std::invalid_argument("--culled-percent requires --scenario=culled");
		if (ParsedOptions.CameraAway && (ParsedOptions.Scenario != "culled" || ParsedOptions.CulledPercent != 100))
			throw std::invalid_argument("--camera-away requires --scenario=culled --culled-percent=100");
#if !defined(GARGANTUAN_WITH_FILAMENT)
		if (ParsedOptions.Backend == "filament" || ParsedOptions.Backend == "filament-headless")
			throw std::invalid_argument("this benchmark was built without GARGANTUAN_FILAMENT_ROOT");
#endif
		const bool MaterialCapture = ParsedOptions.Scenario == "material-check" ||
			ParsedOptions.Scenario == "material-extensions";
		const bool LocalLighting = ParsedOptions.Scenario == "local-lights";
		if (!ParsedOptions.SdlOpaqueGrouping &&
			(!ParsedOptions.Incremental || ParsedOptions.Backend != "sdl" || MaterialCapture))
			throw std::invalid_argument("--sdl-opaque-grouping requires an incremental SDL timing workload");
		const bool IncrementalOnly = ParsedOptions.Scenario == "culled" || MaterialCapture || LocalLighting;
		if ((ParsedOptions.Lights || ParsedOptions.StaticLights) && !LocalLighting)
			throw std::invalid_argument("light count and movement require --scenario=local-lights");
		if (LocalLighting && (ParsedOptions.Backend != "sdl" ||
			ParsedOptions.Lights.value_or(100) > gargantuan::MaximumRenderLocalLights))
			throw std::invalid_argument("local-lights requires SDL and at most 512 authored lights");
		if (ParsedOptions.SamplesPath && (!ParsedOptions.Incremental || MaterialCapture))
			throw std::invalid_argument("sample CSV requires an incremental timed scenario");
		if (ParsedOptions.Scenario != "all" && ParsedOptions.Scenario != "static" && ParsedOptions.Scenario != "dynamic" &&
			ParsedOptions.Scenario != "mostly-static" && ParsedOptions.Scenario != "mixed" && !IncrementalOnly)
			throw std::invalid_argument("unknown renderer benchmark scenario");
		if (!ParsedOptions.Incremental && (IncrementalOnly || ParsedOptions.CapturePrefix ||
			ParsedOptions.Synchronization != FrameSynchronization::Completion))
			throw std::invalid_argument("this scenario or option requires --incremental");
		if (ParsedOptions.Synchronization == FrameSynchronization::BoundedPair && (MaterialCapture ||
			(ParsedOptions.Backend != "sdl" && ParsedOptions.Backend != "filament-headless")))
			throw std::invalid_argument("bounded-2 synchronization requires an offscreen graphical timing workload");
		if (ParsedOptions.Incremental && (ParsedOptions.Count.value_or(1) > 50'000 || ParsedOptions.Frames > 1'800 ||
			ParsedOptions.WarmupFrames > 1'800 || ParsedOptions.Width < 64 || ParsedOptions.Height < 64 ||
			ParsedOptions.Width > 4'096 || ParsedOptions.Height > 4'096))
			throw std::invalid_argument("incremental benchmark dimensions or workload exceed bounds");
		if (MaterialCapture && (!ParsedOptions.CapturePrefix ||
			(ParsedOptions.Backend != "sdl" && ParsedOptions.Backend != "filament-headless")))
			throw std::invalid_argument("material captures require --capture and an offscreen graphical backend");
		if (ParsedOptions.CapturePrefix && !MaterialCapture)
			throw std::invalid_argument("capture requires --scenario=material-check or --scenario=material-extensions");
		if (ParsedOptions.Incremental && ParsedOptions.Backend == "filament")
			throw std::invalid_argument("incremental qualification requires --backend=filament-headless for Filament");
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
		if (BenchmarkOptions.Incremental) Add("culled", {50000});
		if (BenchmarkOptions.Scenario == "local-lights") Add("local-lights", {1000});
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
			Renderer = std::make_unique<SDLRenderer>(RequestedViewport,
				SDLRendererOptions{.EnableHdrScene = BenchmarkOptions.SdlHdr.value_or(true)});
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

	// These fixtures exercise the renderer boundary directly. The legacy benchmark
	// above and Foundation 2 CPU benchmark still measure engine-side extraction.
	constexpr gargantuan::RenderMeshIdentity CommonMesh{1, 1};
	constexpr gargantuan::RenderTextureIdentity FirstTexture{1, 1};
	constexpr gargantuan::RenderTextureIdentity SecondTexture{2, 1};
	constexpr std::uint32_t TextureSide = 64;

	struct VideoGuard {
		VideoGuard() {
			if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
				throw std::runtime_error(std::string("SDL video initialization failed: ") + SDL_GetError());
		}
		~VideoGuard() { SDL_QuitSubSystem(SDL_INIT_VIDEO); }
	};

	gargantuan::RenderFrameState MakeFrame(const Options &Settings, float Distance) {
		gargantuan::RenderFrameState Frame{.ViewportWidth = Settings.Width, .ViewportHeight = Settings.Height};
		auto &Camera = Frame.Camera;
		Camera.Position = {0.0f, 0.0f, Distance};
		Camera.VerticalFieldOfView = 45.0f;
		Camera.ViewMatrix = glm::lookAt(Camera.Position, glm::vec3(0.0f), Camera.UpDirection);
		if (Settings.CameraAway) {
			Camera.LookDirection = {0.0f, 0.0f, 1.0f};
			Camera.RightDirection = {-1.0f, 0.0f, 0.0f};
			Camera.ViewMatrix = glm::lookAt(Camera.Position, Camera.Position + Camera.LookDirection,
				Camera.UpDirection);
		}
		Camera.ProjectionMatrix = glm::perspectiveRH_ZO(glm::radians(Camera.VerticalFieldOfView),
			static_cast<float>(Settings.Width) / Settings.Height, Camera.NearPlane, Camera.FarPlane);
		Camera.ViewProjectionMatrix = Camera.ProjectionMatrix * Camera.ViewMatrix;
		Frame.Environment.SunDirection = {0.0f, 0.0f, 1.0f};
		Frame.Environment.SunIntensity = 2.0f;
		Frame.Environment.AmbientColor = {0.04f, 0.04f, 0.04f};
		Frame.Environment.ExposureMultiplier = 1.0f;
		Frame.Environment.EnvironmentColor = {0.01f, 0.01f, 0.01f};
		return Frame;
	}

	std::shared_ptr<const std::vector<std::uint8_t>> MakePixels(
		std::uint32_t Width, std::uint32_t Height, const std::array<std::uint8_t, 4> &Color
	) {
		auto Pixels = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(Width) * Height * 4);
		for (std::size_t Offset = 0; Offset < Pixels->size(); Offset += 4)
			std::copy(Color.begin(), Color.end(), Pixels->begin() + Offset);
		return Pixels;
	}

	void AddTexturePair(gargantuan::RenderPublication &Publication) {
		using namespace gargantuan;
		Publication.TextureCreates.push_back({FirstTexture, 1, TextureSide, TextureSide,
			RenderTextureFormat::Rgba8Unorm, MakePixels(TextureSide, TextureSide, {224, 24, 24, 255})});
		Publication.TextureCreates.push_back({SecondTexture, 1, TextureSide, TextureSide,
			RenderTextureFormat::Rgba8Unorm, MakePixels(TextureSide, TextureSide, {24, 224, 24, 255})});
	}

	std::shared_ptr<const std::vector<gargantuan::RenderPrimitiveMaterialState>> MakeMaterialPair(
		std::uint32_t FirstCount, std::uint32_t TotalCount
	) {
		using namespace gargantuan;
		const RenderMaterialState First{.BaseColorTexture = FirstTexture, .Roughness = 0.7f, .DoubleSided = true,
			.ShadingModel = RenderShadingModel::MetallicRoughness};
		const RenderMaterialState Second{.BaseColorTexture = SecondTexture, .Metallic = 0.12f, .Roughness = 0.4f,
			.DoubleSided = true, .ShadingModel = RenderShadingModel::MetallicRoughness};
		return std::make_shared<const std::vector<RenderPrimitiveMaterialState>>(
			std::initializer_list<RenderPrimitiveMaterialState>{{0, FirstCount, First},
				{FirstCount, TotalCount - FirstCount, Second}});
	}

	gargantuan::RenderPublication MakeGridInitial(const Options &Settings, const ScenarioCase &Scenario) {
		using namespace gargantuan;
		const auto Side = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(Scenario.Count))));
		const auto Span = 2.0f * static_cast<float>(Side);
		const auto Aspect = static_cast<float>(Settings.Width) / Settings.Height;
		const auto Distance = std::max(4.0f, Span * 1.5f / std::min(1.0f, Aspect));
		RenderPublication Initial{.Id = 1, .FullResync = true, .Frame = MakeFrame(Settings, Distance)};
		if (Scenario.Name == "local-lights") {
			const auto Count = Settings.Lights.value_or(100);
			auto Lights = std::make_shared<std::vector<RenderLocalLightState>>();
			Lights->reserve(Count);
			const auto Grid = static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(Count))));
			for (std::uint32_t Index = 0; Index < Count; ++Index) {
				const float X = ((Index % Grid + 0.5f) / Grid - 0.5f) * Span;
				const float Y = ((Index / Grid + 0.5f) / Grid - 0.5f) * Span;
				const bool Outside = Index % 8 == 7;
				const bool Spot = Index % 2 == 0;
				Lights->push_back({.Object = {static_cast<std::uint32_t>(Scenario.Count) + Index + 1, 1},
					.Kind = Spot ? RenderLocalLightKind::Spot : RenderLocalLightKind::Point,
					.Position = {X + (Outside ? Span * 5.0f : 0.0f), Y, 3.5f}, .Color = {1.0f, 0.7f, 0.4f},
					.Intensity = 16.0f, .Range = std::clamp(Span / 6.0f, 4.0f, 32.0f),
					.InnerConeCosine = Spot ? 0.9f : 1.0f, .OuterConeCosine = Spot ? 0.7f : 1.0f});
			}
			Initial.Frame.Environment.LocalLights = Lights;
			Initial.EnvironmentChanged = true;
		}
		const auto Block = PrimitiveMeshes::Block();
		auto Vertices = std::make_shared<std::vector<RenderVertex>>();
		Vertices->reserve(Block.Vertices.size());
		for (const auto &Vertex : Block.Vertices)
			Vertices->push_back({Vertex.Position, Vertex.Normal, Vertex.Tangent, Vertex.UV});
		Initial.MeshCreates.push_back({CommonMesh, 1, 1, Vertices,
			std::make_shared<const std::vector<std::uint32_t>>(Block.Indices),
			{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}}});
		std::shared_ptr<const std::vector<RenderPrimitiveMaterialState>> Materials;
		if (Scenario.Name == "mixed") {
			AddTexturePair(Initial);
			Materials = MakeMaterialPair(6, static_cast<std::uint32_t>(Block.Indices.size()));
		}
		const RenderMaterialState Material{.BaseColorFactor = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f),
			.ShadingModel = RenderShadingModel::MetallicRoughness};
		Initial.Creates.reserve(Scenario.Count);
		for (std::size_t Index = 0; Index < Scenario.Count; ++Index) {
			const bool BehindCamera = Scenario.Name == "culled" && !Settings.CameraAway &&
				(Settings.CulledPercent == 100 || (Settings.CulledPercent == 50 && Index % 2 != 0));
			const glm::vec3 Position{2.0f * static_cast<float>(Index % Side) - (Span - 2.0f) * 0.5f,
				2.0f * static_cast<float>(Index / Side) - (Span - 2.0f) * 0.5f,
				BehindCamera ? Distance + 5.0f : 0.0f};
			RenderItem Item{.Object = {static_cast<std::uint32_t>(Index + 1), 1},
				.ModelMatrix = glm::translate(glm::mat4(1.0f), Position),
				.InverseModelMatrix = glm::translate(glm::mat4(1.0f), -Position),
				.CastShadow = Settings.ShadowsEnabled};
			Initial.Creates.push_back({.Item = Item, .Mesh = CommonMesh, .Material = Material, .Primitives = Materials});
		}
		return Initial;
	}

	gargantuan::RenderPublication MakeGridDelta(
		const gargantuan::RenderPublication &Initial, const ScenarioCase &Scenario, const Options &Settings,
		std::size_t Frame
	) {
		using namespace gargantuan;
		RenderPublication Delta{.Id = Frame + 2, .BaseId = Frame + 1, .Frame = Initial.Frame};
		if (Scenario.Name == "local-lights" && !Settings.StaticLights) {
			auto Lights = std::make_shared<std::vector<RenderLocalLightState>>(*Initial.Frame.Environment.LocalLights);
			for (auto &Light : *Lights) Light.Position.x += static_cast<float>(Frame % 16) * 0.125f;
			Delta.Frame.Environment.LocalLights = Lights;
			Delta.EnvironmentChanged = true;
		}
		const bool MostlyStatic = Scenario.Name == "mostly-static" || Scenario.Name == "mixed";
		const auto Count = Scenario.Name == "dynamic" ? Scenario.Count :
			MostlyStatic ? std::max<std::size_t>(1, Scenario.Count / 100) : 0;
		Delta.Updates.reserve(Count);
		for (std::size_t Index = 0; Index < Count; ++Index) {
			const auto &InitialObject = Initial.Creates[(Index + (MostlyStatic ? Frame * Count : 0)) % Scenario.Count];
			auto Item = InitialObject.Item;
			Item.ModelMatrix[3].z = 0.05f * static_cast<float>(Frame % 7 + 1);
			Item.InverseModelMatrix[3].z = -Item.ModelMatrix[3].z;
			Delta.Updates.push_back({.Object = Item.Object, .Domains = RenderUpdateDomain::Transform,
				.Item = Item, .Mesh = InitialObject.Mesh, .Material = InitialObject.Material,
				.Primitives = InitialObject.Primitives});
		}
		if (Scenario.Name == "mixed") {
			const auto Side = Frame % 2 == 0 ? 8u : TextureSide;
			Delta.TextureUpdates.push_back({FirstTexture, Frame + 2, 0, 0, Side, Side,
				MakePixels(Side, Side, {224, static_cast<std::uint8_t>(24 + Frame % 64), 24, 255})});
		}
		return Delta;
	}

	struct IncrementalCounters {
		std::optional<std::uint64_t> Submitted;
		std::optional<std::uint64_t> Skipped;
		std::optional<std::uint64_t> Resyncs;
		std::optional<std::uint64_t> DrawCalls;
		std::optional<std::uint64_t> ResourceCreations;
		std::optional<std::uint64_t> UploadedBytes;
		std::optional<std::uint64_t> CameraCulledObjects;
		std::optional<std::uint64_t> WorldGeometryBindings;
		std::optional<std::uint64_t> WorldMaterialBindings;
		std::array<std::optional<std::uint64_t>, 7> OpaqueGrouping;
		std::optional<std::uint64_t> HdrSceneBytes;
		std::optional<std::uint64_t> SceneResolveDraws;
		std::optional<std::uint64_t> LightUploadBytes;
		std::array<std::optional<std::uint64_t>, 4> DirectionalShadow;
	};

	template <typename Metrics>
	std::optional<std::uint64_t> GetNativeFrameUploadBytes(const Metrics &Value) {
		if constexpr (requires { Value.NativeVertexUploadBytes; Value.NativeTextureUploadBytes; })
			return Value.NativeVertexUploadBytes + Value.NativeTextureUploadBytes;
		return std::nullopt;
	}

	template <typename MetricsType>
	IncrementalCounters ReadSdlCounters(const MetricsType &Metrics) {
		IncrementalCounters Result{Metrics.FramesSubmitted, 0, Metrics.FullResyncs, Metrics.DrawCalls,
			Metrics.VertexBufferCreations + Metrics.IndexBufferCreations + Metrics.TransferBufferCreations +
			Metrics.TextureCreations + Metrics.BufferReallocations, Metrics.UploadedBytes};
		if constexpr (requires { Metrics.CameraCulledObjects; Metrics.WorldGeometryBindings; Metrics.WorldMaterialBindings; }) {
			Result.CameraCulledObjects = Metrics.CameraCulledObjects;
			Result.WorldGeometryBindings = Metrics.WorldGeometryBindings;
			Result.WorldMaterialBindings = Metrics.WorldMaterialBindings;
		}
		Result.OpaqueGrouping = {Metrics.OpaqueGroupingCandidates, Metrics.OpaqueGroupedObjects,
			Metrics.OpaqueGroupingUnprovenBounds, Metrics.OpaqueGroupingConflictBarriers, Metrics.OpaqueGroupingBudgetBarriers,
			Metrics.OpaqueGroupingPairTests, Metrics.OpaqueGroupingDepthProofs};
		Result.HdrSceneBytes = Metrics.HdrSceneBytes;
		Result.SceneResolveDraws = Metrics.SceneResolveDraws;
		Result.LightUploadBytes = Metrics.LocalLightUploadBytes;
		Result.DirectionalShadow = {Metrics.ShadowMapBuilds, Metrics.ShadowCacheHits,
			Metrics.ShadowMapPasses, Metrics.ShadowCasterDraws};
		return Result;
	}

	// Keep the historical adapter buildable. Missing counters make its rows
	// ineligible for a comparison, even when it appears to render something.
	template <typename Metrics>
	IncrementalCounters ReadCounters(const Metrics &Value) {
		if constexpr (requires { Value.SubmittedFrames; Value.SkippedFrames; Value.FullResyncs; })
			return {.Submitted = Value.SubmittedFrames, .Skipped = Value.SkippedFrames, .Resyncs = Value.FullResyncs};
		return {};
	}

	std::string CounterDelta(std::optional<std::uint64_t> Start, std::optional<std::uint64_t> End) {
		if (!Start || !End) return "NA";
		if (*End < *Start) throw std::logic_error("benchmark counter decreased during measurement");
		return std::to_string(*End - *Start);
	}

	void RunIncrementalCase(const Options &Settings, const ScenarioCase &Scenario, std::ostream *Samples) {
		using namespace gargantuan;
		const bool CompleteEachFrame = Settings.Synchronization == FrameSynchronization::Completion;
		const bool CompletePairs = Settings.Synchronization == FrameSynchronization::BoundedPair;
		const auto StartSetup = Clock::now();
		const auto Initial = std::make_shared<const RenderPublication>(MakeGridInitial(Settings, Scenario));
		const auto SetupMs = Milliseconds(Clock::now() - StartSetup);
		const Vector2 Viewport(static_cast<float>(Settings.Width), static_cast<float>(Settings.Height));
		std::optional<VideoGuard> Video;
		std::unique_ptr<BaseRenderer> Backend;
		SDLRenderer *Sdl = nullptr;
#if defined(GARGANTUAN_WITH_FILAMENT)
		FilamentRenderer *Filament = nullptr;
#endif
		RenderProjection Projection;
		const auto StartStartup = Clock::now();
		if (Settings.Backend == "sdl") {
			Video.emplace();
			auto Renderer = std::make_unique<SDLRenderer>(Viewport, SDLRendererOptions{
				.Offscreen = true, .WaitForGpuCompletion = CompleteEachFrame, .DebugDevice = false,
				.EnableHdrScene = Settings.SdlHdr.value_or(true),
				.EnableOpaqueGrouping = Settings.SdlOpaqueGrouping,
				.ProfileDetailed = Settings.SdlProfileDetailed,
				.UseCompactTraversal = Settings.SdlCompactTraversal,
				.EnableInstancing = Settings.SdlInstancing,
				.EnableSceneEnvelope = Settings.SdlSceneEnvelope,
			});
			Sdl = Renderer.get();
			Backend = std::move(Renderer);
		} else if (Settings.Backend == "headless") Backend = std::make_unique<HeadlessRenderer>(Viewport);
#if defined(GARGANTUAN_WITH_FILAMENT)
		else if (Settings.Backend == "filament-headless") {
			auto Renderer = std::make_unique<FilamentRenderer>(Viewport, true, Settings.ShadowsEnabled);
			Filament = Renderer.get();
			Backend = std::move(Renderer);
		}
#endif
		const auto StartupMs = Milliseconds(Clock::now() - StartStartup);
		const auto Drain = [&] {
			if (Sdl) Sdl->WaitForIdle();
#if defined(GARGANTUAN_WITH_FILAMENT)
			if (Filament) Filament->FlushAndWait();
#endif
		};
		// Apply the same queue bound to both backends without overriding Filament's
		// native frame admission. Its frame skipper looks two submissions back.
		const auto ShouldDrain = [&](std::size_t CompletedFrames) {
			return (CompleteEachFrame && !Sdl) || (CompletePairs && CompletedFrames % 2 == 0);
		};
		const auto Counters = [&]() -> IncrementalCounters {
			if (Sdl) return ReadSdlCounters(Sdl->GetMetrics());
#if defined(GARGANTUAN_WITH_FILAMENT)
			if (Filament) return ReadCounters(Filament->GetLastMetrics());
#endif
			return {};
		};
		const auto Draw = [&](const RenderPublicationPtr &Publication) {
			if (Backend) Backend->Draw(Publication);
			else static_cast<void>(Projection.Apply(*Publication));
		};
		const auto StartFirstFrame = Clock::now();
		Draw(Initial);
		Drain();
		const auto FirstFrameMs = Milliseconds(Clock::now() - StartFirstFrame);
		if (const auto InitialCounters = Counters(); InitialCounters.Submitted && *InitialCounters.Submitted != 1)
			throw std::runtime_error("initial publication did not submit one frame");
		for (std::size_t Frame = 0; Frame < Settings.WarmupFrames; ++Frame) {
			Draw(std::make_shared<const RenderPublication>(MakeGridDelta(*Initial, Scenario, Settings, Frame)));
			if (ShouldDrain(Frame + 1)) Drain();
		}
		Drain();
		const auto Baseline = Counters();
		const auto DetailedBaseline = Sdl ? std::optional(Sdl->GetMetrics()) : std::nullopt;
		auto DetailedPrevious = DetailedBaseline;
		auto InstancePrevious = DetailedBaseline;
		std::vector<double> FixtureTimes, FrameTimes, ProjectionTimes, ApplyTimes, SubmitTimes, WaitTimes;
		std::array<std::vector<double>, DetailedPhaseNames.size()> DetailedTimes;
		std::array<std::vector<std::uint64_t>, DetailedCounterNames.size()> DetailedCounts;
		std::array<std::vector<std::uint64_t>, InstanceCounterNames.size()> InstanceCounts;
		std::array<std::vector<double>, InstancePhaseNames.size()> InstanceTimes;
		std::array<std::vector<std::uint64_t>, 3> EnvelopeCounts;
		auto EnvelopePrevious = DetailedBaseline;
		FixtureTimes.reserve(Settings.Frames);
		FrameTimes.reserve(Settings.Frames);
		std::uint64_t LogicalUpdates = 0;
		std::uint64_t LogicalTextureBytes = 0;
		std::optional<std::uint64_t> NativeFrameUploadBytes;
		std::vector<double> LightGridTimes;
		for (std::size_t Frame = 0; Frame < Settings.Frames; ++Frame) {
			const auto StartFixture = Clock::now();
			const auto Delta = std::make_shared<const RenderPublication>(
				MakeGridDelta(*Initial, Scenario, Settings, Settings.WarmupFrames + Frame));
			FixtureTimes.push_back(Milliseconds(Clock::now() - StartFixture));
			LogicalUpdates += Delta->Updates.size();
			for (const auto &Update : Delta->TextureUpdates) LogicalTextureBytes += Update.Pixels->size();
			const auto StartFrame = Clock::now();
			Draw(Delta);
			const auto EndDraw = Clock::now();
			const bool DrainFrame = ShouldDrain(Frame + 1);
			if (DrainFrame) Drain();
			const auto EndFrame = Clock::now();
			FrameTimes.push_back(Milliseconds(EndFrame - StartFrame));
			if (CompletePairs) WaitTimes.push_back(DrainFrame ? Milliseconds(EndFrame - EndDraw) : 0.0);
			if (Sdl) {
				const auto Metrics = Sdl->GetMetrics();
				const std::array<std::uint64_t, 3> EnvelopeValues{
					Metrics.EnvelopeRejections, Metrics.EnvelopeBuildNanoseconds, Metrics.EnvelopeRebuilds};
				const std::array<std::uint64_t, 3> PreviousEnvelopeValues{
					EnvelopePrevious->EnvelopeRejections, EnvelopePrevious->EnvelopeBuildNanoseconds,
					EnvelopePrevious->EnvelopeRebuilds};
				for (std::size_t Index = 0; Index < EnvelopeValues.size(); ++Index) {
					if (EnvelopeValues[Index] < PreviousEnvelopeValues[Index]) throw std::logic_error("envelope counter decreased");
					EnvelopeCounts[Index].push_back(EnvelopeValues[Index] - PreviousEnvelopeValues[Index]);
				}
				EnvelopePrevious = Metrics;
				if (Settings.SdlInstancing) {
					const std::array<std::uint64_t, InstancePhaseNames.size()> InstanceNanoseconds{
						Metrics.LastInstanceApplicationNanoseconds, Metrics.LastInstanceUploadNanoseconds,
						Metrics.LastInstancePreparationNanoseconds};
					for (std::size_t Index = 0; Index < InstanceNanoseconds.size(); ++Index)
						InstanceTimes[Index].push_back(static_cast<double>(InstanceNanoseconds[Index]) / 1'000'000.0);
				}
				const std::array<std::uint64_t, InstanceCounterNames.size()> InstanceValues{
					Metrics.InstancedDrawCalls, Metrics.InstancedObjects,
					Metrics.InstanceUploadBytes, Metrics.VisibleIndexUploadBytes};
				const std::array<std::uint64_t, InstanceCounterNames.size()> PreviousInstanceValues{
					InstancePrevious->InstancedDrawCalls, InstancePrevious->InstancedObjects,
					InstancePrevious->InstanceUploadBytes, InstancePrevious->VisibleIndexUploadBytes};
				for (std::size_t Index = 0; Index < InstanceValues.size(); ++Index) {
					if (InstanceValues[Index] < PreviousInstanceValues[Index])
						throw std::logic_error("instance counter decreased");
					InstanceCounts[Index].push_back(InstanceValues[Index] - PreviousInstanceValues[Index]);
				}
				InstancePrevious = Metrics;
				const std::array<std::uint64_t, DetailedPhaseNames.size()> PhaseNanoseconds{
						Metrics.LastShadowClassificationNanoseconds, Metrics.LastProjectionValidationNanoseconds,
						Metrics.LastProjectionMutationNanoseconds, Metrics.LastMeshApplicationNanoseconds,
						Metrics.LastPaletteApplicationNanoseconds, Metrics.LastShadowRecordingNanoseconds,
						Metrics.LastSkyRecordingNanoseconds, Metrics.LastOpaqueRecordingNanoseconds,
						Metrics.LastResolveRecordingNanoseconds, Metrics.LastGuiRecordingNanoseconds,
						Metrics.LastGpuSubmitNanoseconds, Metrics.LastCommandAcquireNanoseconds,
						Metrics.LastCameraTraversalNanoseconds, Metrics.LastCameraCullNanoseconds,
						Metrics.LastCameraResidencyNanoseconds, Metrics.LastOpaqueGroupingNanoseconds,
						Metrics.LastWorldDrawRecordingNanoseconds, Metrics.LastTransparentNanoseconds
				};
				for (std::size_t Index = 0; Index < PhaseNanoseconds.size(); ++Index) {
					// The first twelve clocks are always measured. Object-path
					// clocks are opt-in and must be NA when profiling is off.
					if (Index >= 12 && !Settings.SdlProfileDetailed) continue;
					DetailedTimes[Index].push_back(static_cast<double>(PhaseNanoseconds[Index]) / 1'000'000.0);
				}
				if (Settings.SdlProfileDetailed) {
					const std::array<std::uint64_t, DetailedCounterNames.size()> Counts{
						Metrics.CameraVisitedObjects, Metrics.CameraHiddenObjects, Metrics.CameraFrustumTests,
						Metrics.CameraMeshLookups, Metrics.WorldPrimitiveDraws
					};
					const std::array<std::uint64_t, DetailedCounterNames.size()> PreviousCounts{
						DetailedPrevious->CameraVisitedObjects, DetailedPrevious->CameraHiddenObjects,
						DetailedPrevious->CameraFrustumTests, DetailedPrevious->CameraMeshLookups,
						DetailedPrevious->WorldPrimitiveDraws
					};
					for (std::size_t Index = 0; Index < Counts.size(); ++Index) {
						if (Counts[Index] < PreviousCounts[Index]) throw std::logic_error("detailed counter decreased");
						DetailedCounts[Index].push_back(Counts[Index] - PreviousCounts[Index]);
					}
					DetailedPrevious = Metrics;
				}
				LightGridTimes.push_back(static_cast<double>(Metrics.LastLocalLightGridNanoseconds) / 1'000'000.0);
				ProjectionTimes.push_back(static_cast<double>(Metrics.LastProjectionNanoseconds) / 1'000'000.0);
				ApplyTimes.push_back(static_cast<double>(Metrics.LastMeshTransferNanoseconds +
					Metrics.LastTextureTransferNanoseconds + Metrics.LastUiPreparationNanoseconds) / 1'000'000.0);
				SubmitTimes.push_back(static_cast<double>(Metrics.LastSubmissionNanoseconds) / 1'000'000.0);
				if (CompleteEachFrame)
					WaitTimes.push_back(static_cast<double>(Metrics.LastGpuCompletionWaitNanoseconds) / 1'000'000.0);
			}
#if defined(GARGANTUAN_WITH_FILAMENT)
			if (Filament) {
				const auto Metrics = Filament->GetLastMetrics();
				ProjectionTimes.push_back(Metrics.ProjectionScanMilliseconds);
				ApplyTimes.push_back(Metrics.ChangedObjectApplyMilliseconds);
				SubmitTimes.push_back(Metrics.RendererSubmissionMilliseconds);
				if (const auto Bytes = GetNativeFrameUploadBytes(Metrics))
					NativeFrameUploadBytes = NativeFrameUploadBytes.value_or(0) + *Bytes;
				if (CompleteEachFrame) WaitTimes.push_back(Milliseconds(EndFrame - EndDraw));
			}
#endif
		}
		const auto StartDrain = Clock::now();
		Drain();
		const auto DrainMs = Milliseconds(Clock::now() - StartDrain);
		const auto Final = Counters();
		const auto Submitted = CounterDelta(Baseline.Submitted, Final.Submitted);
		const auto Skipped = CounterDelta(Baseline.Skipped, Final.Skipped);
		const auto Resyncs = CounterDelta(Baseline.Resyncs, Final.Resyncs);
		const auto ExpectedCameraCull = Scenario.Name != "culled" ? 0 : Settings.Frames *
			(Settings.CulledPercent == 100 ? Scenario.Count : Settings.CulledPercent == 50 ? Scenario.Count / 2 : 0);
		const auto ObservedCameraCull = CounterDelta(Baseline.CameraCulledObjects, Final.CameraCulledObjects);
		const bool CullMeasured = ObservedCameraCull != "NA";
		const bool CullGate = !CullMeasured || ObservedCameraCull == std::to_string(ExpectedCameraCull);
		const bool Graphical = Backend && Backend->GetCapabilities().Graphical;
		const bool Comparable = Graphical && Submitted == std::to_string(Settings.Frames) &&
			Skipped == "0" && Resyncs == "0";
		std::string Driver = Sdl ? Sdl->GetDriverName() : "NA";
		std::string Projected = !Backend ? std::to_string(Projection.GetSize()) : "NA";
#if defined(GARGANTUAN_WITH_FILAMENT)
		if (Filament) {
			Driver = Filament->GetBackendName();
			Projected = std::to_string(Filament->GetLastMetrics().ProjectedObjects);
		}
#endif
		const auto MeanOrNa = [](const std::vector<double> &Values) {
			if (Values.empty()) return std::string("NA");
			std::ostringstream Output;
			Output << std::fixed << std::setprecision(4) << Mean(Values);
			return Output.str();
		};
		const auto Synchronization = CompleteEachFrame ? "completion" : CompletePairs ? "bounded-2" : "queued";
		const auto HdrRequested = Sdl ? (Settings.SdlHdr.value_or(true) ? "on" : "off") : "NA";
		const auto HdrActive = Final.HdrSceneBytes ? (*Final.HdrSceneBytes ? "on" : "off") : "NA";
		std::cout << std::fixed << std::setprecision(4) << Scenario.Name << ',' << Scenario.Count << ','
			<< Settings.Backend << ',' << Driver << ',' << Synchronization << ','
			<< Settings.Width << ',' << Settings.Height << ',' << Settings.Frames << ',' << Settings.WarmupFrames << ','
			<< SetupMs << ',' << StartupMs << ',' << FirstFrameMs << ',' << Mean(FixtureTimes) << ','
			<< Mean(FrameTimes) << ',' << Percentile(FrameTimes, 0.50) << ',' << Percentile(FrameTimes, 0.95) << ','
			<< Percentile(FrameTimes, 0.99) << ',' << *std::ranges::max_element(FrameTimes) << ','
			<< MeanOrNa(ProjectionTimes) << ',' << MeanOrNa(ApplyTimes) << ',' << MeanOrNa(SubmitTimes) << ','
			<< MeanOrNa(WaitTimes) << ',' << DrainMs << ','
			<< LogicalUpdates << ',' << LogicalTextureBytes << ',' << Projected << ',' << Submitted << ','
			<< Skipped << ',' << Resyncs << ',' << CounterDelta(Baseline.DrawCalls, Final.DrawCalls) << ','
			<< CounterDelta(Baseline.ResourceCreations, Final.ResourceCreations) << ','
			<< (NativeFrameUploadBytes ? std::to_string(*NativeFrameUploadBytes) :
				CounterDelta(Baseline.UploadedBytes, Final.UploadedBytes)) << ','
			<< ExpectedCameraCull << ',' << ObservedCameraCull << ','
			<< CounterDelta(Baseline.WorldGeometryBindings, Final.WorldGeometryBindings) << ','
			<< CounterDelta(Baseline.WorldMaterialBindings, Final.WorldMaterialBindings) << ','
			<< (CullMeasured ? (CullGate ? "PASS" : "FAIL") : "NA") << ','
			<< (Graphical ? (Comparable ? "PASS" : "FAIL") : "NA") << ",NA,"
			<< (Settings.ShadowsEnabled ? "on" : "off");
		for (std::size_t Index = 0; Index < Final.OpaqueGrouping.size(); ++Index)
			std::cout << ',' << CounterDelta(Baseline.OpaqueGrouping[Index], Final.OpaqueGrouping[Index]);
		std::cout << ',' << HdrRequested << ',' << HdrActive << ','
			<< (Final.HdrSceneBytes ? std::to_string(*Final.HdrSceneBytes) : "NA") << ','
			<< CounterDelta(Baseline.SceneResolveDraws, Final.SceneResolveDraws) << ','
			<< (Initial->Frame.Environment.LocalLights ? Initial->Frame.Environment.LocalLights->size() : 0) << ','
			<< (Scenario.Name == "local-lights" ? (Settings.StaticLights ? "static" : "moving") : "NA") << ','
			<< MeanOrNa(LightGridTimes) << ',' << CounterDelta(Baseline.LightUploadBytes, Final.LightUploadBytes);
		if (Sdl) {
			const auto Metrics = Sdl->GetMetrics();
			std::cout << ',' << Metrics.VisibleLocalLights << ',' << Metrics.OverBudgetLightTiles << ','
				<< Metrics.DroppedLightTileIndices << ',' << Metrics.MaterialViewBytes << ','
				<< Metrics.MaterialSourceBytes << ',' << Metrics.MaterialViewFallbacks << ','
				<< Metrics.MaterialUploadStagingBytes << ','
				<< Metrics.IblBuilds << ',' << Metrics.IblPreprocessPasses;
		} else std::cout << ",NA,NA,NA,NA,NA,NA,NA,NA,NA";
		for (std::size_t Index = 0; Index < Final.DirectionalShadow.size(); ++Index)
			std::cout << ',' << CounterDelta(Baseline.DirectionalShadow[Index], Final.DirectionalShadow[Index]);
		std::cout << ',' << (Sdl ? (Settings.SdlOpaqueGrouping ? "on" : "off") : "NA")
			<< ',' << (Sdl ? (Settings.SdlProfileDetailed ? "on" : "off") : "NA")
			<< ',' << (Scenario.Name == "culled" ? std::to_string(Settings.CulledPercent) : "NA")
			<< ',' << (Sdl ? (Settings.SdlCompactTraversal ? "on" : "off") : "NA")
			<< ',' << (Sdl ? (Settings.SdlInstancing ? "on" : "off") : "NA")
			<< ',' << (Sdl ? (Settings.SdlSceneEnvelope ? "on" : "off") : "NA")
			<< ',' << (Settings.CameraAway ? "on" : "off");
		for (const auto &Times : DetailedTimes) std::cout << ',' << MeanOrNa(Times);
		for (const auto &Counts : DetailedCounts) {
			if (Counts.empty()) std::cout << ",NA";
			else std::cout << ',' << std::accumulate(Counts.begin(), Counts.end(), std::uint64_t{});
		}
		for (const auto &Counts : InstanceCounts) {
			if (Counts.empty()) std::cout << ",NA";
			else std::cout << ',' << std::accumulate(Counts.begin(), Counts.end(), std::uint64_t{});
		}
		for (const auto &Times : InstanceTimes) std::cout << ',' << MeanOrNa(Times);
		for (const auto &Counts : EnvelopeCounts) {
			if (Counts.empty()) std::cout << ",NA";
			else std::cout << ',' << std::accumulate(Counts.begin(), Counts.end(), std::uint64_t{});
		}
		std::cout << '\n';
		if (Samples) {
			for (std::size_t Frame = 0; Frame < FrameTimes.size(); ++Frame) {
				*Samples << Scenario.Name << ',' << Scenario.Count << ',' << Settings.Backend << ',' << Driver << ','
					<< Synchronization << ',' << HdrActive << ',' << Settings.Width << ',' << Settings.Height << ',' << Frame << ','
					<< Settings.WarmupFrames + Frame + 2 << ',' << FixtureTimes[Frame] << ',' << FrameTimes[Frame];
				for (const auto *Phase : {&ProjectionTimes, &ApplyTimes, &SubmitTimes, &WaitTimes, &LightGridTimes}) {
					*Samples << ',';
					if (Frame < Phase->size()) *Samples << (*Phase)[Frame];
					else *Samples << "NA";
				}
				for (const auto &Times : DetailedTimes) {
					*Samples << ',';
					if (Frame < Times.size()) *Samples << Times[Frame];
					else *Samples << "NA";
				}
				for (const auto &Counts : DetailedCounts) {
					*Samples << ',';
					if (Frame < Counts.size()) *Samples << Counts[Frame];
					else *Samples << "NA";
				}
				for (const auto &Counts : InstanceCounts) {
					*Samples << ',';
					if (Frame < Counts.size()) *Samples << Counts[Frame];
					else *Samples << "NA";
				}
				for (const auto &Times : InstanceTimes) {
					*Samples << ',';
					if (Frame < Times.size()) *Samples << Times[Frame];
					else *Samples << "NA";
				}
				*Samples << ',' << (Scenario.Name == "culled" ? std::to_string(Settings.CulledPercent) : "NA")
					<< ',' << (Sdl ? (Settings.SdlCompactTraversal ? "on" : "off") : "NA")
					<< ',' << (Sdl ? (Settings.SdlInstancing ? "on" : "off") : "NA")
					<< ',' << (Sdl ? (Settings.SdlSceneEnvelope ? "on" : "off") : "NA")
					<< ',' << (Settings.CameraAway ? "on" : "off");
				for (const auto &Counts : EnvelopeCounts) {
					*Samples << ',';
					if (Frame < Counts.size()) *Samples << Counts[Frame];
					else *Samples << "NA";
				}
				*Samples << '\n';
			}
			Samples->flush();
			if (!*Samples) throw std::runtime_error("could not write benchmark samples");
		}
		if (Graphical && !Comparable)
			throw std::runtime_error("incremental frame admission gate failed or counters are unavailable");
		if (!CullGate) throw std::runtime_error("measured world camera culls differ from fixture expectation");
	}

	gargantuan::RenderFrameState MakeMaterialFrame(const Options &Settings) {
		const auto Aspect = static_cast<float>(Settings.Width) / Settings.Height;
		return MakeFrame(Settings, 4.5f / std::min(1.0f, Aspect));
	}

	void AppendMaterialQuad(std::vector<gargantuan::RenderVertex> &Vertices, float Left, float Right) {
		const glm::vec3 Normal{0.0f, 0.0f, 1.0f};
		const glm::vec4 Tangent{1.0f, 0.0f, 0.0f, 1.0f};
		Vertices.insert(Vertices.end(), {
			{{Left, -0.7f, 0.0f}, Normal, Tangent, {0.0f, 1.0f}},
			{{Right, -0.7f, 0.0f}, Normal, Tangent, {1.0f, 1.0f}},
			{{Right, 0.7f, 0.0f}, Normal, Tangent, {1.0f, 0.0f}},
			{{Left, 0.7f, 0.0f}, Normal, Tangent, {0.0f, 0.0f}},
		});
	}

	gargantuan::RenderPublication MakeMaterialCheck(const Options &Settings) {
		using namespace gargantuan;
		RenderPublication Initial{.Id = 1, .FullResync = true, .Frame = MakeMaterialFrame(Settings)};
		AddTexturePair(Initial);
		constexpr RenderTextureIdentity NormalTexture{3, 1};
		auto Normal = std::make_shared<std::vector<std::uint8_t>>(TextureSide * TextureSide * 4);
		for (std::uint32_t Y = 0; Y < TextureSide; ++Y) for (std::uint32_t X = 0; X < TextureSide; ++X) {
			const std::array<std::uint8_t, 4> Pixel = X < TextureSide / 2
				? std::array<std::uint8_t, 4>{128, 128, 255, 255}
				: std::array<std::uint8_t, 4>{253, 128, 145, 255};
			std::copy(Pixel.begin(), Pixel.end(), Normal->begin() + (Y * TextureSide + X) * 4);
		}
		Initial.TextureCreates.push_back({NormalTexture, 1, TextureSide, TextureSide,
			RenderTextureFormat::Rgba8Unorm, Normal});
		auto Vertices = std::make_shared<std::vector<RenderVertex>>();
		AppendMaterialQuad(*Vertices, -1.6f, -0.12f);
		AppendMaterialQuad(*Vertices, 0.12f, 1.6f);
		const auto Indices = std::make_shared<const std::vector<std::uint32_t>>(
			std::initializer_list<std::uint32_t>{0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7});
		Initial.MeshCreates.push_back({CommonMesh, 1, 1, Vertices, Indices,
			{{-1.6f, -0.7f, 0.0f}, {1.6f, 0.7f, 0.0f}}});
		auto Materials = std::make_shared<std::vector<RenderPrimitiveMaterialState>>(*MakeMaterialPair(6, 12));
		(*Materials)[1].Material.NormalTexture = NormalTexture;
		Initial.Creates.push_back({.Item = {.Object = {1, 1}, .CastShadow = false}, .Mesh = CommonMesh,
			.Primitives = Materials});
		return Initial;
	}

#if defined(GARGANTUAN_WITH_FILAMENT)
	template <typename Renderer>
	std::vector<std::uint8_t> CaptureFilamentRgb(Renderer &Backend, gargantuan::RenderPublicationPtr Publication) {
		if constexpr (requires { Backend.Capture(Publication); }) {
			const auto Width = Publication->Frame.ViewportWidth;
			const auto Height = Publication->Frame.ViewportHeight;
			const auto Rgba = Backend.Capture(std::move(Publication));
			if (Rgba.size() != static_cast<std::size_t>(Width) * Height * 4)
				throw std::runtime_error("Filament readback byte count does not match the viewport");
			std::vector<std::uint8_t> Rgb(static_cast<std::size_t>(Width) * Height * 3);
			for (std::size_t Pixel = 0; Pixel < static_cast<std::size_t>(Width) * Height; ++Pixel)
				std::copy_n(Rgba.begin() + Pixel * 4, 3, Rgb.begin() + Pixel * 3);
			return Rgb;
		} else {
			throw std::runtime_error("this Filament adapter does not expose image capture");
		}
	}
#endif

	void WriteImage(const std::filesystem::path &Path, const Options &Settings,
		const std::vector<std::uint8_t> &Rgb) {
		if (Rgb.size() != static_cast<std::size_t>(Settings.Width) * Settings.Height * 3)
			throw std::runtime_error("material check readback byte count does not match the viewport");
		if (std::filesystem::exists(Path)) throw std::runtime_error("capture output already exists: " + Path.string());
		std::ofstream Image(Path, std::ios::binary);
		Image << "P6\n" << Settings.Width << ' ' << Settings.Height << "\n255\n";
		Image.write(reinterpret_cast<const char *>(Rgb.data()), static_cast<std::streamsize>(Rgb.size()));
		if (!Image) throw std::runtime_error("failed to write material evidence: " + Path.string());
	}

	struct MaterialCaptures {
		std::vector<std::uint8_t> First;
		std::vector<std::uint8_t> Second;
		double LifecycleMilliseconds = 0.0;
	};

	MaterialCaptures CaptureMaterialPair(const Options &Settings, const gargantuan::RenderPublicationPtr &Initial,
		const gargantuan::RenderPublicationPtr &Updated) {
		using namespace gargantuan;
		MaterialCaptures Captures;
		auto InitialPath = *Settings.CapturePrefix;
		InitialPath += "-initial.ppm";
		auto UpdatedPath = *Settings.CapturePrefix;
		UpdatedPath += "-updated.ppm";
		double InitialWriteMilliseconds = 0.0;
		const auto SaveInitial = [&] {
			const auto WriteStart = Clock::now();
			WriteImage(InitialPath, Settings, Captures.First);
			InitialWriteMilliseconds = Milliseconds(Clock::now() - WriteStart);
		};
		const auto CaptureStart = Clock::now();
		if (Settings.Backend == "sdl") {
			const VideoGuard Video;
			EditorViewportRenderer Renderer(Settings.Width, Settings.Height, Settings.SdlHdr.value_or(true));
			Captures.First = Renderer.Capture(Initial).RgbPixels;
			SaveInitial();
			Captures.Second = Renderer.Capture(Updated).RgbPixels;
		}
#if defined(GARGANTUAN_WITH_FILAMENT)
		else {
			FilamentRenderer Renderer(Vector2(static_cast<float>(Settings.Width), static_cast<float>(Settings.Height)), true);
			Captures.First = CaptureFilamentRgb(Renderer, Initial);
			SaveInitial();
			Captures.Second = CaptureFilamentRgb(Renderer, Updated);
		}
#endif
		Captures.LifecycleMilliseconds = Milliseconds(Clock::now() - CaptureStart) - InitialWriteMilliseconds;
		WriteImage(UpdatedPath, Settings, Captures.Second);
		return Captures;
	}

	std::array<int, 3> ProbeMaterialPixel(const std::vector<std::uint8_t> &Rgb,
		const gargantuan::RenderFrameState &Frame, float WorldX, float WorldY = 0.0f) {
		const auto Clip = Frame.Camera.ViewProjectionMatrix * glm::vec4(WorldX, WorldY, 0.0f, 1.0f);
		const auto X = static_cast<int>((Clip.x / Clip.w * 0.5f + 0.5f) * Frame.ViewportWidth);
		const auto Y = static_cast<int>((0.5f - Clip.y / Clip.w * 0.5f) * Frame.ViewportHeight);
		std::array<int, 3> Result{};
		for (int Dy = -1; Dy <= 1; ++Dy) for (int Dx = -1; Dx <= 1; ++Dx) {
			const auto Offset = (static_cast<std::size_t>(Y + Dy) * Frame.ViewportWidth + X + Dx) * 3;
			for (std::size_t Channel = 0; Channel < Result.size(); ++Channel) Result[Channel] += Rgb.at(Offset + Channel);
		}
		for (auto &Channel : Result) Channel /= 9;
		return Result;
	}

	void RunMaterialCheck(const Options &Settings) {
		using namespace gargantuan;
		const auto Initial = std::make_shared<const RenderPublication>(MakeMaterialCheck(Settings));
		RenderPublication Change{.Id = 2, .BaseId = 1, .Frame = Initial->Frame};
		Change.TextureUpdates.push_back({FirstTexture, 2, 0, 0, TextureSide / 2, TextureSide,
			MakePixels(TextureSide / 2, TextureSide, {24, 24, 224, 255})});
		const auto Updated = std::make_shared<const RenderPublication>(std::move(Change));
		const auto Captures = CaptureMaterialPair(Settings, Initial, Updated);
		const auto InitialRed = ProbeMaterialPixel(Captures.First, Initial->Frame, -1.22f);
		const auto UpdatedBlue = ProbeMaterialPixel(Captures.Second, Initial->Frame, -1.22f);
		const auto UntouchedRed = ProbeMaterialPixel(Captures.Second, Initial->Frame, -0.48f);
		const auto NormalLit = ProbeMaterialPixel(Captures.Second, Initial->Frame, 0.46f);
		const auto NormalTilted = ProbeMaterialPixel(Captures.Second, Initial->Frame, 1.22f);
		const auto Dominates = [](const auto &Rgb, std::size_t Channel) {
			return Rgb[Channel] > Rgb[(Channel + 1) % 3] + 8 && Rgb[Channel] > Rgb[(Channel + 2) % 3] + 8;
		};
		const bool ColorPass = Dominates(InitialRed, 0) && Dominates(UpdatedBlue, 2) &&
			Dominates(UntouchedRed, 0) && Dominates(NormalLit, 1);
		const bool NormalPass = NormalLit[1] > NormalTilted[1] + 8;
		std::cout << "Backend,TwoCaptureLifecycleMs,InitialRedR,UpdatedBlueB,UntouchedRedR,NormalLitG,NormalTiltedG,"
			"ColorAndPartialUpdateGate,NormalGate\n" << Settings.Backend << ',' << Captures.LifecycleMilliseconds << ','
			<< InitialRed[0] << ',' << UpdatedBlue[2] << ',' << UntouchedRed[0] << ',' << NormalLit[1] << ','
			<< NormalTilted[1] << ',' << (ColorPass ? "PASS" : "FAIL") << ',' << (NormalPass ? "PASS" : "FAIL") << '\n';
		if (!ColorPass || !NormalPass)
			throw std::runtime_error("material check failed a readback probe; both evidence images were retained");
	}

	void RunMaterialExtensions(const Options &Settings) {
		using namespace gargantuan;
		constexpr RenderTextureIdentity RoughnessMap{1, 1}, MetallicMap{2, 1}, AlphaMap{3, 1};
		constexpr std::uint32_t AlphaWidth = 8;
		const auto MakeMask = [](bool LeftOpaque) {
			auto Pixels = std::make_shared<std::vector<std::uint8_t>>(AlphaWidth * 4, 255);
			for (std::size_t X = 0; X < AlphaWidth; ++X)
				(*Pixels)[X * 4 + 3] = (X < AlphaWidth / 2) == LeftOpaque ? 255 : 153;
			return Pixels;
		};
		RenderPublication Initial{.Id = 1, .FullResync = true, .Frame = MakeMaterialFrame(Settings)};
		Initial.TextureCreates = {
			{RoughnessMap, 1, 1, 1, RenderTextureFormat::Rgba8Unorm, MakePixels(1, 1, {64, 255, 255, 192})},
			{MetallicMap, 1, 1, 1, RenderTextureFormat::Rgba8Unorm, MakePixels(1, 1, {64, 255, 0, 192})},
			{AlphaMap, 1, AlphaWidth, 1, RenderTextureFormat::Rgba8Unorm, MakeMask(true)},
		};
		const RenderMaterialState Metallic{
			.BaseColorFactor = {0.35f, 0.35f, 0.35f, 1.0f}, .Metallic = 1.0f, .DoubleSided = true,
			.ShadingModel = RenderShadingModel::MetallicRoughness, .MetallicRoughnessTexture = MetallicMap,
		};
		const RenderMaterialState Roughness{
			.BaseColorFactor = {0.05f, 0.05f, 0.05f, 1.0f}, .Metallic = 0.0f, .DoubleSided = true,
			.ShadingModel = RenderShadingModel::MetallicRoughness, .MetallicRoughnessTexture = RoughnessMap,
		};
		const RenderMaterialState Mask{
			.BaseColorFactor = {0.80f, 0.05f, 0.05f, 0.75f}, .BaseColorTexture = AlphaMap,
			.OpacityMode = RenderOpacityMode::Masked, .AlphaCutoff = 0.5f, .DoubleSided = true,
			.ShadingModel = RenderShadingModel::MetallicRoughness,
		};
		auto Vertices = std::make_shared<std::vector<RenderVertex>>();
		AppendMaterialQuad(*Vertices, -1.6f, -0.6f);
		AppendMaterialQuad(*Vertices, -0.49f, 0.35f);
		AppendMaterialQuad(*Vertices, 0.46f, 1.6f);
		const auto Indices = std::make_shared<const std::vector<std::uint32_t>>(
			std::initializer_list<std::uint32_t>{0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11});
		Initial.MeshCreates.push_back({CommonMesh, 1, 1, Vertices, Indices,
			{{-1.6f, -0.7f, 0.0f}, {1.6f, 0.7f, 0.0f}}});
		Initial.Creates.push_back({.Item = {.Object = {1, 1}, .CastShadow = false}, .Mesh = CommonMesh,
			.Primitives = std::make_shared<const std::vector<RenderPrimitiveMaterialState>>(
				std::initializer_list<RenderPrimitiveMaterialState>{{0, 6, Metallic}, {6, 6, Roughness}, {12, 6, Mask}})});

		RenderPublication Updated{.Id = 2, .BaseId = 1, .Frame = Initial.Frame};
		Updated.TextureUpdates = {
			{RoughnessMap, 2, 0, 0, 1, 1, MakePixels(1, 1, {64, 96, 255, 192})},
			{MetallicMap, 2, 0, 0, 1, 1, MakePixels(1, 1, {64, 255, 255, 192})},
			{AlphaMap, 2, 0, 0, AlphaWidth, 1, MakeMask(false)},
		};
		const auto Frame = Initial.Frame;
		const auto Captures = CaptureMaterialPair(Settings, std::make_shared<const RenderPublication>(std::move(Initial)),
			std::make_shared<const RenderPublication>(std::move(Updated)));
		const auto InitialRoughness = ProbeMaterialPixel(Captures.First, Frame, 0.0f)[1];
		const auto UpdatedRoughness = ProbeMaterialPixel(Captures.Second, Frame, 0.0f)[1];
		const auto InitialMetallic = ProbeMaterialPixel(Captures.First, Frame, -1.1f)[1];
		const auto UpdatedMetallic = ProbeMaterialPixel(Captures.Second, Frame, -1.1f)[1];
		const auto MaskContrast = [&](const auto &Image, float WorldX) {
			const auto Pixel = ProbeMaterialPixel(Image, Frame, WorldX);
			const auto Background = ProbeMaterialPixel(Image, Frame, WorldX, 1.1f);
			return std::max({std::abs(Pixel[0] - Background[0]), std::abs(Pixel[1] - Background[1]),
				std::abs(Pixel[2] - Background[2])});
		};
		const auto InitialMaskLeft = MaskContrast(Captures.First, 0.73f);
		const auto InitialMaskRight = MaskContrast(Captures.First, 1.33f);
		const auto UpdatedMaskLeft = MaskContrast(Captures.Second, 0.73f);
		const auto UpdatedMaskRight = MaskContrast(Captures.Second, 1.33f);
		const bool RoughnessPass = UpdatedRoughness > InitialRoughness + 8;
		const bool MetallicPass = InitialMetallic > UpdatedMetallic + 8;
		const bool MaskPass = InitialMaskLeft > 24 && InitialMaskRight <= 4 &&
			UpdatedMaskLeft <= 4 && UpdatedMaskRight > 24;
		std::cout << "Backend,InitialRoughnessG,UpdatedRoughnessG,InitialMetallicG,UpdatedMetallicG,"
			"InitialMaskLeftContrast,InitialMaskRightContrast,UpdatedMaskLeftContrast,UpdatedMaskRightContrast,"
			"PackedRoughnessGate,PackedMetallicGate,MaskGate\n" << Settings.Backend << ','
			<< InitialRoughness << ',' << UpdatedRoughness << ',' << InitialMetallic << ',' << UpdatedMetallic << ','
			<< InitialMaskLeft << ',' << InitialMaskRight << ',' << UpdatedMaskLeft << ',' << UpdatedMaskRight << ','
			<< (RoughnessPass ? "PASS" : "FAIL") << ',' << (MetallicPass ? "PASS" : "FAIL") << ','
			<< (MaskPass ? "PASS" : "FAIL") << '\n';
		if (!RoughnessPass || !MetallicPass || !MaskPass)
			throw std::runtime_error("material extensions failed a readback probe; both evidence images were retained");
	}
}

int main(int ArgumentCount, char **Arguments) {
#ifdef _WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		const auto BenchmarkOptions = ParseOptions(ArgumentCount, Arguments);
		if (BenchmarkOptions.Incremental) {
			if (BenchmarkOptions.Scenario == "material-check") {
				RunMaterialCheck(BenchmarkOptions);
				return 0;
			}
			if (BenchmarkOptions.Scenario == "material-extensions") {
				RunMaterialExtensions(BenchmarkOptions);
				return 0;
			}
			std::cout << "Scenario,Objects,Backend,Driver,Synchronization,Width,Height,Frames,Warmup,FixtureSetupMs,"
				"BackendStartupMs,FirstCompletedFrameMs,FixtureDeltaMeanMs,FrameMeanMs,FrameP50Ms,FrameP95Ms,"
				"FrameP99Ms,FrameMaxMs,ProjectionPhaseMeanMs,BackendApplyPhaseMeanMs,SubmissionPhaseMeanMs,"
				"CompletionWaitMeanMs,FinalDrainMs,"
				"LogicalObjectUpdates,LogicalTextureBytes,ProjectedObjects,SubmittedFrames,SkippedFrames,"
				"MeasuredFullResyncs,ReportedDrawCalls,ReportedResourceCreations,ReportedUploadBytes,"
				"ExpectedCameraCulledObjects,ReportedCameraCulledObjects,ReportedWorldGeometryBindings,"
				"ReportedWorldMaterialBindings,CameraCullGate,"
				"FrameAdmissionGate,GpuTimestampMs,Shadows,OpaqueGroupingCandidates,OpaqueGroupedObjects,"
				"OpaqueGroupingUnprovenBounds,OpaqueGroupingConflictBarriers,OpaqueGroupingBudgetBarriers,"
				"OpaqueGroupingPairTests,OpaqueGroupingDepthProofs,SdlHdrRequested,SdlHdrActive,SdlHdrSceneBytes,SdlSceneResolveDraws,"
				"AuthoredLocalLights,LocalLightMovement,LocalLightGridMeanMs,LocalLightUploadBytes,VisibleLocalLights,"
				"OverBudgetLightTiles,DroppedLightTileIndices,MaterialViewBytes,MaterialSourceBytes,MaterialViewFallbacks,MaterialUploadStagingBytes,"
				"IblBuilds,IblPreprocessPasses,ShadowMapBuilds,ShadowCacheHits,ShadowMapPasses,ShadowCasterDraws,SdlOpaqueGrouping,SdlProfileDetailed,CulledPercent,SdlCompactTraversal,SdlInstancing,SdlSceneEnvelope,CameraAway";
			for (const auto Name : DetailedPhaseNames) std::cout << ',' << Name << "MeanMs";
			for (const auto Name : DetailedCounterNames) std::cout << ',' << Name;
			for (const auto Name : InstanceCounterNames) std::cout << ',' << Name;
			for (const auto Name : InstancePhaseNames) std::cout << ',' << Name << "MeanMs";
			std::cout << ",EnvelopeRejections,EnvelopeBuildNs,EnvelopeRebuilds";
			std::cout << '\n';
			std::ofstream Samples;
			if (BenchmarkOptions.SamplesPath) {
				if (std::filesystem::exists(*BenchmarkOptions.SamplesPath))
					throw std::runtime_error("sample CSV already exists: " + BenchmarkOptions.SamplesPath->string());
				Samples.open(*BenchmarkOptions.SamplesPath);
				if (!Samples) throw std::runtime_error("could not open benchmark sample CSV");
				Samples << "Scenario,Objects,Backend,Driver,Synchronization,SdlHdrActive,Width,Height,Frame,PublicationId,FixtureMs,"
					"FrameMs,ProjectionMs,ApplyMs,SubmitMs,WaitMs,LocalLightGridMs";
				for (const auto Name : DetailedPhaseNames) Samples << ',' << Name << "Ms";
				for (const auto Name : DetailedCounterNames) Samples << ',' << Name;
				for (const auto Name : InstanceCounterNames) Samples << ',' << Name;
				for (const auto Name : InstancePhaseNames) Samples << ',' << Name << "Ms";
				Samples << ",CulledPercent,SdlCompactTraversal,SdlInstancing,SdlSceneEnvelope,CameraAway,EnvelopeRejections,EnvelopeBuildNs,EnvelopeRebuilds";
				Samples << '\n' << std::setprecision(std::numeric_limits<double>::max_digits10);
			}
			for (const auto &Scenario : BuildCases(BenchmarkOptions))
				RunIncrementalCase(BenchmarkOptions, Scenario, Samples.is_open() ? &Samples : nullptr);
			return 0;
		}
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
