// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/render/Renderer.hpp"

#include <memory>
#include <cstdint>
#include <string>

namespace gargantuan {
	struct SDLRendererOptions {
		bool Offscreen = false;
		bool WaitForGpuCompletion = false;
		bool DebugDevice = true;
	};

	struct SDLRendererMetrics {
		std::uint64_t FramesSubmitted = 0;
		std::uint64_t FullResyncs = 0;
		std::uint64_t VertexBufferCreations = 0;
		std::uint64_t IndexBufferCreations = 0;
		std::uint64_t TransferBufferCreations = 0;
		std::uint64_t BufferReallocations = 0;
		std::uint64_t BufferCycleRequests = 0;
		std::uint64_t TextureCreations = 0;
		std::uint64_t TextureUpdates = 0;
		std::uint64_t TextureReleases = 0;
		std::uint64_t UploadOperations = 0;
		std::uint64_t UploadedBytes = 0;
		std::uint64_t DrawCalls = 0;
		std::uint64_t UiBatches = 0;
		std::uint64_t ScissorChanges = 0;
		std::uint64_t PipelineSwitches = 0;
		std::uint64_t CpuProjectionNanoseconds = 0;
		std::uint64_t CpuMeshTransferNanoseconds = 0;
		std::uint64_t CpuTextureTransferNanoseconds = 0;
		std::uint64_t CpuUiPreparationNanoseconds = 0;
		std::uint64_t CpuSubmissionNanoseconds = 0;
		std::uint64_t GpuCompletionWaitNanoseconds = 0;
		std::uint64_t LastProjectionNanoseconds = 0;
		std::uint64_t LastMeshTransferNanoseconds = 0;
		std::uint64_t LastTextureTransferNanoseconds = 0;
		std::uint64_t LastUiPreparationNanoseconds = 0;
		std::uint64_t LastSubmissionNanoseconds = 0;
		std::uint64_t LastGpuCompletionWaitNanoseconds = 0;
	};

	class SDLRenderer final : public BaseRenderer {
	  public:
		explicit SDLRenderer(const Vector2 &ViewportSize);
		SDLRenderer(const Vector2 &ViewportSize, SDLRendererOptions Options);
		~SDLRenderer() override;

		using BaseRenderer::Draw;
		void Draw(RenderPublicationPtr Publication) override;
		void Resize(int WidthValue, int HeightValue) override;
		void Destroy() override;
		[[nodiscard]] std::string GetDriverName() const;
		[[nodiscard]] SDLRendererMetrics GetMetrics() const;
		void WaitForIdle();
		[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> GetViewportSize() const override;

	  private:
		struct Backend;
		std::unique_ptr<Backend> State;
	};
}
