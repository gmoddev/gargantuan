#include "render/sdl/SDLMeshCache.hpp"
#include "gargantuan/render/PrimitiveMeshes.hpp"

#include <SDL3/SDL.h>
#include <format>
#include <memory>
#include <stdexcept>
#include <utility>

namespace gargantuan {
	namespace {
		std::vector<std::pair<RenderGeometry, Mesh>> CreatePrimitiveMeshes() {
			return {
				{RenderGeometry::Ball, PrimitiveMeshes::Block()},
				{RenderGeometry::Block, PrimitiveMeshes::Block()},
				{RenderGeometry::Cylinder, PrimitiveMeshes::Block()},
				{RenderGeometry::Wedge, PrimitiveMeshes::Wedge()},
				{RenderGeometry::CornerWedge, PrimitiveMeshes::Block()},
			};
		}
	}

	void SDLMeshCache::Destroy() {
		if (!Gpu) return;
		for (auto &[geometry, gpuMesh] : Meshes) {
			(void)geometry;
			gpuMesh->Destroy(Gpu);
		}
		Meshes.clear();
		Uploaded = false;
	}

	void SDLMeshCache::UploadToGpu() {
		if (Uploaded) return;
		if (!Gpu) throw std::logic_error("Cannot upload primitive meshes without an SDL GPU device");
		auto cmd = SDL_AcquireGPUCommandBuffer(Gpu);
		if (!cmd) throw std::runtime_error(std::format("Failed to acquire mesh upload commands: {}", SDL_GetError()));
		auto copyPass = SDL_BeginGPUCopyPass(cmd);
		if (!copyPass) {
			SDL_CancelGPUCommandBuffer(cmd);
			throw std::runtime_error(std::format("Failed to begin mesh upload: {}", SDL_GetError()));
		}

		for (auto &[geometry, mesh] : CreatePrimitiveMeshes()) {
			auto gpuMesh = std::make_unique<SDLGpuMesh>(std::move(mesh));
			gpuMesh->Upload(Gpu, copyPass);
			Meshes.emplace(geometry, std::move(gpuMesh));
		}

		SDL_EndGPUCopyPass(copyPass);
		if (!SDL_SubmitGPUCommandBuffer(cmd)) {
			Destroy();
			throw std::runtime_error(std::format("Failed to submit mesh upload: {}", SDL_GetError()));
		}
		Uploaded = true;
	}

	const SDLGpuMesh *SDLMeshCache::Find(RenderGeometry geometry) const {
		auto mesh = Meshes.find(geometry);
		return mesh == Meshes.end() ? nullptr : mesh->second.get();
	}
}
