// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, you can obtain one at http://mozilla.org/MPL/2.0/.

#include "render/sdl/SDLDirectionalShadowCache.hpp"

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
}

int main() try {
	TestCache();
	std::cout << "[Render:ShadowCache] Publication relevance, submission and dependency bounds passed\n";
	return 0;
} catch (const std::exception &Error) {
	std::cerr << "[Render:ShadowCache] " << Error.what() << '\n';
	return 1;
}
