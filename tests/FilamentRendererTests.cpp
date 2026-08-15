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

	std::shared_ptr<gargantuan::RenderSnapshot> MakeSnapshot(
		gargantuan::RenderSnapshotId SnapshotId,
		gargantuan::ObjectId Object,
		std::uint32_t Width,
		std::uint32_t Height,
		float X = 0.0f
	) {
		using namespace gargantuan;
		auto Snapshot = std::make_shared<RenderSnapshot>();
		Snapshot->Id = SnapshotId;
		Snapshot->ViewportWidth = Width;
		Snapshot->ViewportHeight = Height;
		if (Object.IsValid()) {
			RenderItem Item;
			Item.Object = Object;
			Item.ModelMatrix = glm::translate(glm::mat4(1.0f), glm::vec3(X, 0.0f, -5.0f));
			Item.InverseModelMatrix = glm::inverse(Item.ModelMatrix);
			Item.CastShadow = false;
			Snapshot->Items.push_back(Item);
		}
		return Snapshot;
	}

	void TestProjectionLifetime() {
		using namespace gargantuan;
		FilamentRenderer Renderer(Vector2(800.0f, 450.0f), true, false);

		Renderer.Draw(MakeSnapshot(1, {41, 1}, 800, 450));
		auto Metrics = Renderer.GetLastMetrics();
		Check(Metrics.Changes.Created == 1 && Metrics.ProjectedObjects == 1, "first generation creates one projection");

		Renderer.Draw(MakeSnapshot(2, {41, 2}, 800, 450));
		Metrics = Renderer.GetLastMetrics();
		Check(
			Metrics.Changes.Created == 1 && Metrics.Changes.Removed == 1 && Metrics.ProjectedObjects == 1,
			"generation reuse removes the old entity and creates a distinct projection"
		);

		Renderer.Draw(MakeSnapshot(3, {41, 2}, 800, 450, 1.0f));
		Metrics = Renderer.GetLastMetrics();
		Check(
			Metrics.Changes.Updated == 1 && Metrics.Changes.Created == 0 && Metrics.Changes.Removed == 0,
			"the current generation updates only its own projection"
		);

		auto Duplicate = MakeSnapshot(4, {41, 2}, 800, 450);
		Duplicate->Items.push_back(Duplicate->Items.front());
		CheckThrows<std::invalid_argument>([&] { Renderer.Draw(Duplicate); }, "duplicate identities are rejected before mutation");
		Renderer.Draw(MakeSnapshot(5, {41, 2}, 800, 450, 2.0f));
		Check(Renderer.GetLastMetrics().Changes.Updated == 1, "a rejected publication leaves the current projection usable");

		Renderer.Resize(640, 360);
		Check(Renderer.GetViewportSize() == std::pair{640u, 360u}, "headless resize replaces the render target");
		Renderer.Draw(MakeSnapshot(6, {41, 2}, 640, 360, 3.0f));
		Renderer.Draw(MakeSnapshot(7, {}, 640, 360));
		Metrics = Renderer.GetLastMetrics();
		Check(Metrics.Changes.Removed == 1 && Metrics.ProjectedObjects == 0, "removal destroys the matching projection");

		Renderer.Destroy();
		Renderer.Destroy();
		Check(Renderer.GetViewportSize() == std::pair{0u, 0u}, "shutdown is deterministic and idempotent");
		CheckThrows<std::logic_error>(
			[&] { Renderer.Draw(MakeSnapshot(8, {}, 640, 360)); },
			"a destroyed renderer cannot accept publications"
		);
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
	if (Failures != 0) return 1;
	std::cout << "[Render:FilamentTest] All Filament renderer lifecycle tests passed\n";
	return 0;
}
