#include "gargantuan/render/SDLRenderer.hpp"
#include "render/sdl/SDLPipelineBuilder.hpp"
#include "render/sdl/SDLRenderPass.hpp"
#include "render/sdl/SDLSceneColor.hpp"

#include <SDL3/SDL.h>
#include <format>
#include <stdexcept>

namespace gargantuan {
	namespace {
		class SceneResolvePass final : public SDLRenderPass {
		  public:
			SceneResolvePass(SDL_GPUDevice *Gpu, SDL_GPUTextureFormat OutputFormat, SDLRendererMetrics *Metrics)
				: EncodeSrgb(!SDLTargetEncodesSrgb(OutputFormat)) {
				try {
					Shader.VertexFilepath = GetSDLShaderPath("scene-resolve.vert");
					Shader.VertexUniformBufferCount = 0;
					Shader.FragmentFilepath = GetSDLShaderPath("scene-resolve.frag");
					Shader.FragmentSamplerCount = 1;
					Shader.FragmentUniformBufferCount = 1;
					Shader.Init(Gpu, Metrics);
					SDLPipelineBuilder Builder;
					Builder.SetVertexShader(Shader.VertexShader).SetFragmentShader(Shader.FragmentShader)
						.SetColorEnabled(true).SetColorFormat(OutputFormat);
					auto Info = Builder.BuildInfo();
					Info.vertex_input_state = {};
					Info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
					Pipeline = SDL_CreateGPUGraphicsPipeline(Gpu, &Info);
					if (Pipeline && Metrics) ++Metrics->PipelineCreations;
					const SDL_GPUSamplerCreateInfo SamplerInfo{
						.min_filter = SDL_GPU_FILTER_NEAREST, .mag_filter = SDL_GPU_FILTER_NEAREST,
						.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
						.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
						.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
						.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
					};
					Sampler = SDL_CreateGPUSampler(Gpu, &SamplerInfo);
					if (!Pipeline || !Sampler)
						throw std::runtime_error(std::format("[Renderer:HDR] Could not create resolve resources: {}", SDL_GetError()));
				} catch (...) {
					Destroy(Gpu);
					throw;
				}
			}

			void Destroy(SDL_GPUDevice *Gpu) override {
				if (Sampler) SDL_ReleaseGPUSampler(Gpu, Sampler);
				Sampler = nullptr;
				SDLRenderPass::Destroy(Gpu);
			}

			SDL_GPURenderPass *Draw(SDL_GPUDevice *, SDLFrameContext &Context) override {
				const SDL_GPUColorTargetInfo Target{
					.texture = Context.SwapchainTexture,
					.load_op = SDL_GPU_LOADOP_DONT_CARE, .store_op = SDL_GPU_STOREOP_STORE,
				};
				auto *Pass = SDL_BeginGPURenderPass(Context.Commands, &Target, 1, nullptr);
				if (!Pass)
					throw std::runtime_error(std::format("[Renderer:HDR] Could not begin scene resolve: {}", SDL_GetError()));
				const auto &Environment = Context.Projection.GetFrame().Environment;
				const glm::vec4 Uniforms(Environment.ExposureMultiplier, EncodeSrgb ? 1.0f : 0.0f,
					Environment.ToneMapping == RenderToneMapping::Reinhard ? 1.0f : 0.0f, 0.0f);
				const SDL_GPUTextureSamplerBinding Binding{Context.HdrSceneTexture, Sampler};
				SDL_BindGPUGraphicsPipeline(Pass, Pipeline);
				SDL_BindGPUFragmentSamplers(Pass, 0, &Binding, 1);
				SDL_PushGPUFragmentUniformData(Context.Commands, 0, &Uniforms, sizeof(Uniforms));
				SDL_DrawGPUPrimitives(Pass, 3, 1, 0, 0);
				if (Context.Metrics) {
					++Context.Metrics->SceneResolveDraws;
					++Context.Metrics->DrawCalls;
					++Context.Metrics->PipelineSwitches;
				}
				return Pass;
			}

		  private:
			SDL_GPUSampler *Sampler = nullptr;
			bool EncodeSrgb;
		};
	}

	std::unique_ptr<SDLRenderPass> CreateSceneResolvePass(
		SDL_GPUDevice *Gpu, SDL_GPUTextureFormat OutputFormat, SDLRendererMetrics *Metrics
	) {
		return std::make_unique<SceneResolvePass>(Gpu, OutputFormat, Metrics);
	}
}
