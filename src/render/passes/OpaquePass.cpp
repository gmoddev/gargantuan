#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "gargantuan/Log.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "render/sdl/SDLMeshCache.hpp"
#include "render/sdl/SDLPipelineBuilder.hpp"
#include "render/sdl/SDLRenderPass.hpp"
#include "render/sdl/SDLSceneDepth.hpp"
#include "render/sdl/SDLSkinPaletteCache.hpp"
#include "render/sdl/SDLTextureCache.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <vector>

namespace gargantuan {
	namespace {
		using ClipPlanes = std::array<glm::vec4, 6>;
		constexpr RenderBounds PrimitiveBounds{glm::vec3(-0.5f), glm::vec3(0.5f)};

		ClipPlanes GetClipPlanes(const RenderCameraSnapshot &Camera) {
			const auto Rows = glm::transpose(Camera.ProjectionMatrix * Camera.ViewMatrix);
			return {Rows[3] + Rows[0], Rows[3] - Rows[0], Rows[3] + Rows[1], Rows[3] - Rows[1],
				Rows[2], Rows[3] - Rows[2]};
		}

		bool OutsideView(const ClipPlanes &Planes, const glm::mat4 &Model, const RenderBounds &Bounds) {
			const auto Center = Bounds.Minimum * 0.5f + Bounds.Maximum * 0.5f;
			const auto Extent = Bounds.Maximum * 0.5f - Bounds.Minimum * 0.5f;
			const auto ToLocal = glm::transpose(Model);
			for (const auto &Plane : Planes) {
				const auto LocalPlane = ToLocal * Plane;
				const auto Normal = glm::vec3(LocalPlane);
				const auto AbsoluteNormal = glm::abs(Normal);
				const auto Radius = glm::dot(AbsoluteNormal, Extent);
				const auto Distance = glm::dot(Normal, Center) + LocalPlane.w;
				const auto Margin = 1e-5f * (1.0f + glm::dot(AbsoluteNormal, glm::abs(Center)) +
					std::abs(LocalPlane.w) + Radius);
				if (std::isfinite(Distance) && std::isfinite(Radius) && Distance + Radius < -Margin)
					return true;
			}
			return false;
		}
	}

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
			glm::vec4 MaterialFlags;
		};

		struct WorldDraw {
			const RenderProjectedObject *Object;
			const SDLGpuMesh *Mesh;
			const RenderAnimationPoseUpdate *Pose;
			float Depth;
		};

		OpaquePass(SDL_GPUDevice *gpu, SDL_GPUTextureFormat swapchainFormat, SDLRendererMetrics *Metrics) {
			try {
				Shader.VertexFilepath = GetSDLShaderPath("opaque.vert");
				Shader.VertexUniformBufferCount = 2;
				Shader.VertexStorageBufferCount = 1;
				Shader.FragmentFilepath = GetSDLShaderPath("opaque.frag");
				Shader.FragmentUniformBufferCount = 1;
				Shader.FragmentSamplerCount = 4;
				EncodeSrgbOutput = swapchainFormat != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB &&
					swapchainFormat != SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
				Shader.Init(gpu, Metrics);
				auto Builder = SDLPipelineBuilder()
					.SetVertexShader(Shader.VertexShader)
					.SetFragmentShader(Shader.FragmentShader)
					.SetColorEnabled(true)
					.SetColorFormat(swapchainFormat)
					.SetBlendingEnabled(false)
					.SetDepthEnabled(true)
					.SetDepthFormat(SDLSceneDepthFormat);
				Pipeline = Builder.Build(gpu, Metrics);
				auto DoubleSidedInfo = Builder.BuildInfo();
				DoubleSidedInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
				DoubleSidedPipeline = SDL_CreateGPUGraphicsPipeline(gpu, &DoubleSidedInfo);
				if (DoubleSidedPipeline && Metrics) ++Metrics->PipelineCreations;
				Builder.SetBlendingEnabled(true);
				auto BlendedInfo = Builder.BuildInfo();
				BlendedInfo.depth_stencil_state.enable_depth_write = false;
				BlendedPipeline = SDL_CreateGPUGraphicsPipeline(gpu, &BlendedInfo);
				if (BlendedPipeline && Metrics) ++Metrics->PipelineCreations;
				BlendedInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
				BlendedDoubleSidedPipeline = SDL_CreateGPUGraphicsPipeline(gpu, &BlendedInfo);
				if (BlendedDoubleSidedPipeline && Metrics) ++Metrics->PipelineCreations;
				SDL_GPUSamplerCreateInfo SamplerInfo{
					.min_filter = SDL_GPU_FILTER_LINEAR,
					.mag_filter = SDL_GPU_FILTER_LINEAR,
					.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
					.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT,
					.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT,
					.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT,
				};
				MaterialSampler = SDL_CreateGPUSampler(gpu, &SamplerInfo);
				if (!Pipeline || !DoubleSidedPipeline || !BlendedPipeline || !BlendedDoubleSidedPipeline || !MaterialSampler)
					throw std::runtime_error(std::format("Failed to create material rendering resources: {}", SDL_GetError()));
			} catch (...) {
				Destroy(gpu);
				throw;
			}
		};

		void Destroy(SDL_GPUDevice *Gpu) override {
			if (DoubleSidedPipeline) SDL_ReleaseGPUGraphicsPipeline(Gpu, DoubleSidedPipeline);
			if (BlendedPipeline) SDL_ReleaseGPUGraphicsPipeline(Gpu, BlendedPipeline);
			if (BlendedDoubleSidedPipeline) SDL_ReleaseGPUGraphicsPipeline(Gpu, BlendedDoubleSidedPipeline);
			if (MaterialSampler) SDL_ReleaseGPUSampler(Gpu, MaterialSampler);
			DoubleSidedPipeline = nullptr;
			BlendedPipeline = nullptr;
			BlendedDoubleSidedPipeline = nullptr;
			MaterialSampler = nullptr;
			SDLRenderPass::Destroy(Gpu);
		}

		SDL_GPURenderPass *Draw(SDL_GPUDevice *gpu, SDLFrameContext &context) override {
			(void)gpu;
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

			auto *White = context.TextureResources->GetWhiteTexture();
			auto *Pass = SDL_BeginGPURenderPass(context.Commands, &colorTarget, 1, &depthTarget);
			if (!Pass) throw std::runtime_error("[Renderer:Material] Could not begin the world render pass");
			try {
				DrawWorld(Pass, context, shadowBinding, White);
			} catch (...) {
				TransparentDraws.clear();
				SDL_EndGPURenderPass(Pass);
				throw;
			}
			return Pass;
		}

	  private:
		void DrawWorld(SDL_GPURenderPass *Pass, SDLFrameContext &Context, SDL_GPUTextureSamplerBinding ShadowBinding,
			SDL_GPUTexture *White) {
			const auto &Frame = Context.Projection.GetFrame();
			const auto &Environment = Frame.Environment;
			const WorldUniforms Uniforms{
				.ViewMatrix = Frame.Camera.ViewMatrix,
				.ProjectionMatrix = Frame.Camera.ProjectionMatrix,
				.ShadowBiasMatrix = SHADOW_BIAS_MATRIX * Context.ShadowMatrix,
				.SunDirectionIntensity = glm::vec4(Environment.SunDirection, Environment.SunIntensity),
				.AmbientExposure = glm::vec4(Environment.AmbientColor, Environment.ExposureMultiplier),
				.SunColorFogEnabled = glm::vec4(Environment.SunColor, Environment.Fog.Enabled ? 1.0f : 0.0f),
				.FogColorStart = glm::vec4(Environment.Fog.Color, Environment.Fog.Start),
				.CameraPositionFogEnd = glm::vec4(Frame.Camera.Position, Environment.Fog.End),
			};
			SDL_PushGPUVertexUniformData(Context.Commands, 0, &Uniforms, sizeof(Uniforms));
			SDL_PushGPUFragmentUniformData(Context.Commands, 0, &Uniforms, sizeof(Uniforms));

			const auto Planes = GetClipPlanes(Frame.Camera);
			SDL_GPUGraphicsPipeline *BoundPipeline = nullptr;
			const SDLGpuMesh *BoundMesh = nullptr;
			const RenderProjectedObject *BoundObject = nullptr;
			SDL_GPUBuffer *BoundPalette = nullptr;
			std::array<SDL_GPUTexture *, 3> BoundTextures{};
			TransparentDraws.clear();

			auto ResolveTexture = [&](const std::optional<RenderTextureIdentity> &Identity) {
				return Identity ? Context.TextureResources->Find(*Identity) : White;
			};
			auto DrawPrimitive = [&](const WorldDraw &Draw, std::uint32_t FirstIndex, std::uint32_t IndexCount,
				const RenderMaterialState &Material) {
				const auto &Item = Draw.Object->Item;
				const bool Transparent = Material.OpacityMode == RenderOpacityMode::Transparent;
				auto *MaterialPipeline = Transparent
					? (Material.DoubleSided ? BlendedDoubleSidedPipeline : BlendedPipeline)
					: (Material.DoubleSided ? DoubleSidedPipeline : Pipeline);
				if (MaterialPipeline != BoundPipeline) {
					SDL_BindGPUGraphicsPipeline(Pass, MaterialPipeline);
					BoundPipeline = MaterialPipeline;
					BoundMesh = nullptr;
					BoundObject = nullptr;
					BoundPalette = nullptr;
					BoundTextures = {};
					if (Context.Metrics) ++Context.Metrics->PipelineSwitches;
				}
				if (Draw.Mesh != BoundMesh) {
					const SDL_GPUBufferBinding VertexBinding{.buffer = Draw.Mesh->VertexBuffer, .offset = 0};
					const SDL_GPUBufferBinding IndexBinding{.buffer = Draw.Mesh->IndexBuffer, .offset = 0};
					SDL_BindGPUVertexBuffers(Pass, 0, &VertexBinding, 1);
					SDL_BindGPUIndexBuffer(Pass, &IndexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
					BoundMesh = Draw.Mesh;
					if (Context.Metrics) ++Context.Metrics->WorldGeometryBindings;
				}
				if (Draw.Object != BoundObject) {
					const auto Palette = Context.SkinPalettes.Find(Item.Object, Draw.Pose);
					if (Palette.Buffer != BoundPalette) {
						auto *PaletteBuffer = Palette.Buffer;
						SDL_BindGPUVertexStorageBuffers(Pass, 0, &PaletteBuffer, 1);
						BoundPalette = PaletteBuffer;
					}
					if (Item.CastShadow && Environment.SunIntensity > 0.0f) {
						const auto ShadowRevision = Context.ShadowPoseRevisions.find(Item.Object);
						if (ShadowRevision == Context.ShadowPoseRevisions.end() || ShadowRevision->second != Palette.PoseRevision) {
							if (Context.Metrics) ++Context.Metrics->MainShadowPoseMismatches;
							throw std::logic_error("[Renderer:Material] World and shadow passes resolved different animation poses");
						}
					}
					BoundObject = Draw.Object;
				}
				const std::array Textures{
					ResolveTexture(Material.BaseColorTexture), ResolveTexture(Material.NormalTexture),
					ResolveTexture(Material.MetallicRoughnessTexture),
				};
				if (std::ranges::find(Textures, nullptr) != Textures.end())
					throw std::logic_error("[Renderer:Material] A material texture has no SDL residency");
				if (Textures != BoundTextures) {
					const std::array<SDL_GPUTextureSamplerBinding, 4> Bindings{{
						ShadowBinding, {Textures[0], MaterialSampler}, {Textures[1], MaterialSampler}, {Textures[2], MaterialSampler},
					}};
					SDL_BindGPUFragmentSamplers(Pass, 0, Bindings.data(), Bindings.size());
					BoundTextures = Textures;
					if (Context.Metrics) ++Context.Metrics->WorldMaterialBindings;
				}
				const float Shading = Material.MetallicRoughnessTexture ? 2.0f :
					Material.ShadingModel == RenderShadingModel::MetallicRoughness ? 1.0f : 0.0f;
				const PartUniforms Part{
					.ModelMatrix = Item.ModelMatrix,
					.NormalMatrix = glm::transpose(Item.InverseModelMatrix),
					.Color = Material.BaseColorFactor,
					.MaterialValues = {Material.AlphaCutoff, static_cast<float>(Material.OpacityMode),
						Material.Metallic, Material.Roughness},
					.MaterialFlags = {Shading, Material.BaseColorTexture ? 1.0f : 0.0f,
						Material.NormalTexture ? 1.0f : 0.0f, EncodeSrgbOutput ? 1.0f : 0.0f},
				};
				SDL_PushGPUVertexUniformData(Context.Commands, 1, &Part, sizeof(PartUniforms));
				SDL_DrawGPUIndexedPrimitives(Pass, IndexCount, 1, FirstIndex, 0, 0);
				if (Context.Metrics) ++Context.Metrics->DrawCalls;
			};
			auto ForPrimitives = [](const WorldDraw &Draw, auto &&Visit) {
				if (Draw.Object->Primitives) {
					for (const auto &Primitive : *Draw.Object->Primitives)
						Visit(Primitive.FirstIndex, Primitive.IndexCount, Primitive.Material);
				} else Visit(0, Draw.Mesh->IndexCount, Draw.Object->Material);
			};

			for (const auto &[Object, Projected] : Context.Projection.GetObjects()) {
				if (!Projected.Visible) continue;
				const auto &Item = Projected.Item;
				const auto *Pose = Context.Projection.GetAnimationPose(Object);
				const auto *Bounds = Projected.Mesh ? Context.Projection.GetMeshBounds(*Projected.Mesh) : &PrimitiveBounds;
				if (Bounds && (!Pose || Pose->Mode != RenderAnimationSkinningMode::GpuPalette) &&
					OutsideView(Planes, Item.ModelMatrix, *Bounds)) {
					if (Context.Metrics) ++Context.Metrics->CameraCulledObjects;
					continue;
				}
				const auto *Mesh = Projected.Mesh
					? Context.MeshResources.Find(*Projected.Mesh) : Context.MeshResources.Find(Item.Geometry);
				if (!Mesh || !Mesh->VertexBuffer || !Mesh->IndexBuffer) {
					LOG_WARN(App, "[Renderer:Material] Publication %llu skipped ObjectId %u:%u with unavailable GPU geometry",
						static_cast<unsigned long long>(Context.Projection.GetLastPublicationId()), Object.Slot, Object.Generation);
					continue;
				}

				WorldDraw Draw{&Projected, Mesh, Pose, 0.0f};
				bool HasTransparent = false;
				ForPrimitives(Draw, [&](std::uint32_t First, std::uint32_t Count, const RenderMaterialState &Material) {
					if (Material.OpacityMode == RenderOpacityMode::Transparent) HasTransparent = true;
					else DrawPrimitive(Draw, First, Count, Material);
				});
				if (HasTransparent) {
					const auto Center = Bounds ? Bounds->Minimum * 0.5f + Bounds->Maximum * 0.5f : glm::vec3(0.0f);
					const auto Depth = -(Frame.Camera.ViewMatrix * Item.ModelMatrix * glm::vec4(Center, 1.0f)).z;
					Draw.Depth = std::isfinite(Depth) ? Depth : 0.0f;
					TransparentDraws.push_back(Draw);
				}
			}

			std::ranges::sort(TransparentDraws, [](const WorldDraw &Left, const WorldDraw &Right) {
				if (Left.Depth != Right.Depth) return Left.Depth > Right.Depth;
				return Left.Object->Item.Object < Right.Object->Item.Object;
			});
			for (const auto &Draw : TransparentDraws) {
				ForPrimitives(Draw, [&](std::uint32_t First, std::uint32_t Count, const RenderMaterialState &Material) {
					if (Material.OpacityMode == RenderOpacityMode::Transparent) DrawPrimitive(Draw, First, Count, Material);
				});
			}
			TransparentDraws.clear();
		}

		SDL_GPUGraphicsPipeline *DoubleSidedPipeline = nullptr;
		SDL_GPUGraphicsPipeline *BlendedPipeline = nullptr;
		SDL_GPUGraphicsPipeline *BlendedDoubleSidedPipeline = nullptr;
		std::vector<WorldDraw> TransparentDraws;
		SDL_GPUSampler *MaterialSampler = nullptr;
		bool EncodeSrgbOutput = true;
	};

	std::unique_ptr<SDLRenderPass> CreateOpaquePass(
		SDL_GPUDevice *gpu, SDL_GPUTextureFormat swapchainFormat, SDLRendererMetrics *Metrics
	) {
		return std::make_unique<OpaquePass>(gpu, swapchainFormat, Metrics);
	}
} // namespace gargantuan
