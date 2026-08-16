#pragma once

#include "gargantuan/render/Mesh.hpp"

#include <SDL3/SDL_gpu.h>

#include <cstdint>

namespace gargantuan {
	struct SDLGpuMesh final : public Mesh {
		SDL_GPUBuffer *VertexBuffer = nullptr;
		std::uint32_t VertexCount = 0;
		std::uint32_t VertexBufferSize = 0;
		SDL_GPUBuffer *IndexBuffer = nullptr;
		std::uint32_t IndexCount = 0;
		std::uint32_t IndexBufferSize = 0;
		SDL_GPUTransferBuffer *TransferBuffer = nullptr;

		explicit SDLGpuMesh(Mesh MeshData);
		SDL_GPUBuffer *CreateVertexBuffer(SDL_GPUDevice *Gpu);
		SDL_GPUBuffer *CreateIndexBuffer(SDL_GPUDevice *Gpu);
		SDL_GPUTransferBuffer *CreateTransferBuffer(SDL_GPUDevice *Gpu);
		void DestroyTransferBuffer(SDL_GPUDevice *Gpu);
		void Upload(SDL_GPUDevice *Gpu, SDL_GPUCopyPass *CopyPass);
		void Destroy(SDL_GPUDevice *Gpu);

		SDLGpuMesh(const SDLGpuMesh &) = delete;
		SDLGpuMesh &operator=(const SDLGpuMesh &) = delete;
	};
}
