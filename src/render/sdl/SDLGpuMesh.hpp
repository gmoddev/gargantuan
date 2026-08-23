#pragma once

#include "gargantuan/render/Mesh.hpp"
#include "gargantuan/render/RenderPublication.hpp"

#include <SDL3/SDL_gpu.h>

#include <cstdint>

namespace gargantuan {
	struct SDLRendererMetrics;

	struct SDLGpuMesh final : public Mesh {
		SDL_GPUBuffer *VertexBuffer = nullptr;
		std::uint32_t VertexCount = 0;
		std::uint32_t VertexBufferSize = 0;
		SDL_GPUBuffer *IndexBuffer = nullptr;
		std::uint32_t IndexCount = 0;
		std::uint32_t IndexBufferSize = 0;
		SDL_GPUTransferBuffer *TransferBuffer = nullptr;
		SDLRendererMetrics *Metrics = nullptr;

		explicit SDLGpuMesh(Mesh MeshData, SDLRendererMetrics *MetricsValue = nullptr);
		explicit SDLGpuMesh(const RenderMeshCreate &MeshData, SDLRendererMetrics *MetricsValue = nullptr);
		SDL_GPUBuffer *CreateVertexBuffer(SDL_GPUDevice *Gpu);
		SDL_GPUBuffer *CreateIndexBuffer(SDL_GPUDevice *Gpu);
		SDL_GPUTransferBuffer *CreateTransferBuffer(SDL_GPUDevice *Gpu);
		void DestroyTransferBuffer(SDL_GPUDevice *Gpu);
		void Upload(SDL_GPUDevice *Gpu, SDL_GPUCopyPass *CopyPass);
		void UploadVertices(
			SDL_GPUDevice *Gpu,
			SDL_GPUCopyPass *CopyPass,
			std::uint32_t FirstVertex,
			const std::vector<RenderVertex> &UpdatedVertices
		);
		void Destroy(SDL_GPUDevice *Gpu);

		SDLGpuMesh(const SDLGpuMesh &) = delete;
		SDLGpuMesh &operator=(const SDLGpuMesh &) = delete;
	};
}
