// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/datatypes/Vector2.hpp"
#include "gargantuan/render/FilamentRenderer.hpp"
#include "gargantuan/render/RenderSnapshot.hpp"

#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>

#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "[Render:FilamentTest] FAIL: " << Message << '\n';
		++Failures;
	}

	template <typename Exception, typename Callback> void CheckThrows(Callback Test, const char *Message) {
		try {
			Test();
		} catch (const Exception &) {
			return;
		} catch (...) {
		}
		Check(false, Message);
	}

	std::shared_ptr<gargantuan::RenderPublication> MakePublication(
		gargantuan::RenderPublicationId Id, gargantuan::RenderPublicationId Base,
		std::uint32_t Width = 128, std::uint32_t Height = 128
	) {
		using namespace gargantuan;
		auto Publication = std::make_shared<RenderPublication>();
		Publication->Id = Id;
		Publication->BaseId = Base;
		Publication->FullResync = Base == InvalidRenderPublicationId;
		Publication->Frame.ViewportWidth = Width;
		Publication->Frame.ViewportHeight = Height;
		Publication->Frame.Environment.SunDirection = {0.0f, 0.0f, 1.0f};
		return Publication;
	}

	gargantuan::RenderItem MakeItem(gargantuan::ObjectId Object, float X = 0.0f) {
		gargantuan::RenderItem Item;
		Item.Object = Object;
		Item.ModelMatrix = glm::translate(glm::mat4(1.0f), glm::vec3(X, 0.0f, -4.0f));
		Item.InverseModelMatrix = glm::inverse(Item.ModelMatrix);
		Item.CastShadow = false;
		return Item;
	}

	void TestProjectionLifetime() {
		using namespace gargantuan;
		constexpr ObjectId First{41, 1};
		constexpr ObjectId Next{41, 2};
		FilamentRenderer Renderer(Vector2(128.0f, 128.0f), true, false);

		auto Initial = MakePublication(1, 0);
		Initial->Creates.push_back({.Item = MakeItem(First)});
		Renderer.Draw(Initial);
		auto Metrics = Renderer.GetLastMetrics();
		Check(Metrics.Changes.Created == 1 && Metrics.ProjectedObjects == 1 && Metrics.RenderableObjects == 1,
			"first generation creates one native renderable");

		auto Replacement = MakePublication(2, 1);
		Replacement->Removes.push_back({First});
		Replacement->Creates.push_back({.Item = MakeItem(Next)});
		Renderer.Draw(Replacement);
		Metrics = Renderer.GetLastMetrics();
		Check(Metrics.Changes.Created == 1 && Metrics.Changes.Removed == 1 && Metrics.RenderableObjects == 1,
			"generation reuse removes the old entity and creates a distinct projection");

		auto Update = MakePublication(3, 2);
		Update->Updates.push_back({.Object = Next, .Domains = RenderUpdateDomain::Transform, .Item = MakeItem(Next, 1.0f)});
		Renderer.Draw(Update);
		Metrics = Renderer.GetLastMetrics();
		Check(Metrics.Changes.Updated == 1 && Metrics.Changes.Created == 0 && Metrics.Changes.Removed == 0 &&
			Metrics.FullResyncs == 1 && Metrics.NativeVertexUploadBytes == 0,
			"a transform update retains native resources and the publication base");

		auto Duplicate = MakePublication(4, 3);
		Duplicate->Removes = {{Next}, {Next}};
		CheckThrows<std::invalid_argument>([&] { Renderer.Draw(Duplicate); }, "duplicate operations fail before mutation");
		auto Unsupported = MakePublication(4, 3);
		Unsupported->Frame.Environment.Fog.Enabled = true;
		CheckThrows<std::invalid_argument>([&] { Renderer.Draw(Unsupported); }, "unsupported fog fails before mutation");
		Renderer.Draw(MakePublication(4, 3));
		Check(Renderer.GetLastMetrics().RenderableObjects == 1, "rejected publications preserve the native projection");

		Renderer.Resize(96, 80);
		Check(Renderer.GetViewportSize() == std::pair{96u, 80u}, "headless resize replaces the render target");
		auto Hide = MakePublication(5, 4, 96, 80);
		Hide->Updates.push_back({.Object = Next, .Domains = RenderUpdateDomain::Visibility,
			.Item = MakeItem(Next), .Visible = false});
		Renderer.Draw(Hide);
		Metrics = Renderer.GetLastMetrics();
		Check(Metrics.ProjectedObjects == 1 && Metrics.RenderableObjects == 0,
			"hidden entries remain in the neutral projection without a native renderable");
		auto Remove = MakePublication(6, 5, 96, 80);
		Remove->Removes.push_back({Next});
		Renderer.Draw(Remove);
		Check(Renderer.GetLastMetrics().ProjectedObjects == 0, "removal retires the matching hidden projection");

		Renderer.Destroy();
		Renderer.Destroy();
		Check(Renderer.GetViewportSize() == std::pair{0u, 0u}, "shutdown is deterministic and idempotent");
		CheckThrows<std::logic_error>([&] { Renderer.Draw(MakePublication(7, 6)); },
			"a destroyed renderer cannot accept publications");
	}

	void TestNativeResourceDeltas() {
		using namespace gargantuan;
		constexpr ObjectId Object{52, 1};
		constexpr RenderMeshIdentity Mesh{1, 1};
		constexpr RenderTextureIdentity Texture{1, 1};
		const glm::vec3 Normal{0.0f, 0.0f, 1.0f};
		const glm::vec4 Tangent{1.0f, 0.0f, 0.0f, 1.0f};
		const auto Vertices = std::make_shared<const std::vector<RenderVertex>>(
			std::initializer_list<RenderVertex>{
				{{-1.0f, -1.0f, 0.0f}, Normal, Tangent, {0.0f, 1.0f}},
				{{1.0f, -1.0f, 0.0f}, Normal, Tangent, {1.0f, 1.0f}},
				{{1.0f, 1.0f, 0.0f}, Normal, Tangent, {1.0f, 0.0f}},
				{{-1.0f, 1.0f, 0.0f}, Normal, Tangent, {0.0f, 0.0f}},
			});
		const auto Indices = std::make_shared<const std::vector<std::uint32_t>>(
			std::initializer_list<std::uint32_t>{0, 1, 2, 0, 2, 3});
		const auto Red = std::make_shared<const std::vector<std::uint8_t>>(
			std::initializer_list<std::uint8_t>{255, 0, 0, 255});
		const auto Blue = std::make_shared<const std::vector<std::uint8_t>>(
			std::initializer_list<std::uint8_t>{0, 0, 255, 255});
		const RenderBounds Bounds{{-1.0f, -1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}};
		RenderMaterialState Material;
		Material.BaseColorTexture = Texture;
		Material.MetallicRoughnessTexture = Texture;
		Material.ShadingModel = RenderShadingModel::MetallicRoughness;
		Material.Metallic = 0.2f;
		Material.Roughness = 0.35f;
		Material.DoubleSided = true;
		auto Initial = MakePublication(1, 0);
		Initial->MeshCreates.push_back({Mesh, 1, 1, Vertices, Indices, Bounds});
		Initial->TextureCreates.push_back({Texture, 1, 1, 1, RenderTextureFormat::Rgba8Unorm, Red});
		Initial->Creates.push_back({.Item = MakeItem(Object), .Mesh = Mesh, .Material = Material});

		FilamentRenderer Renderer(Vector2(128.0f, 128.0f), true, false);
		const auto Before = Renderer.Capture(Initial);
		auto Metrics = Renderer.GetLastMetrics();
		Check(Metrics.ResidentMeshes == 1 && Metrics.ResidentTextures == 1 && Metrics.SubmittedFrames == 1,
			"a captured frame uses native mesh and texture residency");

		auto TextureUpdate = MakePublication(2, 1);
		TextureUpdate->TextureUpdates.push_back({Texture, 2, 0, 0, 1, 1, Blue});
		const auto After = Renderer.Capture(TextureUpdate);
		Metrics = Renderer.GetLastMetrics();
		Check(Before.size() == 128u * 128u * 4u && After.size() == Before.size() && After != Before,
			"stable texture identity updates the rendered image");
		Check(Metrics.Changes.Updated == 0 && Metrics.NativeTextureUploadBytes == 8 &&
			Metrics.NativeVertexUploadBytes == 0 && Metrics.FullResyncs == 1,
			"a texel update touches both color-space views without rebuilding the mesh or entity");

		auto VertexUpdate = MakePublication(3, 2);
		auto Moved = Vertices->front();
		Moved.Position.x -= 0.25f;
		VertexUpdate->MeshVertexUpdates.push_back({Mesh, 2, 0,
			std::make_shared<const std::vector<RenderVertex>>(1, Moved),
			{{-1.25f, -1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}}});
		Renderer.Draw(VertexUpdate);
		Metrics = Renderer.GetLastMetrics();
		Check(Metrics.Changes.MeshesUpdated == 1 && Metrics.NativeVertexUploadBytes > 0 &&
			Metrics.NativeTextureUploadBytes == 0 && Metrics.RenderableObjects == 1,
			"stable mesh identity applies its vertex range without replacing the renderable");

		auto Remove = MakePublication(4, 3);
		Remove->Removes.push_back({Object});
		Remove->MeshRemoves.push_back({Mesh});
		Remove->TextureRemoves.push_back({Texture});
		Renderer.Draw(Remove);
		Metrics = Renderer.GetLastMetrics();
		Check(Metrics.ResidentMeshes == 0 && Metrics.ResidentTextures == 0 && Metrics.RenderableObjects == 0,
			"removal retires entities before their meshes and textures");
	}

	void TestInvalidConstructionCleanup() {
		using namespace gargantuan;
		CheckThrows<std::invalid_argument>(
			[] { FilamentRenderer Renderer(Vector2(0.0f, 450.0f), true, false); },
			"invalid construction fails without leaving a live backend"
		);

		Check(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"), "the partial-construction test selects SDL's dummy driver");
		Check(SDL_InitSubSystem(SDL_INIT_VIDEO), "the partial-construction test initializes SDL video");
		CheckThrows<std::runtime_error>(
			[] { FilamentRenderer Renderer(Vector2(64.0f, 64.0f), false, false); },
			"a window without a Win32 handle fails after cleaning partially constructed resources"
		);
		SDL_QuitSubSystem(SDL_INIT_VIDEO);
		SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
	}
}

int main() {
	TestInvalidConstructionCleanup();
	TestProjectionLifetime();
	TestNativeResourceDeltas();
	if (Failures != 0) return 1;
	std::cout << "[Render:FilamentTest] All Filament renderer lifecycle tests passed\n";
	return 0;
}
