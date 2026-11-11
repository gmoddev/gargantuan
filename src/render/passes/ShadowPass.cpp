#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "gargantuan/Log.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "render/sdl/SDLDirectionalShadowCache.hpp"
#include "render/sdl/SDLShadowCasterCoverage.hpp"
#include "render/sdl/SDLSpotShadowAtlas.hpp"
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
#include <optional>
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
			RecordedCache = nullptr;
			RecordedAtlas = nullptr;
			if (SolidDoubleSided) SDL_ReleaseGPUGraphicsPipeline(Gpu, SolidDoubleSided);
			SolidDoubleSided = nullptr;
			if (SolidPipeline) SDL_ReleaseGPUGraphicsPipeline(Gpu, SolidPipeline);
			SolidPipeline = nullptr;
			SolidShader.Destroy(Gpu);
			if (DoubleSided) SDL_ReleaseGPUGraphicsPipeline(Gpu, DoubleSided);
			DoubleSided = nullptr;
			SDLRenderPass::Destroy(Gpu);
		}

		void OnFrameAborted() override {
			// A recorded depth target is not a reusable map until its command
			// buffer submits. This also clears candidates from an aborted frame
			// followed by a configured-empty shadow frame with no depth pass.
			if (RecordedCache) RecordedCache->Invalidate();
			if (RecordedAtlas) RecordedAtlas->Invalidate();
			RecordedCache = nullptr;
			RecordedAtlas = nullptr;
		}

		void OnFrameSubmitted() override {
			RecordedCache = nullptr;
			RecordedAtlas = nullptr;
		}

		SDL_GPURenderPass *Draw(SDL_GPUDevice *gpu, SDLFrameContext &context) override {
			context.SolidShadowDepthAttempted = false;
			context.SolidShadowDepthActive = false;
			RecordedCache = &context.ShadowCache;
			RecordedAtlas = context.SpotShadowAtlas;
			context.ShadowPoseRevisions.clear();
			context.LegacyHasCasters = false;
			if (context.CascadedShadows) {
				if (context.CascadePlan.Count > MaximumDirectionalCascades ||
					(context.CascadePlan.Count > 0 && !context.CascadeShadowTexture))
					throw std::logic_error("[Renderer:Shadow] Cascaded shadow resources are incoherent");
				if (context.CascadePlan.Count > 0) DrawCascades(gpu, context);
				DrawSpotlights(gpu, context);
				return nullptr;
			}
			const auto &Environment = context.Projection.GetFrame().Environment;
			if (context.ShadowCache.Restore(Environment, context.ShadowMapTexture, context.ShadowMatrix)) {
				context.LegacyHasCasters = context.ShadowCache.HasLegacyCasters();
				context.ShadowCache.MergeCommittedPoses(0, context.ShadowPoseRevisions);
				if (context.Metrics) ++context.Metrics->ShadowCacheHits;
				DrawSpotlights(gpu, context);
				return nullptr;
			}
			context.ShadowCache.BeginRecording();
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
			if (context.Metrics) ++context.Metrics->ShadowMapPasses;
			try {
				bool HasCasters = false;
				const bool AllResident = Environment.SunIntensity <= 0.0f ||
					DrawCasters(gpu, pass, context, shadowMatrix, White, std::nullopt, HasCasters);
				context.LegacyHasCasters = HasCasters;
				context.ShadowCache.FinishRecording(Environment, context.ShadowMapTexture, shadowMatrix, AllResident,
					HasCasters);
			} catch (...) {
				SDL_EndGPURenderPass(pass);
				throw;
			}
			if (context.SpotShadowAtlas && context.LocalShadowTexture) {
				SDL_EndGPURenderPass(pass);
				DrawSpotlights(gpu, context);
				return nullptr;
			}
			return pass;
		}

	  private:
		bool EnsureSolidPipelines(SDL_GPUDevice *Gpu, SDLRendererMetrics *Metrics) {
			if (SolidPipeline && SolidDoubleSided) return true;
			if (SolidPipelineFailed) return false;
			try {
				SolidShader.VertexFilepath = GetSDLShaderPath("shadow.vert");
				SolidShader.VertexStorageBufferCount = 1;
				SolidShader.FragmentFilepath = GetSDLShaderPath("shadow-opaque.frag");
				SolidShader.Init(Gpu, Metrics);
				auto Builder = SDLPipelineBuilder()
					.SetVertexShader(SolidShader.VertexShader)
					.SetFragmentShader(SolidShader.FragmentShader)
					.SetColorEnabled(false)
					.SetDepthEnabled(true)
					.SetDepthFormat(SDL_GPU_TEXTUREFORMAT_D32_FLOAT);
				SolidPipeline = Builder.Build(Gpu, Metrics);
				if (!SolidPipeline)
					throw std::runtime_error("[Renderer:Shadow] Could not create experimental solid depth pipeline");
				auto Info = Builder.BuildInfo();
				Info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
				SolidDoubleSided = SDL_CreateGPUGraphicsPipeline(Gpu, &Info);
				if (!SolidDoubleSided)
					throw std::runtime_error("[Renderer:Shadow] Could not create experimental solid double-sided depth pipeline");
				if (Metrics) ++Metrics->PipelineCreations;
				return true;
			} catch (const std::exception &Error) {
				LOG_WARN(App, "[Renderer:Shadow] Disabled experimental solid depth: %s", Error.what());
				if (SolidDoubleSided) SDL_ReleaseGPUGraphicsPipeline(Gpu, SolidDoubleSided);
				SolidDoubleSided = nullptr;
				if (SolidPipeline) SDL_ReleaseGPUGraphicsPipeline(Gpu, SolidPipeline);
				SolidPipeline = nullptr;
				SolidShader.Destroy(Gpu);
				SolidPipelineFailed = true;
				return false;
			}
		}

		void DrawCascades(SDL_GPUDevice *Gpu, SDLFrameContext &Context) {
			const auto &Environment = Context.Projection.GetFrame().Environment;
			Context.CascadeHasCasters = false;
			Context.ShadowMatrix = Context.CascadePlan.Cascades[0].Matrix;
			auto *White = Environment.SunIntensity > 0.0f ? Context.TextureResources->GetWhiteTexture() : nullptr;
			for (std::uint32_t Index = 0; Index < Context.CascadePlan.Count; ++Index) {
				const auto &Cascade = Context.CascadePlan.Cascades[Index];
				glm::mat4 Restored;
				if (Context.ShadowCache.RestoreCascade(Index, Environment, Context.CascadeShadowTexture,
					Cascade.Matrix, Restored, Cascade.Origin)) {
					Context.ShadowCache.MergeCommittedPoses(Index, Context.ShadowPoseRevisions);
					Context.CascadeHasCasters |= Context.ShadowCache.HasCascadeCasters(Index);
					if (Context.Metrics) ++Context.Metrics->ShadowCacheHits;
					continue;
				}
				Context.ShadowCache.BeginCascadeRecording(Index);
				SDL_GPUDepthStencilTargetInfo Target{
					.texture = Context.CascadeShadowTexture,
					.clear_depth = 1.0f,
					.load_op = SDL_GPU_LOADOP_CLEAR,
					.store_op = SDL_GPU_STOREOP_STORE,
					.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE,
					.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
					.layer = static_cast<Uint8>(Index),
				};
				auto *Pass = SDL_BeginGPURenderPass(Context.Commands, nullptr, 0, &Target);
				if (!Pass) throw std::runtime_error("[Renderer:Shadow] Could not begin cascade render pass");
				if (Context.Metrics) ++Context.Metrics->ShadowMapPasses;
				try {
					bool HasCasters = false;
					const bool AllResident = Environment.SunIntensity <= 0.0f ||
						DrawCasters(Gpu, Pass, Context, Cascade.Matrix, White, Index, HasCasters,
							std::nullopt, Cascade.Origin);
					Context.CascadeHasCasters |= HasCasters;
					Context.ShadowCache.FinishCascadeRecording(Index, Environment, Context.CascadeShadowTexture,
						Cascade.Matrix, AllResident, HasCasters, Cascade.Origin);
				} catch (...) {
					SDL_EndGPURenderPass(Pass);
					throw;
				}
				SDL_EndGPURenderPass(Pass);
			}
		}

		void DrawSpotlights(SDL_GPUDevice *Gpu, SDLFrameContext &Context) {
			if (!Context.SpotShadowAtlas || !Context.LocalShadowTexture) return;
			auto &Atlas = *Context.SpotShadowAtlas;
			Atlas.Prepare(Context.Projection.GetFrame(), Context.LocalShadowTexture);
			auto *White = Context.TextureResources->GetWhiteTexture();
			for (const auto &Assignment : Atlas.GetAssignments()) {
				if (!Assignment.Dirty) {
					Atlas.MergeCommittedPoses(Assignment.Layer, Context.ShadowPoseRevisions);
					continue;
				}
				Atlas.BeginRecording(Assignment.Layer);
				SDL_GPUDepthStencilTargetInfo Target{
					.texture = Context.LocalShadowTexture,
					.clear_depth = 1.0f,
					.load_op = SDL_GPU_LOADOP_CLEAR,
					.store_op = SDL_GPU_STOREOP_STORE,
					.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE,
					.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
					.layer = static_cast<Uint8>(Assignment.Layer),
				};
				auto *Pass = SDL_BeginGPURenderPass(Context.Commands, nullptr, 0, &Target);
				if (!Pass) throw std::runtime_error("[Renderer:Shadow] Could not begin spotlight render pass");
				if (Context.Metrics) ++Context.Metrics->ShadowMapPasses;
				try {
					bool HasCasters = false;
					const bool AllResident = DrawCasters(Gpu, Pass, Context, Assignment.Matrix, White,
						std::nullopt, HasCasters, Assignment.Layer, Assignment.Origin);
					Atlas.FinishRecording(Assignment.Layer, AllResident, HasCasters);
				} catch (...) {
					SDL_EndGPURenderPass(Pass);
					throw;
				}
				SDL_EndGPURenderPass(Pass);
			}
		}

		bool DrawCasters(SDL_GPUDevice *Gpu, SDL_GPURenderPass *Pass, SDLFrameContext &context, const glm::mat4 &ShadowMatrix,
			SDL_GPUTexture *White, std::optional<std::uint32_t> CascadeIndex, bool &HasCasters,
			std::optional<std::uint32_t> SpotLayer = std::nullopt, glm::vec3 Origin = glm::vec3(0.0f)) {
			SDL_GPUGraphicsPipeline *BoundPipeline = nullptr;
			const SDLGpuMesh *BoundMesh = nullptr;
			SDL_GPUBuffer *BoundPalette = nullptr;
			SDL_GPUTexture *BoundTexture = nullptr;
			SDL_GPUSampler *BoundSampler = nullptr;
			auto *DefaultSampler = context.MaterialSamplers->Get(Gpu, {}, false);
			bool AllResident = true;
			glm::vec4 BoundAlpha(-1.0f);
			for (const auto &[Object, Projected] : context.Projection.GetObjects()) {
				(void)Object;
				if (!Projected.Visible) continue;
				const auto &item = Projected.Item;
				if (!item.CastShadow) {
					continue;
				}
				if (CascadeIndex && !SDLShadowCasterMayAffect(context.Projection, item, Projected.Mesh,
					ShadowMatrix, context.CascadePlan.Resolution, Origin)) continue;
				if (SpotLayer && !context.SpotShadowAtlas->MayAffect(context.Projection, Projected, *SpotLayer)) continue;
				if (CascadeIndex) context.ShadowCache.DependOnObject(*CascadeIndex, item.Object);

				const auto *mesh = Projected.Mesh
					? context.MeshResources.Find(*Projected.Mesh) : context.MeshResources.Find(item.Geometry);
				if (!mesh || !mesh->VertexBuffer || !mesh->IndexBuffer) {
					AllResident = false;
					LOG_WARN(
						App,
						"RenderPublication %llu shadow skipped ObjectId %u:%u because its GPU resource is unavailable",
						static_cast<unsigned long long>(context.Projection.GetLastPublicationId()),
						item.Object.Slot,
						item.Object.Generation
					);
					continue;
				}
				if (Projected.Mesh) {
					if (CascadeIndex) context.ShadowCache.DependOnMesh(*CascadeIndex, *Projected.Mesh);
					else if (SpotLayer) context.SpotShadowAtlas->DependOnMesh(*SpotLayer, *Projected.Mesh);
					else context.ShadowCache.DependOnMesh(*Projected.Mesh);
				}

				auto RelativeModel = item.ModelMatrix;
				RelativeModel[3] -= glm::vec4(Origin, 0.0f);
				Uniforms uniforms{.ShadowMatrix = ShadowMatrix, .PartMatrix = RelativeModel};
				SDL_PushGPUVertexUniformData(context.Commands, 0, &uniforms, sizeof(Uniforms));

				const auto Palette = context.SkinPalettes.Find(
					item.Object, context.Projection.GetAnimationPose(item.Object));
				auto *PaletteBuffer = Palette.Buffer;

				auto DrawPrimitive = [&](std::uint32_t First, std::uint32_t Count, const RenderMaterialState &Material) {
					const bool AlphaMask = Material.OpacityMode == RenderOpacityMode::Masked;
					if (AlphaMask && Material.BaseColorFactor.a < Material.AlphaCutoff) return;
					const bool TexturedMask = AlphaMask && Material.BaseColorTexture.has_value();
					if (!AlphaMask && context.UseSolidShadowDepthExperiment &&
						!context.SolidShadowDepthAttempted) {
						context.SolidShadowDepthAttempted = true;
						context.SolidShadowDepthActive = EnsureSolidPipelines(Gpu, context.Metrics);
					}
					const bool SolidDepth = context.SolidShadowDepthActive && !AlphaMask;
					if (TexturedMask) {
						if (CascadeIndex) context.ShadowCache.DependOnMask(*CascadeIndex, *Material.BaseColorTexture);
						else if (SpotLayer) context.SpotShadowAtlas->DependOnMask(*SpotLayer, *Material.BaseColorTexture);
						else context.ShadowCache.DependOnMask(*Material.BaseColorTexture);
					}
					auto *Selected = SolidDepth
						? (Material.DoubleSided ? SolidDoubleSided : SolidPipeline)
						: (Material.DoubleSided ? DoubleSided : Pipeline);
					if (Selected != BoundPipeline) {
						SDL_BindGPUGraphicsPipeline(Pass, Selected);
						BoundPipeline = Selected;
						BoundMesh = nullptr;
						BoundPalette = nullptr;
						BoundTexture = nullptr;
						BoundAlpha = glm::vec4(-1.0f);
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
					if (!SolidDepth) {
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
					}
					SDL_DrawGPUIndexedPrimitives(Pass, Count, 1, First, 0, 0);
					HasCasters = true;
					if (Palette.GpuSkinned) {
						MergeSDLShadowPoseRevision(context.ShadowPoseRevisions, item.Object, Palette.PoseRevision);
						if (CascadeIndex) context.ShadowCache.RecordPose(*CascadeIndex, item.Object, Palette.PoseRevision);
						else if (SpotLayer) context.SpotShadowAtlas->RecordPose(*SpotLayer, item.Object, Palette.PoseRevision);
						else context.ShadowCache.RecordPose(0, item.Object, Palette.PoseRevision);
					}
					if (context.Metrics) {
						++context.Metrics->DrawCalls;
						++context.Metrics->ShadowCasterDraws;
						if (SolidDepth) ++context.Metrics->SolidShadowCasterDraws;
					}
				};
				if (Projected.Primitives) {
					for (const auto &Primitive : *Projected.Primitives)
						DrawPrimitive(Primitive.FirstIndex, Primitive.IndexCount, Primitive.Material);
				} else DrawPrimitive(0, mesh->IndexCount, Projected.Material);
			}
			return AllResident;
		}

		SDL_GPUGraphicsPipeline *DoubleSided = nullptr;
		SDLFileShader SolidShader;
		SDL_GPUGraphicsPipeline *SolidPipeline = nullptr;
		SDL_GPUGraphicsPipeline *SolidDoubleSided = nullptr;
		bool SolidPipelineFailed = false;
		SDLDirectionalShadowCache *RecordedCache = nullptr;
		SDLSpotShadowAtlas *RecordedAtlas = nullptr;
	};

	std::unique_ptr<SDLRenderPass> CreateShadowPass(
		SDL_GPUDevice *gpu, SDL_GPUTextureFormat swapchainFormat, SDLRendererMetrics *Metrics
	) {
		return std::make_unique<ShadowPass>(gpu, swapchainFormat, Metrics);
	}

} // namespace gargantuan
