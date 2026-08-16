#pragma once

#include "gargantuan/render/RenderSnapshot.hpp"
#include "render/sdl/SDLGpuMesh.hpp"

#include <SDL3/SDL_gpu.h>

#include <memory>
#include <unordered_map>

namespace gargantuan {
	class SDLMeshCache final {
	  public:
		explicit SDLMeshCache(SDL_GPUDevice *GpuDevice) : Gpu(GpuDevice) {}

		SDLMeshCache(const SDLMeshCache &) = delete;
		SDLMeshCache &operator=(const SDLMeshCache &) = delete;

		void UploadToGpu();
		void Destroy();
		[[nodiscard]] const SDLGpuMesh *Find(RenderGeometry Geometry) const;

	  private:
		SDL_GPUDevice *Gpu = nullptr;
		bool Uploaded = false;
		std::unordered_map<RenderGeometry, std::unique_ptr<SDLGpuMesh>> Meshes;
	};
}
