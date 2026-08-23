#pragma once

#include "gargantuan/render/RenderPublication.hpp"
#include "gargantuan/render/RenderSnapshot.hpp"
#include "render/sdl/SDLGpuMesh.hpp"

#include <SDL3/SDL_gpu.h>

#include <memory>
#include <unordered_map>

namespace gargantuan {
	struct SDLRendererMetrics;

	class SDLMeshCache final {
	  public:
		explicit SDLMeshCache(SDL_GPUDevice *GpuDevice, SDLRendererMetrics *MetricsValue = nullptr)
			: Gpu(GpuDevice), Metrics(MetricsValue) {}

		SDLMeshCache(const SDLMeshCache &) = delete;
		SDLMeshCache &operator=(const SDLMeshCache &) = delete;

		void UploadToGpu();
		void ApplyPublication(const RenderPublication &Publication);
		void Destroy();
		[[nodiscard]] const SDLGpuMesh *Find(RenderGeometry Geometry) const;
		[[nodiscard]] const SDLGpuMesh *Find(RenderMeshIdentity Mesh) const;

	  private:
		SDL_GPUDevice *Gpu = nullptr;
		SDLRendererMetrics *Metrics = nullptr;
		bool Uploaded = false;
		std::unordered_map<RenderGeometry, std::unique_ptr<SDLGpuMesh>> Meshes;
		std::unordered_map<RenderMeshIdentity, std::unique_ptr<SDLGpuMesh>, RenderMeshIdentityHash> DynamicMeshes;
		void DestroyDynamic();
	};
}
