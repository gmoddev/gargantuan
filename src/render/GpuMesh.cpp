#include "render/sdl/SDLGpuMesh.hpp"
#include "gargantuan/render/Mesh.hpp"
#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>

namespace gargantuan {
	SDLGpuMesh::SDLGpuMesh(Mesh mesh) {
		this->Vertices = mesh.Vertices;
		this->Indices = mesh.Indices;
		constexpr auto MaximumBufferSize = std::numeric_limits<std::uint32_t>::max();
		if (Vertices.size() > MaximumBufferSize / sizeof(Vertex) ||
			Indices.size() > MaximumBufferSize / sizeof(std::uint32_t)) {
			throw std::length_error("Mesh data exceeds the SDL GPU buffer size limit");
		}
		const auto VertexBytes = Vertices.size() * sizeof(Vertex);
		const auto IndexBytes = Indices.size() * sizeof(std::uint32_t);
		if (VertexBytes > MaximumBufferSize - IndexBytes) {
			throw std::length_error("Combined mesh data exceeds the SDL GPU transfer buffer size limit");
		}
		this->VertexCount = static_cast<std::uint32_t>(Vertices.size());
		this->VertexBufferSize = static_cast<std::uint32_t>(VertexBytes);
		this->IndexCount = static_cast<std::uint32_t>(Indices.size());
		this->IndexBufferSize = static_cast<std::uint32_t>(IndexBytes);
	}

	SDL_GPUBuffer *SDLGpuMesh::CreateVertexBuffer(SDL_GPUDevice *gpu) {
		if (VertexBuffer) {
			return VertexBuffer;
		}

		SDL_GPUBufferCreateInfo info = {.usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = VertexBufferSize};
		VertexBuffer = SDL_CreateGPUBuffer(gpu, &info);
		if (!VertexBuffer) throw std::runtime_error(std::format("Failed to create vertex buffer: {}", SDL_GetError()));

		return VertexBuffer;
	}

	SDL_GPUBuffer *SDLGpuMesh::CreateIndexBuffer(SDL_GPUDevice *gpu) {
		if (IndexBuffer) {
			return IndexBuffer;
		}

		SDL_GPUBufferCreateInfo info = {.usage = SDL_GPU_BUFFERUSAGE_INDEX, .size = IndexBufferSize};
		IndexBuffer = SDL_CreateGPUBuffer(gpu, &info);
		if (!IndexBuffer) throw std::runtime_error(std::format("Failed to create index buffer: {}", SDL_GetError()));

		return IndexBuffer;
	}

	SDL_GPUTransferBuffer *SDLGpuMesh::CreateTransferBuffer(SDL_GPUDevice *gpu) {
		if (TransferBuffer) {
			return TransferBuffer;
		}

		SDL_GPUTransferBufferCreateInfo info = {
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = VertexBufferSize + IndexBufferSize,
		};

		TransferBuffer = SDL_CreateGPUTransferBuffer(gpu, &info);
		if (!TransferBuffer)
			throw std::runtime_error(std::format("Failed to create mesh transfer buffer: {}", SDL_GetError()));

		void *pointer = SDL_MapGPUTransferBuffer(gpu, TransferBuffer, false);
		if (!pointer) {
			SDL_ReleaseGPUTransferBuffer(gpu, TransferBuffer);
			TransferBuffer = nullptr;
			throw std::runtime_error(std::format("Failed to map mesh transfer buffer: {}", SDL_GetError()));
		}
		std::memcpy(pointer, Vertices.data(), VertexBufferSize);
		std::memcpy((uint8_t *)pointer + VertexBufferSize, Indices.data(), IndexBufferSize);
		SDL_UnmapGPUTransferBuffer(gpu, TransferBuffer);

		return TransferBuffer;
	}

	void SDLGpuMesh::DestroyTransferBuffer(SDL_GPUDevice *gpu) {
		if (TransferBuffer) {
			SDL_ReleaseGPUTransferBuffer(gpu, TransferBuffer);
			TransferBuffer = nullptr;
		}
	}

	void SDLGpuMesh::Upload(SDL_GPUDevice *gpu, SDL_GPUCopyPass *copyPass) {
		auto transferBuffer = CreateTransferBuffer(gpu);

		SDL_GPUTransferBufferLocation vertexSource{.transfer_buffer = transferBuffer, .offset = 0};
		SDL_GPUBufferRegion vertexDestination{.buffer = CreateVertexBuffer(gpu), .offset = 0, .size = VertexBufferSize};
		SDL_UploadToGPUBuffer(copyPass, &vertexSource, &vertexDestination, false);

		SDL_GPUTransferBufferLocation indexSource{.transfer_buffer = transferBuffer, .offset = VertexBufferSize};
		SDL_GPUBufferRegion indexDestination{.buffer = CreateIndexBuffer(gpu), .offset = 0, .size = IndexBufferSize};
		SDL_UploadToGPUBuffer(copyPass, &indexSource, &indexDestination, false);

		DestroyTransferBuffer(gpu);
	}

	void SDLGpuMesh::Destroy(SDL_GPUDevice *gpu) {
		DestroyTransferBuffer(gpu);

		if (VertexBuffer) {
			SDL_ReleaseGPUBuffer(gpu, VertexBuffer);
			VertexBuffer = nullptr;
		}

		if (IndexBuffer) {
			SDL_ReleaseGPUBuffer(gpu, IndexBuffer);
			IndexBuffer = nullptr;
		}
	}
} // namespace gargantuan
