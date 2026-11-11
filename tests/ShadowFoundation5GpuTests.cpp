// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, you can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/editor/EditorViewport.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "render/sdl/SDLDirectionalCascadePlan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <format>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

namespace {
	using namespace gargantuan;
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
		Draw(Renderer, Delta(5, 4, Reimport->Frame));
		Require(Renderer.GetMetrics().ShadowMapPasses == Alpha.ShadowMapPasses,
			"valid retry rebuilt committed spotlight layers");
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
	EvidenceDirectory = ArgumentCount > 1 ? std::filesystem::path(Arguments[1])
		: std::filesystem::path("renderer-shadow-evidence");
	std::filesystem::create_directories(EvidenceDirectory);
	EvidenceReceipt.open(EvidenceDirectory / "shadow-receipt.csv", std::ios::trunc);
	if (!EvidenceReceipt) throw std::runtime_error("could not open shadow evidence receipt");
	EvidenceReceipt << "kind,case,iteration,publication_id,image,width,height,center_red,rgb_fingerprint,shadow_passes,shadow_caster_draws,outcome\n" << std::flush;
	CheckDirectionalPixels();
	CheckMaskAndDeformable();
	CheckCascadePasses();
	CheckSpotPixels();
	CheckCachedSkinPoseReceipts();
	CheckLegacyStaticCompatibility();
	CheckEmptyDirectionalCoverage();
	CheckIncomingDynamicCasters();
	CheckAtlasPasses();
	CheckAtlasEvictionPixels();
	RecordResult("passed", "");
	std::cout << "[Render:ShadowGpu] Cascades, spots and temporal pixels passed\n";
	return 0;
} catch (const std::exception &Error) {
	if (EvidenceReceipt) RecordResult("failed", Error.what());
	std::cerr << "[Render:ShadowGpu] " << Error.what() << '\n';
	return 1;
}
