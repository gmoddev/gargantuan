// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/render/SDLRenderer.hpp"
#include "gargantuan/animation/AnimationRuntime.hpp"
#include "gargantuan/editor/EditorViewport.hpp"
#include "gargantuan/render/Mesh.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numbers>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <glm/ext/matrix_transform.hpp>

namespace {
	using namespace gargantuan;
	using Clock = std::chrono::steady_clock;

	struct SDLGuard {
		SDLGuard() {
			if (!SDL_Init(SDL_INIT_VIDEO))
				throw std::runtime_error(std::string("Failed to initialize SDL video: ") + SDL_GetError());
		}
		~SDLGuard() { SDL_Quit(); }
	};

	struct Options {
		std::string Scenario = "all";
		std::size_t Frames = 300;
		std::size_t WarmupFrames = 30;
		std::uint32_t Width = 1280;
		std::uint32_t Height = 720;
		bool Synchronize = true;
	};

	struct Samples {
		std::vector<double> Frame;
		std::vector<double> Projection;
		std::vector<double> MeshTransfer;
		std::vector<double> TextureTransfer;
		std::vector<double> UiPreparation;
		std::vector<double> Submission;
		std::vector<double> CompletionWait;
	};

	struct Statistics {
		double Mean = 0.0;
		double P50 = 0.0;
		double P95 = 0.0;
		double P99 = 0.0;
		double Maximum = 0.0;
	};

	struct SkinningExpectations {
		std::size_t RigCount = 0;
		std::size_t BoneCount = 0;
		std::size_t SourceVertexCount = 0;
		std::size_t SourceIndexCount = 0;
	};

	double Milliseconds(Clock::duration Duration) {
		return std::chrono::duration<double, std::milli>(Duration).count();
	}

	double Milliseconds(std::uint64_t Nanoseconds) {
		return static_cast<double>(Nanoseconds) / 1'000'000.0;
	}

	Statistics Summarize(std::vector<double> Values) {
		if (Values.empty()) return {};
		std::ranges::sort(Values);
		const auto Percentile = [&](double Value) {
			const auto Index = static_cast<std::size_t>(std::ceil(Value * Values.size())) - 1;
			return Values[std::min(Index, Values.size() - 1)];
		};
		return {
			.Mean = std::accumulate(Values.begin(), Values.end(), 0.0) / Values.size(),
			.P50 = Percentile(0.50),
			.P95 = Percentile(0.95),
			.P99 = Percentile(0.99),
			.Maximum = Values.back(),
		};
	}

	std::size_t ParseSize(std::string_view Value, std::string_view Name) {
		std::size_t ParsedCharacters = 0;
		const auto Parsed = std::stoull(std::string(Value), &ParsedCharacters);
		if (ParsedCharacters != Value.size() || Parsed == 0)
			throw std::invalid_argument(std::string(Name) + " must be a positive integer");
		return static_cast<std::size_t>(Parsed);
	}

	Options ParseOptions(int ArgumentCount, char **Arguments) {
		Options Result;
		for (int Index = 1; Index < ArgumentCount; ++Index) {
			const std::string_view Argument(Arguments[Index]);
			const auto Value = [&](std::string_view Prefix) -> std::optional<std::string_view> {
				return Argument.starts_with(Prefix) ? std::optional(Argument.substr(Prefix.size())) : std::nullopt;
			};
			if (Argument == "--help") {
				std::cout << "Usage: gargantuan_renderer_foundation2_gpu_benchmark "
					"[--scenario=all|cloth-full-4k|cloth-full-16k|cloth-full-64k|cloth-partial-16k|"
					"cloth-partial-64k|rubber|topology|gui|mixed|texture-lifecycle|environment|animation|"
					"animation-rigs|animation-bones|animation-shadow|animation-static-50k|"
					"animation-differential] "
					"[--frames=N] [--warmup=N] [--width=N] [--height=N] [--synchronize=on|off]\n";
				std::exit(0);
			} else if (const auto Parsed = Value("--scenario=")) Result.Scenario = *Parsed;
			else if (const auto Parsed = Value("--frames=")) Result.Frames = ParseSize(*Parsed, "frames");
			else if (const auto Parsed = Value("--warmup=")) Result.WarmupFrames = ParseSize(*Parsed, "warmup");
			else if (const auto Parsed = Value("--width=")) Result.Width = static_cast<std::uint32_t>(ParseSize(*Parsed, "width"));
			else if (const auto Parsed = Value("--height=")) Result.Height = static_cast<std::uint32_t>(ParseSize(*Parsed, "height"));
			else if (const auto Parsed = Value("--synchronize=")) {
				if (*Parsed == "on") Result.Synchronize = true;
				else if (*Parsed == "off") Result.Synchronize = false;
				else throw std::invalid_argument("synchronize must be on or off");
			}
			else throw std::invalid_argument("Unknown GPU benchmark argument: " + std::string(Argument));
		}
		return Result;
	}

	RenderFrameState MakeFrame(const Options &Settings) {
		RenderFrameState Frame;
		Frame.ViewportWidth = Settings.Width;
		Frame.ViewportHeight = Settings.Height;
		return Frame;
	}

	std::shared_ptr<const std::vector<std::uint8_t>> MakeAtlas(std::uint32_t Width, std::uint32_t Height, std::uint8_t Seed) {
		auto Pixels = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(Width) * Height * 4u);
		for (std::size_t Index = 0; Index < Pixels->size(); Index += 4) {
			(*Pixels)[Index] = static_cast<std::uint8_t>(Seed + Index / 4u);
			(*Pixels)[Index + 1] = static_cast<std::uint8_t>(255u - Seed);
			(*Pixels)[Index + 2] = static_cast<std::uint8_t>((Index / 4u) * 13u);
			(*Pixels)[Index + 3] = 255;
		}
		return Pixels;
	}

	std::shared_ptr<const std::vector<RenderVertex>> MakeGridVertices(
		std::uint32_t Side,
		std::uint32_t FirstVertex,
		std::uint32_t Count,
		std::size_t Frame,
		bool Rubber
	) {
		auto Vertices = std::make_shared<std::vector<RenderVertex>>(Count);
		for (std::uint32_t Local = 0; Local < Count; ++Local) {
			const auto Index = FirstVertex + Local;
			const auto X = Index % Side;
			const auto Y = Index / Side;
			const float UnitX = static_cast<float>(X) / static_cast<float>(Side - 1);
			const float UnitY = static_cast<float>(Y) / static_cast<float>(Side - 1);
			const float Phase = static_cast<float>(Frame) * 0.075f + UnitX * 8.0f + UnitY * 5.0f;
			const float Wave = Rubber
				? std::sin(Phase) * std::cos(UnitY * 9.0f + static_cast<float>(Frame) * 0.031f) * 0.12f
				: std::sin(Phase) * 0.06f;
			auto &Vertex = (*Vertices)[Local];
			Vertex.Position = {UnitX * 1.6f - 0.8f, UnitY * 1.6f - 0.8f, Wave};
			Vertex.Normal = glm::normalize(glm::vec3(-std::cos(Phase) * 0.08f, -std::sin(Phase) * 0.05f, 1.0f));
			Vertex.TextureCoordinate = {UnitX, UnitY};
		}
		return Vertices;
	}

	std::shared_ptr<const std::vector<std::uint32_t>> MakeGridIndices(std::uint32_t Side) {
		auto Indices = std::make_shared<std::vector<std::uint32_t>>();
		Indices->reserve(static_cast<std::size_t>(Side - 1) * (Side - 1) * 6u);
		for (std::uint32_t Y = 0; Y + 1 < Side; ++Y) {
			for (std::uint32_t X = 0; X + 1 < Side; ++X) {
				const auto TopLeft = Y * Side + X;
				Indices->insert(Indices->end(), {
					TopLeft, TopLeft + Side, TopLeft + 1,
					TopLeft + 1, TopLeft + Side, TopLeft + Side + 1,
				});
			}
		}
		return Indices;
	}

	RenderBounds MakeDeformBounds(std::size_t Frame, bool Rubber) {
		const float ZExtent = Rubber
			? 0.13f + (std::sin(static_cast<float>(Frame) * 0.047f) + 1.0f) * 0.01f
			: 0.2f;
		return {{-0.8f, -0.8f, -ZExtent}, {0.8f, 0.8f, ZExtent}};
	}

	RenderObjectCreate MakeObject(ObjectId Object, std::optional<RenderMeshIdentity> Mesh = std::nullopt) {
		RenderItem Item{.Object = Object};
		Item.CastShadow = false;
		return {.Item = Item, .Mesh = Mesh};
	}

	RenderUiFrame MakeUi(const Options &Settings, std::size_t QuadCount, std::size_t BatchCount) {
		RenderUiFrame Ui{Settings.Width, Settings.Height, 1.0f, {}};
		Ui.Batches.reserve(BatchCount);
		const auto QuadsPerBatch = (QuadCount + BatchCount - 1) / BatchCount;
		std::size_t GlobalQuad = 0;
		for (std::size_t BatchIndex = 0; BatchIndex < BatchCount && GlobalQuad < QuadCount; ++BatchIndex) {
			const auto BatchQuads = std::min(QuadsPerBatch, QuadCount - GlobalQuad);
			RenderUiBatch Batch;
			Batch.Texture = RenderTextureIdentity{1 + BatchIndex % 8, 1};
			Batch.Clip = RenderUiClipRect{
				static_cast<float>((BatchIndex % 4) * 8), static_cast<float>((BatchIndex % 3) * 6),
				static_cast<float>(Settings.Width - (BatchIndex % 4) * 16),
				static_cast<float>(Settings.Height - (BatchIndex % 3) * 12),
			};
			Batch.Layer = static_cast<std::int32_t>(BatchIndex);
			Batch.Opacity = 0.65f + static_cast<float>(BatchIndex % 4) * 0.1f;
			Batch.Vertices.reserve(BatchQuads * 4);
			Batch.Indices.reserve(BatchQuads * 6);
			for (std::size_t Quad = 0; Quad < BatchQuads; ++Quad, ++GlobalQuad) {
				const float X = static_cast<float>((GlobalQuad % 100) * 13);
				const float Y = static_cast<float>(((GlobalQuad / 100) % 60) * 12);
				const auto Base = static_cast<std::uint32_t>(Batch.Vertices.size());
				const glm::vec4 Color{0.5f + static_cast<float>(BatchIndex % 3) * 0.2f, 0.75f, 1.0f, 0.8f};
				Batch.Vertices.insert(Batch.Vertices.end(), {
					{{X, Y}, {0.0f, 0.0f}, Color}, {{X + 11.0f, Y}, {1.0f, 0.0f}, Color},
					{{X + 11.0f, Y + 10.0f}, {1.0f, 1.0f}, Color}, {{X, Y + 10.0f}, {0.0f, 1.0f}, Color},
				});
				Batch.Indices.insert(Batch.Indices.end(), {Base, Base + 1, Base + 2, Base + 2, Base + 3, Base});
			}
			Ui.Batches.push_back(std::move(Batch));
		}
		return Ui;
	}

	void AddAtlases(RenderPublication &Publication) {
		for (std::uint64_t Texture = 1; Texture <= 8; ++Texture)
			Publication.TextureCreates.push_back({
				{Texture, 1}, 1, 64, 64, RenderTextureFormat::Rgba8Unorm,
				MakeAtlas(64, 64, static_cast<std::uint8_t>(Texture * 17)),
			});
	}

	void AppendSamples(Samples &Result, double FrameMilliseconds, const SDLRendererMetrics &Metrics) {
		Result.Frame.push_back(FrameMilliseconds);
		Result.Projection.push_back(Milliseconds(Metrics.LastProjectionNanoseconds));
		Result.MeshTransfer.push_back(Milliseconds(Metrics.LastMeshTransferNanoseconds));
		Result.TextureTransfer.push_back(Milliseconds(Metrics.LastTextureTransferNanoseconds));
		Result.UiPreparation.push_back(Milliseconds(Metrics.LastUiPreparationNanoseconds));
		Result.Submission.push_back(Milliseconds(Metrics.LastSubmissionNanoseconds));
		Result.CompletionWait.push_back(Milliseconds(Metrics.LastGpuCompletionWaitNanoseconds));
	}

	template <typename PublicationFactory>
	void Run(
		std::string_view Name,
		const Options &Settings,
		RenderPublication Initial,
		PublicationFactory MakeNext,
		bool StableResourcesExpected,
		std::string_view RestartGate = "NA",
		std::optional<SkinningExpectations> Skinning = std::nullopt
	) {
		SDLRenderer Renderer(
			Vector2(static_cast<float>(Settings.Width), static_cast<float>(Settings.Height)),
			{.Offscreen = true, .WaitForGpuCompletion = Settings.Synchronize, .DebugDevice = false}
		);
		Renderer.Draw(std::make_shared<const RenderPublication>(std::move(Initial)));
		for (std::size_t Frame = 0; Frame < Settings.WarmupFrames; ++Frame)
			Renderer.Draw(std::make_shared<const RenderPublication>(MakeNext(Frame)));
		const auto Baseline = Renderer.GetMetrics();
		Samples Timings;
		for (std::size_t Frame = 0; Frame < Settings.Frames; ++Frame) {
			auto Publication = std::make_shared<const RenderPublication>(MakeNext(Settings.WarmupFrames + Frame));
			const auto Start = Clock::now();
			Renderer.Draw(std::move(Publication));
			AppendSamples(Timings, Milliseconds(Clock::now() - Start), Renderer.GetMetrics());
		}
		Renderer.WaitForIdle();
		const auto Final = Renderer.GetMetrics();
		const auto VertexCreations = Final.VertexBufferCreations - Baseline.VertexBufferCreations;
		const auto IndexCreations = Final.IndexBufferCreations - Baseline.IndexBufferCreations;
		const auto TransferCreations = Final.TransferBufferCreations - Baseline.TransferBufferCreations;
		const auto Reallocations = Final.BufferReallocations - Baseline.BufferReallocations;
		const auto PipelineCreations = Final.PipelineCreations - Baseline.PipelineCreations;
		const auto ShaderCreations = Final.ShaderCreations - Baseline.ShaderCreations;
		const auto PaletteBufferCreations = Final.PaletteBufferCreations - Baseline.PaletteBufferCreations;
		const auto PaletteTransferCreations =
			Final.PaletteTransferBufferCreations - Baseline.PaletteTransferBufferCreations;
		const auto PaletteScratchAllocations =
			Final.PaletteScratchAllocations - Baseline.PaletteScratchAllocations;
		const bool Stable = VertexCreations == 0 && IndexCreations == 0 && TransferCreations == 0 &&
			Reallocations == 0 && PipelineCreations == 0 && ShaderCreations == 0 &&
			PaletteBufferCreations == 0 && PaletteTransferCreations == 0 &&
			PaletteScratchAllocations == 0;
		const auto StabilityGate = StableResourcesExpected ? (Stable ? "PASS" : "FAIL") : "NA";
		std::string_view SkinningGate = "NA";
		if (Skinning) {
			const auto ExpectedUploads = Settings.Frames * Skinning->RigCount;
			const auto ExpectedPaletteBytes = ExpectedUploads * Skinning->BoneCount * sizeof(RenderSkinPaletteEntry);
			const bool Passed = Final.GpuSkinningRigs == Skinning->RigCount && Final.CpuFallbackRigs == 0 &&
				Final.PaletteUploads - Baseline.PaletteUploads == ExpectedUploads &&
				Final.PaletteUploadBytes - Baseline.PaletteUploadBytes == ExpectedPaletteBytes &&
				PaletteBufferCreations == 0 && PaletteTransferCreations == 0 && PaletteScratchAllocations == 0 &&
				Final.PaletteResourceReleases - Baseline.PaletteResourceReleases == 0 &&
				Final.PaletteBufferCreations == Skinning->RigCount &&
				Final.PaletteTransferBufferCreations == Skinning->RigCount &&
				Final.SkinnedSourceResourceCreations == 1 && Final.CpuSkinnedVertexUploads == 0 &&
				Final.MainShadowPoseMismatches == 0 && Final.FullResyncs - Baseline.FullResyncs == 0;
			SkinningGate = Passed ? "PASS" : "FAIL";
			if (!Passed) throw std::runtime_error("GPU skinning steady-state/resource-sharing gate failed");
		}

		std::cout << Name << ',' << Renderer.GetDriverName() << ',' << (Settings.Synchronize ? "fence" : "queued")
			<< ',' << Settings.Frames << ',' << Settings.WarmupFrames;
		for (const auto &Values : {Timings.Frame, Timings.Projection, Timings.MeshTransfer, Timings.TextureTransfer,
			Timings.UiPreparation, Timings.Submission, Timings.CompletionWait}) {
			const auto Stats = Summarize(Values);
			std::cout << ',' << Stats.Mean << ',' << Stats.P50 << ',' << Stats.P95 << ',' << Stats.P99 << ',' << Stats.Maximum;
		}
		std::cout << ',' << (Final.UploadedBytes - Baseline.UploadedBytes) / Settings.Frames
			<< ',' << Final.UploadOperations - Baseline.UploadOperations
			<< ',' << VertexCreations << ',' << IndexCreations << ',' << TransferCreations << ',' << Reallocations
			<< ',' << Final.BufferCycleRequests - Baseline.BufferCycleRequests
			<< ',' << Final.TextureCreations - Baseline.TextureCreations
			<< ',' << Final.TextureUpdates - Baseline.TextureUpdates
			<< ',' << Final.TextureReleases - Baseline.TextureReleases
			<< ',' << Final.DrawCalls - Baseline.DrawCalls
			<< ',' << Final.UiBatches - Baseline.UiBatches
			<< ',' << Final.ScissorChanges - Baseline.ScissorChanges
			<< ',' << Final.PipelineSwitches - Baseline.PipelineSwitches
			<< ',' << PipelineCreations << ',' << ShaderCreations
			<< ',' << Final.FullResyncs - Baseline.FullResyncs
			<< ',' << Final.EnvironmentApplications - Baseline.EnvironmentApplications
			<< ',' << Final.SkyDraws - Baseline.SkyDraws
			<< ',' << Final.GpuSkinningRigs << ',' << Final.CpuFallbackRigs
			<< ',' << Final.PaletteUploads - Baseline.PaletteUploads
			<< ',' << Final.PaletteUploadBytes - Baseline.PaletteUploadBytes
			<< ',' << PaletteBufferCreations << ',' << PaletteTransferCreations
			<< ',' << Final.PaletteResourceReleases - Baseline.PaletteResourceReleases
			<< ',' << PaletteScratchAllocations
			<< ',' << Final.FallbackTransitions - Baseline.FallbackTransitions
			<< ',' << Final.StalePoseDrops - Baseline.StalePoseDrops
			<< ',' << Final.SkinnedSourceResourceCreations
			<< ',' << Final.CpuSkinnedVertexUploads
			<< ',' << Final.MainShadowPoseMismatches
			<< ',' << (Skinning ? Skinning->SourceVertexCount * sizeof(Vertex) +
				Skinning->SourceIndexCount * sizeof(std::uint32_t) : 0)
			<< ',' << (Skinning ? Skinning->BoneCount * sizeof(RenderSkinPaletteEntry) : 0)
			<< ',' << (Skinning ? Skinning->RigCount * Skinning->BoneCount *
				sizeof(RenderSkinPaletteEntry) : 0)
			<< ',' << StabilityGate << ',' << RestartGate << ',' << SkinningGate << ",NA,"
			<< (Settings.Synchronize ? "FenceCompletionLatency" : "QueuedCpuPacing") << '\n';
	}

	RenderSkeletonIdentity MakeSkeletonIdentity(std::uint8_t Seed) {
		RenderSkeletonIdentity Skeleton;
		for (std::size_t Index = 0; Index < Skeleton.Bytes.size(); ++Index)
			Skeleton.Bytes[Index] = static_cast<std::uint8_t>(Seed + Index * 17u);
		return Skeleton;
	}

	RenderMeshCreate MakeSkinnedSource(
		RenderMeshIdentity Mesh,
		RenderSkeletonIdentity Skeleton,
		std::uint32_t Bones,
		std::uint32_t Side
	) {
		const auto VertexCount = Side * Side;
		auto Vertices = MakeGridVertices(Side, 0, VertexCount, 0, false);
		auto Influences = std::make_shared<std::vector<RenderSkinInfluence>>(VertexCount);
		for (std::size_t Index = 0; Index < Influences->size(); ++Index) {
			auto &Influence = Influences->at(Index);
			Influence.Joints = {
				static_cast<std::uint16_t>(Index % Bones),
				static_cast<std::uint16_t>((Index + 1) % Bones),
				static_cast<std::uint16_t>((Index + 3) % Bones),
				static_cast<std::uint16_t>((Index + 7) % Bones),
			};
			Influence.Weights = {0.4f, 0.3f, 0.2f, 0.1f};
		}
		return {
			.Mesh = Mesh,
			.TopologyRevision = 1,
			.VertexRevision = 1,
			.Vertices = Vertices,
			.Indices = MakeGridIndices(Side),
			.Bounds = {{-0.9f, -0.9f, -0.25f}, {0.9f, 0.9f, 0.25f}},
			.SkinInfluences = Influences,
			.Skeleton = Skeleton,
			.SkeletonJointCount = Bones,
		};
	}

	std::shared_ptr<const std::vector<RenderSkinPaletteEntry>> MakeSkinPalette(
		std::uint32_t Bones,
		std::size_t Rig,
		std::uint64_t PoseRevision
	) {
		auto Palette = std::make_shared<std::vector<RenderSkinPaletteEntry>>(Bones);
		for (std::size_t Bone = 0; Bone < Bones; ++Bone) {
			const auto Phase = static_cast<float>(PoseRevision) * 0.019f +
				static_cast<float>(Rig) * 0.007f + static_cast<float>(Bone) * 0.011f;
			const glm::vec3 Scale{
				1.0f + std::sin(Phase) * 0.04f,
				1.0f + std::cos(Phase * 1.3f) * 0.03f,
				1.0f + std::sin(Phase * 0.7f) * 0.02f,
			};
			auto &Entry = Palette->at(Bone);
			Entry.PositionMatrix =
				glm::translate(glm::mat4(1.0f), glm::vec3(std::sin(Phase), std::cos(Phase), 0.0f) * 0.008f) *
				glm::rotate(glm::mat4(1.0f), std::sin(Phase) * 0.025f, glm::vec3(0.0f, 0.0f, 1.0f)) *
				glm::scale(glm::mat4(1.0f), Scale);
			Entry.NormalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(Entry.PositionMatrix))));
		}
		return Palette;
	}

	RenderAnimationPoseUpdate MakeSkinPose(
		ObjectId Object,
		RenderMeshIdentity SourceMesh,
		RenderSkeletonIdentity Skeleton,
		std::uint32_t Bones,
		std::size_t Rig,
		std::uint64_t PoseRevision
	) {
		return {
			.Object = Object,
			.SourceMesh = SourceMesh,
			.PoseRevision = PoseRevision,
			.Palette = {Skeleton, MakeSkinPalette(Bones, Rig, PoseRevision)},
		};
	}

	RenderFrameState MakeSkinFrame(const Options &Settings, bool Shadows) {
		auto Frame = MakeFrame(Settings);
		Frame.Environment.AmbientColor = {0.25f, 0.22f, 0.2f};
		Frame.Environment.SunDirection = glm::normalize(glm::vec3(0.35f, 0.8f, 0.48f));
		Frame.Environment.SunIntensity = Shadows ? 1.5f : 0.8f;
		Frame.Environment.EnvironmentColor = {0.01f, 0.015f, 0.025f};
		return Frame;
	}

	void ProveSkinningRestart(
		const Options &Settings,
		const RenderMeshCreate &Source,
		RenderSkeletonIdentity Skeleton,
		std::uint32_t Bones
	) {
		constexpr ObjectId Object{1, 1};
		const auto MakeResync = [&](RenderPublicationId Id, std::uint64_t PoseRevision) {
			RenderPublication Publication{
				.Id = Id, .FullResync = true, .Frame = MakeSkinFrame(Settings, true),
			};
			Publication.MeshCreates.push_back(Source);
			auto Created = MakeObject(Object, Source.Mesh);
			Created.Item.CastShadow = true;
			Created.Material.DoubleSided = true;
			Publication.Creates.push_back(std::move(Created));
			Publication.AnimationPoseUpdates.push_back(
				MakeSkinPose(Object, Source.Mesh, Skeleton, Bones, 0, PoseRevision));
			return Publication;
		};
		const auto RequireInjectedConstructionFailure = [&](SDLRendererOptions Options, std::string_view Name) {
			bool FailureObserved = false;
			try {
				SDLRenderer Failing(Vector2(static_cast<float>(Settings.Width), static_cast<float>(Settings.Height)),
					Options);
			} catch (const std::runtime_error &) {
				FailureObserved = true;
			}
			if (!FailureObserved)
				throw std::runtime_error(std::string(Name) + " failure injection did not stop renderer creation");
		};
		RequireInjectedConstructionFailure(
			{.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false,
			 .InjectShaderCreationFailures = 1}, "shader");
		RequireInjectedConstructionFailure(
			{.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false,
			 .InjectPipelineCreationFailures = 1}, "pipeline");
		{
			SDLRenderer Recovering(Vector2(static_cast<float>(Settings.Width), static_cast<float>(Settings.Height)),
				{.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false,
				 .InjectPaletteUploadFailures = 1});
			bool InjectedFailureObserved = false;
			try {
				Recovering.Draw(std::make_shared<const RenderPublication>(MakeResync(1, 17)));
			} catch (const std::runtime_error &) {
				InjectedFailureObserved = true;
			}
			if (!InjectedFailureObserved)
				throw std::runtime_error("palette failure injection did not reach renderer recovery");
			Recovering.Draw(std::make_shared<const RenderPublication>(MakeResync(2, 17)));
			const auto Metrics = Recovering.GetMetrics();
			if (Metrics.GpuSkinningRigs != 1 || Metrics.PaletteUploads != 1 ||
				Metrics.PaletteResourceReleases < 1 || Metrics.CpuSkinnedVertexUploads != 0)
				throw std::runtime_error("palette upload failure did not recover through a complete resync");
		}
		{
			SDLRenderer Renderer(Vector2(static_cast<float>(Settings.Width), static_cast<float>(Settings.Height)),
				{.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false});
			Renderer.Draw(std::make_shared<const RenderPublication>(MakeResync(1, 17)));
			const auto Metrics = Renderer.GetMetrics();
			if (Metrics.GpuSkinningRigs != 1 || Metrics.PaletteUploads != 1 ||
				Metrics.SkinnedSourceResourceCreations != 1 || Metrics.MainShadowPoseMismatches != 0)
				throw std::runtime_error("initial GPU skinning residency did not become coherent");
			RenderPublication Stale{
				.Id = 2, .BaseId = 1, .Frame = MakeSkinFrame(Settings, true),
			};
			Stale.AnimationPoseUpdates.push_back(
				MakeSkinPose(Object, Source.Mesh, Skeleton, Bones, 0, 17));
			bool StaleRejected = false;
			try {
				Renderer.Draw(std::make_shared<const RenderPublication>(std::move(Stale)));
			} catch (const std::invalid_argument &) {
				StaleRejected = true;
			}
			if (!StaleRejected || Renderer.GetMetrics().StalePoseDrops != 1)
				throw std::runtime_error("stale GPU pose did not increment the bounded rejection counter");
			Renderer.Draw(std::make_shared<const RenderPublication>(MakeResync(3, 18)));
			const auto Recovered = Renderer.GetMetrics();
			if (Recovered.GpuSkinningRigs != 1 || Recovered.PaletteUploads != 2 ||
				Recovered.SkinnedSourceResourceCreations != 2 || Recovered.CpuSkinnedVertexUploads != 0)
				throw std::runtime_error("source/palette recreation after stale-state recovery was incoherent");
		}
		SDLRenderer Restarted(Vector2(static_cast<float>(Settings.Width), static_cast<float>(Settings.Height)),
			{.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false});
		Restarted.Draw(std::make_shared<const RenderPublication>(MakeResync(50, 17)));
		const auto RestartedMetrics = Restarted.GetMetrics();
		if (RestartedMetrics.GpuSkinningRigs != 1 || RestartedMetrics.PaletteUploads != 1 ||
			RestartedMetrics.PaletteBufferCreations != 1 || RestartedMetrics.SkinnedSourceResourceCreations != 1 ||
			RestartedMetrics.MainShadowPoseMismatches != 0)
			throw std::runtime_error("renderer restart did not recreate the current GPU skinning pose");
	}

	void RunGpuSkinning(
		const Options &Settings,
		std::string_view Name,
		std::size_t RigCount,
		std::uint32_t Bones,
		bool Shadows,
		std::size_t StaticObjectCount = 0,
		bool RestartProof = false
	) {
		if (Bones == 0 || Bones > MaximumRenderSkinPaletteEntries)
			throw std::invalid_argument("GPU skinning benchmark bone count is outside the semantic limit");
		const RenderMeshIdentity SourceMesh{500, 1};
		const auto Skeleton = MakeSkeletonIdentity(41);
		const auto Source = MakeSkinnedSource(SourceMesh, Skeleton, Bones, 32);
		if (RestartProof) ProveSkinningRestart(Settings, Source, Skeleton, Bones);
		RenderPublication Initial{
			.Id = 1, .FullResync = true, .Frame = MakeSkinFrame(Settings, Shadows),
		};
		Initial.MeshCreates.push_back(Source);
		Initial.Creates.reserve(StaticObjectCount + RigCount);
		for (std::size_t Index = 0; Index < StaticObjectCount; ++Index) {
			auto Object = MakeObject({static_cast<std::uint32_t>(RigCount + Index + 1), 1});
			Object.Item.CastShadow = false;
			Object.Item.ModelMatrix = glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 2.0f, 0.5f));
			Object.Item.InverseModelMatrix = glm::inverse(Object.Item.ModelMatrix);
			Initial.Creates.push_back(std::move(Object));
		}
		Initial.AnimationPoseUpdates.reserve(RigCount);
		for (std::size_t Rig = 0; Rig < RigCount; ++Rig) {
			const ObjectId Object{static_cast<std::uint32_t>(Rig + 1), 1};
			auto Created = MakeObject(Object, SourceMesh);
			Created.Item.CastShadow = Shadows;
			Created.Material.DoubleSided = true;
			Initial.Creates.push_back(std::move(Created));
			Initial.AnimationPoseUpdates.push_back(MakeSkinPose(Object, SourceMesh, Skeleton, Bones, Rig, 1));
		}
		auto CurrentId = RenderPublicationId{1};
		const auto MakeNext = [=](std::size_t Frame) mutable {
			const auto PoseRevision = static_cast<std::uint64_t>(Frame + 2);
			RenderPublication Publication{
				.Id = ++CurrentId,
				.BaseId = CurrentId - 1,
				.Frame = MakeSkinFrame(Settings, Shadows),
			};
			Publication.AnimationPoseUpdates.reserve(RigCount);
			for (std::size_t Rig = 0; Rig < RigCount; ++Rig) {
				const ObjectId Object{static_cast<std::uint32_t>(Rig + 1), 1};
				Publication.AnimationPoseUpdates.push_back(
					MakeSkinPose(Object, SourceMesh, Skeleton, Bones, Rig, PoseRevision));
			}
			return Publication;
		};
		Run(Name, Settings, std::move(Initial), MakeNext, true, RestartProof ? "PASS" : "NA",
			SkinningExpectations{
				.RigCount = RigCount,
				.BoneCount = Bones,
				.SourceVertexCount = Source.Vertices->size(),
				.SourceIndexCount = Source.Indices->size(),
			});
	}

	struct DifferentialFixture {
		RenderMeshCreate Source;
		ImportedMesh CpuSource;
	};

	DifferentialFixture MakeDifferentialFixture(
		RenderMeshIdentity SourceMesh,
		RenderSkeletonIdentity Skeleton
	) {
		auto Vertices = std::make_shared<std::vector<RenderVertex>>();
		auto Influences = std::make_shared<std::vector<RenderSkinInfluence>>();
		auto CpuInfluences = std::make_shared<std::vector<ImportedSkinInfluence>>();
		auto Indices = std::make_shared<std::vector<std::uint32_t>>();
		const auto AddTriangle = [&](glm::vec2 Center, glm::vec3 Normal, RenderSkinInfluence Influence) {
			const std::array<glm::vec2, 3> Offsets{{{-0.18f, -0.14f}, {0.2f, -0.12f}, {0.01f, 0.2f}}};
			for (std::size_t Index = 0; Index < Offsets.size(); ++Index) {
				RenderVertex Vertex;
				Vertex.Position = {Center + Offsets[Index], 0.45f + static_cast<float>(Index) * 0.005f};
				Vertex.Normal = glm::normalize(Normal);
				Vertex.Tangent = {glm::normalize(glm::cross(Vertex.Normal, glm::vec3(0.0f, 0.0f, 1.0f))), 1.0f};
				if (glm::length(glm::vec3(Vertex.Tangent)) < 1.0e-4f)
					Vertex.Tangent = {1.0f, 0.0f, 0.0f, 1.0f};
				Vertex.TextureCoordinate = {
					Index == 1 ? 1.0f : 0.0f,
					Index == 2 ? 1.0f : 0.0f,
				};
				Indices->push_back(static_cast<std::uint32_t>(Vertices->size()));
				Vertices->push_back(Vertex);
				Influences->push_back(Influence);
				CpuInfluences->push_back({Influence.Joints, Influence.Weights});
			}
		};
		AddTriangle({-0.38f, 0.36f}, {0.2f, 0.65f, 0.73f}, {{0, 0, 0, 0}, {1.0f, 0.0f, 0.0f, 0.0f}});
		AddTriangle({0.35f, 0.34f}, {-0.35f, 0.75f, 0.56f}, {{0, 1, 0, 0}, {0.35f, 0.65f, 0.0f, 0.0f}});
		AddTriangle({-0.35f, -0.36f}, {0.55f, 0.35f, 0.76f}, {{0, 1, 2, 3}, {0.1f, 0.2f, 0.3f, 0.4f}});
		AddTriangle({0.36f, -0.35f}, {-0.5f, 0.45f, 0.74f}, {{2, 0, 0, 0}, {1.0f, 0.0f, 0.0f, 0.0f}});
		RenderMeshCreate Source{
			.Mesh = SourceMesh,
			.TopologyRevision = 1,
			.VertexRevision = 1,
			.Vertices = Vertices,
			.Indices = Indices,
			.Bounds = {{-0.7f, -0.7f, 0.25f}, {0.7f, 0.7f, 0.7f}},
			.SkinInfluences = Influences,
			.Skeleton = Skeleton,
			.SkeletonJointCount = 4,
		};
		ImportedMesh CpuSource;
		CpuSource.Vertices = Vertices;
		CpuSource.Indices = Indices;
		CpuSource.Bounds = Source.Bounds;
		CpuSource.SkinInfluences = CpuInfluences;
		return {std::move(Source), std::move(CpuSource)};
	}

	std::shared_ptr<const std::vector<RenderSkinPaletteEntry>> MakeDifferentialPalette(std::size_t Pose) {
		auto Palette = std::make_shared<std::vector<RenderSkinPaletteEntry>>(4);
		for (std::size_t Bone = 0; Bone < Palette->size(); ++Bone) {
			const float Phase = static_cast<float>(Pose) * 0.31f + static_cast<float>(Bone) * 0.73f;
			const glm::vec3 Axis = glm::normalize(glm::vec3(
				0.3f + static_cast<float>(Bone) * 0.17f, 0.8f, 0.45f + static_cast<float>(Bone) * 0.09f));
			const glm::vec3 Scale{
				0.82f + static_cast<float>(Bone) * 0.07f + std::sin(Phase) * 0.04f,
				1.16f - static_cast<float>(Bone) * 0.05f + std::cos(Phase) * 0.06f,
				0.91f + std::sin(Phase * 0.7f) * 0.08f,
			};
			auto &Entry = Palette->at(Bone);
			Entry.PositionMatrix =
				glm::translate(glm::mat4(1.0f), glm::vec3(std::sin(Phase) * 0.06f,
					std::cos(Phase * 0.8f) * 0.045f, std::sin(Phase * 0.5f) * 0.025f)) *
				glm::rotate(glm::mat4(1.0f), 0.08f + std::sin(Phase) * 0.12f, Axis) *
				glm::scale(glm::mat4(1.0f), Scale);
			Entry.NormalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(Entry.PositionMatrix))));
		}
		return Palette;
	}

	RenderFrameState MakeDifferentialFrame(std::uint32_t Width, std::uint32_t Height) {
		RenderFrameState Frame;
		Frame.ViewportWidth = Width;
		Frame.ViewportHeight = Height;
		Frame.Environment.AmbientColor = {0.04f, 0.05f, 0.06f};
		Frame.Environment.SunDirection = glm::normalize(glm::vec3(0.27f, 0.72f, 0.64f));
		Frame.Environment.SunColor = {1.0f, 0.72f, 0.45f};
		Frame.Environment.SunIntensity = 2.25f;
		Frame.Environment.EnvironmentColor = {0.005f, 0.008f, 0.012f};
		return Frame;
	}

	void RunSkinningDifferential() {
		constexpr std::uint32_t Width = 192;
		constexpr std::uint32_t Height = 192;
		constexpr ObjectId Object{701, 1};
		constexpr RenderMeshIdentity SourceMesh{701, 1};
		constexpr RenderMeshIdentity PosedMesh{702, 1};
		const auto Skeleton = MakeSkeletonIdentity(93);
		const auto Fixture = MakeDifferentialFixture(SourceMesh, Skeleton);
		EditorViewportRenderer GpuRenderer(Width, Height);
		EditorViewportRenderer CpuRenderer(Width, Height);
		std::uint64_t DifferenceSum = 0;
		std::uint64_t ComparedChannels = 0;
		std::size_t MismatchedPixels = 0;
		std::size_t ComparedPixels = 0;
		std::uint8_t MaximumDifference = 0;
		std::uint8_t MinimumGpuChannel = 255;
		std::uint8_t MaximumGpuChannel = 0;

		for (std::size_t Pose = 0; Pose < 8; ++Pose) {
			const auto PoseRevision = static_cast<std::uint64_t>(Pose + 1);
			const auto Palette = MakeDifferentialPalette(Pose);
			std::vector<RenderVertex> CpuVertices;
			RenderBounds CpuBounds;
			if (!AnimationRuntime::SkinMeshCpu(Fixture.CpuSource, *Palette, CpuVertices, CpuBounds))
				throw std::runtime_error("CPU reference rejected a differential fixture pose");
			auto SharedCpuVertices = std::make_shared<const std::vector<RenderVertex>>(std::move(CpuVertices));

			const auto MakePublication = [&](bool Gpu) {
				RenderPublication Publication{
					.Id = PoseRevision,
					.BaseId = Pose == 0 ? InvalidRenderPublicationId : PoseRevision - 1,
					.FullResync = Pose == 0,
					.Frame = MakeDifferentialFrame(Width, Height),
				};
				if (Pose == 0) {
					Publication.MeshCreates.push_back(Fixture.Source);
					if (!Gpu) Publication.MeshCreates.push_back({
						.Mesh = PosedMesh,
						.TopologyRevision = 1,
						.VertexRevision = 1,
						.Vertices = SharedCpuVertices,
						.Indices = Fixture.Source.Indices,
						.Bounds = CpuBounds,
					});
					RenderItem Item{.Object = Object};
					Item.CastShadow = false;
					Item.ModelMatrix =
						glm::rotate(glm::mat4(1.0f), 0.37f, glm::vec3(0.0f, 0.0f, 1.0f)) *
						glm::scale(glm::mat4(1.0f), glm::vec3(0.78f, 0.61f, 1.08f));
					Item.InverseModelMatrix = glm::inverse(Item.ModelMatrix);
					RenderObjectCreate Created{.Item = Item, .Mesh = Gpu ? SourceMesh : PosedMesh};
					Created.Material.DoubleSided = true;
					Publication.Creates.push_back(std::move(Created));
				} else if (!Gpu) {
					Publication.MeshVertexUpdates.push_back({
						.Mesh = PosedMesh,
						.VertexRevision = PoseRevision,
						.FirstVertex = 0,
						.Vertices = SharedCpuVertices,
						.Bounds = CpuBounds,
					});
				}
				Publication.AnimationPoseUpdates.push_back({
					.Object = Object,
					.SourceMesh = SourceMesh,
					.PosedMesh = Gpu ? RenderMeshIdentity{} : PosedMesh,
					.PoseRevision = PoseRevision,
					.Palette = {Skeleton, Palette},
					.Mode = Gpu ? RenderAnimationSkinningMode::GpuPalette :
						RenderAnimationSkinningMode::CpuFallback,
				});
				return std::make_shared<const RenderPublication>(std::move(Publication));
			};

			const auto GpuCapture = GpuRenderer.CaptureBgra(MakePublication(true));
			const std::vector<std::uint8_t> GpuPixels(GpuCapture.BgraPixels.begin(), GpuCapture.BgraPixels.end());
			const auto CpuCapture = CpuRenderer.CaptureBgra(MakePublication(false));
			if (GpuPixels.size() != CpuCapture.BgraPixels.size())
				throw std::runtime_error("CPU/GPU differential readbacks have different dimensions");
			for (std::size_t Pixel = 0; Pixel < GpuPixels.size(); Pixel += 4) {
				std::uint8_t PixelDifference = 0;
				for (std::size_t Channel = 0; Channel < 3; ++Channel) {
					const auto Difference = static_cast<std::uint8_t>(std::abs(
						static_cast<int>(GpuPixels[Pixel + Channel]) -
						static_cast<int>(CpuCapture.BgraPixels[Pixel + Channel])));
					DifferenceSum += Difference;
					++ComparedChannels;
					PixelDifference = std::max(PixelDifference, Difference);
					MaximumDifference = std::max(MaximumDifference, Difference);
					MinimumGpuChannel = std::min(MinimumGpuChannel, GpuPixels[Pixel + Channel]);
					MaximumGpuChannel = std::max(MaximumGpuChannel, GpuPixels[Pixel + Channel]);
				}
				if (PixelDifference > 8) ++MismatchedPixels;
				++ComparedPixels;
			}
		}
		const auto MeanDifference = static_cast<double>(DifferenceSum) / static_cast<double>(ComparedChannels);
		const auto MismatchFraction = static_cast<double>(MismatchedPixels) / static_cast<double>(ComparedPixels);
		if (MaximumGpuChannel - MinimumGpuChannel < 32 || MeanDifference > 0.5 ||
			MismatchFraction > 0.01 || MaximumDifference > 48)
			throw std::runtime_error("CPU/GPU skinning rendered-output differential exceeded tolerance");
		std::cerr << "[Animation:GpuDifferential] poses=8 influences=1/2/4 meanChannelDifference="
			<< MeanDifference << " maximumChannelDifference=" << static_cast<unsigned>(MaximumDifference)
			<< " mismatchedPixelFraction=" << MismatchFraction
			<< " rotatedNonuniformOwner=PASS normalPalette=PASS\n";
	}

	void RunDeformable(const Options &Settings, std::string_view Name, std::uint32_t Side, bool Partial, bool Rubber, bool Topology) {
		const auto VertexCount = Side * Side;
		const RenderMeshIdentity InitialMesh{1, 1};
		RenderPublication Initial{.Id = 1, .FullResync = true, .Frame = MakeFrame(Settings)};
		Initial.MeshCreates.push_back({InitialMesh, 1, 1, MakeGridVertices(Side, 0, VertexCount, 0, Rubber),
			MakeGridIndices(Side), MakeDeformBounds(0, Rubber)});
		Initial.Creates.push_back(MakeObject({1, 1}, InitialMesh));
		auto CurrentId = RenderPublicationId{1};
		auto CurrentGeneration = std::uint32_t{1};
		auto Indices = MakeGridIndices(Side);
		const auto MakeNext = [=](std::size_t Frame) mutable {
			RenderPublication Publication{.Id = ++CurrentId, .BaseId = CurrentId - 1, .Frame = MakeFrame(Settings)};
			if (Topology) {
				const RenderMeshIdentity OldMesh{1, CurrentGeneration};
				const RenderMeshIdentity NewMesh{1, ++CurrentGeneration};
				Publication.MeshRemoves.push_back({OldMesh});
				Publication.MeshCreates.push_back({NewMesh, CurrentGeneration, CurrentGeneration,
					MakeGridVertices(Side, 0, VertexCount, Frame + 1, Rubber), Indices,
					MakeDeformBounds(Frame + 1, Rubber)});
				auto Object = MakeObject({1, 1}, NewMesh);
				Publication.Updates.push_back({Object.Item.Object, RenderUpdateDomain::Geometry, Object.Item, NewMesh, Object.Material, true});
			} else {
				const auto Count = Partial ? VertexCount / 4u : VertexCount;
				const auto First = Partial ? (VertexCount - Count) / 2u : 0u;
				Publication.MeshVertexUpdates.push_back({InitialMesh, Frame + 2, First,
					MakeGridVertices(Side, First, Count, Frame + 1, Rubber),
					MakeDeformBounds(Frame + 1, Rubber)});
			}
			return Publication;
		};
		Run(Name, Settings, std::move(Initial), MakeNext, !Topology);
	}

	void RunGui(const Options &Settings, bool Mixed) {
		constexpr std::size_t RigidCount = 25000;
		constexpr std::uint32_t DynamicSide = 128;
		const auto QuadCount = Mixed ? 5000u : 10000u;
		RenderPublication Initial{.Id = 1, .FullResync = true, .Frame = MakeFrame(Settings)};
		AddAtlases(Initial);
		Initial.Ui = MakeUi(Settings, QuadCount, 40);
		if (Mixed) {
			Initial.Creates.reserve(RigidCount + 1);
			for (std::size_t Index = 0; Index < RigidCount; ++Index) {
				auto Object = MakeObject({static_cast<std::uint32_t>(Index + 1), 1});
				Object.Item.ModelMatrix[3] = glm::vec4(
					static_cast<float>(Index % 160) * 0.01f - 0.8f,
					static_cast<float>(Index / 160) * 0.01f - 0.8f, 0.2f, 1.0f
				);
				Object.Item.InverseModelMatrix[3] = -Object.Item.ModelMatrix[3];
				Object.Item.InverseModelMatrix[3].w = 1.0f;
				Initial.Creates.push_back(std::move(Object));
			}
			const RenderMeshIdentity Mesh{2, 1};
			Initial.MeshCreates.push_back({Mesh, 1, 1, MakeGridVertices(DynamicSide, 0, DynamicSide * DynamicSide, 0, false),
				MakeGridIndices(DynamicSide), {{-0.8f, -0.8f, -0.2f}, {0.8f, 0.8f, 0.2f}}});
			Initial.Creates.push_back(MakeObject({static_cast<std::uint32_t>(RigidCount + 1), 1}, Mesh));
		}
		auto CurrentId = RenderPublicationId{1};
		const auto MakeNext = [=](std::size_t Frame) mutable {
			RenderPublication Publication{.Id = ++CurrentId, .BaseId = CurrentId - 1, .Frame = MakeFrame(Settings)};
			Publication.Ui = MakeUi(Settings, QuadCount, 40);
			Publication.TextureUpdates.push_back({{1 + Frame % 8, 1}, Frame + 2, 8, 8, 8, 8,
				MakeAtlas(8, 8, static_cast<std::uint8_t>(Frame))});
			if (Mixed) {
				Publication.Frame.Camera.ViewMatrix[3].x = std::sin(static_cast<float>(Frame) * 0.02f) * 0.02f;
				Publication.Frame.Camera.ViewProjectionMatrix = Publication.Frame.Camera.ProjectionMatrix * Publication.Frame.Camera.ViewMatrix;
				Publication.Updates.reserve(500);
				for (std::size_t Index = 0; Index < 500; ++Index) {
					const auto Slot = static_cast<std::uint32_t>((Frame * 500 + Index) % RigidCount + 1);
					auto Object = MakeObject({Slot, 1});
					Object.Item.ModelMatrix[3].z = 0.2f + std::sin(static_cast<float>(Frame + Index) * 0.01f) * 0.02f;
					Object.Item.InverseModelMatrix[3].z = -Object.Item.ModelMatrix[3].z;
					Publication.Updates.push_back({Object.Item.Object, RenderUpdateDomain::Transform, Object.Item,
						std::nullopt, Object.Material, true});
				}
				Publication.MeshVertexUpdates.push_back({{2, 1}, Frame + 2, 0,
					MakeGridVertices(DynamicSide, 0, DynamicSide * DynamicSide, Frame + 1, false),
					{{-0.8f, -0.8f, -0.2f}, {0.8f, 0.8f, 0.2f}}});
			}
			return Publication;
		};
		Run(Mixed ? "mixed-25k-rigid-16k-cloth-5k-ui" : "gui-10k-40-batches-8-atlases",
			Settings, std::move(Initial), MakeNext, true);
	}

	void RunTextureLifecycle(const Options &Settings) {
		RenderPublication Initial{.Id = 1, .FullResync = true, .Frame = MakeFrame(Settings)};
		Initial.TextureCreates.push_back({{1, 1}, 1, 64, 64, RenderTextureFormat::Rgba8Unorm, MakeAtlas(64, 64, 1)});
		auto CurrentId = RenderPublicationId{1};
		auto Revision = std::uint64_t{1};
		const auto MakeNext = [=](std::size_t Frame) mutable {
			RenderPublication Publication{.Id = ++CurrentId, .BaseId = CurrentId - 1, .Frame = MakeFrame(Settings)};
			if (Frame % 2 == 0) Publication.TextureUpdates.push_back({{1, 1}, ++Revision, 7, 9, 8, 8, MakeAtlas(8, 8, static_cast<std::uint8_t>(Frame))});
			else Publication.TextureUpdates.push_back({{1, 1}, ++Revision, 0, 0, 64, 64, MakeAtlas(64, 64, static_cast<std::uint8_t>(Frame))});
			return Publication;
		};
		Run("texture-subregion-full-replacement", Settings, std::move(Initial), MakeNext, true, "PASS");

		// Exercise remove and renderer restart/full-resync on the real SDL resource path.
		{
			SDLRenderer Renderer(Vector2(static_cast<float>(Settings.Width), static_cast<float>(Settings.Height)),
				{.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false});
			RenderPublication Create{.Id = 1, .FullResync = true, .Frame = MakeFrame(Settings)};
			Create.TextureCreates.push_back({{9, 1}, 1, 4, 4, RenderTextureFormat::Rgba8Unorm, MakeAtlas(4, 4, 9)});
			Renderer.Draw(std::make_shared<const RenderPublication>(Create));
			RenderPublication Remove{.Id = 2, .BaseId = 1, .Frame = MakeFrame(Settings)};
			Remove.TextureRemoves.push_back({{9, 1}});
			Renderer.Draw(std::make_shared<const RenderPublication>(Remove));
		}
		SDLRenderer Restarted(Vector2(static_cast<float>(Settings.Width), static_cast<float>(Settings.Height)),
			{.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false});
		RenderPublication Resync{.Id = 50, .FullResync = true, .Frame = MakeFrame(Settings)};
		Resync.TextureCreates.push_back({{9, 2}, 1, 4, 4, RenderTextureFormat::Rgba8Unorm, MakeAtlas(4, 4, 19)});
		Restarted.Draw(std::make_shared<const RenderPublication>(Resync));
	}

	RenderEnvironmentState MakeEnvironment(std::size_t Frame, bool SkyEnabled) {
		RenderEnvironmentState Environment;
		const float Angle = (static_cast<float>(Frame % 24) - 6.0f) * 2.0f * std::numbers::pi_v<float> / 24.0f;
		Environment.AmbientColor = {0.18f, 0.2f, 0.24f};
		Environment.SunDirection = {std::cos(Angle), std::sin(Angle), 0.0f};
		Environment.SunColor = {1.0f, 0.92f, 0.8f};
		Environment.SunIntensity = 2.0f * std::max(Environment.SunDirection.y, 0.0f);
		Environment.ExposureMultiplier = Frame % 2 == 0 ? 1.0f : 2.0f;
		Environment.EnvironmentColor = {0.025f, 0.055f, 0.12f};
		Environment.Fog = {.Enabled = Frame % 2 != 0, .Color = {0.35f, 0.42f, 0.5f}, .Start = 8.0f, .End = 120.0f};
		if (SkyEnabled) {
			RenderSkyState Sky{.FaceDimension = 4};
			for (std::size_t Index = 0; Index < Sky.Faces.size(); ++Index)
				Sky.Faces[Index] = {{20 + Index, 1}, 1};
			Environment.Sky = Sky;
		}
		return Environment;
	}

	void RunEnvironment(const Options &Settings) {
		{
			RenderPublication Initial{.Id = 1, .FullResync = true, .Frame = MakeFrame(Settings)};
			Initial.Frame.Environment = MakeEnvironment(0, false);
			auto CurrentId = RenderPublicationId{1};
			const auto MakeNext = [=](std::size_t) mutable {
				RenderPublication Publication{.Id = ++CurrentId, .BaseId = CurrentId - 1, .Frame = MakeFrame(Settings)};
				Publication.Frame.Environment = MakeEnvironment(0, false);
				return Publication;
			};
			Run("environment-fallback-static", Settings, std::move(Initial), MakeNext, true);
		}

		RenderPublication Initial{
			.Id = 1, .FullResync = true, .Frame = MakeFrame(Settings), .EnvironmentChanged = true
		};
		Initial.Frame.Environment = MakeEnvironment(0, true);
		for (std::size_t Index = 0; Index < 6; ++Index)
			Initial.TextureCreates.push_back(
				{{20 + Index, 1},
				 1,
				 4,
				 4,
				 RenderTextureFormat::Rgba8Unorm,
				 MakeAtlas(4, 4, static_cast<std::uint8_t>(31 + Index * 29))}
			);

		// A fresh backend must recreate disposable texture state and apply the complete
		// semantic environment without any DataModel mutation.
		{
			SDLRenderer Restarted(
				Vector2(static_cast<float>(Settings.Width), static_cast<float>(Settings.Height)),
				{.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false}
			);
			Restarted.Draw(std::make_shared<const RenderPublication>(Initial));
			const auto Metrics = Restarted.GetMetrics();
			if (Metrics.FullResyncs != 1 || Metrics.EnvironmentApplications != 1 || Metrics.SkyDraws != 1)
				throw std::runtime_error("environment renderer restart did not apply the complete Sky state");
		}

		auto CurrentId = RenderPublicationId{1};
		auto FaceRevision = std::uint64_t{1};
		const auto MakeNext = [=](std::size_t Frame) mutable {
			RenderPublication Publication{
				.Id = ++CurrentId,
				.BaseId = CurrentId - 1,
				.Frame = MakeFrame(Settings),
				.EnvironmentChanged = true,
			};
			Publication.Frame.Environment = MakeEnvironment(Frame + 1, true);
			if (Frame % 10 == 0) {
				++FaceRevision;
				Publication.TextureUpdates.push_back(
					{{20, 1}, FaceRevision, 0, 0, 4, 4, MakeAtlas(4, 4, static_cast<std::uint8_t>(Frame + 97))}
				);
			}
			Publication.Frame.Environment.Sky->Faces[0].ContentRevision = FaceRevision;
			return Publication;
		};
		Run("environment-sky-fog-clock-exposure", Settings, std::move(Initial), MakeNext, true, "PASS");
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		const SDLGuard SDL;
		const auto Settings = ParseOptions(ArgumentCount, Arguments);
		std::cout << std::fixed << std::setprecision(4);
		std::cout << "Scenario,Driver,Synchronization,Frames,Warmup";
		for (const auto *Stage : {"Frame", "Projection", "MeshTransfer", "TextureTransfer", "UiPreparation", "Submission", "CompletionWait"})
			std::cout << ',' << Stage << "MeanMs," << Stage << "P50Ms," << Stage << "P95Ms," << Stage << "P99Ms," << Stage << "MaxMs";
		std::cout << ",UploadBytesPerFrame,UploadOperations,VertexBufferCreations,IndexBufferCreations,TransferBufferCreations,BufferReallocations,"
			"BufferCycleRequests,TextureCreations,TextureUpdates,TextureReleases,DrawCalls,UiBatches,ScissorChanges,"
			"PipelineSwitches,PipelineCreations,ShaderCreations,FullResyncs,EnvironmentApplications,SkyDraws,"
			"GpuSkinningRigs,CpuFallbackRigs,"
			"PaletteUploads,PaletteUploadBytes,PaletteBufferCreations,PaletteTransferBufferCreations,"
			"PaletteResourceReleases,PaletteScratchAllocations,FallbackTransitions,StalePoseDrops,"
			"SkinnedSourceResourceCreations,"
			"CpuSkinnedVertexUploads,MainShadowPoseMismatches,SharedSourceGpuBytes,PaletteBytesPerRig,"
			"PaletteResidentBytes,StableResourceGate,RestartGate,SkinningGate,"
			"GpuTimestampMeanMs,GpuTimingSource\n";
		std::cout.flush();
		const auto Selected = [&](std::string_view Name) { return Settings.Scenario == "all" || Settings.Scenario == Name; };
		if (Selected("cloth-full-4k")) RunDeformable(Settings, "cloth-full-4096", 64, false, false, false);
		if (Selected("cloth-full-16k")) RunDeformable(Settings, "cloth-full-16384", 128, false, false, false);
		if (Selected("cloth-full-64k")) RunDeformable(Settings, "cloth-full-65536", 256, false, false, false);
		if (Selected("cloth-partial-16k")) RunDeformable(Settings, "cloth-partial-4096-of-16384", 128, true, false, false);
		if (Selected("cloth-partial-64k")) RunDeformable(Settings, "cloth-partial-16384-of-65536", 256, true, false, false);
		if (Selected("rubber")) RunDeformable(Settings, "rubber-full-32761", 181, false, true, false);
		if (Selected("topology")) RunDeformable(Settings, "topology-replacement-16384", 128, false, false, true);
		if (Selected("gui")) RunGui(Settings, false);
		if (Selected("mixed")) RunGui(Settings, true);
		if (Selected("texture-lifecycle")) RunTextureLifecycle(Settings);
		if (Selected("environment")) RunEnvironment(Settings);
		const bool AllAnimation = Settings.Scenario == "all" || Settings.Scenario == "animation";
		if (AllAnimation || Settings.Scenario == "animation-rigs") {
			RunGpuSkinning(Settings, "animation-rigs-1-bones-64", 1, 64, false, 0, true);
			RunGpuSkinning(Settings, "animation-rigs-10-bones-64", 10, 64, false);
			RunGpuSkinning(Settings, "animation-rigs-100-bones-64", 100, 64, false);
			RunGpuSkinning(Settings, "animation-rigs-500-bones-64", 500, 64, false);
		}
		if (AllAnimation || Settings.Scenario == "animation-bones") {
			for (const auto Bones : {16u, 64u, 128u, 256u})
				RunGpuSkinning(Settings, "animation-bones-" + std::to_string(Bones) + "-rigs-10",
					10, Bones, false);
		}
		if (AllAnimation || Settings.Scenario == "animation-shadow")
			RunGpuSkinning(Settings, "animation-main-shadow-rigs-100-bones-64", 100, 64, true);
		if (Settings.Scenario == "animation-static-50k")
			RunGpuSkinning(Settings, "animation-static-50000-rigs-1-bones-64", 1, 64, false, 50'000);
		if (AllAnimation || Settings.Scenario == "animation-differential") RunSkinningDifferential();
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Render:Foundation2GpuBenchmark] " << Error.what() << '\n';
		return 1;
	}
}
