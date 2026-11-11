// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "../src/runtime/RuntimeWorkDiagnostics.hpp"
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <Windows.h>
#endif
#ifdef GARGANTUAN_RUNTIME6_ALLOCATION_PROBE
#include "BenchmarkAllocationProbe.hpp"
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
	using Clock = std::chrono::steady_clock;
	struct RuntimeExperimentOptions {
		enum class DrawBackend { None, Headless, Sdl };
		bool DensityMatrix = false;
		bool SplitConstruction = false;
		std::string_view Property = "transform";
		DrawBackend Backend = DrawBackend::None;
		bool ContiguousInstancing = false;
	};
	RuntimeExperimentOptions RuntimeOptions;

	std::string_view BackendName() {
		switch (RuntimeOptions.Backend) {
		case RuntimeExperimentOptions::DrawBackend::Headless: return "headless";
		case RuntimeExperimentOptions::DrawBackend::Sdl: return "sdl";
		default: return "none";
		}
	}

	struct SDLGuard {
		SDLGuard() {
			if (!SDL_Init(SDL_INIT_VIDEO))
				throw std::runtime_error(std::string("[Render:Foundation2Benchmark] SDL video initialization failed: ") + SDL_GetError());
		}
		~SDLGuard() { SDL_Quit(); }
	};

	struct DrawFrameSample {
		double DrawAndWaitMs = 0.0;
		double MutationToCompletionMs = 0.0;
		std::uint64_t ProjectedObjects = 0;
		std::uint64_t ProjectedUpdates = 0;
		std::uint64_t FramesSubmitted = 0;
		std::uint64_t FullResyncs = 0;
		std::uint64_t DrawCalls = 0;
		std::uint64_t UploadedBytes = 0;
		std::uint64_t InstanceUploadBytes = 0;
		std::uint64_t InstancedDrawCalls = 0;
		std::uint64_t ContiguousInstancedDrawCalls = 0;
		std::uint64_t CameraVisitedObjects = 0;
		std::uint64_t InstancedObjects = 0;
		std::uint64_t ProjectionNs = 0;
		std::uint64_t InstanceApplicationNs = 0;
		std::uint64_t InstanceUploadNs = 0;
		std::uint64_t CompletionWaitNs = 0;
	};

	double Milliseconds(Clock::duration Duration) {
		return std::chrono::duration<double, std::milli>(Duration).count();
	}

	double Mean(const std::vector<double> &Samples) {
		return Samples.empty() ? 0.0 : std::accumulate(Samples.begin(), Samples.end(), 0.0) / Samples.size();
	}

	struct PipelineBreakdown {
		double PropertySetterOtherMeanMs = 0.0;
		double JournalMeanMs = 0.0;
		double ClassificationMeanMs = 0.0;
		double AccumulatorMeanMs = 0.0;
		double DirtyCaptureMeanMs = 0.0;
		double DirtyExpansionMeanMs = 0.0;
		double FinalStateExtractionMeanMs = 0.0;
		double PublicationConstructionMeanMs = 0.0;
		double PublisherReconciliationMeanMs = 0.0;
	};

	void Print(
		std::string_view Scenario,
		std::size_t Elements,
		std::size_t Frames,
		const std::vector<double> &Accumulation,
		const std::vector<double> &Generation,
		const std::vector<double> &Application,
		std::size_t Creates,
		std::size_t Updates,
		std::size_t Removes,
		std::size_t UploadBytes,
		std::size_t Draws,
		std::size_t FullResyncs,
		const PipelineBreakdown &Breakdown = {},
		const std::vector<DrawFrameSample> *DrawSamples = nullptr
	) {
		std::cout << Scenario << ',' << Elements << ',' << Frames << ',' << std::fixed << std::setprecision(4)
			<< Mean(Accumulation) << ',' << Mean(Generation) << ',' << Mean(Application) << ',' << Creates << ',' << Updates << ',' << Removes << ','
			<< UploadBytes << ',' << Draws << ',' << FullResyncs << ','
			<< Breakdown.PropertySetterOtherMeanMs << ',' << Breakdown.JournalMeanMs << ','
			<< Breakdown.ClassificationMeanMs << ',' << Breakdown.AccumulatorMeanMs << ','
			<< Breakdown.DirtyCaptureMeanMs << ',' << Breakdown.DirtyExpansionMeanMs << ','
			<< Breakdown.FinalStateExtractionMeanMs << ',' << Breakdown.PublicationConstructionMeanMs << ','
			<< Breakdown.PublisherReconciliationMeanMs << ',' << BackendName() << ','
			<< (DrawSamples && !DrawSamples->empty() ? std::to_string(std::accumulate(
				DrawSamples->begin(), DrawSamples->end(), 0.0,
				[](double Sum, const DrawFrameSample &Sample) { return Sum + Sample.DrawAndWaitMs; }) / DrawSamples->size()) : "NA")
			<< ',' << (DrawSamples && !DrawSamples->empty() ? std::to_string(std::accumulate(
				DrawSamples->begin(), DrawSamples->end(), 0.0,
				[](double Sum, const DrawFrameSample &Sample) { return Sum + Sample.MutationToCompletionMs; }) / DrawSamples->size()) : "NA")
			<< ',' << RuntimeOptions.ContiguousInstancing << '\n' << std::flush;
	}

	void RunRigid(
		std::string_view Name,
		std::size_t Count,
		std::size_t ChangedPerFrame,
		std::size_t WritesPerObject,
		std::size_t Frames,
		bool ProfileEnabled,
		std::uint32_t GroupWidth,
		bool RendererOnly,
		std::ofstream *Samples,
		bool DiagnosticMarks
	) {
		using namespace gargantuan;
		runtime_detail::WorkSample Work{};
		std::optional<runtime_detail::WorkCapture> WorkCapture;
		if (ProfileEnabled) WorkCapture.emplace(&Work);
		const auto ScenarioStart = Clock::now();
		const auto Mark = [&](std::string_view Stage) {
			if (DiagnosticMarks)
				std::cerr << "[Render:Foundation2Benchmark] scenario=" << Name << '-' << Count
					<< " stage=" << Stage << " elapsed_ms=" << Milliseconds(Clock::now() - ScenarioStart)
					<< '\n' << std::flush;
		};
		Mark("start");
		ChangeJournal::Get().Clear();
		RenderDirtyAccumulator::Get().Clear();
		auto Game = std::make_shared<DataModel>();
		auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		Mark("world-ready");
		// This benchmark variant measures renderer publication without constructing
		// native rigid bodies during scene setup. Keep the existing backend alive
		// locally; WorldRoot::CreatePartBody declines while Physics is moved from.
		std::optional<PhysicsWorld> RetainedPhysics;
		if (RendererOnly) {
			RetainedPhysics.emplace(std::move(World->Physics));
			if (World->Physics.IsValid() || !RetainedPhysics->IsValid())
				throw std::runtime_error("renderer-only fixture did not detach its physics backend");
		}
		std::vector<std::shared_ptr<Part>> Parts;
		Parts.reserve(Count);
		std::vector<std::shared_ptr<Folder>> Groups;
		if (GroupWidth != 0) {
			Groups.reserve((Count + GroupWidth - 1) / GroupWidth);
			for (std::size_t Index = 0; Index < Count; Index += GroupWidth) {
				auto Group = std::make_shared<Folder>();
				Group->SetParent(World);
				Groups.push_back(std::move(Group));
			}
		}
		Mark("groups-ready");
		const auto Side = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(Count))));
		const auto Initialize = [&](const std::shared_ptr<Part> &Value, std::size_t Index) {
			Value->SetCFrame(CFrame(glm::vec3(
				static_cast<float>(Index % Side) * 2.0f, static_cast<float>(Index / Side) * 2.0f, 0.0f)));
		};
		const auto Adopt = [&](const std::shared_ptr<Part> &Value, std::size_t Index) {
			if (GroupWidth == 0) Value->SetParent(World);
			else Value->SetParent(Groups[Index / GroupWidth]);
			if (RendererOnly) World->Parts.push_back(Value);
		};
		for (std::size_t Index = 0; Index < Count; ++Index) {
			auto Value = std::make_shared<Part>();
			if (!RuntimeOptions.SplitConstruction) {
				Initialize(Value, Index);
				Adopt(Value, Index);
			}
			Parts.push_back(std::move(Value));
		}
		Mark(RuntimeOptions.SplitConstruction ? "parts-allocated" : "interleaved-ready");
		if (RuntimeOptions.SplitConstruction) {
			for (std::size_t Index = 0; Index < Count; ++Index) Initialize(Parts[Index], Index);
			Mark("detached-transforms");
			for (std::size_t Index = 0; Index < Count; ++Index) Adopt(Parts[Index], Index);
			Mark("parts-adopted");
		}
		Mark("parts-ready");
		RenderPublisher Publisher;
		RenderProjection Projection;
		std::optional<SDLGuard> Video;
		std::unique_ptr<BaseRenderer> DrawRenderer;
		SDLRenderer *Sdl = nullptr;
		HeadlessRenderer *Headless = nullptr;
		if (RuntimeOptions.Backend == RuntimeExperimentOptions::DrawBackend::Sdl) {
			Video.emplace();
			auto Renderer = std::make_unique<SDLRenderer>(Vector2(1280.0f, 720.0f), SDLRendererOptions{
				.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false,
				.EnableInstancing = RuntimeOptions.ContiguousInstancing,
				.UseContiguousInstancing = RuntimeOptions.ContiguousInstancing,
			});
			Sdl = Renderer.get();
			std::cerr << "[Render:Foundation2Benchmark] backend=sdl driver=" << Sdl->GetDriverName()
				<< " instancing=" << RuntimeOptions.ContiguousInstancing << '\n';
			DrawRenderer = std::move(Renderer);
		} else if (RuntimeOptions.Backend == RuntimeExperimentOptions::DrawBackend::Headless) {
			auto Renderer = std::make_unique<HeadlessRenderer>(Vector2(1280.0f, 720.0f));
			Headless = Renderer.get();
			DrawRenderer = std::move(Renderer);
		}
		const auto Camera = MakeLookAtRenderCameraInput(
			glm::vec3(0.0f, 0.0f, 1000.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)
		);
		auto Initial = Publisher.Publish(*World, Camera, 1280, 720);
		if (DrawRenderer) {
			DrawRenderer->Draw(Initial);
			if (Sdl) {
				Sdl->WaitForIdle();
				const auto Metrics = Sdl->GetMetrics();
				if (Metrics.FramesSubmitted != 1 || Metrics.FullResyncs != 1)
					throw std::runtime_error("[Render:Foundation2Benchmark] Initial SDL submission/resync mismatch");
				if (Metrics.DrawCalls == 0 || (RuntimeOptions.ContiguousInstancing &&
					Metrics.ContiguousInstancedDrawCalls == 0))
					throw std::runtime_error("[Render:Foundation2Benchmark] Initial SDL object draw/contiguous gate failed");
			} else if (Headless->GetProjection().GetObjects().size() != Count ||
				Headless->GetLastChanges().Created != Count)
				throw std::runtime_error("[Render:Foundation2Benchmark] Initial headless draw mismatch");
		}
		(void)Projection.Apply(*Initial);
		if (Initial->Creates.size() != Count || Projection.GetObjects().size() != Count)
			throw std::runtime_error("[Render:Foundation2Benchmark] Rigid fixture publication/object count mismatch");
		Mark("initial-ready");
		const auto PhaseChanges = RuntimeOptions.DensityMatrix
			? std::vector<std::size_t>{0, std::max<std::size_t>(1, Count / 1000),
				std::max<std::size_t>(1, Count / 100), Count / 10, Count / 2, Count}
			: std::vector<std::size_t>{ChangedPerFrame};
		std::size_t GlobalFrame = 0;
		for (const auto PhaseChanged : PhaseChanges) {
		ChangedPerFrame = PhaseChanged;
		const auto PhaseName = std::string(Name) + (RuntimeOptions.DensityMatrix
			? "-changed-" + std::to_string(ChangedPerFrame) : "");
		auto &Dirty = RenderDirtyAccumulator::Get();
		auto &Journal = ChangeJournal::Get();
		Dirty.ResetProfile();
		Journal.ResetProfile();
		Dirty.SetProfilingEnabled(ProfileEnabled);
		Journal.SetProfilingEnabled(ProfileEnabled);
		Publisher.SetProfilingEnabled(ProfileEnabled);
		std::vector<double> Accumulation;
		std::vector<double> Generation;
		std::vector<double> Application;
		std::vector<double> DirtyCapture;
		std::vector<double> DirtyExpansion;
		std::vector<double> FinalStateExtraction;
		std::vector<double> PublicationConstruction;
		std::vector<double> PublisherReconciliation;
		std::vector<std::uint64_t> JournalRecords;
		std::vector<DrawFrameSample> DrawSamples;
		if (DrawRenderer) DrawSamples.reserve(Frames);
		JournalRecords.reserve(Frames);
		for (auto *Values : {&Accumulation, &Generation, &Application, &DirtyCapture, &DirtyExpansion,
			&FinalStateExtraction, &PublicationConstruction, &PublisherReconciliation}) Values->reserve(Frames);
#ifdef GARGANTUAN_RUNTIME6_ALLOCATION_PROBE
		std::vector<BenchmarkAllocationProbe::Counts> FrameAllocations(Frames);
#endif
		std::size_t Creates = 0, Updates = 0, Removes = 0, UploadBytes = 0, FullResyncs = 0;
		for (std::size_t Frame = 0; Frame < Frames; ++Frame) {
#ifdef GARGANTUAN_RUNTIME6_ALLOCATION_PROBE
			BenchmarkAllocationProbe::Scope AllocationScope(FrameAllocations[Frame]);
#endif
			++GlobalFrame;
			const auto JournalBefore = Journal.CreateCursor(Game->GetObjectId()).NextSequence;
			const auto AccumulationStart = Clock::now();
			for (std::size_t Index = 0; Index < ChangedPerFrame; ++Index) {
				const auto ObjectIndex = (Index + Frame * ChangedPerFrame) % Count;
				for (std::size_t Write = 0; Write < WritesPerObject; ++Write) {
					if (RuntimeOptions.Property != "material") {
						auto Current = Parts[ObjectIndex]->GetCFrame();
						Current.Position.z = static_cast<float>(GlobalFrame) * 0.01f + static_cast<float>(Write) * 0.000001f;
						Parts[ObjectIndex]->SetCFrame(Current);
					}
					if (RuntimeOptions.Property != "transform")
						Parts[ObjectIndex]->SetTransparency(0.1f + static_cast<float>(GlobalFrame) * 0.0001f +
							static_cast<float>(Write) * 0.0000001f);
				}
			}
			Accumulation.push_back(Milliseconds(Clock::now() - AccumulationStart));
			const auto CommittedRecords = Journal.CreateCursor(Game->GetObjectId()).NextSequence - JournalBefore;
			const auto ExpectedRecords = ChangedPerFrame * WritesPerObject *
				(RuntimeOptions.Property == "mixed" ? 2 : 1);
			if (CommittedRecords != ExpectedRecords)
				throw std::runtime_error("[Runtime:Benchmark] Authoritative journal count mismatch");
			JournalRecords.push_back(CommittedRecords);
			const auto GenerationStart = Clock::now();
			auto Publication = Publisher.Publish(*World, Camera, 1280, 720);
			Generation.push_back(Milliseconds(Clock::now() - GenerationStart));
			if (ProfileEnabled) {
				const auto PublisherProfile = Publisher.GetLastProfile();
				DirtyCapture.push_back(static_cast<double>(PublisherProfile.DirtyCaptureNanoseconds) / 1'000'000.0);
				DirtyExpansion.push_back(static_cast<double>(PublisherProfile.DirtyExpansionNanoseconds) / 1'000'000.0);
				FinalStateExtraction.push_back(static_cast<double>(PublisherProfile.FinalStateExtractionNanoseconds) / 1'000'000.0);
				PublicationConstruction.push_back(static_cast<double>(PublisherProfile.PublicationConstructionNanoseconds) / 1'000'000.0);
				PublisherReconciliation.push_back(static_cast<double>(PublisherProfile.StateCacheReconciliationNanoseconds) / 1'000'000.0);
			}
			if (DrawRenderer) {
				const auto Before = Sdl ? Sdl->GetMetrics() : SDLRendererMetrics{};
				const auto DrawStart = Clock::now();
				DrawRenderer->Draw(Publication);
				if (Sdl) Sdl->WaitForIdle();
				const auto Completed = Clock::now();
				DrawFrameSample Sample{
					.DrawAndWaitMs = Milliseconds(Completed - DrawStart),
					.MutationToCompletionMs = Milliseconds(Completed - AccumulationStart),
				};
				if (Sdl) {
					const auto After = Sdl->GetMetrics();
					if (After.FramesSubmitted != Before.FramesSubmitted + 1 ||
						After.FullResyncs != Before.FullResyncs)
						throw std::runtime_error("[Render:Foundation2Benchmark] SDL frame/resync gate failed");
					if (After.DrawCalls <= Before.DrawCalls || (RuntimeOptions.ContiguousInstancing &&
						RuntimeOptions.Property == "transform" &&
						After.ContiguousInstancedDrawCalls <= Before.ContiguousInstancedDrawCalls))
						throw std::runtime_error("[Render:Foundation2Benchmark] SDL object draw/contiguous gate failed");
					Sample.FramesSubmitted = After.FramesSubmitted;
					Sample.FullResyncs = After.FullResyncs;
					Sample.DrawCalls = After.DrawCalls - Before.DrawCalls;
					Sample.UploadedBytes = After.UploadedBytes - Before.UploadedBytes;
					Sample.InstanceUploadBytes = After.InstanceUploadBytes - Before.InstanceUploadBytes;
					Sample.InstancedDrawCalls = After.InstancedDrawCalls - Before.InstancedDrawCalls;
					Sample.ContiguousInstancedDrawCalls = After.ContiguousInstancedDrawCalls - Before.ContiguousInstancedDrawCalls;
					Sample.CameraVisitedObjects = After.CameraVisitedObjects - Before.CameraVisitedObjects;
					Sample.InstancedObjects = After.InstancedObjects - Before.InstancedObjects;
					Sample.ProjectionNs = After.LastProjectionNanoseconds;
					Sample.InstanceApplicationNs = After.LastInstanceApplicationNanoseconds;
					Sample.InstanceUploadNs = After.LastInstanceUploadNanoseconds;
					Sample.CompletionWaitNs = After.LastGpuCompletionWaitNanoseconds;
				} else {
					Sample.ProjectedObjects = Headless->GetProjection().GetObjects().size();
					Sample.ProjectedUpdates = Headless->GetLastChanges().Updated;
					if (Sample.ProjectedObjects != Count || Sample.ProjectedUpdates != ChangedPerFrame ||
						Headless->GetLastChanges().Created != 0 || Headless->GetLastChanges().Removed != 0)
						throw std::runtime_error("[Render:Foundation2Benchmark] Headless draw delta gate failed");
				}
				DrawSamples.push_back(Sample);
			}
			const auto ApplyStart = Clock::now();
			const auto Changes = Projection.Apply(*Publication);
			Application.push_back(Milliseconds(Clock::now() - ApplyStart));
			if (Publication->FullResync || Changes.Created != 0 || Changes.Removed != 0 ||
				Changes.Updated != ChangedPerFrame || Projection.GetObjects().size() != Count)
				throw std::runtime_error("[Render:Foundation2Benchmark] Rigid fixture delta count mismatch");
			Creates += Changes.Created;
			Updates += Changes.Updated;
			Removes += Changes.Removed;
			UploadBytes += Changes.VertexUploadBytes;
			FullResyncs += Publication->FullResync ? 1 : 0;
		}
		Dirty.SetProfilingEnabled(false);
		Journal.SetProfilingEnabled(false);
		PipelineBreakdown Breakdown;
		if (ProfileEnabled) {
			const auto DirtyProfile = Dirty.GetProfile();
			const auto JournalProfile = Journal.GetProfile();
			Breakdown = {
				.JournalMeanMs = static_cast<double>(JournalProfile.JournalNanoseconds) / 1'000'000.0 / Frames,
				.ClassificationMeanMs = static_cast<double>(DirtyProfile.ClassificationNanoseconds) / 1'000'000.0 / Frames,
				.AccumulatorMeanMs = static_cast<double>(DirtyProfile.AccumulationNanoseconds) / 1'000'000.0 / Frames,
				.DirtyCaptureMeanMs = Mean(DirtyCapture),
				.DirtyExpansionMeanMs = Mean(DirtyExpansion),
				.FinalStateExtractionMeanMs = Mean(FinalStateExtraction),
				.PublicationConstructionMeanMs = Mean(PublicationConstruction),
				.PublisherReconciliationMeanMs = Mean(PublisherReconciliation),
			};
			Breakdown.PropertySetterOtherMeanMs = std::max(
				0.0,
				Mean(Accumulation) - Breakdown.JournalMeanMs - Breakdown.ClassificationMeanMs - Breakdown.AccumulatorMeanMs
			);
		}
		Print(PhaseName, Count, Frames, Accumulation, Generation, Application, Creates, Updates, Removes, UploadBytes, Count,
			FullResyncs, Breakdown, DrawRenderer ? &DrawSamples : nullptr);
		if (Samples) {
			const auto Stage = [ProfileEnabled](const std::vector<double> &Values, std::size_t Frame) {
				return ProfileEnabled ? std::to_string(Values[Frame]) : std::string("NA");
			};
			for (std::size_t Frame = 0; Frame < Frames; ++Frame) {
				*Samples << PhaseName << ',' << Count << ',' << Frame << ',' << GroupWidth << ','
					<< RendererOnly << ',' << ProfileEnabled << ',' << ChangedPerFrame << ',' << WritesPerObject << ','
					<< Accumulation[Frame] << ',' << Generation[Frame] << ',' << Application[Frame] << ','
					<< Stage(DirtyCapture, Frame) << ',' << Stage(DirtyExpansion, Frame) << ','
					<< Stage(FinalStateExtraction, Frame) << ',' << Stage(PublicationConstruction, Frame) << ','
					<< Stage(PublisherReconciliation, Frame) << ",NA,NA,NA," << JournalRecords[Frame];
#ifdef GARGANTUAN_RUNTIME6_ALLOCATION_PROBE
				*Samples << ',' << FrameAllocations[Frame].Calls << ',' << FrameAllocations[Frame].Bytes;
#else
				*Samples << ",NA,NA";
#endif
				*Samples << ',' << BackendName() << ',' << RuntimeOptions.ContiguousInstancing;
				if (DrawRenderer) {
					const auto &Draw = DrawSamples[Frame];
					*Samples << ',' << Draw.DrawAndWaitMs << ',' << Draw.MutationToCompletionMs;
					if (Headless) *Samples << ',' << Draw.ProjectedObjects << ',' << Draw.ProjectedUpdates;
					else *Samples << ",NA,NA";
					if (Sdl) {
						*Samples << ',' << Draw.FramesSubmitted << ',' << Draw.FullResyncs << ','
							<< Draw.DrawCalls << ',' << Draw.UploadedBytes << ',' << Draw.InstanceUploadBytes << ','
							<< Draw.InstancedDrawCalls << ',' << Draw.ContiguousInstancedDrawCalls << ','
							<< Draw.CameraVisitedObjects << ',' << Draw.InstancedObjects << ','
							<< Draw.ProjectionNs << ',' << Draw.InstanceApplicationNs << ','
							<< Draw.InstanceUploadNs << ',' << Draw.CompletionWaitNs;
					} else *Samples << ",NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA";
				} else *Samples << ",NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA";
				*Samples << '\n';
			}
			Samples->flush();
			if (!*Samples) throw std::runtime_error("[Render:Foundation2Benchmark] Could not write frame samples");
		}
		}
		Mark("measured");
		if (Sdl) {
			const auto Metrics = Sdl->GetMetrics();
			if (Metrics.FramesSubmitted != GlobalFrame + 1 || Metrics.FullResyncs != 1)
				throw std::runtime_error("[Render:Foundation2Benchmark] Final SDL submission/resync mismatch");
			std::cerr << "[Render:Foundation2Benchmark] backend=sdl driver=" << Sdl->GetDriverName()
				<< " frames_submitted=" << Metrics.FramesSubmitted
				<< " full_resyncs=" << Metrics.FullResyncs << '\n';
		}
		DrawRenderer.reset();
		Video.reset();
		if (RetainedPhysics) World->Physics = std::move(*RetainedPhysics);
		Game->Destroy();
		Mark("destroyed");
		Parts.clear();
		Groups.clear();
		World.reset();
		Game.reset();
		Mark("released");
		if (ProfileEnabled) {
			for (const auto Phase : {runtime_detail::WorkPhase::PhysicsBodyCreate,
				runtime_detail::WorkPhase::PhysicsBodyDestroy}) {
				const auto &Entry = Work[static_cast<std::size_t>(Phase)];
				std::cerr << "[Runtime:Benchmark] phase=" << runtime_detail::WorkPhaseNames[static_cast<std::size_t>(Phase)]
					<< " calls=" << Entry.Calls << " units=" << Entry.Units << " inclusive_ns=" << Entry.Nanoseconds << '\n';
			}
		}
	}

	void RunDeformable(std::size_t VertexCount, std::size_t Frames) {
		using namespace gargantuan;
		RenderProjection Projection;
		auto Vertices = std::make_shared<const std::vector<RenderVertex>>(std::vector<RenderVertex>(VertexCount));
		auto Indices = std::make_shared<const std::vector<std::uint32_t>>(std::vector<std::uint32_t>{0, 1, 2});
		RenderPublication Initial{.Id = 1, .FullResync = true, .Frame = {.ViewportWidth = 1280, .ViewportHeight = 720}};
		Initial.MeshCreates.push_back({{1, 1}, 1, 1, Vertices, Indices, {glm::vec3(-1.0f), glm::vec3(1.0f)}});
		(void)Projection.Apply(Initial);
		std::vector<double> Generation;
		std::vector<double> Application;
		std::size_t UploadBytes = 0;
		for (std::size_t Frame = 0; Frame < Frames; ++Frame) {
			const auto GenerationStart = Clock::now();
			auto Changed = std::make_shared<std::vector<RenderVertex>>(VertexCount);
			for (std::size_t Index = 0; Index < VertexCount; ++Index) {
				const auto Phase = static_cast<float>(Index + Frame) * 0.01f;
				(*Changed)[Index].Position = {static_cast<float>(Index), std::sin(Phase), 0.0f};
				(*Changed)[Index].Normal = glm::normalize(glm::vec3(-std::cos(Phase) * 0.01f, 1.0f, 0.0f));
			}
			RenderPublication Publication{.Id = Frame + 2, .BaseId = Frame + 1, .Frame = Initial.Frame};
			Publication.MeshVertexUpdates.push_back({
				{1, 1}, Frame + 2, 0, std::shared_ptr<const std::vector<RenderVertex>>(Changed),
				{glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(static_cast<float>(VertexCount), 1.0f, 0.0f)},
			});
			Generation.push_back(Milliseconds(Clock::now() - GenerationStart));
			const auto ApplyStart = Clock::now();
			const auto Changes = Projection.Apply(Publication);
			Application.push_back(Milliseconds(Clock::now() - ApplyStart));
			UploadBytes += Changes.VertexUploadBytes;
		}
		Print("deformable", VertexCount, Frames, {}, Generation, Application, 0, Frames, 0, UploadBytes, 1, 0);
	}

	void RunGui(std::size_t QuadCount, std::size_t Frames) {
		using namespace gargantuan;
		RenderProjection Projection;
		RenderPublication Initial{.Id = 1, .FullResync = true, .Frame = {.ViewportWidth = 1280, .ViewportHeight = 720}};
		auto AtlasPixels = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>(4, 255));
		for (std::uint64_t Texture = 1; Texture <= 8; ++Texture)
			Initial.TextureCreates.push_back({{Texture, 1}, 1, 1, 1, RenderTextureFormat::Rgba8Unorm, AtlasPixels});
		(void)Projection.Apply(Initial);
		std::vector<double> Generation;
		std::vector<double> Application;
		std::size_t UploadBytes = 0;
		constexpr std::size_t QuadsPerBatch = 256;
		for (std::size_t Frame = 0; Frame < Frames; ++Frame) {
			const auto GenerationStart = Clock::now();
			RenderPublication Publication{.Id = Frame + 2, .BaseId = Frame + 1, .Frame = Initial.Frame};
			Publication.Ui = {1280, 720, 1.0f, {}};
			for (std::size_t First = 0; First < QuadCount; First += QuadsPerBatch) {
				const auto BatchQuads = std::min(QuadsPerBatch, QuadCount - First);
				RenderUiBatch Batch;
				Batch.Texture = RenderTextureIdentity{1 + (First / QuadsPerBatch) % 8, 1};
				Batch.Layer = static_cast<std::int32_t>(First / QuadsPerBatch);
				Batch.Clip = RenderUiClipRect{0.0f, 0.0f, 1280.0f, 720.0f};
				Batch.Vertices.resize(BatchQuads * 4);
				Batch.Indices.reserve(BatchQuads * 6);
				for (std::size_t Quad = 0; Quad < BatchQuads; ++Quad) {
					const auto Base = static_cast<std::uint32_t>(Quad * 4);
					Batch.Indices.insert(Batch.Indices.end(), {Base, Base + 1, Base + 2, Base + 2, Base + 3, Base});
				}
				Publication.Ui.Batches.push_back(std::move(Batch));
			}
			Generation.push_back(Milliseconds(Clock::now() - GenerationStart));
			const auto ApplyStart = Clock::now();
			const auto Changes = Projection.Apply(Publication);
			Application.push_back(Milliseconds(Clock::now() - ApplyStart));
			UploadBytes += Changes.UiVertices * sizeof(RenderUiVertex) + Changes.UiIndices * sizeof(std::uint32_t);
		}
		Print("gui", QuadCount, Frames, {}, Generation, Application, 0, 0, 0, UploadBytes,
			(QuadCount + QuadsPerBatch - 1) / QuadsPerBatch, 0);
	}

	void RunMixed(std::size_t Frames) {
		using namespace gargantuan;
		constexpr std::size_t RigidCount = 50000;
		constexpr std::size_t ChangedRigid = 500;
		constexpr std::size_t VertexCount = 16384;
		constexpr std::size_t QuadCount = 5000;
		constexpr std::size_t QuadsPerBatch = 256;
		RenderProjection Projection;
		RenderPublication Initial{.Id = 1, .FullResync = true, .Frame = {.ViewportWidth = 1280, .ViewportHeight = 720}};
		Initial.Creates.reserve(RigidCount);
		for (std::size_t Index = 0; Index < RigidCount; ++Index)
			Initial.Creates.push_back({RenderItem{.Object = {static_cast<std::uint32_t>(Index + 1), 1}}});
		auto Vertices = std::make_shared<const std::vector<RenderVertex>>(std::vector<RenderVertex>(VertexCount));
		auto Indices = std::make_shared<const std::vector<std::uint32_t>>(std::vector<std::uint32_t>{0, 1, 2});
		Initial.MeshCreates.push_back({{1, 1}, 1, 1, Vertices, Indices, {glm::vec3(-1.0f), glm::vec3(1.0f)}});
		auto AtlasPixels = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>(4, 255));
		for (std::uint64_t Texture = 1; Texture <= 8; ++Texture)
			Initial.TextureCreates.push_back({{Texture, 1}, 1, 1, 1, RenderTextureFormat::Rgba8Unorm, AtlasPixels});
		(void)Projection.Apply(Initial);
		std::vector<double> Generation;
		std::vector<double> Application;
		std::size_t Updates = 0;
		std::size_t UploadBytes = 0;
		for (std::size_t Frame = 0; Frame < Frames; ++Frame) {
			const auto GenerationStart = Clock::now();
			RenderPublication Publication{.Id = Frame + 2, .BaseId = Frame + 1, .Frame = Initial.Frame};
			Publication.Updates.reserve(ChangedRigid);
			for (std::size_t Index = 0; Index < ChangedRigid; ++Index) {
				const auto Slot = static_cast<std::uint32_t>((Index + Frame * ChangedRigid) % RigidCount + 1);
				RenderItem Item{.Object = {Slot, 1}};
				Item.ModelMatrix[3].z = static_cast<float>(Frame + 1) * 0.01f;
				Item.InverseModelMatrix[3].z = -Item.ModelMatrix[3].z;
				Publication.Updates.push_back({Item.Object, RenderUpdateDomain::Transform, Item});
			}
			auto ChangedVertices = std::make_shared<std::vector<RenderVertex>>(VertexCount);
			for (std::size_t Index = 0; Index < VertexCount; ++Index) {
				const auto Phase = static_cast<float>(Index + Frame) * 0.01f;
				(*ChangedVertices)[Index].Position = {static_cast<float>(Index), std::sin(Phase), 0.0f};
				(*ChangedVertices)[Index].Normal = glm::normalize(glm::vec3(-std::cos(Phase) * 0.01f, 1.0f, 0.0f));
			}
			Publication.MeshVertexUpdates.push_back({
				{1, 1}, Frame + 2, 0, std::shared_ptr<const std::vector<RenderVertex>>(ChangedVertices),
				{glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(static_cast<float>(VertexCount), 1.0f, 0.0f)},
			});
			Publication.Ui = {1280, 720, 1.0f, {}};
			for (std::size_t First = 0; First < QuadCount; First += QuadsPerBatch) {
				const auto BatchQuads = std::min(QuadsPerBatch, QuadCount - First);
				RenderUiBatch Batch;
				Batch.Texture = RenderTextureIdentity{1 + (First / QuadsPerBatch) % 8, 1};
				Batch.Clip = RenderUiClipRect{0.0f, 0.0f, 1280.0f, 720.0f};
				Batch.Vertices.resize(BatchQuads * 4);
				for (std::size_t Quad = 0; Quad < BatchQuads; ++Quad) {
					const auto Base = static_cast<std::uint32_t>(Quad * 4);
					Batch.Indices.insert(Batch.Indices.end(), {Base, Base + 1, Base + 2, Base + 2, Base + 3, Base});
				}
				Publication.Ui.Batches.push_back(std::move(Batch));
			}
			Generation.push_back(Milliseconds(Clock::now() - GenerationStart));
			const auto ApplyStart = Clock::now();
			const auto Changes = Projection.Apply(Publication);
			Application.push_back(Milliseconds(Clock::now() - ApplyStart));
			Updates += Changes.Updated + Changes.MeshesUpdated;
			UploadBytes += Changes.VertexUploadBytes + Changes.UiVertices * sizeof(RenderUiVertex) +
				Changes.UiIndices * sizeof(std::uint32_t);
		}
		Print("mixed", RigidCount, Frames, {}, Generation, Application, 0, Updates, 0, UploadBytes,
			RigidCount + 1 + (QuadCount + QuadsPerBatch - 1) / QuadsPerBatch, 0);
	}
}

int main(int ArgumentCount, char **Arguments) {
#ifdef _WIN32
	SetErrorMode(0x8003u);
#endif
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		const std::size_t Frames = ArgumentCount > 1 ? std::stoull(Arguments[1]) : 30;
		if (Frames == 0 || Frames > 120) throw std::invalid_argument("frame count must be in [1,120]");
		bool ProfileEnabled = false, RigidOnly = false, RendererOnly = false, DiagnosticMarks = false;
		std::uint32_t GroupWidth = 0;
		std::filesystem::path SamplesPath;
		std::string_view SelectedScenario;
		std::optional<std::size_t> SelectedCount, SelectedChanges, SelectedWrites;
		bool DrawBackendSpecified = false, DrawInstancingSpecified = false;
		const auto ParseBound = [](std::string_view Text, std::size_t Minimum, std::size_t Maximum) {
			std::size_t Value = 0;
			const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Value);
			if (Error != std::errc{} || End != Text.data() + Text.size() || Value < Minimum || Value > Maximum)
				throw std::invalid_argument("[Runtime:Benchmark] Numeric option outside its bound");
			return Value;
		};
		for (int Index = 2; Index < ArgumentCount; ++Index) {
			const std::string_view Option(Arguments[Index]);
			if (Option == "--profile") ProfileEnabled = true;
			else if (Option == "--rigid-only") RigidOnly = true;
			else if (Option == "--renderer-only") RendererOnly = true;
			else if (Option == "--diagnostic-marks") DiagnosticMarks = true;
			else if (Option == "--density-matrix") RuntimeOptions.DensityMatrix = true;
			else if (Option == "--split-construction") RuntimeOptions.SplitConstruction = true;
			else if (Option.starts_with("--draw-backend=")) {
				if (DrawBackendSpecified) throw std::invalid_argument("draw backend may appear only once");
				DrawBackendSpecified = true;
				const auto Value = Option.substr(sizeof("--draw-backend=") - 1);
				if (Value == "headless") RuntimeOptions.Backend = RuntimeExperimentOptions::DrawBackend::Headless;
				else if (Value == "sdl") RuntimeOptions.Backend = RuntimeExperimentOptions::DrawBackend::Sdl;
				else throw std::invalid_argument("draw backend must be headless or sdl");
			}
			else if (Option.starts_with("--draw-instancing=")) {
				if (DrawInstancingSpecified) throw std::invalid_argument("draw instancing may appear only once");
				DrawInstancingSpecified = true;
				if (Option.substr(sizeof("--draw-instancing=") - 1) != "contiguous")
					throw std::invalid_argument("draw instancing must be contiguous");
				RuntimeOptions.ContiguousInstancing = true;
			}
			else if (Option.starts_with("--count=")) SelectedCount = ParseBound(Option.substr(8), 1, 65536);
			else if (Option.starts_with("--changes=")) SelectedChanges = ParseBound(Option.substr(10), 0, 65536);
			else if (Option.starts_with("--writes=")) SelectedWrites = ParseBound(Option.substr(9), 1, 16);
			else if (Option.starts_with("--property=")) {
				RuntimeOptions.Property = Option.substr(11);
				if (RuntimeOptions.Property != "transform" && RuntimeOptions.Property != "material" &&
					RuntimeOptions.Property != "mixed") throw std::invalid_argument("unknown property workload");
			}
			else if (Option.starts_with("--scenario=")) {
				if (!SelectedScenario.empty()) throw std::invalid_argument("scenario may appear only once");
				SelectedScenario = Option.substr(sizeof("--scenario=") - 1);
				if (SelectedScenario != "static-1000" && SelectedScenario != "static-10000" &&
					SelectedScenario != "static-50000" && SelectedScenario != "mostly-static" &&
					SelectedScenario != "dynamic" && SelectedScenario != "dynamic-redundant")
					throw std::invalid_argument("unknown rigid scenario");
				RigidOnly = true;
			}
			else if (Option.starts_with("--samples=")) {
				if (!SamplesPath.empty()) throw std::invalid_argument("samples path may appear only once");
				const auto Text = Option.substr(sizeof("--samples=") - 1);
				if (Text.empty() || Text.size() > 240)
					throw std::invalid_argument("samples path is empty or too long");
				SamplesPath = std::filesystem::path(std::string(Text));
			} else if (Option.starts_with("--group-width=")) {
				if (GroupWidth != 0) throw std::invalid_argument("group width may appear only once");
				const auto Text = Option.substr(sizeof("--group-width=") - 1);
				const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), GroupWidth);
				if (Error != std::errc{} || End != Text.data() + Text.size() ||
					GroupWidth < 64 || GroupWidth > 4096)
					throw std::invalid_argument("group width must be in [64,4096]");
			} else throw std::invalid_argument("unknown semantic benchmark option");
		}
		if ((RuntimeOptions.DensityMatrix || SelectedChanges || SelectedWrites) && !SelectedCount)
			throw std::invalid_argument("[Runtime:Benchmark] Runtime options require --count");
		if (RuntimeOptions.Property != "transform" && !SelectedCount)
			throw std::invalid_argument("[Runtime:Benchmark] Property workloads require --count");
		if (RuntimeOptions.DensityMatrix && SelectedChanges)
			throw std::invalid_argument("[Runtime:Benchmark] Density matrix and explicit changes are exclusive");
		if (SelectedCount && SelectedChanges.value_or(0) > *SelectedCount)
			throw std::invalid_argument("[Runtime:Benchmark] Changes exceed object count");
		if (SelectedCount) RigidOnly = true;
		if (RuntimeOptions.ContiguousInstancing && RuntimeOptions.Backend != RuntimeExperimentOptions::DrawBackend::Sdl)
			throw std::invalid_argument("contiguous draw instancing requires --draw-backend=sdl");
		if (RuntimeOptions.Backend != RuntimeExperimentOptions::DrawBackend::None &&
			!SelectedCount && SelectedScenario.empty())
			throw std::invalid_argument("draw backend requires --count or --scenario");
		if (DiagnosticMarks) std::cerr << "[Runtime:Benchmark] property=" << RuntimeOptions.Property
			<< " split_construction=" << RuntimeOptions.SplitConstruction
			<< " density_matrix=" << RuntimeOptions.DensityMatrix << '\n';
		std::ofstream SamplesFile;
		if (!SamplesPath.empty()) {
			if (std::filesystem::exists(SamplesPath))
				throw std::invalid_argument("samples file must not already exist");
			SamplesFile.open(SamplesPath, std::ios::out | std::ios::trunc);
			if (!SamplesFile) throw std::runtime_error("could not open samples file");
			SamplesFile << "Scenario,Elements,Frame,GroupWidth,RendererOnly,ProfileEnabled,ExpectedUpdates,WritesPerObject,DirtyAccumulationMs,PublicationGenerationMs,ProjectionApplyMs,DirtyCaptureMs,DirtyExpansionMs,FinalStateExtractionMs,PublicationConstructionMs,PublisherReconciliationMs,JournalMs,ClassificationMs,AccumulatorMs,JournalRecords,FrameCppNewCalls,FrameCppNewBytes,DrawBackend,ContiguousInstancing,DrawAndWaitMs,MutationToCompletionMs,DrawProjectedObjects,DrawProjectedUpdates,SdlFramesSubmitted,SdlFullResyncs,SdlDrawCalls,SdlUploadedBytes,SdlInstanceUploadBytes,SdlInstancedDrawCalls,SdlContiguousInstancedDrawCalls,SdlCameraVisitedObjects,SdlInstancedObjects,SdlProjectionNs,SdlInstanceApplicationNs,SdlInstanceUploadNs,SdlCompletionWaitNs\n" << std::flush;
		}
		std::ofstream *Samples = SamplesPath.empty() ? nullptr : &SamplesFile;
		std::cout << "Scenario,Elements,Frames,DirtyAccumulationMeanMs,PublicationGenerationMeanMs,ProjectionApplyMeanMs,Creates,Updates,Removes,UploadBytes,DrawOrBatchCount,FullResyncs,PropertySetterOtherMeanMs,JournalMeanMs,ClassificationMeanMs,AccumulatorMeanMs,DirtyCaptureMeanMs,DirtyExpansionMeanMs,FinalStateExtractionMeanMs,PublicationConstructionMeanMs,PublisherReconciliationMeanMs,DrawBackend,DrawAndWaitMeanMs,MutationToCompletionMeanMs,ContiguousInstancing\n" << std::flush;
		if (SelectedCount) {
			RunRigid(RuntimeOptions.Property, *SelectedCount, SelectedChanges.value_or(*SelectedCount),
				SelectedWrites.value_or(1), Frames, ProfileEnabled, GroupWidth, RendererOnly, Samples, DiagnosticMarks);
		} else {
		for (const auto Count : {1000u, 10000u, 50000u}) {
			const auto Scenario = std::string("static-") + std::to_string(Count);
			if (SelectedScenario.empty() || SelectedScenario == Scenario)
				RunRigid("static", Count, 0, 1, Frames, ProfileEnabled, GroupWidth, RendererOnly, Samples, DiagnosticMarks);
		}
		if (SelectedScenario.empty() || SelectedScenario == "mostly-static")
			RunRigid("mostly-static", 50000, 500, 1, Frames, ProfileEnabled, GroupWidth, RendererOnly, Samples, DiagnosticMarks);
		if (SelectedScenario.empty() || SelectedScenario == "dynamic")
			RunRigid("dynamic", 50000, 50000, 1, Frames, ProfileEnabled, GroupWidth, RendererOnly, Samples, DiagnosticMarks);
		if (SelectedScenario.empty() || SelectedScenario == "dynamic-redundant")
			RunRigid("dynamic-redundant", 50000, 50000, 4, Frames, ProfileEnabled, GroupWidth, RendererOnly, Samples, DiagnosticMarks);
		}
		if (!RigidOnly) {
			for (const auto Count : {4096u, 16384u, 65536u}) RunDeformable(Count, Frames);
			RunGui(10000, Frames);
			RunMixed(Frames);
		}
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Render:Foundation2Benchmark] " << Error.what() << '\n';
		return 1;
	}
}
