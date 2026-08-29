#include "render/sdl/SDLGpuMesh.hpp"
#include "gargantuan/render/Mesh.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>

namespace gargantuan {
	namespace {
		Mesh ConvertMesh(const RenderMeshCreate &MeshData) {
			Mesh Result;
			Result.Vertices.reserve(MeshData.Vertices->size());
			for (std::size_t Index = 0; Index < MeshData.Vertices->size(); ++Index) {
				const auto &Source = MeshData.Vertices->at(Index);
				Vertex Converted{Source.Position, Source.Normal, Source.TextureCoordinate};
				Converted.Tangent = Source.Tangent;
				if (MeshData.SkinInfluences) {
					Converted.Joints = MeshData.SkinInfluences->at(Index).Joints;
					Converted.Weights = MeshData.SkinInfluences->at(Index).Weights;
				}
				Result.Vertices.push_back(Converted);
			}
			Result.Indices = *MeshData.Indices;
			return Result;
		}
	}

	SDLGpuMesh::SDLGpuMesh(Mesh mesh, SDLRendererMetrics *MetricsValue) : Metrics(MetricsValue) {
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

	SDLGpuMesh::SDLGpuMesh(const RenderMeshCreate &MeshData, SDLRendererMetrics *MetricsValue)
		: SDLGpuMesh(ConvertMesh(MeshData), MetricsValue) {
		if (Metrics && MeshData.SkinInfluences) ++Metrics->SkinnedSourceResourceCreations;
	}

	SDL_GPUBuffer *SDLGpuMesh::CreateVertexBuffer(SDL_GPUDevice *gpu) {
		if (VertexBuffer) {
			return VertexBuffer;
		}

		SDL_GPUBufferCreateInfo info = {.usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = VertexBufferSize};
		VertexBuffer = SDL_CreateGPUBuffer(gpu, &info);
		if (!VertexBuffer) throw std::runtime_error(std::format("Failed to create vertex buffer: {}", SDL_GetError()));
		if (Metrics) ++Metrics->VertexBufferCreations;

		return VertexBuffer;
	}

	SDL_GPUBuffer *SDLGpuMesh::CreateIndexBuffer(SDL_GPUDevice *gpu) {
		if (IndexBuffer) {
			return IndexBuffer;
		}

		SDL_GPUBufferCreateInfo info = {.usage = SDL_GPU_BUFFERUSAGE_INDEX, .size = IndexBufferSize};
		IndexBuffer = SDL_CreateGPUBuffer(gpu, &info);
		if (!IndexBuffer) throw std::runtime_error(std::format("Failed to create index buffer: {}", SDL_GetError()));
		if (Metrics) ++Metrics->IndexBufferCreations;

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
		if (Metrics) ++Metrics->TransferBufferCreations;

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
		if (Metrics) {
			Metrics->UploadOperations += 2;
			Metrics->UploadedBytes += static_cast<std::uint64_t>(VertexBufferSize) + IndexBufferSize;
		}
	}

	void SDLGpuMesh::UploadVertices(
		SDL_GPUDevice *Gpu,
		SDL_GPUCopyPass *CopyPass,
		std::uint32_t FirstVertex,
		const std::vector<RenderVertex> &UpdatedVertices
	) {
		if (static_cast<std::size_t>(FirstVertex) > Vertices.size() ||
			UpdatedVertices.size() > Vertices.size() - FirstVertex)
			throw std::out_of_range("Dynamic mesh vertex update exceeds its persistent buffer");
		if (UpdatedVertices.size() > std::numeric_limits<std::uint32_t>::max() / sizeof(Vertex))
			throw std::length_error("Dynamic mesh vertex update exceeds the SDL transfer limit");
		for (std::size_t Index = 0; Index < UpdatedVertices.size(); ++Index) {
			const auto &Source = UpdatedVertices[Index];
			auto &Destination = Vertices[FirstVertex + Index];
			Destination.Position = Source.Position;
			Destination.Normal = Source.Normal;
			Destination.UV = Source.TextureCoordinate;
			Destination.Tangent = Source.Tangent;
		}
		const auto Bytes = static_cast<std::uint32_t>(UpdatedVertices.size() * sizeof(Vertex));
		auto *RangeTransfer = CreateTransferBuffer(Gpu);
		auto *Mapped = SDL_MapGPUTransferBuffer(Gpu, RangeTransfer, true);
		if (!Mapped)
			throw std::runtime_error(std::format("Failed to map dynamic mesh transfer buffer: {}", SDL_GetError()));
		std::memcpy(Mapped, Vertices.data() + FirstVertex, Bytes);
		SDL_UnmapGPUTransferBuffer(Gpu, RangeTransfer);
		SDL_GPUTransferBufferLocation Source{.transfer_buffer = RangeTransfer, .offset = 0};
		SDL_GPUBufferRegion Destination{
			.buffer = VertexBuffer,
			.offset = FirstVertex * static_cast<std::uint32_t>(sizeof(Vertex)),
			.size = Bytes,
		};
		const bool FullReplacement = FirstVertex == 0 && UpdatedVertices.size() == Vertices.size();
		// Cycling the destination is only valid when every byte is replaced. A partial
		// deformable update must preserve the untouched regions of the current backing.
		SDL_UploadToGPUBuffer(CopyPass, &Source, &Destination, FullReplacement);
		if (Metrics) {
			if (FullReplacement) ++Metrics->BufferCycleRequests;
			++Metrics->UploadOperations;
			Metrics->UploadedBytes += Bytes;
		}
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
