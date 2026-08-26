// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/render/SDLRenderer.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
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
					"cloth-partial-64k|rubber|topology|gui|mixed|texture-lifecycle|environment] "
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
		std::string_view RestartGate = "NA"
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
		const bool Stable = VertexCreations == 0 && IndexCreations == 0 && TransferCreations == 0 && Reallocations == 0;
		const auto StabilityGate = StableResourcesExpected ? (Stable ? "PASS" : "FAIL") : "NA";

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
			<< ',' << Final.FullResyncs - Baseline.FullResyncs
			<< ',' << Final.EnvironmentApplications - Baseline.EnvironmentApplications
			<< ',' << Final.SkyDraws - Baseline.SkyDraws
			<< ',' << StabilityGate << ',' << RestartGate << ",NA,"
			<< (Settings.Synchronize ? "FenceCompletionLatency" : "QueuedCpuPacing") << '\n';
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
			"PipelineSwitches,FullResyncs,EnvironmentApplications,SkyDraws,StableResourceGate,RestartGate,"
			"GpuTimestampMeanMs,GpuTimingSource\n";
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
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Render:Foundation2GpuBenchmark] " << Error.what() << '\n';
		return 1;
	}
}
