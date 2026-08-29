#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "gargantuan/render/SDLRenderer.hpp"
#include "render/sdl/SDLPipelineBuilder.hpp"
#include "render/sdl/SDLRenderPass.hpp"
#include "render/sdl/SDLTextureCache.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <stdexcept>

namespace gargantuan {
	class SkyPass final : public SDLRenderPass {
	  public:
		struct alignas(16) Uniforms {
			glm::vec4 RightTanAspect;
			glm::vec4 UpTan;
			glm::vec4 LookExposure;
			glm::vec4 EnvironmentColorHasSky;
		};

		SkyPass(SDL_GPUDevice *Gpu, SDL_GPUTextureFormat SwapchainFormat, SDLRendererMetrics *Metrics) {
			try {
				Shader.VertexFilepath = GetSDLShaderPath("sky.vert");
				Shader.VertexUniformBufferCount = 1;
				Shader.FragmentFilepath = GetSDLShaderPath("sky.frag");
				Shader.FragmentUniformBufferCount = 1;
				Shader.FragmentSamplerCount = 6;
				Shader.Init(Gpu, Metrics);
				auto PipelineInfo = SDLPipelineBuilder()
										.SetVertexShader(Shader.VertexShader)
										.SetFragmentShader(Shader.FragmentShader)
										.SetColorEnabled(true)
										.SetColorFormat(SwapchainFormat)
										.BuildInfo();
				PipelineInfo.vertex_input_state = {};
				PipelineInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
				Pipeline = SDL_CreateGPUGraphicsPipeline(Gpu, &PipelineInfo);
				if (Pipeline && Metrics) ++Metrics->PipelineCreations;
				SDL_GPUSamplerCreateInfo SamplerInfo{
					.min_filter = SDL_GPU_FILTER_LINEAR,
					.mag_filter = SDL_GPU_FILTER_LINEAR,
					.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
					.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
					.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
					.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
				};
				Sampler = SDL_CreateGPUSampler(Gpu, &SamplerInfo);
				if (!Pipeline || !Sampler)
					throw std::runtime_error(
						std::format("Failed to create Sky rendering resources: {}", SDL_GetError())
					);
			} catch (...) {
				Destroy(Gpu);
				throw;
			}
		}

		void Destroy(SDL_GPUDevice *Gpu) override {
			if (FallbackTexture) SDL_ReleaseGPUTexture(Gpu, FallbackTexture);
			if (FallbackTransferBuffer) SDL_ReleaseGPUTransferBuffer(Gpu, FallbackTransferBuffer);
			if (Sampler) SDL_ReleaseGPUSampler(Gpu, Sampler);
			FallbackTexture = nullptr;
			FallbackTransferBuffer = nullptr;
			Sampler = nullptr;
			SDLRenderPass::Destroy(Gpu);
		}

		SDL_GPURenderPass *Draw(SDL_GPUDevice *Gpu, SDLFrameContext &Context) override {
			EnsureFallbackTexture(Gpu, Context.Metrics);
			const auto &Frame = Context.Projection.GetFrame();
			const auto &Environment = Frame.Environment;
			SDL_GPUColorTargetInfo ColorTarget{
				.texture = Context.SwapchainTexture,
				.clear_color =
					{Environment.EnvironmentColor.r,
					 Environment.EnvironmentColor.g,
					 Environment.EnvironmentColor.b,
					 1.0f},
				.load_op = SDL_GPU_LOADOP_CLEAR,
				.store_op = SDL_GPU_STOREOP_STORE,
			};
			auto *Pass = SDL_BeginGPURenderPass(Context.Commands, &ColorTarget, 1, nullptr);
			if (!Pass) throw std::runtime_error(std::format("Failed to begin Sky pass: {}", SDL_GetError()));
			SDL_BindGPUGraphicsPipeline(Pass, Pipeline);
			if (Context.Metrics) ++Context.Metrics->PipelineSwitches;

			std::array<SDL_GPUTextureSamplerBinding, 6> Bindings;
			for (auto &Binding : Bindings)
				Binding = {.texture = FallbackTexture, .sampler = Sampler};
			if (Environment.Sky) {
				for (std::size_t Index = 0; Index < Bindings.size(); ++Index) {
					auto *Texture = Context.TextureResources
										? Context.TextureResources->Find(Environment.Sky->Faces[Index].Texture)
										: nullptr;
					if (!Texture) throw std::logic_error("Sky face references a texture without SDL residency");
					Bindings[Index].texture = Texture;
				}
			}
			SDL_BindGPUFragmentSamplers(Pass, 0, Bindings.data(), Bindings.size());
			const auto TanHalfFov = std::tan(glm::radians(Frame.Camera.VerticalFieldOfView) * 0.5f);
			const auto Aspect = static_cast<float>(Frame.ViewportWidth) / static_cast<float>(Frame.ViewportHeight);
			Uniforms SkyUniforms{
				.RightTanAspect = glm::vec4(Frame.Camera.RightDirection, TanHalfFov * Aspect),
				.UpTan = glm::vec4(Frame.Camera.UpDirection, TanHalfFov),
				.LookExposure = glm::vec4(Frame.Camera.LookDirection, Environment.ExposureMultiplier),
				.EnvironmentColorHasSky = glm::vec4(Environment.EnvironmentColor, Environment.Sky ? 1.0f : 0.0f),
			};
			SDL_PushGPUVertexUniformData(Context.Commands, 0, &SkyUniforms, sizeof(SkyUniforms));
			SDL_PushGPUFragmentUniformData(Context.Commands, 0, &SkyUniforms, sizeof(SkyUniforms));
			SDL_DrawGPUPrimitives(Pass, 3, 1, 0, 0);
			if (Context.Metrics) {
				++Context.Metrics->DrawCalls;
				++Context.Metrics->SkyDraws;
			}
			return Pass;
		}

	  private:
		void EnsureFallbackTexture(SDL_GPUDevice *Gpu, SDLRendererMetrics *Metrics) {
			if (FallbackTexture) return;
			SDL_GPUTextureCreateInfo TextureInfo{
				.type = SDL_GPU_TEXTURETYPE_2D,
				.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
				.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
				.width = 1,
				.height = 1,
				.layer_count_or_depth = 1,
				.num_levels = 1,
			};
			FallbackTexture = SDL_CreateGPUTexture(Gpu, &TextureInfo);
			SDL_GPUTransferBufferCreateInfo TransferInfo{.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = 4};
			FallbackTransferBuffer = SDL_CreateGPUTransferBuffer(Gpu, &TransferInfo);
			if (!FallbackTexture || !FallbackTransferBuffer)
				throw std::runtime_error(std::format("Failed to create Sky fallback texture: {}", SDL_GetError()));
			auto *Mapped = static_cast<std::uint8_t *>(SDL_MapGPUTransferBuffer(Gpu, FallbackTransferBuffer, false));
			if (!Mapped)
				throw std::runtime_error(std::format("Failed to map Sky fallback texture: {}", SDL_GetError()));
			std::memset(Mapped, 0xff, 4);
			SDL_UnmapGPUTransferBuffer(Gpu, FallbackTransferBuffer);
			auto *Commands = SDL_AcquireGPUCommandBuffer(Gpu);
			auto *CopyPass = Commands ? SDL_BeginGPUCopyPass(Commands) : nullptr;
			if (!CopyPass) {
				if (Commands) SDL_CancelGPUCommandBuffer(Commands);
				throw std::runtime_error(std::format("Failed to begin Sky fallback upload: {}", SDL_GetError()));
			}
			SDL_GPUTextureTransferInfo Source{
				.transfer_buffer = FallbackTransferBuffer, .pixels_per_row = 1, .rows_per_layer = 1
			};
			SDL_GPUTextureRegion Destination{.texture = FallbackTexture, .w = 1, .h = 1, .d = 1};
			SDL_UploadToGPUTexture(CopyPass, &Source, &Destination, false);
			SDL_EndGPUCopyPass(CopyPass);
			if (!SDL_SubmitGPUCommandBuffer(Commands))
				throw std::runtime_error(std::format("Failed to upload Sky fallback texture: {}", SDL_GetError()));
			if (Metrics) {
				++Metrics->TextureCreations;
				++Metrics->TransferBufferCreations;
				++Metrics->UploadOperations;
				Metrics->UploadedBytes += 4;
			}
		}

		SDL_GPUTexture *FallbackTexture = nullptr;
		SDL_GPUTransferBuffer *FallbackTransferBuffer = nullptr;
		SDL_GPUSampler *Sampler = nullptr;
	};

	std::unique_ptr<SDLRenderPass> CreateSkyPass(
		SDL_GPUDevice *Gpu, SDL_GPUTextureFormat SwapchainFormat, SDLRendererMetrics *Metrics
	) {
		return std::make_unique<SkyPass>(Gpu, SwapchainFormat, Metrics);
	}
}
