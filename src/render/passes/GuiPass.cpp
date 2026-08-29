#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "gargantuan/render/SDLRenderer.hpp"
#include "render/sdl/SDLRenderPass.hpp"
#include "render/sdl/SDLTextureCache.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <stdexcept>

namespace gargantuan {
	namespace {
		using Clock = std::chrono::steady_clock;

		std::uint64_t Nanoseconds(Clock::duration Duration) {
			return static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(Duration).count()
			);
		}

		std::uint32_t GrowCapacity(std::size_t Required) {
			if (Required == 0 || Required > std::numeric_limits<std::uint32_t>::max())
				throw std::length_error("UI geometry exceeds the SDL GPU buffer size limit");
			std::uint64_t Capacity = 4096;
			while (Capacity < Required) Capacity *= 2;
			if (Capacity > std::numeric_limits<std::uint32_t>::max()) Capacity = Required;
			return static_cast<std::uint32_t>(Capacity);
		}
	}

	class GuiPass final : public SDLRenderPass {
	  public:
		static constexpr std::string_view LABEL = "Gui";

		struct alignas(16) ViewportUniforms {
			glm::vec2 ViewportSize;
			glm::vec2 Padding{0.0f};
		};

		struct alignas(16) BatchUniforms {
			glm::vec4 Values{1.0f, 0.0f, 0.0f, 0.0f};
		};

		GuiPass(SDL_GPUDevice *Gpu, SDL_GPUTextureFormat SwapchainFormat, SDLRendererMetrics *Metrics) {
			try {
				Shader.VertexFilepath = GetSDLShaderPath("gui.vert");
				Shader.VertexUniformBufferCount = 1;
				Shader.FragmentFilepath = GetSDLShaderPath("gui.frag");
				Shader.FragmentUniformBufferCount = 1;
				Shader.FragmentSamplerCount = 1;
				Shader.Init(Gpu, Metrics);

				static const std::array<SDL_GPUVertexBufferDescription, 1> BufferDescriptions{{{
					.slot = 0,
					.pitch = sizeof(RenderUiVertex),
					.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
				}}};
				static const std::array<SDL_GPUVertexAttribute, 3> Attributes{{
					{.location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = offsetof(RenderUiVertex, Position)},
					{.location = 1, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = offsetof(RenderUiVertex, TextureCoordinate)},
					{.location = 2, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, .offset = offsetof(RenderUiVertex, Color)},
				}};
				SDL_GPUColorTargetDescription ColorTarget{.format = SwapchainFormat};
				ColorTarget.blend_state = {
					.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA,
					.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
					.color_blend_op = SDL_GPU_BLENDOP_ADD,
					.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE,
					.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
					.alpha_blend_op = SDL_GPU_BLENDOP_ADD,
					.enable_blend = true,
				};
				SDL_GPUGraphicsPipelineCreateInfo PipelineInfo{};
				PipelineInfo.vertex_shader = Shader.VertexShader;
				PipelineInfo.fragment_shader = Shader.FragmentShader;
				PipelineInfo.vertex_input_state = {
					.vertex_buffer_descriptions = BufferDescriptions.data(),
					.num_vertex_buffers = static_cast<std::uint32_t>(BufferDescriptions.size()),
					.vertex_attributes = Attributes.data(),
					.num_vertex_attributes = static_cast<std::uint32_t>(Attributes.size()),
				};
				PipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
				PipelineInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
				PipelineInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
				PipelineInfo.target_info.color_target_descriptions = &ColorTarget;
				PipelineInfo.target_info.num_color_targets = 1;
				Pipeline = SDL_CreateGPUGraphicsPipeline(Gpu, &PipelineInfo);
				if (Pipeline && Metrics) ++Metrics->PipelineCreations;
				if (!Pipeline)
					throw std::runtime_error(std::format("Failed to create GUI pipeline: {}", SDL_GetError()));

				SDL_GPUSamplerCreateInfo SamplerInfo{
					.min_filter = SDL_GPU_FILTER_LINEAR,
					.mag_filter = SDL_GPU_FILTER_LINEAR,
					.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
					.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
					.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
					.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
				};
				Sampler = SDL_CreateGPUSampler(Gpu, &SamplerInfo);
				if (!Sampler)
					throw std::runtime_error(std::format("Failed to create GUI sampler: {}", SDL_GetError()));
			} catch (...) {
				Destroy(Gpu);
				throw;
			}
		}

		void Destroy(SDL_GPUDevice *Gpu) override {
			if (VertexBuffer) SDL_ReleaseGPUBuffer(Gpu, VertexBuffer);
			if (IndexBuffer) SDL_ReleaseGPUBuffer(Gpu, IndexBuffer);
			if (TransferBuffer) SDL_ReleaseGPUTransferBuffer(Gpu, TransferBuffer);
			if (WhiteTexture) SDL_ReleaseGPUTexture(Gpu, WhiteTexture);
			if (WhiteTransferBuffer) SDL_ReleaseGPUTransferBuffer(Gpu, WhiteTransferBuffer);
			if (Sampler) SDL_ReleaseGPUSampler(Gpu, Sampler);
			VertexBuffer = nullptr;
			IndexBuffer = nullptr;
			TransferBuffer = nullptr;
			WhiteTexture = nullptr;
			WhiteTransferBuffer = nullptr;
			Sampler = nullptr;
			SDLRenderPass::Destroy(Gpu);
		}

		SDL_GPURenderPass *Draw(SDL_GPUDevice *Gpu, SDLFrameContext &Context) override {
			const auto PreparationStart = Clock::now();
			Prepare(Gpu, Context);
			const auto PreparationNanoseconds = Nanoseconds(Clock::now() - PreparationStart);
			if (Context.Metrics) {
				Context.Metrics->LastUiPreparationNanoseconds = PreparationNanoseconds;
				Context.Metrics->CpuUiPreparationNanoseconds += PreparationNanoseconds;
			}

			SDL_GPUColorTargetInfo ColorTarget{
				.texture = Context.SwapchainTexture,
				.load_op = SDL_GPU_LOADOP_LOAD,
				.store_op = SDL_GPU_STOREOP_STORE,
			};
			auto *Pass = SDL_BeginGPURenderPass(Context.Commands, &ColorTarget, 1, nullptr);
			if (!Pass) throw std::runtime_error(std::format("Failed to begin GUI pass: {}", SDL_GetError()));
			if (Context.Projection.GetUi().Batches.empty()) return Pass;

			SDL_BindGPUGraphicsPipeline(Pass, Pipeline);
			if (Context.Metrics) ++Context.Metrics->PipelineSwitches;
			SDL_GPUBufferBinding VertexBinding{.buffer = VertexBuffer};
			SDL_GPUBufferBinding IndexBinding{.buffer = IndexBuffer};
			SDL_BindGPUVertexBuffers(Pass, 0, &VertexBinding, 1);
			SDL_BindGPUIndexBuffer(Pass, &IndexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
			ViewportUniforms Viewport{{static_cast<float>(Context.Width), static_cast<float>(Context.Height)}};
			SDL_PushGPUVertexUniformData(Context.Commands, 0, &Viewport, sizeof(Viewport));

			std::uint32_t FirstIndex = 0;
			std::int32_t VertexOffset = 0;
			for (const auto &Batch : Context.Projection.GetUi().Batches) {
				auto *Texture = Batch.Texture && Context.TextureResources
					? Context.TextureResources->Find(*Batch.Texture) : WhiteTexture;
				if (!Texture) throw std::logic_error("GUI batch references a texture without SDL residency");
				SDL_GPUTextureSamplerBinding TextureBinding{.texture = Texture, .sampler = Sampler};
				SDL_BindGPUFragmentSamplers(Pass, 0, &TextureBinding, 1);
				BatchUniforms Uniforms{{Batch.Opacity, 0.0f, 0.0f, 0.0f}};
				SDL_PushGPUFragmentUniformData(Context.Commands, 0, &Uniforms, sizeof(Uniforms));

				SDL_Rect Scissor{0, 0, static_cast<int>(Context.Width), static_cast<int>(Context.Height)};
				if (Batch.Clip) {
					const auto Left = std::clamp(static_cast<int>(std::floor(Batch.Clip->X)), 0, Scissor.w);
					const auto Top = std::clamp(static_cast<int>(std::floor(Batch.Clip->Y)), 0, Scissor.h);
					const auto Right = std::clamp(static_cast<int>(std::ceil(Batch.Clip->X + Batch.Clip->Width)), Left, Scissor.w);
					const auto Bottom = std::clamp(static_cast<int>(std::ceil(Batch.Clip->Y + Batch.Clip->Height)), Top, Scissor.h);
					Scissor = {Left, Top, Right - Left, Bottom - Top};
				}
				SDL_SetGPUScissor(Pass, &Scissor);
				SDL_DrawGPUIndexedPrimitives(
					Pass,
					static_cast<std::uint32_t>(Batch.Indices.size()),
					1,
					FirstIndex,
					VertexOffset,
					0
				);
				FirstIndex += static_cast<std::uint32_t>(Batch.Indices.size());
				VertexOffset += static_cast<std::int32_t>(Batch.Vertices.size());
				if (Context.Metrics) {
					++Context.Metrics->DrawCalls;
					++Context.Metrics->UiBatches;
					++Context.Metrics->ScissorChanges;
				}
			}
			return Pass;
		}

	  private:
		void EnsureWhiteTexture(SDL_GPUDevice *Gpu, SDLRendererMetrics *Metrics) {
			if (WhiteTexture) return;
			SDL_GPUTextureCreateInfo TextureInfo{
				.type = SDL_GPU_TEXTURETYPE_2D,
				.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
				.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
				.width = 1,
				.height = 1,
				.layer_count_or_depth = 1,
				.num_levels = 1,
			};
			WhiteTexture = SDL_CreateGPUTexture(Gpu, &TextureInfo);
			SDL_GPUTransferBufferCreateInfo TransferInfo{.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = 4};
			WhiteTransferBuffer = SDL_CreateGPUTransferBuffer(Gpu, &TransferInfo);
			if (!WhiteTexture || !WhiteTransferBuffer)
				throw std::runtime_error(std::format("Failed to create the GUI white texture: {}", SDL_GetError()));
			auto *Mapped = static_cast<std::uint8_t *>(SDL_MapGPUTransferBuffer(Gpu, WhiteTransferBuffer, false));
			if (!Mapped) throw std::runtime_error(std::format("Failed to map the GUI white texture: {}", SDL_GetError()));
			std::memset(Mapped, 0xff, 4);
			SDL_UnmapGPUTransferBuffer(Gpu, WhiteTransferBuffer);
			auto *Commands = SDL_AcquireGPUCommandBuffer(Gpu);
			if (!Commands) throw std::runtime_error(std::format("Failed to acquire GUI texture commands: {}", SDL_GetError()));
			auto *CopyPass = SDL_BeginGPUCopyPass(Commands);
			if (!CopyPass) {
				SDL_CancelGPUCommandBuffer(Commands);
				throw std::runtime_error(std::format("Failed to begin GUI texture upload: {}", SDL_GetError()));
			}
			SDL_GPUTextureTransferInfo Source{.transfer_buffer = WhiteTransferBuffer, .pixels_per_row = 1, .rows_per_layer = 1};
			SDL_GPUTextureRegion Destination{.texture = WhiteTexture, .w = 1, .h = 1, .d = 1};
			SDL_UploadToGPUTexture(CopyPass, &Source, &Destination, false);
			SDL_EndGPUCopyPass(CopyPass);
			if (!SDL_SubmitGPUCommandBuffer(Commands))
				throw std::runtime_error(std::format("Failed to upload the GUI white texture: {}", SDL_GetError()));
			if (Metrics) {
				++Metrics->TextureCreations;
				++Metrics->TransferBufferCreations;
				++Metrics->UploadOperations;
				Metrics->UploadedBytes += 4;
			}
		}

		void EnsureCapacity(
			SDL_GPUDevice *Gpu,
			std::size_t VertexBytes,
			std::size_t IndexBytes,
			SDLRendererMetrics *Metrics
		) {
			if (VertexBytes <= VertexCapacity && IndexBytes <= IndexCapacity) return;
			const auto ReplacementVertexCapacity = GrowCapacity(std::max(VertexBytes, static_cast<std::size_t>(VertexCapacity)));
			const auto ReplacementIndexCapacity = GrowCapacity(std::max(IndexBytes, static_cast<std::size_t>(IndexCapacity)));
			if (static_cast<std::uint64_t>(ReplacementVertexCapacity) + ReplacementIndexCapacity >
				std::numeric_limits<std::uint32_t>::max())
				throw std::length_error("Combined UI geometry exceeds the SDL GPU transfer-buffer size limit");
			SDL_GPUBufferCreateInfo VertexInfo{.usage = SDL_GPU_BUFFERUSAGE_VERTEX, .size = ReplacementVertexCapacity};
			SDL_GPUBufferCreateInfo IndexInfo{.usage = SDL_GPU_BUFFERUSAGE_INDEX, .size = ReplacementIndexCapacity};
			SDL_GPUTransferBufferCreateInfo TransferInfo{
				.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
				.size = ReplacementVertexCapacity + ReplacementIndexCapacity,
			};
			auto *ReplacementVertex = SDL_CreateGPUBuffer(Gpu, &VertexInfo);
			auto *ReplacementIndex = SDL_CreateGPUBuffer(Gpu, &IndexInfo);
			auto *ReplacementTransfer = SDL_CreateGPUTransferBuffer(Gpu, &TransferInfo);
			if (!ReplacementVertex || !ReplacementIndex || !ReplacementTransfer) {
				if (ReplacementVertex) SDL_ReleaseGPUBuffer(Gpu, ReplacementVertex);
				if (ReplacementIndex) SDL_ReleaseGPUBuffer(Gpu, ReplacementIndex);
				if (ReplacementTransfer) SDL_ReleaseGPUTransferBuffer(Gpu, ReplacementTransfer);
				throw std::runtime_error(std::format("Failed to allocate persistent GUI buffers: {}", SDL_GetError()));
			}
			const bool Reallocated = VertexBuffer || IndexBuffer || TransferBuffer;
			if (VertexBuffer) SDL_ReleaseGPUBuffer(Gpu, VertexBuffer);
			if (IndexBuffer) SDL_ReleaseGPUBuffer(Gpu, IndexBuffer);
			if (TransferBuffer) SDL_ReleaseGPUTransferBuffer(Gpu, TransferBuffer);
			VertexBuffer = ReplacementVertex;
			IndexBuffer = ReplacementIndex;
			TransferBuffer = ReplacementTransfer;
			VertexCapacity = ReplacementVertexCapacity;
			IndexCapacity = ReplacementIndexCapacity;
			Uploaded = false;
			if (Metrics) {
				++Metrics->VertexBufferCreations;
				++Metrics->IndexBufferCreations;
				++Metrics->TransferBufferCreations;
				if (Reallocated) ++Metrics->BufferReallocations;
			}
		}

		void Prepare(SDL_GPUDevice *Gpu, SDLFrameContext &Context) {
			const auto &Ui = Context.Projection.GetUi();
			if (Ui.Batches.empty()) return;
			EnsureWhiteTexture(Gpu, Context.Metrics);
			std::uint64_t VertexBytes64 = 0;
			std::uint64_t IndexBytes64 = 0;
			std::uint64_t VertexCount64 = 0;
			std::uint64_t IndexCount64 = 0;
			for (const auto &Batch : Ui.Batches) {
				VertexBytes64 += static_cast<std::uint64_t>(Batch.Vertices.size()) * sizeof(RenderUiVertex);
				IndexBytes64 += static_cast<std::uint64_t>(Batch.Indices.size()) * sizeof(std::uint32_t);
				VertexCount64 += Batch.Vertices.size();
				IndexCount64 += Batch.Indices.size();
			}
			if (VertexBytes64 > std::numeric_limits<std::uint32_t>::max() ||
				IndexBytes64 > std::numeric_limits<std::uint32_t>::max() ||
				VertexCount64 > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) ||
				IndexCount64 > std::numeric_limits<std::uint32_t>::max())
				throw std::length_error("UI geometry exceeds SDL GPU draw limits");
			const auto VertexBytes = static_cast<std::size_t>(VertexBytes64);
			const auto IndexBytes = static_cast<std::size_t>(IndexBytes64);
			EnsureCapacity(Gpu, VertexBytes, IndexBytes, Context.Metrics);
			auto *Mapped = static_cast<std::uint8_t *>(SDL_MapGPUTransferBuffer(Gpu, TransferBuffer, Uploaded));
			if (!Mapped) throw std::runtime_error(std::format("Failed to map persistent GUI buffers: {}", SDL_GetError()));
			std::size_t VertexOffset = 0;
			std::size_t IndexOffset = 0;
			for (const auto &Batch : Ui.Batches) {
				const auto BatchVertexBytes = Batch.Vertices.size() * sizeof(RenderUiVertex);
				const auto BatchIndexBytes = Batch.Indices.size() * sizeof(std::uint32_t);
				std::memcpy(Mapped + VertexOffset, Batch.Vertices.data(), BatchVertexBytes);
				std::memcpy(Mapped + VertexCapacity + IndexOffset, Batch.Indices.data(), BatchIndexBytes);
				VertexOffset += BatchVertexBytes;
				IndexOffset += BatchIndexBytes;
			}
			SDL_UnmapGPUTransferBuffer(Gpu, TransferBuffer);
			auto *CopyPass = SDL_BeginGPUCopyPass(Context.Commands);
			if (!CopyPass) throw std::runtime_error(std::format("Failed to begin GUI upload: {}", SDL_GetError()));
			SDL_GPUTransferBufferLocation VertexSource{.transfer_buffer = TransferBuffer};
			SDL_GPUBufferRegion VertexDestination{.buffer = VertexBuffer, .size = static_cast<std::uint32_t>(VertexBytes)};
			SDL_UploadToGPUBuffer(CopyPass, &VertexSource, &VertexDestination, Uploaded);
			SDL_GPUTransferBufferLocation IndexSource{.transfer_buffer = TransferBuffer, .offset = VertexCapacity};
			SDL_GPUBufferRegion IndexDestination{.buffer = IndexBuffer, .size = static_cast<std::uint32_t>(IndexBytes)};
			SDL_UploadToGPUBuffer(CopyPass, &IndexSource, &IndexDestination, Uploaded);
			SDL_EndGPUCopyPass(CopyPass);
			if (Context.Metrics) {
				Context.Metrics->UploadOperations += 2;
				Context.Metrics->UploadedBytes += VertexBytes + IndexBytes;
				if (Uploaded) Context.Metrics->BufferCycleRequests += 3;
			}
			Uploaded = true;
		}

		SDL_GPUSampler *Sampler = nullptr;
		SDL_GPUBuffer *VertexBuffer = nullptr;
		SDL_GPUBuffer *IndexBuffer = nullptr;
		SDL_GPUTransferBuffer *TransferBuffer = nullptr;
		SDL_GPUTexture *WhiteTexture = nullptr;
		SDL_GPUTransferBuffer *WhiteTransferBuffer = nullptr;
		std::uint32_t VertexCapacity = 0;
		std::uint32_t IndexCapacity = 0;
		bool Uploaded = false;
	};

	std::unique_ptr<SDLRenderPass> CreateGuiPass(
		SDL_GPUDevice *Gpu, SDL_GPUTextureFormat SwapchainFormat, SDLRendererMetrics *Metrics
	) {
		return std::make_unique<GuiPass>(Gpu, SwapchainFormat, Metrics);
	}
}
