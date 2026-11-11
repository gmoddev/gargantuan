// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, you can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/editor/EditorViewport.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "render/sdl/SDLDirectionalCascadePlan.hpp"
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <format>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

namespace gargantuan {
	struct EditorViewportRendererTestAccess {
		static void InjectPostShadowFailure(EditorViewportRenderer &Renderer) {
			Renderer.InjectPostShadowFailureForTesting();
		}
		static SDLRendererMetrics GetMetrics(const EditorViewportRenderer &Renderer) {
			return Renderer.GetMetricsForTesting();
		}
	};
}

namespace {
	using namespace gargantuan;
	struct SDLVideoReference {
		SDLVideoReference() {
			if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
				throw std::runtime_error(std::string("could not initialize shadow fixture video: ") + SDL_GetError());
		}
		~SDLVideoReference() { SDL_QuitSubSystem(SDL_INIT_VIDEO); }
	};
	constexpr std::uint32_t Size = 96;
	constexpr RenderMeshIdentity ReceiverMesh{100, 1};
	constexpr RenderMeshIdentity CasterMesh{101, 1};
	constexpr RenderTextureIdentity Mask{200, 1};
	constexpr ObjectId ReceiverObject{300, 1};
	constexpr ObjectId CasterObject{301, 1};
	std::filesystem::path EvidenceDirectory;
	std::ofstream EvidenceReceipt;
	std::string CurrentCase;
	std::uint64_t EvidenceSerial = 0;
	bool PointCostMode = false;

	void RecordResult(std::string_view Result, std::string_view Detail) {
		EvidenceReceipt << "result," << CurrentCase << "," << EvidenceSerial << ",0,,0,0,0,0,0,0," << Result;
		if (!Detail.empty()) EvidenceReceipt << ':' << Detail;
		EvidenceReceipt << '\n' << std::flush;
	}

	EditorViewportFrame Capture(EditorViewportRenderer &Renderer,
		const std::shared_ptr<RenderPublication> &Publication) {
		EditorViewportFrame Frame;
		try {
			Frame = Renderer.Capture(Publication);
		} catch (...) {
			EvidenceReceipt << "capture-failed," << CurrentCase << ',' << ++EvidenceSerial << ','
				<< Publication->Id << ",,0,0,0,0,0,0,renderer-capture\n" << std::flush;
			throw;
		}
		const auto Serial = ++EvidenceSerial;
		const auto Name = std::format("{}-{:04}-publication-{}.ppm", CurrentCase, Serial, Publication->Id);
		const auto Path = EvidenceDirectory / Name;
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		if (!Output) throw std::runtime_error("could not open shadow evidence image");
		Output << "P6\n" << Frame.Width << ' ' << Frame.Height << "\n255\n";
		Output.write(reinterpret_cast<const char *>(Frame.RgbPixels.data()),
			static_cast<std::streamsize>(Frame.RgbPixels.size()));
		Output.close();
		if (!Output) throw std::runtime_error("could not write shadow evidence image");
		std::uint64_t Hash = 14695981039346656037ull;
		for (const auto Byte : Frame.RgbPixels) Hash = (Hash ^ Byte) * 1099511628211ull;
		const auto Center = Frame.RgbPixels.at((std::size_t(Frame.Height / 2) * Frame.Width + Frame.Width / 2) * 3);
		EvidenceReceipt << "capture," << CurrentCase << ',' << Serial << ',' << Publication->Id << ',' << Name
			<< ',' << Frame.Width << ',' << Frame.Height << ',' << static_cast<unsigned>(Center)
			<< ',' << Hash << ",0,0,ok\n" << std::flush;
		return Frame;
	}

	void Draw(SDLRenderer &Renderer, const std::shared_ptr<RenderPublication> &Publication) {
		try {
			Renderer.Draw(Publication);
		} catch (...) {
			EvidenceReceipt << "draw-failed," << CurrentCase << ',' << ++EvidenceSerial << ','
				<< Publication->Id << ",,0,0,0,0," << Renderer.GetMetrics().ShadowMapPasses
				<< ',' << Renderer.GetMetrics().ShadowCasterDraws << ",renderer-draw\n" << std::flush;
			throw;
		}
		const auto Metrics = Renderer.GetMetrics();
		EvidenceReceipt << "draw," << CurrentCase << ',' << ++EvidenceSerial << ',' << Publication->Id
			<< ",,0,0,0,0," << Metrics.ShadowMapPasses << ',' << Metrics.ShadowCasterDraws
			<< ",cache_hits=" << Metrics.ShadowCacheHits << ";pose_checks=" << Metrics.ShadowPoseChecks
			<< ";pose_mismatches=" << Metrics.MainShadowPoseMismatches << '\n' << std::flush;
	}

	void Require(bool Condition, const char *Message) {
		if (!Condition) throw std::runtime_error(Message);
	}

	std::shared_ptr<const std::vector<RenderVertex>> Quad(float OffsetX = 0.0f) {
		return std::make_shared<const std::vector<RenderVertex>>(std::initializer_list<RenderVertex>{
			{{-1 + OffsetX, -1, 0}, {0, 0, 1}, {1, 0, 0, 1}, {0, 0}},
			{{ 1 + OffsetX, -1, 0}, {0, 0, 1}, {1, 0, 0, 1}, {1, 0}},
			{{ 1 + OffsetX,  1, 0}, {0, 0, 1}, {1, 0, 0, 1}, {1, 1}},
			{{-1 + OffsetX,  1, 0}, {0, 0, 1}, {1, 0, 0, 1}, {0, 1}},
		});
	}

	void SetCamera(RenderCameraSnapshot &Camera, glm::vec3 Position) {
		Camera.Position = Position;
		Camera.RightDirection = {1, 0, 0};
		Camera.UpDirection = {0, 1, 0};
		Camera.LookDirection = {0, 0, -1};
		Camera.NearPlane = 0.1f;
		Camera.FarPlane = 10.0f;
		Camera.VerticalFieldOfView = 45.0f;
		Camera.ViewMatrix = glm::lookAtRH(Position, Position + Camera.LookDirection, Camera.UpDirection);
		Camera.ProjectionMatrix = glm::perspectiveRH_ZO(glm::radians(Camera.VerticalFieldOfView),
			1.0f, Camera.NearPlane, Camera.FarPlane);
		Camera.ViewProjectionMatrix = Camera.ProjectionMatrix * Camera.ViewMatrix;
	}

	std::shared_ptr<RenderPublication> Scene(RenderPublicationId Id, bool Caster,
		bool Cascades = true, bool Spot = false, glm::vec3 Offset = {}, float ReceiverZ = 0.0f) {
		auto Value = std::make_shared<RenderPublication>();
		Value->Id = Id;
		Value->FullResync = true;
		Value->Frame.ViewportWidth = Size;
		Value->Frame.ViewportHeight = Size;
		SetCamera(Value->Frame.Camera, Offset + glm::vec3(0, 0, 3));
		auto &Light = Value->Frame.Environment;
		Light.AmbientColor = glm::vec3(0);
		Light.EnvironmentColor = glm::vec3(0);
		Light.SunDirection = glm::normalize(glm::vec3(0.5f, 0, 1));
		Light.SunIntensity = Spot ? 0.0f : 1.0f;
		Light.DirectionalShadows.CascadeCount = Cascades ? 4 : 0;
		Light.DirectionalShadows.Resolution = 512;
		Light.DirectionalShadows.Distance = 10.0f;
		Light.DirectionalShadows.SplitLambda = 0.6f;
		Light.DirectionalShadows.TransitionFraction = 0.1f;
		Light.IblIntensity = 0.0f;
		Value->EnvironmentChanged = true;
		if (Spot) {
			RenderLocalLightState Local;
			Local.Object = {400, 1};
			Local.Kind = RenderLocalLightKind::Spot;
			Local.Position = Offset + glm::vec3(0.5f, 0, ReceiverZ + 1.5f);
			Local.Direction = glm::normalize(glm::vec3(-0.5f, 0, -1.5f));
			Local.Color = glm::vec3(1);
			Local.Intensity = 8.0f;
			Local.Range = 6.0f;
			Local.InnerConeCosine = 0.95f;
			Local.OuterConeCosine = 0.8f;
			Local.ShadowEnabled = true;
			Light.LocalLights = std::make_shared<const std::vector<RenderLocalLightState>>(
				std::initializer_list<RenderLocalLightState>{Local});
		}
		const auto Indices = std::make_shared<const std::vector<std::uint32_t>>(
			std::initializer_list<std::uint32_t>{0, 1, 2, 0, 2, 3});
		Value->MeshCreates.push_back({.Mesh = ReceiverMesh, .TopologyRevision = 1, .VertexRevision = 1,
			.Vertices = Quad(), .Indices = Indices, .Bounds = {{-1, -1, 0}, {1, 1, 0}}});
		Value->MeshCreates.push_back({.Mesh = CasterMesh, .TopologyRevision = 1, .VertexRevision = 1,
			.Vertices = Quad(), .Indices = Indices, .Bounds = {{-1, -1, 0}, {1, 1, 0}}});
		RenderMaterialState Material;
		Material.DoubleSided = true;
		Material.ShadingModel = RenderShadingModel::MetallicRoughness;
		RenderItem Receiver;
		Receiver.Object = ReceiverObject;
		Receiver.CastShadow = false;
		Receiver.ModelMatrix = glm::translate(glm::mat4(1), Offset + glm::vec3(0, 0, ReceiverZ));
		Receiver.InverseModelMatrix = glm::inverse(Receiver.ModelMatrix);
		Value->Creates.push_back({.Item = Receiver, .Mesh = ReceiverMesh, .Material = Material});
		if (Caster) {
			RenderItem Part;
			Part.Object = CasterObject;
			Part.CastShadow = true;
			Part.ModelMatrix = glm::translate(glm::mat4(1), Offset +
				(Spot ? glm::vec3(0.25f, 0, ReceiverZ + 0.75f)
					: glm::vec3(0.5f, 0, ReceiverZ + 1.0f))) *
				glm::scale(glm::mat4(1), glm::vec3(0.16f));
			Part.InverseModelMatrix = glm::inverse(Part.ModelMatrix);
			auto CasterMaterial = Material;
			CasterMaterial.OpacityMode = RenderOpacityMode::Masked;
			CasterMaterial.BaseColorTexture = Mask;
			Value->Creates.push_back({.Item = Part, .Mesh = CasterMesh, .Material = CasterMaterial});
			Value->TextureCreates.push_back({Mask, 1, 1, 1, RenderTextureFormat::Rgba8Unorm,
				std::make_shared<const std::vector<std::uint8_t>>(
					std::initializer_list<std::uint8_t>{255, 255, 255, 255})});
		}
		return Value;
	}

	std::shared_ptr<RenderPublication> Delta(RenderPublicationId Id, RenderPublicationId Base,
		const RenderFrameState &Frame) {
		auto Value = std::make_shared<RenderPublication>();
		Value->Id = Id;
		Value->BaseId = Base;
		Value->Frame = Frame;
		return Value;
	}

	void AddGpuSkinnedCaster(RenderPublication &Publication) {
		auto &Mesh = Publication.MeshCreates.back();
		Mesh.Skeleton.Bytes[0] = 1;
		Mesh.SkeletonJointCount = 1;
		Mesh.SkinInfluences = std::make_shared<const std::vector<RenderSkinInfluence>>(
			Mesh.Vertices->size(), RenderSkinInfluence{{0, 0, 0, 0}, {1, 0, 0, 0}});
		RenderSkinPaletteEntry Entry;
		Publication.AnimationPoseUpdates.push_back({
			.Object = CasterObject, .SourceMesh = CasterMesh, .PoseRevision = 1,
			.Palette = {Mesh.Skeleton,
				std::make_shared<const std::vector<RenderSkinPaletteEntry>>(
					std::initializer_list<RenderSkinPaletteEntry>{Entry})},
		});
	}

	void SetNearPlane(RenderPublication &Publication, float NearPlane) {
		auto &Camera = Publication.Frame.Camera;
		Camera.NearPlane = NearPlane;
		Camera.ProjectionMatrix = glm::perspectiveRH_ZO(glm::radians(Camera.VerticalFieldOfView),
			1.0f, Camera.NearPlane, Camera.FarPlane);
		Camera.ViewProjectionMatrix = Camera.ProjectionMatrix * Camera.ViewMatrix;
	}

	std::shared_ptr<RenderPublication> PointCostScene(std::uint32_t Layers) {
		auto Publication = Scene(1, true, true, true);
		Publication->Frame.Environment.DirectionalShadows.Distance = 1.0f;
		SetNearPlane(*Publication, 2.0f); // Configured-empty cascades keep directional work out of this comparison.
		std::vector<RenderVertex> Vertices;
		for (const glm::vec3 Position : {glm::vec3(-1, -1, -1), glm::vec3(1, -1, -1),
			glm::vec3(1, 1, -1), glm::vec3(-1, 1, -1), glm::vec3(-1, -1, 1),
			glm::vec3(1, -1, 1), glm::vec3(1, 1, 1), glm::vec3(-1, 1, 1)})
			Vertices.push_back({.Position = Position});
		const std::vector<std::uint32_t> Indices{
			0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6,
			0, 5, 1, 0, 4, 5, 3, 2, 6, 3, 6, 7,
			0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2,
		};
		auto &Mesh = Publication->MeshCreates.back();
		Mesh.Vertices = std::make_shared<const std::vector<RenderVertex>>(std::move(Vertices));
		Mesh.Indices = std::make_shared<const std::vector<std::uint32_t>>(Indices);
		Mesh.Bounds = {{-1, -1, -1}, {1, 1, 1}};
		auto &Caster = Publication->Creates.back();
		Caster.Item.ModelMatrix = glm::mat4(1);
		Caster.Item.InverseModelMatrix = glm::mat4(1);
		std::vector<RenderLocalLightState> Lights;
		const std::array CubeFaces{glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0),
			glm::vec3(0, 1, 0), glm::vec3(0, -1, 0), glm::vec3(0, 0, 1), glm::vec3(0, 0, -1)};
		const std::array Octants{glm::vec3(1, 1, 1), glm::vec3(1, 1, -1),
			glm::vec3(1, -1, 1), glm::vec3(1, -1, -1), glm::vec3(-1, 1, 1),
			glm::vec3(-1, 1, -1), glm::vec3(-1, -1, 1), glm::vec3(-1, -1, -1)};
		for (std::uint32_t Index = 0; Index < Layers; ++Index) {
			auto Light = Publication->Frame.Environment.LocalLights->front();
			Light.Object = {400 + Index, 1};
			Light.Position = glm::vec3(0);
			Light.Direction = glm::normalize(Layers == 6 ? CubeFaces[Index] : Octants[Index]);
			Light.Intensity = 1.0f;
			Light.Range = 6.0f;
			Light.InnerConeCosine = 0.8f;
			Light.OuterConeCosine = 0.70710678f; // 90-degree face cone.
			Lights.push_back(Light);
		}
		Publication->Frame.Environment.LocalLights =
			std::make_shared<const std::vector<RenderLocalLightState>>(std::move(Lights));
		return Publication;
	}

	struct PointCostSample {
		std::uint64_t ShadowRecordNs = 0;
		std::uint64_t CpuSubmitNs = 0;
		std::uint64_t FenceWaitNs = 0;
		std::uint64_t WallNs = 0;
	};

	void CheckPointCostGate(const std::filesystem::path &Directory) {
		constexpr std::uint32_t WarmupFrames = 4;
		constexpr std::uint32_t MeasuredFrames = 24;
		struct Workload {
			std::uint32_t Layers;
			std::shared_ptr<RenderPublication> Initial;
			std::unique_ptr<SDLRenderer> Renderer;
			std::vector<PointCostSample> Samples;
		};
		std::array<Workload, 3> Workloads{{{1, PointCostScene(1)},
			{6, PointCostScene(6)}, {8, PointCostScene(8)}}};
		const SDLRendererOptions Options{.Offscreen = true, .WaitForGpuCompletion = true,
			.DebugDevice = false, .EnableHdrScene = false};
		std::ofstream Manifest(Directory / "point-shadow-cost-manifest.txt", std::ios::trunc);
		if (!Manifest) throw std::runtime_error("could not open point-shadow cost manifest");
		Manifest << "one/six/eight production spotlight depth layers, 1024x1024 D32, "
			"36-index cube, four warmups, 24 measured frames, one transform update per frame\n"
			<< std::flush;
		for (auto &Workload : Workloads) {
			Workload.Renderer = std::make_unique<SDLRenderer>(Vector2(Size, Size), Options);
			Workload.Renderer->Draw(Workload.Initial);
			Manifest << Workload.Layers << " layers: " << Workload.Renderer->GetDriverName()
				<< "\n" << std::flush;
		}
		std::ofstream Csv(Directory / "point-shadow-cost.csv", std::ios::trunc);
		if (!Csv) throw std::runtime_error("could not open point-shadow cost receipt");
		Csv << "mode,iteration,publication_id,layers,shadow_passes,shadow_caster_draws,shadow_cache_hits,"
			"upload_operations,uploaded_bytes,"
			"local_light_upload_bytes,shadow_record_cpu_ns,cpu_submission_ns,fence_wait_ns,wall_ns\n" << std::flush;
		for (std::uint32_t Iteration = 0; Iteration < WarmupFrames + MeasuredFrames; ++Iteration) {
			const std::array Order = Iteration % 2 == 0 ? std::array{0u, 1u, 2u}
				: std::array{2u, 1u, 0u};
			for (const auto Index : Order) {
				auto &Workload = Workloads[Index];
				const auto PublicationId = static_cast<RenderPublicationId>(Iteration + 2);
				auto Publication = Delta(PublicationId, PublicationId - 1, Workload.Initial->Frame);
				auto Caster = Workload.Initial->Creates.back();
				const float Offset = Iteration % 2 == 0 ? 0.02f : -0.02f;
				Caster.Item.ModelMatrix = glm::translate(glm::mat4(1), glm::vec3(Offset, 0, 0));
				Caster.Item.InverseModelMatrix = glm::inverse(Caster.Item.ModelMatrix);
				Publication->Updates.push_back({.Object = CasterObject, .Domains = RenderUpdateDomain::Transform,
					.Item = Caster.Item, .Mesh = Caster.Mesh, .Material = Caster.Material});
				const auto Before = Workload.Renderer->GetMetrics();
				const auto Started = std::chrono::steady_clock::now();
				Workload.Renderer->Draw(Publication);
				const auto WallNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - Started).count());
				const auto After = Workload.Renderer->GetMetrics();
				const auto Passes = After.ShadowMapPasses - Before.ShadowMapPasses;
				const auto Draws = After.ShadowCasterDraws - Before.ShadowCasterDraws;
				Require(After.FramesSubmitted == Before.FramesSubmitted + 1 &&
					Passes == Workload.Layers && Draws == Workload.Layers,
					"spotlight and cube-face workloads must issue exactly one cube depth draw per active layer");
				const auto Mode = Workload.Layers == 1 ? "one-spot" :
					Workload.Layers == 6 ? "six-face-equivalent" : "eight-spot-atlas";
				const auto Phase = Iteration < WarmupFrames ? "warmup" : "measured";
				Csv << Mode << '-' << Phase << ',' << Iteration << ',' << PublicationId << ','
					<< Workload.Layers << ',' << Passes << ',' << Draws << ','
					<< After.ShadowCacheHits - Before.ShadowCacheHits << ','
					<< After.UploadOperations - Before.UploadOperations << ','
					<< After.UploadedBytes - Before.UploadedBytes << ','
					<< After.LocalLightUploadBytes - Before.LocalLightUploadBytes << ','
					<< After.LastShadowRecordingNanoseconds << ',' << After.LastSubmissionNanoseconds << ','
					<< After.LastGpuCompletionWaitNanoseconds << ',' << WallNs << '\n' << std::flush;
				if (Iteration >= WarmupFrames) Workload.Samples.push_back({
					After.LastShadowRecordingNanoseconds, After.LastSubmissionNanoseconds,
					After.LastGpuCompletionWaitNanoseconds, WallNs});
			}
		}
		const auto Median = [](const std::vector<PointCostSample> &Samples,
			std::uint64_t PointCostSample::*Field) {
			std::vector<std::uint64_t> Values;
			for (const auto &Sample : Samples) Values.push_back(Sample.*Field);
			std::ranges::sort(Values);
			return Values[Values.size() / 2];
		};
		std::ofstream Summary(Directory / "point-shadow-cost-summary.txt", std::ios::trunc);
		if (!Summary) throw std::runtime_error("could not open point-shadow cost summary");
		Summary << "D32 1024x1024 per layer, one submitted 36-index double-sided cube draw per layer.\n"
			"Six orthogonal 90-degree spot cones approximate cube faces; no production point-shadow path is used.\n"
			"All workloads allocate the production eight-layer atlas (32 MiB logical); active layers touch 4, 24 or 32 MiB.\n"
			"CPU recording and submission are host clocks. Fence wait includes the entire rendered frame, not a GPU timestamp.\n";
		for (const auto &Workload : Workloads)
			Summary << (Workload.Layers == 1 ? "one-spot" :
				Workload.Layers == 6 ? "six-face-equivalent" : "eight-spot-atlas")
				<< " median_shadow_record_cpu_ns=" << Median(Workload.Samples, &PointCostSample::ShadowRecordNs)
				<< " median_cpu_submission_ns=" << Median(Workload.Samples, &PointCostSample::CpuSubmitNs)
				<< " median_fence_wait_ns=" << Median(Workload.Samples, &PointCostSample::FenceWaitNs)
				<< " median_wall_ns=" << Median(Workload.Samples, &PointCostSample::WallNs) << '\n';
		Summary << std::flush;
	}

	int RedCenter(const std::vector<std::uint8_t> &Pixels) {
		return Pixels.at((std::size_t(Size / 2) * Size + Size / 2) * 3);
	}

	int RedCenter(const EditorViewportFrame &Frame) {
		return RedCenter(Frame.RgbPixels);
	}

	void CheckDirectionalPixels() {
		CurrentCase = "directional-pixels";
		EditorViewportRenderer Renderer(Size, Size);
		const auto Lit = Capture(Renderer, Scene(1, false)).RgbPixels;
		auto ShadowScene = Scene(2, true);
		const auto Shadow = Capture(Renderer, ShadowScene).RgbPixels;
		Require(RedCenter(Lit) > RedCenter(Shadow) + 25,
			"camera-relative directional caster did not darken the receiver");
		Require(Capture(Renderer, Delta(3, 2, ShadowScene->Frame)).RgbPixels == Shadow,
			"static cascaded depth changed exact pixels on a warm publication");
		RenderPublicationId Base = 3;
		for (const float X : {0.005f, 0.02f, 0.06f, 0.12f}) {
			auto Moved = Delta(Base + 1, Base, ShadowScene->Frame);
			SetCamera(Moved->Frame.Camera, {X, 0, 3});
			const auto Cached = Capture(Renderer, Moved).RgbPixels;
			auto Rebuilt = Scene(Base + 2, true);
			Rebuilt->Frame = Moved->Frame;
			Require(Capture(Renderer, Rebuilt).RgbPixels == Cached,
				"camera motion differs between reused and freshly built cascades");
			Base += 2;
		}
		const auto Settings = SDLDirectionalCascadeSettings{.Count = 4, .Resolution = 512,
			.MaximumDistance = 10.0f, .SplitLambda = 0.6f, .TransitionFraction = 0.1f};
		const auto Plan = MakeSDLDirectionalCascadePlan(ShadowScene->Frame.Camera,
			ShadowScene->Frame.Environment.SunDirection, 1.0f, Settings);
		Require(Plan.has_value(), "split-boundary GPU fixture did not build a numerical plan");
		const float Boundary = Plan->Cascades[1].Far;
		for (const float Distance : {Boundary - 0.03f, Boundary + 0.03f}) {
			auto BoundaryScene = Scene(++Base, true, true, false, {}, 3.0f - Distance);
			const auto WithShadow = Capture(Renderer, BoundaryScene).RgbPixels;
			auto NoCaster = Scene(++Base, false, true, false, {}, 3.0f - Distance);
			const auto Control = Capture(Renderer, NoCaster).RgbPixels;
			Require(RedCenter(Control) > RedCenter(WithShadow) + 12,
				"cascade transition lost a shadowed receiver");
		}
		auto Distant = Scene(++Base, true, true, false, {900000, 2000, -900000});
		const auto DistantShadow = Capture(Renderer, Distant).RgbPixels;
		Require(std::abs(RedCenter(DistantShadow) - RedCenter(Shadow)) < 24,
			"camera-relative cascade changed distant-world receiver lighting");
		auto DistantWarm = Delta(++Base, Distant->Id, Distant->Frame);
		Require(Capture(Renderer, DistantWarm).RgbPixels == DistantShadow,
			"large-coordinate warm cascade changed exact pixels");
	}

	std::shared_ptr<RenderPublication> OutdoorResolutionScene(RenderPublicationId Id, bool Caster,
		std::uint32_t Resolution) {
		// Preserve the near fixture's screen-space layout at view depth 200:
		// the receiver remains under the center pixel while the caster is off
		// the center camera ray and lies on its sun ray.
		constexpr float Scale = 200.0f / 3.0f;
		constexpr float ReceiverZ = 3.0f - 200.0f;
		auto Publication = Scene(Id, Caster, true, false, {}, ReceiverZ);
		auto &Camera = Publication->Frame.Camera;
		Camera.FarPlane = 512.0f;
		Camera.ProjectionMatrix = glm::perspectiveRH_ZO(glm::radians(Camera.VerticalFieldOfView),
			1.0f, Camera.NearPlane, Camera.FarPlane);
		Camera.ViewProjectionMatrix = Camera.ProjectionMatrix * Camera.ViewMatrix;
		auto &Settings = Publication->Frame.Environment.DirectionalShadows;
		Settings = RenderDirectionalShadowSettings{};
		Settings.CascadeCount = 4;
		if (Resolution != Settings.Resolution) Settings.Resolution = Resolution;
		auto &Receiver = Publication->Creates.front().Item;
		Receiver.ModelMatrix = glm::translate(glm::mat4(1), glm::vec3(0, 0, ReceiverZ)) *
			glm::scale(glm::mat4(1), glm::vec3(Scale));
		Receiver.InverseModelMatrix = glm::inverse(Receiver.ModelMatrix);
		if (Caster) {
			auto &Part = Publication->Creates.back().Item;
			Part.ModelMatrix = glm::translate(glm::mat4(1),
				glm::vec3(0.5f * Scale, 0, ReceiverZ + Scale)) *
				glm::scale(glm::mat4(1), glm::vec3(0.16f * Scale));
			Part.InverseModelMatrix = glm::inverse(Part.ModelMatrix);
		}
		return Publication;
	}

	void CheckOutdoorResolutionCase(std::uint32_t Resolution) {
		CurrentCase = std::format("outdoor-resolution-{}", Resolution);
		const RenderDirectionalShadowSettings Defaults{};
		Require(Defaults.Resolution == 2048 && Defaults.Distance == 250.0f,
			"outdoor fixture no longer exercises the default shadow extent");
		auto LitScene = OutdoorResolutionScene(1, false, Resolution);
		const auto Plan = MakeSDLDirectionalCascadePlan(LitScene->Frame.Camera,
			LitScene->Frame.Environment.SunDirection, 1.0f, {
				.Count = 4, .Resolution = Resolution, .MaximumDistance = Defaults.Distance,
				.SplitLambda = Defaults.SplitLambda, .TransitionFraction = Defaults.TransitionFraction});
		Require(Plan && Plan->Count == 4 && Plan->Cascades[3].Far >= 200.0f,
			"outdoor receiver is outside the four-cascade plan");
		std::vector<std::uint8_t> Shadow;
		{
			EditorViewportRenderer Renderer(Size, Size);
			const auto Lit = Capture(Renderer, LitScene).RgbPixels;
			auto ShadowScene = OutdoorResolutionScene(2, true, Resolution);
			const auto &Camera = ShadowScene->Frame.Camera;
			const auto &Receiver = ShadowScene->Creates.front().Item;
			const auto &Caster = ShadowScene->Creates.back().Item;
			const glm::vec4 ReceiverView = Camera.ViewMatrix * Receiver.ModelMatrix * glm::vec4(0, 0, 0, 1);
			const glm::vec4 CasterLeftClip = Camera.ProjectionMatrix * Camera.ViewMatrix *
				Caster.ModelMatrix * glm::vec4(-1, 0, 0, 1);
			const glm::vec3 ShadowRay = glm::normalize(glm::vec3(Caster.ModelMatrix[3] -
				Receiver.ModelMatrix[3]));
			Require(std::abs(ReceiverView.z + 200.0f) < 0.02f && CasterLeftClip.w > 0.0f &&
				CasterLeftClip.x > 0.3f * CasterLeftClip.w &&
				glm::dot(ShadowRay, ShadowScene->Frame.Environment.SunDirection) > 0.999f,
				"outdoor fixture no longer isolates a depth-200 receiver from direct caster occlusion");
			const auto Before = EditorViewportRendererTestAccess::GetMetrics(Renderer);
			Shadow = Capture(Renderer, ShadowScene).RgbPixels;
			const auto Cold = EditorViewportRendererTestAccess::GetMetrics(Renderer);
			Require(RedCenter(Lit) > 100 && RedCenter(Lit) > RedCenter(Shadow) + 25,
				"outdoor receiver was unlit or its distant caster failed to shadow the center");
			Require(Cold.ShadowMapPasses == Before.ShadowMapPasses + 4 &&
				Cold.ShadowCasterDraws > Before.ShadowCasterDraws,
				"outdoor scene did not build four depth layers with its caster");
			const auto Warm = Capture(Renderer, Delta(3, 2, ShadowScene->Frame)).RgbPixels;
			const auto WarmMetrics = EditorViewportRendererTestAccess::GetMetrics(Renderer);
			Require(Warm == Shadow && WarmMetrics.ShadowMapPasses == Cold.ShadowMapPasses &&
				WarmMetrics.ShadowCacheHits >= Cold.ShadowCacheHits + 4,
				"warm outdoor cascades changed pixels or rebuilt static layers");
		}
		EditorViewportRenderer FreshRenderer(Size, Size);
		const auto Fresh = Capture(FreshRenderer, OutdoorResolutionScene(1, true, Resolution)).RgbPixels;
		Require(Fresh == Shadow, "fresh and warm outdoor shadow pixels differ");
	}

	std::shared_ptr<RenderPublication> ZenithScene(RenderPublicationId Id, float Hour, bool Caster = true) {
		auto Publication = Scene(Id, Caster);
		const float Angle = (Hour - 6.0f) * (2.0f * std::numbers::pi_v<float> / 24.0f);
		Publication->Frame.Environment.SunDirection =
			glm::normalize(glm::vec3(std::cos(Angle), std::sin(Angle), 0.0f));
		const auto Ground = std::make_shared<const std::vector<RenderVertex>>(
			std::initializer_list<RenderVertex>{
				{{-1, 0, -1}, {0, 1, 0}, {1, 0, 0, 1}, {0, 0}},
				{{-1, 0,  1}, {0, 1, 0}, {1, 0, 0, 1}, {0, 1}},
				{{ 1, 0,  1}, {0, 1, 0}, {1, 0, 0, 1}, {1, 1}},
				{{ 1, 0, -1}, {0, 1, 0}, {1, 0, 0, 1}, {1, 0}},
			});
		for (auto &Mesh : Publication->MeshCreates) {
			Mesh.Vertices = Ground;
			Mesh.Bounds = {{-1, 0, -1}, {1, 0, 1}};
		}
		Publication->Creates.front().Item.ModelMatrix = glm::mat4(1);
		Publication->Creates.front().Item.InverseModelMatrix = glm::mat4(1);
		if (Caster) {
			auto &Item = Publication->Creates.back().Item;
			Item.ModelMatrix = glm::translate(glm::mat4(1), glm::vec3(0, 0.75f, 0)) *
				glm::scale(glm::mat4(1), glm::vec3(0.3f));
			Item.InverseModelMatrix = glm::inverse(Item.ModelMatrix);
		}
		auto &Camera = Publication->Frame.Camera;
		Camera.Position = {0, 2, 3};
		Camera.LookDirection = glm::normalize(-Camera.Position);
		Camera.RightDirection = glm::normalize(glm::cross(Camera.LookDirection, glm::vec3(0, 1, 0)));
		Camera.UpDirection = glm::normalize(glm::cross(Camera.RightDirection, Camera.LookDirection));
		Camera.ViewMatrix = glm::lookAtRH(Camera.Position, glm::vec3(0), Camera.UpDirection);
		Camera.ViewProjectionMatrix = Camera.ProjectionMatrix * Camera.ViewMatrix;
		return Publication;
	}

	void CheckSunZenithPixels() {
		CurrentCase = "sun-zenith-pixels";
		EditorViewportRenderer WarmRenderer(Size, Size);
		EditorViewportRenderer FreshRenderer(Size, Size);
		std::vector<int> Centers;
		for (const auto [Index, Hour] : {std::pair{1u, 11.99f}, std::pair{2u, 12.0f},
			std::pair{3u, 12.01f}}) {
			auto Fresh = ZenithScene(Index, Hour);
			std::shared_ptr<RenderPublication> Warm = Fresh;
			if (Index > 1) {
				Warm = Delta(Index, Index - 1, Fresh->Frame);
				Warm->EnvironmentChanged = true;
			}
			const auto CachedPixels = Capture(WarmRenderer, Warm).RgbPixels;
			Require(CachedPixels == Capture(FreshRenderer, Fresh).RgbPixels,
				"noon sun motion differs between warm and freshly built cascades");
			Centers.push_back(RedCenter(CachedPixels));
		}
		const auto Control = Capture(FreshRenderer, ZenithScene(4, 12.0f, false));
		Require(RedCenter(Control) > Centers[1] + 15,
			"noon pixel control did not show the overhead caster shadow");
		Require(std::abs(Centers[0] - Centers[1]) <= 12 &&
			std::abs(Centers[1] - Centers[2]) <= 12,
			"canonical noon motion caused a discontinuous shadow pixel");
	}

	void CheckMaskAndDeformable() {
		CurrentCase = "mask-deformable";
		EditorViewportRenderer Renderer(Size, Size);
		auto Initial = Scene(1, true);
		const auto Dark = Capture(Renderer, Initial).RgbPixels;
		auto Cutout = Delta(2, 1, Initial->Frame);
		Cutout->TextureUpdates.push_back({Mask, 2, 0, 0, 1, 1,
			std::make_shared<const std::vector<std::uint8_t>>(
				std::initializer_list<std::uint8_t>{255, 255, 255, 0})});
		const auto Bright = Capture(Renderer, Cutout).RgbPixels;
		Require(RedCenter(Bright) > RedCenter(Dark) + 25,
			"alpha reimport left stale cascade depth");
		auto Restore = Delta(3, 2, Initial->Frame);
		Restore->TextureUpdates.push_back({Mask, 3, 0, 0, 1, 1,
			std::make_shared<const std::vector<std::uint8_t>>(
				std::initializer_list<std::uint8_t>{255, 255, 255, 255})});
		Require(Capture(Renderer, Restore).RgbPixels == Dark,
			"restored mask alpha did not restore exact cascade pixels");
		auto Moved = Delta(4, 3, Initial->Frame);
		Moved->MeshVertexUpdates.push_back({.Mesh = CasterMesh, .VertexRevision = 2,
			.FirstVertex = 0, .Vertices = Quad(10.0f), .Bounds = {{9, -1, 0}, {11, 1, 0}}});
		Require(RedCenter(Capture(Renderer, Moved)) > RedCenter(Dark) + 25,
			"deformable caster vertex update left stale cascade depth");
	}

	void CheckCascadePasses() {
		CurrentCase = "cascade-passes";
		SDLRenderer Renderer(Vector2(Size, Size), {
			.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false,
		});
		auto Initial = Scene(1, true);
		Draw(Renderer, Initial);
		const auto Cold = Renderer.GetMetrics();
		Require(Cold.ShadowMapPasses == 4 && Cold.ShadowCasterDraws > 0,
			"four cascades did not record separate depth passes");
		Draw(Renderer, Delta(2, 1, Initial->Frame));
		const auto Warm = Renderer.GetMetrics();
		Require(Warm.ShadowMapPasses == Cold.ShadowMapPasses &&
			Warm.ShadowCasterDraws == Cold.ShadowCasterDraws &&
			Warm.ShadowCacheHits >= Cold.ShadowCacheHits + 4,
			"static cascades did not reuse all four depth layers");
		auto Alpha = Delta(3, 2, Initial->Frame);
		Alpha->TextureUpdates.push_back({Mask, 2, 0, 0, 1, 1,
			std::make_shared<const std::vector<std::uint8_t>>(
				std::initializer_list<std::uint8_t>{255, 255, 255, 0})});
		Draw(Renderer, Alpha);
		const auto AlphaMetrics = Renderer.GetMetrics();
		Require(AlphaMetrics.ShadowMapPasses > Warm.ShadowMapPasses &&
			AlphaMetrics.ShadowMapPasses <= Warm.ShadowMapPasses + 4,
			"mask alpha revision did not invalidate bounded cascade layers");
		auto Moved = Delta(4, 3, Initial->Frame);
		auto Caster = Initial->Creates.back();
		Caster.Item.ModelMatrix = glm::translate(glm::mat4(1), glm::vec3(2, 0, 1)) *
			glm::scale(glm::mat4(1), glm::vec3(0.16f));
		Caster.Item.InverseModelMatrix = glm::inverse(Caster.Item.ModelMatrix);
		Moved->Updates.push_back({.Object = CasterObject, .Domains = RenderUpdateDomain::Transform,
			.Item = Caster.Item, .Mesh = Caster.Mesh, .Material = Caster.Material});
		Draw(Renderer, Moved);
		const auto MoveMetrics = Renderer.GetMetrics();
		Require(MoveMetrics.ShadowMapPasses > AlphaMetrics.ShadowMapPasses &&
			MoveMetrics.ShadowMapPasses <= AlphaMetrics.ShadowMapPasses + 4,
			"caster motion did not invalidate bounded cascade layers");
		std::cout << "[Render:ShadowGpu] cascade passes cold=" << Cold.ShadowMapPasses
			<< " warm=" << Warm.ShadowMapPasses << " alpha=" << AlphaMetrics.ShadowMapPasses
			<< " motion=" << MoveMetrics.ShadowMapPasses << '\n';
	}

	void CheckSpotPixels() {
		CurrentCase = "spot-pixels";
		EditorViewportRenderer Renderer(Size, Size);
		auto ShadowScene = Scene(1, true, false, true);
		const auto Shadow = Capture(Renderer, ShadowScene).RgbPixels;
		auto Unshadowed = Scene(2, true, false, true);
		auto Lights = std::make_shared<std::vector<RenderLocalLightState>>(
			*Unshadowed->Frame.Environment.LocalLights);
		Lights->front().ShadowEnabled = false;
		Unshadowed->Frame.Environment.LocalLights = Lights;
		const auto Control = Capture(Renderer, Unshadowed).RgbPixels;
		Require(RedCenter(Control) > RedCenter(Shadow) + 20,
			"spot shadow toggle did not change the receiver pixel");
		auto RestoredScene = Scene(3, true, false, true);
		const auto Restored = Capture(Renderer, RestoredScene).RgbPixels;
		Require(Restored == Shadow, "spot shadow toggle did not restore exact pixels");
		Require(Capture(Renderer, Delta(4, 3, RestoredScene->Frame)).RgbPixels == Shadow,
			"static spotlight depth changed exact warm pixels");
		auto Distant = Scene(5, true, false, true, {900000, 2000, -900000});
		const auto DistantShadow = Capture(Renderer, Distant).RgbPixels;
		Require(std::abs(RedCenter(DistantShadow) - RedCenter(Shadow)) < 24,
			"large-coordinate spotlight altered receiver shadowing");
		Require(Capture(Renderer, Delta(6, 5, Distant->Frame)).RgbPixels == DistantShadow,
			"large-coordinate spotlight map changed exact warm pixels");
	}

	void CheckCachedSkinPoseReceipts() {
		CurrentCase = "cached-skin-pose";
		for (const bool Spot : {false, true}) {
			SDLRenderer Renderer(Vector2(Size, Size), {
				.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false,
			});
			auto Initial = Scene(1, true, !Spot, Spot);
			AddGpuSkinnedCaster(*Initial);
			Draw(Renderer, Initial);
			const auto Cold = Renderer.GetMetrics();
			Require(Cold.ShadowPoseChecks > 0 && Cold.MainShadowPoseMismatches == 0,
				"first directional or local map did not compare a submitted skin pose");
			Draw(Renderer, Delta(2, 1, Initial->Frame));
			const auto Warm = Renderer.GetMetrics();
			Require(Warm.ShadowMapPasses == Cold.ShadowMapPasses &&
				Warm.ShadowPoseChecks > Cold.ShadowPoseChecks && Warm.MainShadowPoseMismatches == 0,
				"cached directional or local map lost its exact skin pose receipt");
		}
	}

	void CheckLegacyStaticCompatibility() {
		CurrentCase = "legacy-static-compatibility";
		EditorViewportRenderer Viewport(Size, Size);
		const auto Lit = Capture(Viewport, Scene(1, false, false)).RgbPixels;
		auto Initial = Scene(2, true, false);
		const auto Shadow = Capture(Viewport, Initial).RgbPixels;
		Require(RedCenter(Lit) > RedCenter(Shadow) + 25,
			"legacy static caster lost its receiver shadow without a skin pose ledger");
		Require(Capture(Viewport, Delta(3, 2, Initial->Frame)).RgbPixels == Shadow,
			"legacy static shadow changed exact warm pixels");
		SDLRenderer Renderer(Vector2(Size, Size), {
			.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false,
		});
		auto PassScene = Scene(1, true, false);
		Draw(Renderer, PassScene);
		const auto Cold = Renderer.GetMetrics();
		Require(Cold.ShadowMapPasses == 1 && Cold.ShadowPoseChecks == 0,
			"legacy static depth did not record one map without GPU skin checks");
		Draw(Renderer, Delta(2, 1, PassScene->Frame));
		const auto Warm = Renderer.GetMetrics();
		Require(Warm.ShadowMapPasses == Cold.ShadowMapPasses && Warm.ShadowCacheHits > Cold.ShadowCacheHits,
			"legacy static depth did not reuse its submitted cache");
	}

	void CheckEmptyDirectionalCoverage() {
		CurrentCase = "empty-directional-coverage";
		SDLRenderer Renderer(Vector2(Size, Size), {
			.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false,
		});
		auto Initial = Scene(1, true);
		Initial->Frame.Environment.DirectionalShadows.Distance = 1.0f;
		SetNearPlane(*Initial, 2.0f);
		AddGpuSkinnedCaster(*Initial);
		Draw(Renderer, Initial);
		const auto Empty = Renderer.GetMetrics();
		Require(Empty.ShadowMapPasses == 0 && Empty.ShadowPoseChecks == 0 &&
			Empty.MainShadowPoseMismatches == 0,
			"shadow distance below camera near plane recorded legacy directional depth");
		auto Local = Scene(1, true, true, true);
		Local->Frame.Environment.SunIntensity = 1.0f;
		Local->Frame.Environment.DirectionalShadows.Distance = 1.0f;
		SetNearPlane(*Local, 2.0f);
		SDLRenderer LocalRenderer(Vector2(Size, Size), {
			.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false,
		});
		Draw(LocalRenderer, Local);
		Require(LocalRenderer.GetMetrics().ShadowMapPasses == 1,
			"empty directional coverage suppressed a valid local spotlight depth pass");
	}

	void CheckCanceledShadowCandidate() {
		CurrentCase = "canceled-shadow-candidate";
		EditorViewportRenderer Renderer(Size, Size);
		auto Canceled = Scene(1, true);
		EditorViewportRendererTestAccess::InjectPostShadowFailure(Renderer);
		bool AbortedAfterShadow = false;
		try { (void)Capture(Renderer, Canceled); }
		catch (const std::runtime_error &Error) {
			AbortedAfterShadow = std::string_view(Error.what()).find("injected post-shadow failure") !=
				std::string_view::npos;
		}
		Require(AbortedAfterShadow, "GPU fixture did not cancel a recorded cascade command buffer");
		const auto Attempted = EditorViewportRendererTestAccess::GetMetrics(Renderer);
		Require(Attempted.ShadowMapPasses == 4,
			"canceled frame did not attempt all four cascade depth layers");
		auto Empty = Delta(2, 1, Canceled->Frame);
		Empty->Frame.Environment.DirectionalShadows.Distance = 1.0f;
		SetNearPlane(*Empty, 2.0f);
		Empty->EnvironmentChanged = true;
		(void)Capture(Renderer, Empty);
		const auto AfterEmpty = EditorViewportRendererTestAccess::GetMetrics(Renderer);
		Require(AfterEmpty.ShadowMapPasses == Attempted.ShadowMapPasses,
			"configured-empty frame recorded unexpected legacy or canceled cascade depth");
		auto Resumed = Delta(3, 2, Canceled->Frame);
		Resumed->EnvironmentChanged = true;
		const auto ResumedPixels = Capture(Renderer, Resumed).RgbPixels;
		const auto AfterResume = EditorViewportRendererTestAccess::GetMetrics(Renderer);
		Require(AfterResume.ShadowMapPasses == Attempted.ShadowMapPasses + 4,
			"resumed frame reused canceled depth instead of rebuilding all cascades");
		EditorViewportRenderer Fresh(Size, Size);
		auto FreshScene = Scene(4, true);
		FreshScene->Frame = Resumed->Frame;
		const auto FreshPixels = Capture(Fresh, FreshScene).RgbPixels;
		Require(ResumedPixels == FreshPixels,
			"resumed cascade pixels differ from a newly submitted reference map");
	}

	void CheckIncomingDynamicCasters() {
		CurrentCase = "incoming-dynamic-casters";
		const auto MoveOutside = [](RenderPublication &Publication) {
			auto &Item = Publication.Creates.back().Item;
			Item.ModelMatrix = glm::translate(glm::mat4(1), glm::vec3(10, 0, 1)) *
				glm::scale(glm::mat4(1), glm::vec3(0.16f));
			Item.InverseModelMatrix = glm::inverse(Item.ModelMatrix);
		};
		{
			EditorViewportRenderer Renderer(Size, Size);
			auto Initial = Scene(1, true);
			MoveOutside(*Initial);
			const auto Bright = Capture(Renderer, Initial).RgbPixels;
			auto Moved = Delta(2, 1, Initial->Frame);
			const auto Vertices = Quad(-59.375f);
			Moved->MeshVertexUpdates.push_back({.Mesh = CasterMesh, .VertexRevision = 2,
				.FirstVertex = 0, .Vertices = Vertices,
				.Bounds = {{-60.375f, -1, 0}, {-58.375f, 1, 0}}});
			const auto Dark = Capture(Renderer, Moved).RgbPixels;
			Require(RedCenter(Bright) > RedCenter(Dark) + 25,
				"outside deformable caster entered without invalidating empty cascade depth");
		}
		{
			EditorViewportRenderer Renderer(Size, Size);
			auto Initial = Scene(1, true);
			MoveOutside(*Initial);
			auto &Mesh = Initial->MeshCreates.back();
			Mesh.Skeleton.Bytes[0] = 1;
			Mesh.SkeletonJointCount = 1;
			Mesh.SkinInfluences = std::make_shared<const std::vector<RenderSkinInfluence>>(
				Mesh.Vertices->size(), RenderSkinInfluence{{0, 0, 0, 0}, {1, 0, 0, 0}});
			const auto Bright = Capture(Renderer, Initial).RgbPixels;
			auto Posed = Delta(2, 1, Initial->Frame);
			RenderSkinPaletteEntry Entry;
			Entry.PositionMatrix = glm::translate(glm::mat4(1), glm::vec3(-59.375f, 0, 0));
			Posed->AnimationPoseUpdates.push_back({
				.Object = CasterObject, .SourceMesh = CasterMesh, .PoseRevision = 1,
				.Palette = {Mesh.Skeleton,
					std::make_shared<const std::vector<RenderSkinPaletteEntry>>(
						std::initializer_list<RenderSkinPaletteEntry>{Entry})},
			});
			const auto Dark = Capture(Renderer, Posed).RgbPixels;
			Require(RedCenter(Bright) > RedCenter(Dark) + 25,
				"newly posed outside caster entered without invalidating empty cascade depth");
		}
		{
			EditorViewportRenderer Renderer(Size, Size);
			auto Initial = Scene(1, true, false, true);
			auto &Item = Initial->Creates.back().Item;
			Item.ModelMatrix = glm::translate(glm::mat4(1), glm::vec3(10, 0, 0.75f)) *
				glm::scale(glm::mat4(1), glm::vec3(0.16f));
			Item.InverseModelMatrix = glm::inverse(Item.ModelMatrix);
			const auto Bright = Capture(Renderer, Initial).RgbPixels;
			auto Moved = Delta(2, 1, Initial->Frame);
			Moved->MeshVertexUpdates.push_back({.Mesh = CasterMesh, .VertexRevision = 2,
				.FirstVertex = 0, .Vertices = Quad(-60.9375f),
				.Bounds = {{-61.9375f, -1, 0}, {-59.9375f, 1, 0}}});
			const auto Dark = Capture(Renderer, Moved).RgbPixels;
			Require(RedCenter(Bright) > RedCenter(Dark) + 20,
				"outside deformable caster entered without invalidating empty spot depth");
		}
		{
			EditorViewportRenderer Renderer(Size, Size);
			auto Initial = Scene(1, true, false, true);
			auto &Item = Initial->Creates.back().Item;
			Item.ModelMatrix = glm::translate(glm::mat4(1), glm::vec3(10, 0, 0.75f)) *
				glm::scale(glm::mat4(1), glm::vec3(0.16f));
			Item.InverseModelMatrix = glm::inverse(Item.ModelMatrix);
			auto &Mesh = Initial->MeshCreates.back();
			Mesh.Skeleton.Bytes[0] = 1;
			Mesh.SkeletonJointCount = 1;
			Mesh.SkinInfluences = std::make_shared<const std::vector<RenderSkinInfluence>>(
				Mesh.Vertices->size(), RenderSkinInfluence{{0, 0, 0, 0}, {1, 0, 0, 0}});
			const auto Bright = Capture(Renderer, Initial).RgbPixels;
			auto Posed = Delta(2, 1, Initial->Frame);
			RenderSkinPaletteEntry Entry;
			Entry.PositionMatrix = glm::translate(glm::mat4(1), glm::vec3(-60.9375f, 0, 0));
			Posed->AnimationPoseUpdates.push_back({
				.Object = CasterObject, .SourceMesh = CasterMesh, .PoseRevision = 1,
				.Palette = {Mesh.Skeleton,
					std::make_shared<const std::vector<RenderSkinPaletteEntry>>(
						std::initializer_list<RenderSkinPaletteEntry>{Entry})},
			});
			const auto Dark = Capture(Renderer, Posed).RgbPixels;
			Require(RedCenter(Bright) > RedCenter(Dark) + 20,
				"newly posed outside caster entered without invalidating empty spot depth");
		}
	}

	void CheckAtlasPasses() {
		CurrentCase = "atlas-passes";
		SDLRenderer Renderer(Vector2(Size, Size), {
			.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false,
		});
		auto Initial = Scene(1, true, false, true);
		auto Lights = std::make_shared<std::vector<RenderLocalLightState>>();
		for (std::uint32_t Index = 0; Index < 9; ++Index) {
			auto Light = Initial->Frame.Environment.LocalLights->front();
			Light.Object = {400 + Index, 1};
			Light.ShadowPriority = Index < 8 ? 10 : 0;
			Lights->push_back(Light);
		}
		Initial->Frame.Environment.LocalLights = Lights;
		Draw(Renderer, Initial);
		const auto Cold = Renderer.GetMetrics();
		Require(Cold.ShadowMapPasses >= 8 && Cold.ShadowMapPasses <= 9 && Cold.ShadowCasterDraws >= 8,
			"nine overlapping spots did not produce eight bounded depth layers");
		Draw(Renderer, Delta(2, 1, Initial->Frame));
		const auto Warm = Renderer.GetMetrics();
		Require(Warm.ShadowMapPasses == Cold.ShadowMapPasses &&
			Warm.ShadowCasterDraws == Cold.ShadowCasterDraws,
			"static spotlight atlas rebuilt depth work");
		auto Evict = Delta(3, 2, Initial->Frame);
		auto Reprioritized = std::make_shared<std::vector<RenderLocalLightState>>(*Lights);
		Reprioritized->back().ShadowPriority = 20;
		Evict->Frame.Environment.LocalLights = Reprioritized;
		Evict->EnvironmentChanged = true;
		Draw(Renderer, Evict);
		const auto Changed = Renderer.GetMetrics();
		Require(Changed.ShadowMapPasses == Warm.ShadowMapPasses + 1,
			"one spotlight eviction rebuilt unrelated atlas layers");
		auto Reimport = Delta(4, 3, Evict->Frame);
		Reimport->TextureUpdates.push_back({Mask, 2, 0, 0, 1, 1,
			std::make_shared<const std::vector<std::uint8_t>>(
				std::initializer_list<std::uint8_t>{255, 255, 255, 0})});
		Draw(Renderer, Reimport);
		const auto Alpha = Renderer.GetMetrics();
		Require(Alpha.ShadowMapPasses == Changed.ShadowMapPasses + 8,
			"sampled alpha reimport failed to invalidate all eight overlapping spot maps");
		bool Rejected = false;
		auto Stale = Delta(5, 2, Reimport->Frame);
	try { Draw(Renderer, Stale); } catch (const std::invalid_argument &) {
		Rejected = true;
		EvidenceReceipt << "rejected," << CurrentCase << ',' << ++EvidenceSerial << ',' << Stale->Id
			<< ",,0,0,0,0," << Renderer.GetMetrics().ShadowMapPasses
			<< ',' << Renderer.GetMetrics().ShadowCasterDraws << ",stale-publication\n" << std::flush;
	}
		Require(Rejected && Renderer.GetMetrics().ShadowMapPasses == Alpha.ShadowMapPasses,
			"rejected publication changed submitted shadow passes");
		bool NeedsResync = false;
		try { Draw(Renderer, Delta(5, 4, Reimport->Frame)); }
		catch (const std::logic_error &Error) {
			NeedsResync = std::string_view(Error.what()).find("requires a full RenderPublication") != std::string_view::npos;
			if (!NeedsResync) throw;
		}
		Require(NeedsResync && Renderer.GetMetrics().FramesSubmitted == Alpha.FramesSubmitted,
			"runtime projection rejection did not enforce its full-resync recovery contract");
		auto Recovery = std::make_shared<RenderPublication>(*Initial);
		Recovery->Id = 6;
		Recovery->Frame = Reimport->Frame;
		for (auto &Texture : Recovery->TextureCreates) if (Texture.Texture == Mask) {
			Texture.Revision = Reimport->TextureUpdates.front().Revision;
			Texture.Pixels = Reimport->TextureUpdates.front().Pixels;
		}
		Draw(Renderer, Recovery);
		const auto Recovered = Renderer.GetMetrics();
		Require(Recovered.FramesSubmitted == Alpha.FramesSubmitted + 1 &&
			Recovered.ShadowMapPasses >= Alpha.ShadowMapPasses + 8 &&
			Recovered.ShadowMapPasses <= Alpha.ShadowMapPasses + 9,
			"full resync did not rebuild all spotlight depth after runtime rejection");
		Draw(Renderer, Delta(7, 6, Recovery->Frame));
		Require(Renderer.GetMetrics().ShadowMapPasses == Recovered.ShadowMapPasses,
			"recovered static spotlight layers did not return to cache reuse");
		std::cout << "[Render:ShadowGpu] passes cold=" << Cold.ShadowMapPasses
			<< " warm=" << Warm.ShadowMapPasses << " eviction=" << Changed.ShadowMapPasses
			<< " alpha=" << Alpha.ShadowMapPasses << '\n';
	}

	void CheckAtlasEvictionPixels() {
		CurrentCase = "atlas-eviction-pixels";
		EditorViewportRenderer Renderer(Size, Size);
		auto Initial = Scene(1, true, false, true);
		auto Lights = std::make_shared<std::vector<RenderLocalLightState>>();
		for (std::uint32_t Index = 0; Index < 9; ++Index) {
			auto Light = Initial->Frame.Environment.LocalLights->front();
			Light.Object = {400 + Index, 1};
			Light.ShadowPriority = Index < 8 ? 10 : 0;
			Light.Intensity = Index < 8 ? 0.5f : 8.0f;
			Light.Color = Index < 8 ? glm::vec3(1) : glm::vec3(1, 0, 0);
			Lights->push_back(Light);
		}
		Initial->Frame.Environment.LocalLights = Lights;
		const auto Denied = Capture(Renderer, Initial).RgbPixels;
		auto Evict = Delta(2, 1, Initial->Frame);
		auto Changed = std::make_shared<std::vector<RenderLocalLightState>>(*Lights);
		Changed->back().ShadowPriority = 20;
		Evict->Frame.Environment.LocalLights = Changed;
		Evict->EnvironmentChanged = true;
		const auto Allocated = Capture(Renderer, Evict).RgbPixels;
		Require(RedCenter(Denied) > RedCenter(Allocated) + 20,
			"spotlight eviction left stale packed atlas indices or shadowed a denied light");
		Require(Capture(Renderer, Delta(3, 2, Evict->Frame)).RgbPixels == Allocated,
			"stable spotlight atlas assignment changed exact warm pixels");
	}
}

int main(int ArgumentCount, char **Arguments) try {
	const SDLVideoReference Video;
	PointCostMode = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--point-cost";
	if (PointCostMode && ArgumentCount != 3)
		throw std::invalid_argument("--point-cost requires an evidence directory");
	EvidenceDirectory = PointCostMode ? std::filesystem::path(Arguments[2]) :
		ArgumentCount > 1 ? std::filesystem::path(Arguments[1]) :
		std::filesystem::path("renderer-shadow-evidence");
	std::filesystem::create_directories(EvidenceDirectory);
	if (PointCostMode) {
		CheckPointCostGate(EvidenceDirectory);
		std::ofstream(EvidenceDirectory / "point-shadow-cost-status.txt", std::ios::trunc)
			<< "passed\n";
		std::cout << "[Render:ShadowGpu] Point-equivalent cost receipt passed\n";
		return 0;
	}
	EvidenceReceipt.open(EvidenceDirectory / "shadow-receipt.csv", std::ios::trunc);
	if (!EvidenceReceipt) throw std::runtime_error("could not open shadow evidence receipt");
	EvidenceReceipt << "kind,case,iteration,publication_id,image,width,height,center_red,rgb_fingerprint,shadow_passes,shadow_caster_draws,outcome\n" << std::flush;
	CheckDirectionalPixels();
	for (const std::uint32_t Resolution : {256u, 1024u, 2048u})
		CheckOutdoorResolutionCase(Resolution);
	CheckSunZenithPixels();
	CheckMaskAndDeformable();
	CheckCascadePasses();
	CheckSpotPixels();
	CheckCachedSkinPoseReceipts();
	CheckLegacyStaticCompatibility();
	CheckEmptyDirectionalCoverage();
	CheckCanceledShadowCandidate();
	CheckIncomingDynamicCasters();
	CheckAtlasPasses();
	CheckAtlasEvictionPixels();
	RecordResult("passed", "");
	std::cout << "[Render:ShadowGpu] Cascades, spots and temporal pixels passed\n";
	return 0;
} catch (const std::exception &Error) {
	if (PointCostMode && !EvidenceDirectory.empty()) {
		std::ofstream(EvidenceDirectory / "point-shadow-cost-status.txt", std::ios::trunc)
			<< "failed: " << Error.what() << '\n';
	}
	if (EvidenceReceipt) RecordResult("failed", Error.what());
	std::cerr << "[Render:ShadowGpu] " << Error.what() << '\n';
	return 1;
}
