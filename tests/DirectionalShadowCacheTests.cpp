// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, you can obtain one at http://mozilla.org/MPL/2.0/.

#include "render/sdl/SDLDirectionalShadowCache.hpp"
#include "render/sdl/SDLDirectionalCascadePlan.hpp"

#include <cmath>
#include <glm/ext/matrix_transform.hpp>
#include <iostream>
#include <stdexcept>

namespace {
	using namespace gargantuan;
	constexpr ObjectId CASTER{1, 1};
	constexpr ObjectId NONCASTER{2, 1};
	constexpr RenderMeshIdentity CASTER_MESH{10, 1};
	constexpr RenderTextureIdentity MASK{20, 1};

	void Require(bool Condition, const char *Message) {
		if (!Condition) throw std::runtime_error(Message);
	}

	RenderPublication InitialScene() {
		RenderPublication Scene{.Id = 1, .FullResync = true, .Frame = {.ViewportWidth = 64, .ViewportHeight = 64}};
		Scene.TextureCreates.push_back({MASK, 1, 1, 1, RenderTextureFormat::Rgba8Unorm,
			std::make_shared<const std::vector<std::uint8_t>>(4, 255)});
		RenderMaterialState Masked;
		Masked.OpacityMode = RenderOpacityMode::Masked;
		Masked.BaseColorTexture = MASK;
		Scene.Creates.push_back({.Item = {.Object = CASTER}, .Material = Masked});
		Scene.Creates.push_back({.Item = {.Object = NONCASTER, .CastShadow = false}});
		return Scene;
	}

	RenderObjectUpdate UpdateObject(const RenderProjection &Projection, ObjectId Object) {
		const auto &Current = Projection.GetObjects().at(Object);
		return {Object, RenderUpdateDomain::Material, Current.Item, Current.Mesh,
			Current.Material, Current.Visible, Current.Primitives};
	}

	void TestCache() {
		RenderProjection Projection;
		const auto Scene = InitialScene();
		(void)Projection.Apply(Scene);
		const auto &Environment = Scene.Frame.Environment;
		const glm::mat4 Matrix(2.0f);
		glm::mat4 Restored(0.0f);
		SDLDirectionalShadowCache Cache;
		const auto Prepare = [&] {
			Cache.BeginRecording();
			Cache.DependOnMesh(CASTER_MESH);
			Cache.DependOnMask(MASK);
			Cache.FinishRecording(Environment, nullptr, Matrix, true, true);
			Require(!Cache.Restore(Environment, nullptr, Restored), "recording cannot publish depth before successful submit");
			Require(Cache.OnFrameSubmitted(), "complete submitted map becomes reusable");
		};
		const auto CheckUpdate = [&](const RenderPublication &Delta, bool ExpectedReuse, const char *Message) {
			Prepare();
			Cache.ApplyPublication(Projection, Delta);
			Require(Cache.Restore(Delta.Frame.Environment, nullptr, Restored) == ExpectedReuse, Message);
			if (ExpectedReuse) Require(Restored == Matrix, "reused depth restores its exact matrix");
		};
		RenderPublication Delta{.Id = 2, .BaseId = 1, .Frame = Scene.Frame};
		Prepare();
		for (int Frame = 0; Frame < 4096; ++Frame) {
			Cache.ApplyPublication(Projection, Delta);
			Require(Cache.Restore(Environment, nullptr, Restored), "ordinary static frame reuses depth");
		}
		Delta.Frame.Camera.Position.x = 8.0f;
		Delta.Frame.Environment.ExposureMultiplier = 2.0f;
		Delta.Frame.Environment.SunColor = glm::vec3(0.5f);
		Delta.Frame.Environment.SunIntensity = 2.0f;
		CheckUpdate(Delta, true, "camera, exposure, sun color and positive intensity preserve depth geometry");
		Delta.Frame.Environment.SunDirection = {1.0f, 0.0f, 0.0f};
		CheckUpdate(Delta, false, "sun direction changes the shadow projection");
		Delta.Frame.Environment = Environment;
		Delta.Frame.Environment.SunIntensity = 0.0f;
		CheckUpdate(Delta, false, "sun-off transition replaces lit depth");
		Delta.Frame.Environment = Environment;

		Delta.Updates = {UpdateObject(Projection, NONCASTER)};
		Delta.Updates[0].Item.ModelMatrix[3].x += 1.0f;
		CheckUpdate(Delta, true, "noncaster movement preserves the map");
		Delta.Updates = {UpdateObject(Projection, CASTER)};
		Delta.Updates[0].Material.BaseColorFactor.r = 0.5f;
		Delta.Updates[0].Material.Roughness = 0.1f;
		CheckUpdate(Delta, true, "masked caster RGB and roughness do not alter alpha coverage");
		Delta.Updates[0].Item.ModelMatrix[3].x += 1.0f;
		CheckUpdate(Delta, false, "caster movement invalidates depth");
		Delta.Updates = {UpdateObject(Projection, CASTER)};
		Delta.Updates[0].Material.TextureSamplers[0].WrapU = RenderTextureWrap::ClampToEdge;
		CheckUpdate(Delta, false, "masked base-color sampler changes invalidate depth");
		Delta.Updates = {UpdateObject(Projection, CASTER)};
		Delta.Updates[0].Visible = false;
		CheckUpdate(Delta, false, "visibility loss invalidates depth");
		Delta.Updates.clear();

		Delta.TextureUpdates = {{.Texture = {21, 1}}};
		CheckUpdate(Delta, true, "unreferenced texture updates preserve depth");
		Delta.TextureUpdates = {{.Texture = MASK}};
		CheckUpdate(Delta, false, "a rectangle in a sampled alpha-mask texture invalidates depth");
		Delta.TextureUpdates.clear();
		Delta.MeshVertexUpdates = {{.Mesh = CASTER_MESH}};
		CheckUpdate(Delta, false, "deformable vertex edits on a drawn caster invalidate depth");
		Delta.MeshVertexUpdates.clear();
		Delta.AnimationPoseUpdates = {{.Object = CASTER}};
		CheckUpdate(Delta, false, "skeletal pose update on a caster invalidates depth");
		Delta.AnimationPoseUpdates.clear();
		Delta.Removes = {{CASTER}};
		CheckUpdate(Delta, false, "caster removal invalidates depth");
		Delta.Removes.clear();
		Delta.FullResync = true;
		CheckUpdate(Delta, false, "full resync cannot inherit the old shadow depth");

		Cache.BeginRecording();
		Cache.FinishRecording(Environment, nullptr, Matrix, false, true);
		Require(!Cache.OnFrameSubmitted() && !Cache.Restore(Environment, nullptr, Restored),
			"missing caster resources leave submitted depth ineligible for reuse");
		Cache.BeginRecording();
		Cache.FinishRecording(Environment, nullptr, Matrix, true, false);
		Cache.BeginRecording();
		Require(!Cache.OnFrameSubmitted(), "a superseding recording discards the prior unsubmitted depth candidate");
		Cache.BeginRecording();
		for (std::uint32_t Slot = 1; Slot <= 32'769; ++Slot) Cache.DependOnMesh({Slot, 1});
		Cache.FinishRecording(Environment, nullptr, Matrix, true, true);
		Require(!Cache.OnFrameSubmitted(), "dependency overflow falls back to fresh shadow work");

		auto WithoutCasters = Scene;
		WithoutCasters.Creates.erase(WithoutCasters.Creates.begin());
		(void)Projection.Apply(WithoutCasters);
		Cache.BeginRecording();
		Cache.FinishRecording(Environment, nullptr, Matrix, true, false);
		Require(Cache.OnFrameSubmitted(), "empty depth commits after submission");
		Delta.FullResync = false;
		Delta.Updates = {UpdateObject(Projection, NONCASTER)};
		Cache.ApplyPublication(Projection, Delta);
		Require(Cache.Restore(Environment, nullptr, Restored), "empty depth is reused for noncaster edits");
		Delta.Updates.front().Item.CastShadow = true;
		Cache.ApplyPublication(Projection, Delta);
		Require(!Cache.Restore(Environment, nullptr, Restored), "a newly eligible caster invalidates empty depth");
	}

	void TestCascadePlan() {
		RenderCameraSnapshot Camera;
		Camera.Position = {0.0f, 3.0f, 0.0f};
		const SDLDirectionalCascadeSettings Settings{.Count = 4, .Resolution = 1024,
			.MaximumDistance = 250.0f, .SplitLambda = 0.6f, .TransitionFraction = 0.1f};
		const auto Plan = MakeSDLDirectionalCascadePlan(Camera,
			glm::normalize(glm::vec3(0.3f, 0.8f, 0.2f)), 16.0f / 9.0f, Settings);
		Require(Plan && Plan->Count == 4 && Plan->Resolution == 1024, "bounded cascade plan builds");
		float Previous = Camera.NearPlane;
		for (const auto &Cascade : Plan->Cascades) {
			Require(Cascade.Near >= Previous && Cascade.Far > Cascade.Near &&
				Cascade.TransitionStart > Cascade.Near && Cascade.TransitionStart < Cascade.Far,
				"practical cascade splits and transition intervals are monotonic");
			Require(Cascade.WorldUnitsPerTexel > 0.0f && std::isfinite(Cascade.Matrix[3].x),
				"cascade matrix and scale are finite");
			Require(Cascade.DepthSpan > 0.0f && Cascade.DepthSpan * 0.0005f < 0.6f,
				"normalized comparison bias maps to a bounded world-space offset");
			Previous = Cascade.Far;
		}
		const auto CheckCorners = [&](const RenderCameraSnapshot &View, const SDLDirectionalCascadePlan &Value) {
			const double Tangent = std::tan(glm::radians(static_cast<double>(View.VerticalFieldOfView)) * 0.5);
			for (std::uint32_t Index = 0; Index < Value.Count; ++Index) {
				const auto &Cascade = Value.Cascades[Index];
				const float CoverageNear = Index == 0 ? Cascade.Near : Value.Cascades[Index - 1].TransitionStart;
				for (const float Depth : {CoverageNear, Cascade.Far})
					for (const float X : {-1.0f, 1.0f}) for (const float Y : {-1.0f, 1.0f}) {
						const glm::dvec3 World = glm::dvec3(View.Position) + glm::dvec3(View.LookDirection) *
							static_cast<double>(Depth) + glm::dvec3(View.RightDirection) *
							(X * Depth * Tangent * 16.0 / 9.0) + glm::dvec3(View.UpDirection) *
							(Y * Depth * Tangent);
						const auto Clip = glm::dmat4(Cascade.Matrix) *
							glm::dvec4(World - glm::dvec3(Cascade.Origin), 1.0);
						Require(std::abs(Clip.x) <= 1.01 && std::abs(Clip.y) <= 1.01,
							"all split frustum corners fit the snapped shadow projection");
					}
			}
		};
		CheckCorners(Camera, *Plan);
		Require(std::abs(Previous - 250.0f) < 1e-4f, "shadow distance caps a distant camera far plane");
		auto Moved = Camera;
		Moved.Position.x += Plan->Cascades[0].WorldUnitsPerTexel * 0.1f;
		const auto Stable = MakeSDLDirectionalCascadePlan(Moved,
			glm::normalize(glm::vec3(0.3f, 0.8f, 0.2f)), 16.0f / 9.0f, Settings);
		Require(Stable && Stable->Cascades[0].Matrix == Plan->Cascades[0].Matrix,
			"subtexel camera movement preserves the near cascade projection");
		Moved.Position = glm::vec3(900000.0f, 2000.0f, -900000.0f);
		const auto Distant = MakeSDLDirectionalCascadePlan(Moved,
			glm::normalize(glm::vec3(0.3f, 0.8f, 0.2f)), 16.0f / 9.0f, Settings);
		Require(Distant && std::isfinite(Distant->Cascades[0].Matrix[3].x),
			"large finite world offsets produce finite cascade matrices");
		CheckCorners(Moved, *Distant);
		auto Rotated = Camera;
		Rotated.LookDirection = glm::normalize(glm::vec3(0.4f, -0.1f, -1.0f));
		Rotated.RightDirection = glm::normalize(glm::cross(Rotated.LookDirection, glm::vec3(0, 1, 0)));
		Rotated.UpDirection = glm::normalize(glm::cross(Rotated.RightDirection, Rotated.LookDirection));
		const auto RotationPlan = MakeSDLDirectionalCascadePlan(Rotated,
			glm::normalize(glm::vec3(0.3f, 0.8f, 0.2f)), 16.0f / 9.0f, Settings);
		Require(RotationPlan.has_value(), "rotated camera builds a cascade plan");
		CheckCorners(Rotated, *RotationPlan);
		Require(!MakeSDLDirectionalCascadePlan(Camera, {0, 0, 0}, 1.0f, Settings),
			"a degenerate light direction is rejected");
		Require(!MakeSDLDirectionalCascadePlan(Camera, {0, 1, 0}, 1.0f,
			SDLDirectionalCascadeSettings{.Count = 5}), "cascade count is bounded");
	}

	void TestCascadeIndependence() {
		RenderProjection Projection;
		const auto Scene = InitialScene();
		(void)Projection.Apply(Scene);
		SDLDirectionalShadowCache Cache;
		const auto &Environment = Scene.Frame.Environment;
		const glm::mat4 Near(1.0f);
		const auto Far = glm::translate(glm::mat4(1.0f), glm::vec3(-10.0f, 0.0f, 0.0f));
		Cache.BeginCascadeRecording(0);
		Cache.FinishCascadeRecording(0, Environment, nullptr, Near, true, true);
		Cache.BeginCascadeRecording(1);
		Cache.FinishCascadeRecording(1, Environment, nullptr, Far, true, false);
		Require(Cache.OnFrameSubmitted(), "submitted cascade layers become reusable");
		RenderPublication Delta{.Id = 2, .BaseId = 1, .Frame = Scene.Frame};
		Delta.Updates = {UpdateObject(Projection, NONCASTER)};
		Delta.Updates[0].Item.CastShadow = true;
		Delta.Updates[0].Item.ModelMatrix[3].x = 10.0f;
		Cache.ApplyPublication(Projection, Delta);
		glm::mat4 Restored;
		Require(Cache.RestoreCascade(0, Environment, nullptr, Near, Restored),
			"a far caster addition preserves the unrelated near layer");
		Require(!Cache.RestoreCascade(1, Environment, nullptr, Far, Restored),
			"a far caster addition invalidates its own layer");
	}

	void TestUnboundedCasterMotion() {
		auto Scene = InitialScene();
		Scene.MeshCreates.push_back({.Mesh = CASTER_MESH, .TopologyRevision = 1, .VertexRevision = 1,
			.Vertices = std::make_shared<const std::vector<RenderVertex>>(
				std::initializer_list<RenderVertex>{{{-0.5f, -0.5f, 0}}, {{0.5f, -0.5f, 0}}, {{0, 0.5f, 0}}}),
			.Indices = std::make_shared<const std::vector<std::uint32_t>>(
				std::initializer_list<std::uint32_t>{0, 1, 2}),
			.Bounds = {{-0.5f, -0.5f, 0}, {0.5f, 0.5f, 0}}});
		Scene.Creates.front().Mesh = CASTER_MESH;
		RenderProjection Projection;
		(void)Projection.Apply(Scene);
		SDLDirectionalShadowCache Cache;
		const auto &Environment = Scene.Frame.Environment;
		const auto Far = glm::translate(glm::mat4(1.0f), glm::vec3(-10.0f, 0.0f, 0.0f));
		const auto PrepareEmpty = [&] {
			Cache.BeginCascadeRecording(1);
			Cache.FinishCascadeRecording(1, Environment, nullptr, Far, true, false);
			Require(Cache.OnFrameSubmitted(), "empty far layer commits after submission");
		};
		glm::mat4 Restored;
		PrepareEmpty();
		RenderPublication Pose{.Id = 2, .BaseId = 1, .Frame = Scene.Frame};
		Pose.AnimationPoseUpdates.push_back({.Object = CASTER});
		Cache.ApplyPublication(Projection, Pose);
		Require(!Cache.RestoreCascade(1, Environment, nullptr, Far, Restored),
			"a newly posed outside caster can enter an empty cascade");
		PrepareEmpty();
		RenderPublication Deform{.Id = 3, .BaseId = 1, .Frame = Scene.Frame};
		Deform.MeshVertexUpdates.push_back({.Mesh = CASTER_MESH});
		Cache.ApplyPublication(Projection, Deform);
		Require(!Cache.RestoreCascade(1, Environment, nullptr, Far, Restored),
			"new deformable bounds can enter an empty cascade");
	}
}

int main() try {
	TestCache();
	TestCascadePlan();
	TestCascadeIndependence();
	TestUnboundedCasterMotion();
	std::cout << "[Render:ShadowCache] Publication relevance, submission and dependency bounds passed\n";
	return 0;
} catch (const std::exception &Error) {
	std::cerr << "[Render:ShadowCache] " << Error.what() << '\n';
	return 1;
}
