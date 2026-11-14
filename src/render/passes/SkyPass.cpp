#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "gargantuan/render/SDLRenderer.hpp"
#include "render/sdl/SDLPipelineBuilder.hpp"
#include "render/sdl/SDLRenderPass.hpp"
#include "render/sdl/SDLTextureCache.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cmath>
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
			glm::vec4 SceneFlags;
			glm::vec4 LinearFaceFlags0;
			glm::vec4 LinearFaceFlags1;
		};

		SkyPass(SDL_GPUDevice *Gpu, SDL_GPUTextureFormat SwapchainFormat, SDLRendererMetrics *Metrics)
			: ImageBasedLighting(Gpu), ReflectionProbes(Gpu) {
			try {
				Shader.VertexFilepath = GetSDLShaderPath("sky.vert");
				Shader.VertexUniformBufferCount = 1;
				Shader.FragmentFilepath = GetSDLShaderPath("sky.frag");
				Shader.FragmentUniformBufferCount = 1;
				Shader.FragmentSamplerCount = 6;
				Shader.Init(Gpu, Metrics);
				SDLPipelineBuilder Builder;
				Builder.SetVertexShader(Shader.VertexShader).SetFragmentShader(Shader.FragmentShader)
					.SetColorEnabled(true).SetColorFormat(SwapchainFormat);
				auto PipelineInfo = Builder.BuildInfo();
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
			ReflectionProbes.Destroy();
			ImageBasedLighting.Destroy();
			if (Sampler) SDL_ReleaseGPUSampler(Gpu, Sampler);
			Sampler = nullptr;
			SDLRenderPass::Destroy(Gpu);
		}

		SDL_GPURenderPass *Draw(SDL_GPUDevice *, SDLFrameContext &Context) override {
			const auto &Frame = Context.Projection.GetFrame();
			const auto &Environment = Frame.Environment;
			if (!Context.TextureResources) throw std::logic_error("Sky rendering requires the SDL texture cache");
			std::array<SDL_GPUTextureSamplerBinding, 6> Bindings;
			if (Environment.Sky) {
				for (std::size_t Index = 0; Index < Bindings.size(); ++Index) {
					auto *Texture = Context.TextureResources->Find(Environment.Sky->Faces[Index].Texture);
					if (!Texture) throw std::logic_error("[Renderer:Sky] Face texture has no SDL residency");
					Bindings[Index] = {Texture, Sampler};
				}
			} else {
				Bindings.fill({Context.TextureResources->GetWhiteTexture(), Sampler});
			}
			const auto Before = Context.Metrics ? Context.Metrics->IblBuilds : 0;
			Context.EnvironmentLighting = ImageBasedLighting.Prepare(Environment.Sky, *Context.TextureResources, Context.Metrics);
			const bool GlobalBuilt = Context.Metrics && Context.Metrics->IblBuilds != Before;
			Context.ReflectionProbes = ReflectionProbes.Prepare(Context, !GlobalBuilt && ImageBasedLighting.PollPreparation());
			if (Context.ReflectionProbes.BrdfLut.texture)
				Context.EnvironmentLighting.BrdfLut = Context.ReflectionProbes.BrdfLut;
			SDL_GPUColorTargetInfo ColorTarget{
				.texture = Context.GetSceneColorTexture(),
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

			SDL_BindGPUFragmentSamplers(Pass, 0, Bindings.data(), Bindings.size());
			const auto TanHalfFov = std::tan(glm::radians(Frame.Camera.VerticalFieldOfView) * 0.5f);
			const auto Aspect = static_cast<float>(Frame.ViewportWidth) / static_cast<float>(Frame.ViewportHeight);
			const auto EnvironmentAngle = glm::radians(Environment.EnvironmentRotation);
			std::array<float, 8> LinearFaceFlags{};
			if (Environment.Sky) for (std::size_t Index = 0; Index < 6; ++Index)
				LinearFaceFlags[Index] = Context.TextureResources->GetFormat(Environment.Sky->Faces[Index].Texture) ==
					RenderTextureFormat::Rgba32Float ? 1.0f : 0.0f;
			Uniforms SkyUniforms{
				.RightTanAspect = glm::vec4(Frame.Camera.RightDirection, TanHalfFov * Aspect),
				.UpTan = glm::vec4(Frame.Camera.UpDirection, TanHalfFov),
				.LookExposure = glm::vec4(Frame.Camera.LookDirection, Environment.ExposureMultiplier),
				.EnvironmentColorHasSky = glm::vec4(Environment.EnvironmentColor, Environment.Sky ? 1.0f : 0.0f),
				.SceneFlags = glm::vec4(Context.HdrSceneTexture ? 1.0f : 0.0f,
					std::sin(EnvironmentAngle), std::cos(EnvironmentAngle),
					Environment.ToneMapping == RenderToneMapping::Reinhard ? 1.0f : 0.0f),
				.LinearFaceFlags0 = glm::vec4(LinearFaceFlags[0], LinearFaceFlags[1], LinearFaceFlags[2], LinearFaceFlags[3]),
				.LinearFaceFlags1 = glm::vec4(LinearFaceFlags[4], LinearFaceFlags[5], 0.0f, 0.0f),
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
		SDLImageBasedLighting ImageBasedLighting;
		SDLReflectionProbes ReflectionProbes;
		SDL_GPUSampler *Sampler = nullptr;
	  public:
		void OnFrameSubmitted() override { ReflectionProbes.OnFrameSubmitted(); }
		void OnFrameAborted() override { ReflectionProbes.OnFrameAborted(); }
	};

	std::unique_ptr<SDLRenderPass> CreateSkyPass(
		SDL_GPUDevice *Gpu, SDL_GPUTextureFormat SwapchainFormat, SDLRendererMetrics *Metrics
	) {
		return std::make_unique<SkyPass>(Gpu, SwapchainFormat, Metrics);
	}
}
