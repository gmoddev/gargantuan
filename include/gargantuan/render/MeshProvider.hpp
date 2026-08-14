#pragma once

#include "gargantuan/render/GpuMesh.hpp"
#include "gargantuan/render/RenderSnapshot.hpp"

#include <memory>
#include <unordered_map>

namespace gargantuan {
	class GpuMeshCache {
	  public:
		explicit GpuMeshCache(SDL_GPUDevice *gpu) : Gpu(gpu) {}
		~GpuMeshCache() = default;

		GpuMeshCache(const GpuMeshCache &) = delete;
		GpuMeshCache &operator=(const GpuMeshCache &) = delete;

		void UploadToGpu();
		void Destroy();
		[[nodiscard]] const GpuMesh *Find(RenderGeometry geometry) const;

	  private:
		SDL_GPUDevice *Gpu = nullptr;
		bool Uploaded = false;
		std::unordered_map<RenderGeometry, std::unique_ptr<GpuMesh>> Meshes;
	};
}
