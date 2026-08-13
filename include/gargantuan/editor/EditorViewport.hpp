// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/render/RenderPass.hpp"
#include "gargantuan/runtime/ObjectId.hpp"

#include <SDL3/SDL_gpu.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace gargantuan {
	struct EditorViewportFrame {
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		std::vector<std::uint8_t> RgbPixels;
	};

	struct EditorViewportPick {
		ObjectId Object;
		float Distance = 0.0f;
	};

	[[nodiscard]] std::optional<EditorViewportPick> PickEditorViewport(
		const std::shared_ptr<WorldRoot> &world,
		const std::shared_ptr<Camera> &camera,
		std::uint32_t width,
		std::uint32_t height,
		float x,
		float y
	);

	class EditorViewportRenderer final {
	  public:
		EditorViewportRenderer(std::uint32_t width, std::uint32_t height);
		~EditorViewportRenderer();

		EditorViewportRenderer(const EditorViewportRenderer &) = delete;
		EditorViewportRenderer &operator=(const EditorViewportRenderer &) = delete;

		void Resize(std::uint32_t width, std::uint32_t height);
		[[nodiscard]] EditorViewportFrame Capture(const DrawContext &context);

	  private:
		void Destroy();
		void RecreateTargets();

		bool OwnsVideoSubsystem = false;
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		SDL_GPUDevice *Gpu = nullptr;
		SDL_GPUTexture *ColorTexture = nullptr;
		SDL_GPUTexture *DepthTexture = nullptr;
		SDL_GPUTexture *ShadowMapTexture = nullptr;
		SDL_GPUSampler *ShadowSampler = nullptr;
		SDL_GPUTransferBuffer *DownloadBuffer = nullptr;
		std::vector<std::unique_ptr<RenderPass>> RenderPasses;
	};
}
