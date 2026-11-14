#pragma once

#include "gargantuan/render/RenderProjection.hpp"
#include "render/sdl/SDLShadowResources.hpp"

#include <SDL3/SDL_gpu.h>

#include <array>
#include <memory>
#include <vector>

namespace gargantuan {
	struct SDLRendererMetrics;
	class SDLSpotShadowAtlas;

	class SDLLocalLightGrid final {
	  public:
		explicit SDLLocalLightGrid(SDL_GPUDevice *Device) : Gpu(Device) {}
		void Destroy();
		void Prepare(const RenderFrameState &Frame, SDLRendererMetrics *Metrics,
			const SDLSpotShadowAtlas *Atlas = nullptr);
		[[nodiscard]] std::array<SDL_GPUBuffer *, 2> GetBuffers() const { return {Lights.GpuBuffer, Tiles.GpuBuffer}; }
		[[nodiscard]] glm::vec4 GetUniforms() const {
			return {static_cast<float>(VisibleCount), static_cast<float>(Columns), static_cast<float>(TilePixels), 0.0f};
		}

	  private:
		struct Buffer {
			SDL_GPUBuffer *GpuBuffer = nullptr;
			SDL_GPUTransferBuffer *Transfer = nullptr;
			std::uint32_t Capacity = 0;
		};
		struct GpuLight {
			glm::vec4 PositionRange;
			glm::vec4 DirectionOuter;
			glm::vec4 ColorIntensity;
			glm::vec4 ShapeInner;
		};
		struct ShadowBinding {
			ObjectId Light;
			std::uint32_t LayerPlusOne = 0;
			bool operator==(const ShadowBinding &) const = default;
		};

		void Release(Buffer &Value);
		void Upload(Buffer &Value, SDL_GPUCopyPass *Pass, const void *Data, std::uint32_t Size,
			SDLRendererMetrics *Metrics);

		SDL_GPUDevice *Gpu;
		Buffer Lights;
		Buffer Tiles;
		std::vector<GpuLight> PackedLights;
		std::vector<std::uint32_t> TileWords;
		std::shared_ptr<const std::vector<RenderLocalLightState>> LastLights;
		std::array<ShadowBinding, MaximumShadowedSpotlights> LastShadowBindings{};
		glm::dmat4 LastClip{1.0};
		glm::vec3 LastCamera{0.0f};
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		std::uint32_t Columns = 1;
		std::uint32_t TilePixels = 32;
		std::uint32_t VisibleCount = 0;
		std::uint32_t OverBudgetTiles = 0;
		std::uint32_t DroppedIndices = 0;
		bool Valid = false;
	};
}
