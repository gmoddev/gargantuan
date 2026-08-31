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
		std::uint32_t InjectPaletteUploadFailures = 0;
		std::uint32_t InjectShaderCreationFailures = 0;
		std::uint32_t InjectPipelineCreationFailures = 0;
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
		std::uint64_t PipelineCreations = 0;
		std::uint64_t ShaderCreations = 0;
		std::uint64_t EnvironmentApplications = 0;
		std::uint64_t SkyDraws = 0;
		std::uint64_t GpuSkinningRigs = 0;
		std::uint64_t CpuFallbackRigs = 0;
		std::uint64_t PaletteBufferCreations = 0;
		std::uint64_t PaletteTransferBufferCreations = 0;
		std::uint64_t PaletteUploads = 0;
		std::uint64_t PaletteUploadBytes = 0;
		std::uint64_t PaletteResourceReleases = 0;
		std::uint64_t PaletteScratchAllocations = 0;
		std::uint64_t FallbackTransitions = 0;
		std::uint64_t StalePoseDrops = 0;
		std::uint64_t SkinnedSourceResourceCreations = 0;
		std::uint64_t CpuSkinnedVertexUploads = 0;
		std::uint64_t MainShadowPoseMismatches = 0;
		std::uint64_t CpuProjectionNanoseconds = 0;
		std::uint64_t CpuVisibilityNanoseconds = 0;
		std::uint64_t CpuMeshTransferNanoseconds = 0;
		std::uint64_t CpuTextureTransferNanoseconds = 0;
		std::uint64_t CpuUiPreparationNanoseconds = 0;
		std::uint64_t CpuSubmissionNanoseconds = 0;
		std::uint64_t GpuCompletionWaitNanoseconds = 0;
		std::uint64_t LastProjectionNanoseconds = 0;
		std::uint64_t LastVisibilityNanoseconds = 0;
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
		[[nodiscard]] RendererCapabilities GetCapabilities() const override { return {true, true}; }
		[[nodiscard]] RenderAnimationVisibilityFeedback GetAnimationVisibilityFeedback() const override;
		[[nodiscard]] std::string GetDriverName() const;
		[[nodiscard]] SDLRendererMetrics GetMetrics() const;
		void WaitForIdle();
		[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> GetViewportSize() const override;

	  private:
		struct Backend;
		std::unique_ptr<Backend> State;
	};
}
