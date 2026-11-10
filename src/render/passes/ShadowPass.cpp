#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "gargantuan/Log.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "render/sdl/SDLMaterialSamplers.hpp"
#include "render/sdl/SDLMeshCache.hpp"
#include "render/sdl/SDLPipelineBuilder.hpp"
#include "render/sdl/SDLRenderPass.hpp"
#include "render/sdl/SDLSkinPaletteCache.hpp"
#include "render/sdl/SDLTextureCache.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gpu.h>
#include <cmath>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <memory>
#include <stdexcept>

namespace gargantuan {

	class ShadowPass final : public SDLRenderPass {
	  public:
		static constexpr std::string_view LABEL = "Shadow";

		struct alignas(16) Uniforms {
			glm::mat4 ShadowMatrix;
			glm::mat4 PartMatrix;
		};

		ShadowPass(SDL_GPUDevice *gpu, SDL_GPUTextureFormat swapchainFormat, SDLRendererMetrics *Metrics) {
			(void)swapchainFormat;
			try {
				Shader.VertexFilepath = GetSDLShaderPath("shadow.vert");
				Shader.VertexStorageBufferCount = 1;
				Shader.FragmentFilepath = GetSDLShaderPath("shadow.frag");
				Shader.FragmentUniformBufferCount = 1;
				Shader.FragmentSamplerCount = 1;
				Shader.Init(gpu, Metrics);
				auto Builder = SDLPipelineBuilder()
					.SetVertexShader(Shader.VertexShader)
					.SetFragmentShader(Shader.FragmentShader)
					.SetColorEnabled(false)
					.SetDepthEnabled(true)
					.SetDepthFormat(SDL_GPU_TEXTUREFORMAT_D32_FLOAT);
				Pipeline = Builder.Build(gpu, Metrics);
				auto Info = Builder.BuildInfo();
				Info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
				DoubleSided = SDL_CreateGPUGraphicsPipeline(gpu, &Info);
				if (!DoubleSided) throw std::runtime_error("[Renderer:Shadow] Could not create the double-sided pipeline");
				if (Metrics) ++Metrics->PipelineCreations;
			} catch (...) {
				Destroy(gpu);
				throw;
			}
		}

		void Destroy(SDL_GPUDevice *Gpu) override {
			if (DoubleSided) SDL_ReleaseGPUGraphicsPipeline(Gpu, DoubleSided);
			DoubleSided = nullptr;
			SDLRenderPass::Destroy(Gpu);
		}

		SDL_GPURenderPass *Draw(SDL_GPUDevice *gpu, SDLFrameContext &context) override {
			const auto &Environment = context.Projection.GetFrame().Environment;
			glm::mat4 shadowProjection = glm::ortho<float>(-30.0f, 30.0f, -30.0f, 30.0f, -50.0f, 150.0f);
			const auto SunDirection = glm::normalize(Environment.SunDirection);
			const auto Up = std::abs(glm::dot(SunDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.99f
								? glm::vec3(0.0f, 0.0f, 1.0f)
								: glm::vec3(0.0f, 1.0f, 0.0f);
			glm::vec3 lightPosition = SunDirection * 40.0f;
			glm::mat4 shadowView = glm::lookAt(lightPosition, glm::vec3(0), Up);
			glm::mat4 shadowMatrix = shadowProjection * shadowView;
			context.ShadowMatrix = shadowMatrix;

			SDL_GPUDepthStencilTargetInfo depthTarget{
				.texture = context.ShadowMapTexture,
				.clear_depth = 1.0f,
				.load_op = SDL_GPU_LOADOP_CLEAR,
				.store_op = SDL_GPU_STOREOP_STORE,
				.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE,
				.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
			};

			auto *White = Environment.SunIntensity > 0.0f ? context.TextureResources->GetWhiteTexture() : nullptr;
			SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(context.Commands, nullptr, 0, &depthTarget);
			if (!pass) throw std::runtime_error("[Renderer:Shadow] Could not begin the shadow render pass");
			if (Environment.SunIntensity <= 0.0f) return pass;
			try {
				DrawCasters(gpu, pass, context, shadowMatrix, White);
			} catch (...) {
				SDL_EndGPURenderPass(pass);
				throw;
			}
			return pass;
		}

	  private:
		void DrawCasters(SDL_GPUDevice *Gpu, SDL_GPURenderPass *Pass, SDLFrameContext &context, const glm::mat4 &ShadowMatrix,
			SDL_GPUTexture *White) {
			SDL_GPUGraphicsPipeline *BoundPipeline = nullptr;
			const SDLGpuMesh *BoundMesh = nullptr;
			SDL_GPUBuffer *BoundPalette = nullptr;
			SDL_GPUTexture *BoundTexture = nullptr;
			SDL_GPUSampler *BoundSampler = nullptr;
			auto *DefaultSampler = context.MaterialSamplers->Get(Gpu, {}, false);
			glm::vec4 BoundAlpha(-1.0f);
			for (const auto &[Object, Projected] : context.Projection.GetObjects()) {
				(void)Object;
				if (!Projected.Visible) continue;
				const auto &item = Projected.Item;
				if (!item.CastShadow) {
					continue;
				}

				const auto *mesh = Projected.Mesh
					? context.MeshResources.Find(*Projected.Mesh) : context.MeshResources.Find(item.Geometry);
				if (!mesh || !mesh->VertexBuffer || !mesh->IndexBuffer) {
					LOG_WARN(
						App,
						"RenderPublication %llu shadow skipped ObjectId %u:%u because its GPU resource is unavailable",
						static_cast<unsigned long long>(context.Projection.GetLastPublicationId()),
						item.Object.Slot,
						item.Object.Generation
					);
					continue;
				}

				Uniforms uniforms{.ShadowMatrix = ShadowMatrix, .PartMatrix = item.ModelMatrix};
				SDL_PushGPUVertexUniformData(context.Commands, 0, &uniforms, sizeof(Uniforms));

				const auto Palette = context.SkinPalettes.Find(
					item.Object, context.Projection.GetAnimationPose(item.Object));
				auto *PaletteBuffer = Palette.Buffer;
				context.ShadowPoseRevisions.insert_or_assign(item.Object, Palette.PoseRevision);

				auto DrawPrimitive = [&](std::uint32_t First, std::uint32_t Count, const RenderMaterialState &Material) {
					const bool AlphaMask = Material.OpacityMode == RenderOpacityMode::Masked;
					if (AlphaMask && Material.BaseColorFactor.a < Material.AlphaCutoff) return;
					const bool TexturedMask = AlphaMask && Material.BaseColorTexture.has_value();
					auto *Selected = Material.DoubleSided ? DoubleSided : Pipeline;
					if (Selected != BoundPipeline) {
						SDL_BindGPUGraphicsPipeline(Pass, Selected);
						BoundPipeline = Selected;
						BoundMesh = nullptr;
						BoundPalette = nullptr;
						BoundTexture = nullptr;
						if (context.Metrics) ++context.Metrics->PipelineSwitches;
					}
					if (mesh != BoundMesh) {
						const SDL_GPUBufferBinding VertexBinding{.buffer = mesh->VertexBuffer, .offset = 0};
						const SDL_GPUBufferBinding IndexBinding{.buffer = mesh->IndexBuffer, .offset = 0};
						SDL_BindGPUVertexBuffers(Pass, 0, &VertexBinding, 1);
						SDL_BindGPUIndexBuffer(Pass, &IndexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
						BoundMesh = mesh;
					}
					if (PaletteBuffer != BoundPalette) {
						SDL_BindGPUVertexStorageBuffers(Pass, 0, &PaletteBuffer, 1);
						BoundPalette = PaletteBuffer;
					}
					auto *Texture = TexturedMask ? context.TextureResources->Find(*Material.BaseColorTexture) : White;
					if (!Texture) throw std::logic_error("[Renderer:Shadow] A mask image has no SDL residency");
					auto *Sampler = TexturedMask ? context.MaterialSamplers->Get(Gpu, Material.TextureSamplers[0], false)
						: DefaultSampler;
					if (Texture != BoundTexture || Sampler != BoundSampler) {
						const SDL_GPUTextureSamplerBinding Binding{Texture, Sampler};
						SDL_BindGPUFragmentSamplers(Pass, 0, &Binding, 1);
						BoundTexture = Texture;
						BoundSampler = Sampler;
					}
					const auto AlphaFactorCutoff = TexturedMask
						? glm::vec4(Material.BaseColorFactor.a, Material.AlphaCutoff, 1, 0) : glm::vec4(0);
					if (AlphaFactorCutoff != BoundAlpha) {
						SDL_PushGPUFragmentUniformData(context.Commands, 0, &AlphaFactorCutoff, sizeof(AlphaFactorCutoff));
						BoundAlpha = AlphaFactorCutoff;
					}
					SDL_DrawGPUIndexedPrimitives(Pass, Count, 1, First, 0, 0);
					if (context.Metrics) ++context.Metrics->DrawCalls;
				};
				if (Projected.Primitives) {
					for (const auto &Primitive : *Projected.Primitives)
						DrawPrimitive(Primitive.FirstIndex, Primitive.IndexCount, Primitive.Material);
				} else DrawPrimitive(0, mesh->IndexCount, Projected.Material);
			}
		}

		SDL_GPUGraphicsPipeline *DoubleSided = nullptr;
	};

	std::unique_ptr<SDLRenderPass> CreateShadowPass(
		SDL_GPUDevice *gpu, SDL_GPUTextureFormat swapchainFormat, SDLRendererMetrics *Metrics
	) {
		return std::make_unique<ShadowPass>(gpu, swapchainFormat, Metrics);
	}

} // namespace gargantuan
