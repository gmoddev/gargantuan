#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "gargantuan/Log.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "render/sdl/SDLMeshCache.hpp"
#include "render/sdl/SDLPipelineBuilder.hpp"
#include "render/sdl/SDLRenderPass.hpp"
#include "render/sdl/SDLSkinPaletteCache.hpp"
#include "render/sdl/SDLTextureCache.hpp"

#include <SDL3/SDL.h>
#include <array>
#include <cstring>
#include <format>
#include <memory>
#include <string>

namespace gargantuan {
	static const glm::mat4 SHADOW_BIAS_MATRIX{
		//
		0.5f,
		0.0f,
		0.0f,
		0.0f,
		//
		0.0f,
		-0.5f,
		0.0f,
		0.0f,
		//
		0.0f,
		0.0f,
		1.0f,
		0.0f,
		//
		0.5f,
		0.5f,
		0.0f,
		1.0f
	};

	class OpaquePass final : public SDLRenderPass {
	  public:
		static constexpr std::string_view LABEL = "Opaque";

		struct alignas(16) WorldUniforms {
			glm::mat4 ViewMatrix;
			glm::mat4 ProjectionMatrix;
			glm::mat4 ShadowBiasMatrix;
			glm::vec4 SunDirectionIntensity;
			glm::vec4 AmbientExposure;
			glm::vec4 SunColorFogEnabled;
			glm::vec4 FogColorStart;
			glm::vec4 CameraPositionFogEnd;
		};

		struct alignas(16) PartUniforms {
			glm::mat4 ModelMatrix;
			glm::mat4 NormalMatrix;
			glm::vec4 Color;
			glm::vec4 MaterialValues;
		};

		OpaquePass(SDL_GPUDevice *gpu, SDL_GPUTextureFormat swapchainFormat, SDLRendererMetrics *Metrics) {
			try {
				Shader.VertexFilepath = GetSDLShaderPath("opaque.vert");
				Shader.VertexUniformBufferCount = 2;
				Shader.VertexStorageBufferCount = 1;
				Shader.FragmentFilepath = GetSDLShaderPath("opaque.frag");
				Shader.FragmentUniformBufferCount = 1;
				Shader.FragmentSamplerCount = 2;
				Shader.Init(gpu, Metrics);
				auto Builder = SDLPipelineBuilder()
					.SetVertexShader(Shader.VertexShader)
					.SetFragmentShader(Shader.FragmentShader)
					.SetColorEnabled(true)
					.SetColorFormat(swapchainFormat)
					.SetBlendingEnabled(true)
					.SetDepthEnabled(true)
					.SetDepthFormat(SDL_GPU_TEXTUREFORMAT_D16_UNORM);
				Pipeline = Builder.Build(gpu, Metrics);
				auto DoubleSidedInfo = Builder.BuildInfo();
				DoubleSidedInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
				DoubleSidedPipeline = SDL_CreateGPUGraphicsPipeline(gpu, &DoubleSidedInfo);
				if (DoubleSidedPipeline && Metrics) ++Metrics->PipelineCreations;
				SDL_GPUSamplerCreateInfo SamplerInfo{
					.min_filter = SDL_GPU_FILTER_LINEAR,
					.mag_filter = SDL_GPU_FILTER_LINEAR,
					.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
					.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT,
					.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT,
					.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT,
				};
				MaterialSampler = SDL_CreateGPUSampler(gpu, &SamplerInfo);
				if (!Pipeline || !DoubleSidedPipeline || !MaterialSampler)
					throw std::runtime_error(std::format("Failed to create material rendering resources: {}", SDL_GetError()));
			} catch (...) {
				Destroy(gpu);
				throw;
			}
		};

		void Destroy(SDL_GPUDevice *Gpu) override {
			if (DoubleSidedPipeline) SDL_ReleaseGPUGraphicsPipeline(Gpu, DoubleSidedPipeline);
			if (WhiteTexture) SDL_ReleaseGPUTexture(Gpu, WhiteTexture);
			if (WhiteTransferBuffer) SDL_ReleaseGPUTransferBuffer(Gpu, WhiteTransferBuffer);
			if (MaterialSampler) SDL_ReleaseGPUSampler(Gpu, MaterialSampler);
			DoubleSidedPipeline = nullptr;
			WhiteTexture = nullptr;
			WhiteTransferBuffer = nullptr;
			MaterialSampler = nullptr;
			SDLRenderPass::Destroy(Gpu);
		}

		SDL_GPURenderPass *Draw(SDL_GPUDevice *gpu, SDLFrameContext &context) override {
			SDL_GPUColorTargetInfo colorTarget = {
				.texture = context.SwapchainTexture,
				.load_op = SDL_GPU_LOADOP_LOAD,
				.store_op = SDL_GPU_STOREOP_STORE,
			};

			SDL_GPUDepthStencilTargetInfo depthTarget = {
				.texture = context.DepthTexture,
				.clear_depth = 1.0f,
				.load_op = SDL_GPU_LOADOP_CLEAR,
				.store_op = SDL_GPU_STOREOP_DONT_CARE,
				.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE,
				.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
			};

			SDL_GPUTextureSamplerBinding shadowBinding{
				.texture = context.ShadowMapTexture,
				.sampler = context.ShadowSampler,
			};

			auto pass = SDL_BeginGPURenderPass(context.Commands, &colorTarget, 1, &depthTarget);
			EnsureWhiteTexture(gpu, context.Metrics);
			SDL_BindGPUGraphicsPipeline(pass, Pipeline);
			if (context.Metrics) ++context.Metrics->PipelineSwitches;

			const auto &Frame = context.Projection.GetFrame();
			const auto &Environment = Frame.Environment;
			WorldUniforms worldUniforms{
				.ViewMatrix = Frame.Camera.ViewMatrix,
				.ProjectionMatrix = Frame.Camera.ProjectionMatrix,
				.ShadowBiasMatrix = SHADOW_BIAS_MATRIX * context.ShadowMatrix,
				.SunDirectionIntensity = glm::vec4(Environment.SunDirection, Environment.SunIntensity),
				.AmbientExposure = glm::vec4(Environment.AmbientColor, Environment.ExposureMultiplier),
				.SunColorFogEnabled = glm::vec4(Environment.SunColor, Environment.Fog.Enabled ? 1.0f : 0.0f),
				.FogColorStart = glm::vec4(Environment.Fog.Color, Environment.Fog.Start),
				.CameraPositionFogEnd = glm::vec4(Frame.Camera.Position, Environment.Fog.End),
			};
			SDL_PushGPUVertexUniformData(context.Commands, 0, &worldUniforms, sizeof(WorldUniforms));
			SDL_PushGPUFragmentUniformData(context.Commands, 0, &worldUniforms, sizeof(WorldUniforms));

			for (const auto &[Object, Projected] : context.Projection.GetObjects()) {
				(void)Object;
				if (!Projected.Visible) continue;
				const auto &item = Projected.Item;
				const auto *mesh = Projected.Mesh
					? context.MeshResources.Find(*Projected.Mesh) : context.MeshResources.Find(item.Geometry);
				if (!mesh || !mesh->VertexBuffer || !mesh->IndexBuffer) {
					LOG_WARN(
						App,
						"RenderPublication %llu skipped ObjectId %u:%u because its GPU resource is unavailable",
						static_cast<unsigned long long>(context.Projection.GetLastPublicationId()),
						item.Object.Slot,
						item.Object.Generation
					);
					continue;
				}

				SDL_GPUBufferBinding vertexBinding{.buffer = mesh->VertexBuffer, .offset = 0};
				SDL_BindGPUVertexBuffers(pass, 0, &vertexBinding, 1);

				SDL_GPUBufferBinding indexBinding{.buffer = mesh->IndexBuffer, .offset = 0};
				SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
				const auto Palette = context.SkinPalettes.Find(
					item.Object, context.Projection.GetAnimationPose(item.Object));
				auto *PaletteBuffer = Palette.Buffer;
				SDL_BindGPUVertexStorageBuffers(pass, 0, &PaletteBuffer, 1);
				if (item.CastShadow && Environment.SunIntensity > 0.0f) {
					const auto ShadowRevision = context.ShadowPoseRevisions.find(item.Object);
					if (ShadowRevision == context.ShadowPoseRevisions.end() ||
						ShadowRevision->second != Palette.PoseRevision) {
						if (context.Metrics) ++context.Metrics->MainShadowPoseMismatches;
						throw std::logic_error("Opaque and shadow passes resolved different animation pose revisions");
					}
				}

				auto DrawPrimitive = [&](std::uint32_t FirstIndex, std::uint32_t IndexCount,
					const RenderMaterialState &Material) {
					auto *MaterialPipeline = Material.DoubleSided ? DoubleSidedPipeline : Pipeline;
					SDL_BindGPUGraphicsPipeline(pass, MaterialPipeline);
					if (context.Metrics) ++context.Metrics->PipelineSwitches;
					auto *BaseColorTexture = Material.BaseColorTexture && context.TextureResources ?
						context.TextureResources->Find(*Material.BaseColorTexture) : WhiteTexture;
					if (!BaseColorTexture) throw std::logic_error("Material references a texture without SDL residency");
					const std::array<SDL_GPUTextureSamplerBinding, 2> TextureBindings{{
						shadowBinding,
						{.texture = BaseColorTexture, .sampler = MaterialSampler},
					}};
					SDL_BindGPUFragmentSamplers(pass, 0, TextureBindings.data(), TextureBindings.size());
					PartUniforms Uniforms{
						.ModelMatrix = item.ModelMatrix,
						.NormalMatrix = glm::transpose(item.InverseModelMatrix),
						.Color = Material.BaseColorFactor,
						.MaterialValues = {Material.AlphaCutoff, static_cast<float>(Material.OpacityMode),
							Material.Metallic, Material.Roughness},
					};
					SDL_PushGPUVertexUniformData(context.Commands, 1, &Uniforms, sizeof(PartUniforms));
					SDL_DrawGPUIndexedPrimitives(pass, IndexCount, 1, FirstIndex, 0, 0);
					if (context.Metrics) ++context.Metrics->DrawCalls;
				};
				if (Projected.Primitives) {
					for (const auto &Primitive : *Projected.Primitives)
						DrawPrimitive(Primitive.FirstIndex, Primitive.IndexCount, Primitive.Material);
				} else DrawPrimitive(0, mesh->IndexCount, Projected.Material);
			}

			return pass;
		};

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
				throw std::runtime_error(std::format("Failed to create the material white texture: {}", SDL_GetError()));
			auto *Mapped = static_cast<std::uint8_t *>(SDL_MapGPUTransferBuffer(Gpu, WhiteTransferBuffer, false));
			if (!Mapped) throw std::runtime_error(std::format("Failed to map the material white texture: {}", SDL_GetError()));
			std::memset(Mapped, 0xff, 4);
			SDL_UnmapGPUTransferBuffer(Gpu, WhiteTransferBuffer);
			auto *Commands = SDL_AcquireGPUCommandBuffer(Gpu);
			if (!Commands) throw std::runtime_error(std::format("Failed to acquire material texture commands: {}", SDL_GetError()));
			auto *CopyPass = SDL_BeginGPUCopyPass(Commands);
			if (!CopyPass) {
				SDL_CancelGPUCommandBuffer(Commands);
				throw std::runtime_error(std::format("Failed to begin material texture upload: {}", SDL_GetError()));
			}
			SDL_GPUTextureTransferInfo Source{.transfer_buffer = WhiteTransferBuffer, .pixels_per_row = 1, .rows_per_layer = 1};
			SDL_GPUTextureRegion Destination{.texture = WhiteTexture, .w = 1, .h = 1, .d = 1};
			SDL_UploadToGPUTexture(CopyPass, &Source, &Destination, false);
			SDL_EndGPUCopyPass(CopyPass);
			if (!SDL_SubmitGPUCommandBuffer(Commands))
				throw std::runtime_error(std::format("Failed to upload the material white texture: {}", SDL_GetError()));
			if (Metrics) {
				++Metrics->TextureCreations;
				++Metrics->TransferBufferCreations;
				++Metrics->UploadOperations;
				Metrics->UploadedBytes += 4;
			}
		}

		SDL_GPUGraphicsPipeline *DoubleSidedPipeline = nullptr;
		SDL_GPUTexture *WhiteTexture = nullptr;
		SDL_GPUTransferBuffer *WhiteTransferBuffer = nullptr;
		SDL_GPUSampler *MaterialSampler = nullptr;
	};

	std::unique_ptr<SDLRenderPass> CreateOpaquePass(
		SDL_GPUDevice *gpu, SDL_GPUTextureFormat swapchainFormat, SDLRendererMetrics *Metrics
	) {
		return std::make_unique<OpaquePass>(gpu, swapchainFormat, Metrics);
	}
} // namespace gargantuan
