#pragma once

#include "render/sdl/SDLRenderPass.hpp"
#include "render/sdl/SDLSkinPaletteCache.hpp"

#include <array>

namespace gargantuan {
	class SDLViewportRenderer final {
	  public:
		void Draw(SDL_GPUDevice *Gpu, SDLFrameContext &Frame);
		void OnFrameSubmitted();
		void OnFrameAborted();
		void Destroy(SDL_GPUDevice *Gpu, SDLRendererMetrics &Metrics);
		[[nodiscard]] SDL_GPUTexture *Find(ObjectId Viewport) const;

	  private:
		struct Target {
			ObjectId Viewport;
			SDL_GPUTexture *Color = nullptr;
			SDL_GPUTexture *Depth = nullptr;
			std::uint32_t Width = 0;
			std::uint32_t Height = 0;
			std::shared_ptr<const RenderViewportScene> Rendered;
			std::shared_ptr<const RenderViewportScene> Pending;
			std::unique_ptr<SDLSkinPaletteCache> Palettes;
			std::array<RenderAnimationPoseUpdate, MaximumRenderViewportRigs> UploadedPoses;
			std::size_t UploadedCount = 0;
		};
		static void Release(SDL_GPUDevice *Gpu, Target &Value, SDLRendererMetrics &Metrics);
		static bool Resize(SDL_GPUDevice *Gpu, Target &Value, std::uint32_t Width, std::uint32_t Height,
			SDLRendererMetrics &Metrics);
		static void ApplyPalettes(SDL_GPUDevice *Gpu, Target &Value, const RenderProjection &Scene,
			SDLRendererMetrics &Metrics);
		std::array<Target, MaximumRenderViewports> Targets;
		RenderProjection Projection;
		std::unique_ptr<SDLRenderPass> Opaque;
		std::size_t Cursor = 0;
		std::array<ObjectId, MaximumRenderViewports> ReferencedViewports;
		std::size_t ReferenceCount = 0;
		std::size_t TargetCount = 0;
		RenderPublicationId UiPublicationId = InvalidRenderPublicationId;
		bool ResourcesFailed = false;
	};
}
