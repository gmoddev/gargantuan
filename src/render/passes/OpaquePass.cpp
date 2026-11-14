#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#include "gargantuan/Log.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "render/sdl/SDLMaterialSamplers.hpp"
#include "render/sdl/SDLLocalLightGrid.hpp"
#include "render/sdl/SDLInstanceGpuCache.hpp"
#include "render/sdl/SDLMeshCache.hpp"
#include "render/sdl/SDLPipelineBuilder.hpp"
#include "render/sdl/SDLRenderPass.hpp"
#include "render/sdl/SDLSceneColor.hpp"
#include "render/sdl/SDLSceneDepth.hpp"
#include "render/sdl/SDLSkinPaletteCache.hpp"
#include "render/sdl/SDLTextureCache.hpp"
#include "render/sdl/SDLWorldUniforms.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace gargantuan {
	namespace {
		using ClipPlanes = std::array<glm::vec4, 6>;
		constexpr RenderBounds PrimitiveBounds{glm::vec3(-0.5f), glm::vec3(0.5f)};
		constexpr std::size_t MAX_OPAQUE_BATCH_OBJECTS = 512;
		constexpr std::int32_t CulledInstanceAction = -3;
		constexpr std::size_t MAX_OPAQUE_BATCH_RANGES = 8;
		constexpr unsigned COVERAGE_GRID_SIZE = 128;
		constexpr unsigned MAX_CELLS_PER_OBJECT = 16;
		constexpr unsigned MAX_PAIR_TESTS_PER_OBJECT = 96;
		constexpr double MAX_TRANSFORM_MAGNITUDE = 1e6;
		constexpr double CLIP_ROUNDOFF = 64.0 * std::numeric_limits<float>::epsilon();
		constexpr double DEPTH_PADDING = 1.0 / 4096.0;

		auto BindingLayout(const RenderPrimitiveMaterialState &Primitive) {
			const auto &Material = Primitive.Material;
			return std::tie(Material.OpacityMode, Material.DoubleSided, Material.ShadingModel, Material.BaseColorTexture,
				Material.NormalTexture, Material.MetallicRoughnessTexture, Material.EmissiveTexture,
				Material.OcclusionTexture, Material.TextureSamplers);
		}

		glm::dmat4 Absolute(const glm::dmat4 &Matrix) {
			return {glm::abs(Matrix[0]), glm::abs(Matrix[1]), glm::abs(Matrix[2]), glm::abs(Matrix[3])};
		}

		bool BoundedMatrix(const glm::dmat4 &Matrix) {
			const glm::dvec4 Limit(MAX_TRANSFORM_MAGNITUDE);
			return glm::all(glm::lessThanEqual(glm::abs(Matrix[0]), Limit)) &&
				glm::all(glm::lessThanEqual(glm::abs(Matrix[1]), Limit)) &&
				glm::all(glm::lessThanEqual(glm::abs(Matrix[2]), Limit)) &&
				glm::all(glm::lessThanEqual(glm::abs(Matrix[3]), Limit));
		}

		struct OpaqueCoverage {
			glm::dvec3 Minimum;
			glm::dvec3 Maximum;
			unsigned Left, Right, Bottom, Top;
			[[nodiscard]] unsigned CellCount() const { return (Right - Left + 1) * (Top - Bottom + 1); }
		};

		struct CoverageProjector {
			glm::dmat4 ViewProjection;
			glm::dmat4 Magnitude;
			glm::dvec2 ScreenPadding;

			[[nodiscard]] std::optional<OpaqueCoverage> Project(const glm::mat4 &Model, const RenderBounds &Bounds) const {
				const glm::dmat4 Local(Model);
				const glm::dvec3 Minimum(Bounds.Minimum), Maximum(Bounds.Maximum);
				const auto LocalMagnitude = glm::max(glm::abs(Minimum), glm::abs(Maximum));
				if (!BoundedMatrix(Local) || !glm::all(glm::lessThanEqual(LocalMagnitude,
					glm::dvec3(MAX_TRANSFORM_MAGNITUDE)))) return std::nullopt;
				const auto ToClip = ViewProjection * Local;
				const auto Extent = (Maximum - Minimum) * 0.5;
				const auto Center = ToClip * glm::dvec4((Maximum + Minimum) * 0.5, 1.0);
				// Bound float matrix/dot association and contraction using absolute products,
				// so cancellations do not shrink the error allowance.
				const auto Error = Magnitude * (Absolute(Local) * glm::dvec4(LocalMagnitude, 1.0)) * CLIP_ROUNDOFF;
				const auto MinimumW = Center.w - std::abs(ToClip[0].w) * Extent.x -
					std::abs(ToClip[1].w) * Extent.y - std::abs(ToClip[2].w) * Extent.z - Error.w - 1e-12;
				if (MinimumW <= 1e-6) return std::nullopt;
				const auto Ndc = glm::dvec3(Center) / Center.w;
				// Bound (clip.xyz - Ndc * clip.w) / clip.w. Subtracting the
				// center ratio before finding extrema retains z/w correlation.
				const auto Offset = [&](const glm::dvec4 &Column) { return glm::abs(glm::dvec3(Column) - Ndc * Column.w); };
				const auto Radius = (Offset(ToClip[0]) * Extent.x + Offset(ToClip[1]) * Extent.y +
					Offset(ToClip[2]) * Extent.z + glm::dvec3(Error) + glm::abs(Ndc) * Error.w +
					glm::dvec3(1e-12)) / MinimumW + glm::dvec3(ScreenPadding, 0.0);
				auto Low = Ndc - Radius;
				auto High = Ndc + Radius;
				// Preserve the original order for any clipping or out-of-range depth.
				if (!glm::all(glm::greaterThan(Low, glm::dvec3(-1.0, -1.0, 0.0))) ||
					!glm::all(glm::lessThan(High, glm::dvec3(1.0)))) return std::nullopt;
				Low.z -= DEPTH_PADDING;
				High.z += DEPTH_PADDING;
				const auto Cell = [](double Axis) {
					return std::min(static_cast<unsigned>((Axis + 1.0) * (COVERAGE_GRID_SIZE * 0.5)), COVERAGE_GRID_SIZE - 1);
				};
				return OpaqueCoverage{Low, High, Cell(Low.x), Cell(High.x), Cell(Low.y), Cell(High.y)};
			}
		};

		class OpaqueCoverageGrid {
		  public:
			enum class Admission { Safe, Conflict, Budget };

			[[nodiscard]] Admission Check(const OpaqueCoverage &Candidate, SDLRendererMetrics *Metrics) const {
				unsigned PairTests = 0;
				unsigned DepthProofs = 0;
				const auto Finish = [&](Admission Value) {
					if (Metrics) {
						Metrics->OpaqueGroupingPairTests += PairTests;
						Metrics->OpaqueGroupingDepthProofs += DepthProofs;
					}
					return Value;
				};
				for (unsigned Y = Candidate.Bottom; Y <= Candidate.Top; ++Y) {
					for (unsigned X = Candidate.Left; X <= Candidate.Right; ++X) {
						for (auto Link = Heads[Y * COVERAGE_GRID_SIZE + X]; Link; Link = Links[Link - 1].Previous) {
							if (PairTests == MAX_PAIR_TESTS_PER_OBJECT) return Finish(Admission::Budget);
							++PairTests;
							const auto &Other = Coverage[Links[Link - 1].Object];
							if (Candidate.Maximum.x < Other.Minimum.x || Other.Maximum.x < Candidate.Minimum.x ||
								Candidate.Maximum.y < Other.Minimum.y || Other.Maximum.y < Candidate.Minimum.y) continue;
							if (Candidate.Maximum.z < Other.Minimum.z || Other.Maximum.z < Candidate.Minimum.z) {
								++DepthProofs;
								continue;
							}
							return Finish(Admission::Conflict);
						}
					}
				}
				return Finish(Admission::Safe);
			}

			void Add(const OpaqueCoverage &Candidate, std::uint16_t Object) {
				Coverage[Object] = Candidate;
				for (unsigned Y = Candidate.Bottom; Y <= Candidate.Top; ++Y) {
					for (unsigned X = Candidate.Left; X <= Candidate.Right; ++X) {
						const auto Cell = static_cast<std::uint16_t>(Y * COVERAGE_GRID_SIZE + X);
						if (!Heads[Cell]) Touched[TouchedCount++] = Cell;
						Links[LinkCount] = {Object, Heads[Cell]};
						Heads[Cell] = ++LinkCount;
					}
				}
			}

			void Clear() noexcept {
				while (TouchedCount) Heads[Touched[--TouchedCount]] = 0;
				LinkCount = 0;
			}

		  private:
			struct Link { std::uint16_t Object, Previous; };
			static constexpr auto Capacity = MAX_OPAQUE_BATCH_OBJECTS * MAX_CELLS_PER_OBJECT;
			static_assert(Capacity < std::numeric_limits<std::uint16_t>::max());
			std::array<OpaqueCoverage, MAX_OPAQUE_BATCH_OBJECTS> Coverage;
			std::array<std::uint16_t, COVERAGE_GRID_SIZE * COVERAGE_GRID_SIZE> Heads{};
			std::array<std::uint16_t, Capacity> Touched;
			std::array<Link, Capacity> Links;
			std::uint16_t LinkCount = 0;
			std::uint16_t TouchedCount = 0;
		};

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

		bool OutsideSceneEnvelope(const ClipPlanes &Planes, const RenderSceneEnvelope &Envelope) {
			for (const auto &FloatPlane : Planes) {
				for (int Axis = 0; Axis < 4; ++Axis)
					if (!std::isfinite(FloatPlane[Axis]) || std::abs(FloatPlane[Axis]) > 1e7f) return false;
				const glm::dvec4 Plane(FloatPlane);
				glm::dvec3 Support;
				for (int Axis = 0; Axis < 3; ++Axis)
					Support[Axis] = Plane[Axis] >= 0.0 ? Envelope.Maximum[Axis] : Envelope.Minimum[Axis];
				const double Distance = glm::dot(glm::dvec3(Plane), Support) + Plane.w;
				const auto AbsoluteExtent = glm::max(glm::abs(Envelope.Minimum), glm::abs(Envelope.Maximum));
				const double Margin = 1e-4 * (1.0 + glm::dot(glm::abs(glm::dvec3(Plane)), AbsoluteExtent) + std::abs(Plane.w));
				if (std::isfinite(Distance) && std::isfinite(Margin) && Distance < -Margin) return true;
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

		using WorldUniforms = SDLWorldUniforms;

		struct alignas(16) PartUniforms {
			glm::mat4 ModelMatrix;
			glm::mat4 NormalMatrix;
			glm::vec4 Color;
			glm::vec4 MaterialValues;
			glm::vec4 MaterialFlags;
			glm::vec4 EmissiveOcclusion;
			glm::vec4 MaterialControls;
			glm::vec4 ColorSamplerModes;
		};

		struct WorldDraw {
			const RenderProjectedObject *Object;
			const SDLGpuMesh *Mesh;
			const RenderAnimationPoseUpdate *Pose;
			float Depth;
		};
		struct InstanceBatch {
			const SDLGpuMesh *Mesh = nullptr;
			std::uint32_t FirstIndex = 0;
			std::uint32_t IndexCount = 0;
			std::uint32_t FirstInstance = 0;
			std::uint32_t Count = 0;
			bool DoubleSided = false;
		};

		OpaquePass(SDL_GPUDevice *gpu, SDL_GPUTextureFormat swapchainFormat, SDLRendererMetrics *Metrics)
			: LocalLights(gpu), VisibleInstances(gpu) {
			try {
				constexpr auto ProbeUsage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
				ProbeArrayShader = SDL_GPUTextureSupportsFormat(gpu, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
					SDL_GPU_TEXTURETYPE_CUBE_ARRAY, ProbeUsage);
				Shader.VertexFilepath = GetSDLShaderPath("opaque.vert");
				Shader.VertexUniformBufferCount = 2;
				Shader.VertexStorageBufferCount = 1;
				Shader.FragmentFilepath = GetSDLShaderPath(ProbeArrayShader ? "opaque.frag" : "opaque-portable.frag");
				Shader.FragmentUniformBufferCount = 1;
				Shader.FragmentSamplerCount = 13;
				Shader.FragmentStorageBufferCount = 3;
				OutputMode = swapchainFormat == SDLHdrSceneFormat ? 2.0f :
					SDLTargetEncodesSrgb(swapchainFormat) ? 0.0f : 1.0f;
				InstanceColorFormat = swapchainFormat;
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
				if (!Pipeline || !DoubleSidedPipeline || !BlendedPipeline || !BlendedDoubleSidedPipeline)
					throw std::runtime_error(std::format("Failed to create material rendering resources: {}", SDL_GetError()));
				const auto *Driver = SDL_GetGPUDeviceDriver(gpu);
				GroupingSupported = Driver && std::string_view(Driver) == "vulkan";
			} catch (...) {
				Destroy(gpu);
				throw;
			}
		};

		void Destroy(SDL_GPUDevice *Gpu) override {
			LocalLights.Destroy();
			VisibleInstances.Destroy();
			if (InstancedPipeline) SDL_ReleaseGPUGraphicsPipeline(Gpu, InstancedPipeline);
			if (InstancedDoubleSidedPipeline) SDL_ReleaseGPUGraphicsPipeline(Gpu, InstancedDoubleSidedPipeline);
			InstancedPipeline = nullptr;
			InstancedDoubleSidedPipeline = nullptr;
			InstancedShader.Destroy(Gpu);
			if (DoubleSidedPipeline) SDL_ReleaseGPUGraphicsPipeline(Gpu, DoubleSidedPipeline);
			if (BlendedPipeline) SDL_ReleaseGPUGraphicsPipeline(Gpu, BlendedPipeline);
			if (BlendedDoubleSidedPipeline) SDL_ReleaseGPUGraphicsPipeline(Gpu, BlendedDoubleSidedPipeline);
			DoubleSidedPipeline = nullptr;
			BlendedPipeline = nullptr;
			BlendedDoubleSidedPipeline = nullptr;
			SDLRenderPass::Destroy(Gpu);
		}

		void OnFrameSubmitted() override { VisibleInstances.OnFrameSubmitted(); }
		void OnFrameAborted() override { VisibleInstances.OnFrameAborted(); }

		SDL_GPURenderPass *Draw(SDL_GPUDevice *gpu, SDLFrameContext &context) override {
			RejectSceneEnvelope = false;
			if (context.EnableSceneEnvelope) {
				const auto Result = context.Projection.GetSceneEnvelope();
				if (context.Metrics && Result.Rebuilt) {
					context.Metrics->EnvelopeBuildNanoseconds += Result.BuildNanoseconds;
					++context.Metrics->EnvelopeRebuilds;
				}
				if (Result.Envelope && OutsideSceneEnvelope(GetClipPlanes(context.Projection.GetFrame().Camera), *Result.Envelope)) {
					RejectSceneEnvelope = true;
					if (context.Metrics) {
						++context.Metrics->EnvelopeRejections;
						context.Metrics->CameraCulledObjects += Result.Envelope->VisibleObjects;
					}
				}
			}
			SDL_GPUColorTargetInfo colorTarget = {
				.texture = context.GetSceneColorTexture(),
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
			LocalLights.Prepare(context.Projection.GetFrame(), context.Metrics, context.SpotShadowAtlas);
			if (!RejectSceneEnvelope && context.EnableInstancing && context.InstanceResources &&
				context.InstanceResources->GetBuffer() && EnsureInstancedPipeline(gpu, context.Metrics)) {
				try {
					const auto InstanceStart = std::chrono::steady_clock::now();
					PrepareInstanceBatches(context);
					if (context.Metrics) context.Metrics->LastInstancePreparationNanoseconds =
						std::chrono::duration_cast<std::chrono::nanoseconds>(
							std::chrono::steady_clock::now() - InstanceStart).count();
				}
				catch (const std::exception &Error) {
					LOG_WARN(App, "[Renderer:Instances] Disabled optional instancing: %s", Error.what());
					VisibleInstances.OnFrameAborted();
					InstancingDisabled = true;
					InstanceActions.clear();
					InstanceBatches.clear();
				}
			} else {
				InstanceActions.clear();
				InstanceBatches.clear();
			}
			auto *Pass = SDL_BeginGPURenderPass(context.Commands, &colorTarget, 1, &depthTarget);
			if (!Pass) throw std::runtime_error("[Renderer:Material] Could not begin the world render pass");
			try {
				DrawWorld(gpu, Pass, context, shadowBinding, White);
			} catch (...) {
				OpaqueBatch.clear();
				CoverageGrid.Clear();
				TransparentDraws.clear();
				SDL_EndGPURenderPass(Pass);
				throw;
			}
			return Pass;
		}

	  private:
		bool EnsureInstancedPipeline(SDL_GPUDevice *Gpu, SDLRendererMetrics *Metrics) {
			if (InstancingDisabled) return false;
			if (InstancedPipeline && InstancedDoubleSidedPipeline) return true;
			if (InstancedPipelineFailed) return false;
			try {
				InstancedShader.VertexFilepath = GetSDLShaderPath("opaque-instanced.vert");
				InstancedShader.VertexUniformBufferCount = 1;
				InstancedShader.VertexStorageBufferCount = 3;
				InstancedShader.FragmentFilepath = GetSDLShaderPath(
					ProbeArrayShader ? "opaque.frag" : "opaque-portable.frag");
				InstancedShader.FragmentUniformBufferCount = 1;
				InstancedShader.FragmentSamplerCount = 13;
				InstancedShader.FragmentStorageBufferCount = 3;
				InstancedShader.Init(Gpu, Metrics);
				auto Builder = SDLPipelineBuilder()
					.SetVertexShader(InstancedShader.VertexShader)
					.SetFragmentShader(InstancedShader.FragmentShader)
					.SetColorEnabled(true)
					.SetColorFormat(InstanceColorFormat)
					.SetBlendingEnabled(false)
					.SetDepthEnabled(true)
					.SetDepthFormat(SDLSceneDepthFormat);
				InstancedPipeline = Builder.Build(Gpu, Metrics);
				auto DoubleSided = Builder.BuildInfo();
				DoubleSided.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
				InstancedDoubleSidedPipeline = SDL_CreateGPUGraphicsPipeline(Gpu, &DoubleSided);
				if (InstancedDoubleSidedPipeline && Metrics) ++Metrics->PipelineCreations;
				if (!InstancedPipeline || !InstancedDoubleSidedPipeline)
					throw std::runtime_error(std::format("Could not create instanced pipelines: {}", SDL_GetError()));
				return true;
			} catch (const std::exception &Error) {
				LOG_WARN(App, "[Renderer:Instances] Disabled optional instancing: %s", Error.what());
				if (InstancedPipeline) SDL_ReleaseGPUGraphicsPipeline(Gpu, InstancedPipeline);
				if (InstancedDoubleSidedPipeline) SDL_ReleaseGPUGraphicsPipeline(Gpu, InstancedDoubleSidedPipeline);
				InstancedPipeline = nullptr;
				InstancedDoubleSidedPipeline = nullptr;
				InstancedShader.Destroy(Gpu);
				InstancedPipelineFailed = true;
				return false;
			}
		}

		void PrepareInstanceBatches(SDLFrameContext &Context) {
			using Clock = std::chrono::steady_clock;
			// Rotate the one-in-128 sample across publications so batch boundaries do not
			// systematically occupy the same sampled position. The clocks stay off
			// the ordinary benchmark path.
			const bool SampleDetailed = Context.ProfileDetailed && Context.Metrics;
			const auto SampleOffset = Context.Projection.GetLastPublicationId() & 127u;
			const auto SampleElapsed = [](Clock::time_point Start) {
				return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					Clock::now() - Start).count());
			};
			const auto TraversalStart = Clock::now();
			// The ordinary camera pass walks GetObjects() in its current map order.
			// Only request the derived vector when that pass will also use it.
			const auto *Ordered = Context.UseCompactTraversal ? &Context.Projection.GetCompactTraversal() : nullptr;
			const auto CandidateStart = Clock::now();
			if (Context.Metrics) Context.Metrics->LastInstanceTraversalNanoseconds =
				std::chrono::duration_cast<std::chrono::nanoseconds>(CandidateStart - TraversalStart).count();
			InstanceActions.assign(Context.Projection.GetObjects().size(), -1);
			InstanceBatches.clear();
			InstanceCoverageGrid.Clear();
			VisibleInstances.BeginFrame();
			const auto &Frame = Context.Projection.GetFrame();
			const auto Planes = GetClipPlanes(Frame.Camera);
			// A single-range instance draw preserves draw-major primitive order only
			// on Vulkan's strict rasterization path. Proof mode remains the control.
			const bool ContiguousOrder = Context.UseContiguousInstancing && GroupingSupported;
			std::optional<CoverageProjector> Projector;
			if (!ContiguousOrder && GroupingSupported && Context.Width && Context.Height) {
				const glm::dmat4 Projection(Frame.Camera.ProjectionMatrix), View(Frame.Camera.ViewMatrix);
				if (BoundedMatrix(Projection) && BoundedMatrix(View))
					Projector = CoverageProjector{Projection * View, Absolute(Projection) * Absolute(View),
						{0.25 / Context.Width, 0.25 / Context.Height}};
			}
			std::vector<std::pair<std::size_t, std::uint32_t>> Pending;
			Pending.reserve(MAX_OPAQUE_BATCH_OBJECTS);
			const SDLGpuMesh *PendingMesh = nullptr;
			std::uint32_t PendingFirst = 0, PendingCount = 0;
			bool PendingDoubleSided = false;
			RenderShadingModel PendingShading = RenderShadingModel::Legacy;
			const auto Flush = [&] {
				if (Pending.size() > 1) {
					const auto BatchIndex = static_cast<std::int32_t>(InstanceBatches.size());
					const auto FirstInstance = static_cast<std::uint32_t>(VisibleInstances.Size());
					for (const auto &[Position, Slot] : Pending) {
						InstanceActions[Position] = -2;
						(void)VisibleInstances.Append(Slot);
					}
					InstanceActions[Pending.front().first] = BatchIndex;
					InstanceBatches.push_back({PendingMesh, PendingFirst, PendingCount, FirstInstance,
						static_cast<std::uint32_t>(Pending.size()), PendingDoubleSided});
				}
				Pending.clear();
				InstanceCoverageGrid.Clear();
			};
			const auto VisitCandidate = [&](std::size_t Position, ObjectId Object,
				const RenderProjectedObject &Projected, const RenderAnimationPoseUpdate *Pose,
				const RenderBounds *MeshBounds, bool Sampled) {
				if (Sampled) ++Context.Metrics->InstanceSampleObjects;
				if (!Projected.Visible) { Flush(); return; }
				const auto &Item = Projected.Item;
				const auto *Bounds = Projected.Mesh ? MeshBounds : &PrimitiveBounds;
				const auto CullStart = Sampled ? Clock::now() : Clock::time_point{};
				const bool TestFrustum = Bounds && (!Pose || Pose->Mode != RenderAnimationSkinningMode::GpuPalette);
				const bool Culled = TestFrustum && OutsideView(Planes, Item.ModelMatrix, *Bounds);
				if (Sampled && TestFrustum) {
					++Context.Metrics->InstanceSampleCullTests;
					Context.Metrics->LastInstanceSampleCullNanoseconds += SampleElapsed(CullStart);
				}
				if (Culled) {
					InstanceActions[Position] = CulledInstanceAction;
					return;
				}
				const auto EligibilityStart = Sampled ? Clock::now() : Clock::time_point{};
				const auto *Mesh = Projected.Mesh
					? Context.MeshResources.Find(*Projected.Mesh) : Context.MeshResources.Find(Item.Geometry);
				if (!Mesh || !Mesh->VertexBuffer || !Mesh->IndexBuffer || Pose ||
					(Item.CastShadow && !ContiguousOrder) || !Mesh->ImmutablePositionBounds ||
					(!ContiguousOrder && !Projector) ||
					(Projected.Primitives && Projected.Primitives->size() != 1)) {
					Flush();
					if (Sampled) {
						++Context.Metrics->InstanceSampleEligibilityChecks;
						Context.Metrics->LastInstanceSampleEligibilityNanoseconds += SampleElapsed(EligibilityStart);
					}
					return;
				}
				// Pose-bearing objects are excluded above. Static casters use the
				// already-recorded shadow pass and have no GPU-skinned pose receipt.
				const auto &Material = Projected.Primitives ? Projected.Primitives->front().Material : Projected.Material;
				if (Material.OpacityMode != RenderOpacityMode::Opaque || Material.BaseColorTexture ||
					Material.NormalTexture || Material.MetallicRoughnessTexture || Material.EmissiveTexture ||
					Material.OcclusionTexture) {
					Flush();
					if (Sampled) {
						++Context.Metrics->InstanceSampleEligibilityChecks;
						Context.Metrics->LastInstanceSampleEligibilityNanoseconds += SampleElapsed(EligibilityStart);
					}
					return;
				}
				if (Sampled) {
					++Context.Metrics->InstanceSampleEligibilityChecks;
					Context.Metrics->LastInstanceSampleEligibilityNanoseconds += SampleElapsed(EligibilityStart);
				}
				const auto SlotStart = Sampled ? Clock::now() : Clock::time_point{};
				const auto Slot = Context.InstanceResources->Find(Object);
				if (Sampled) {
					++Context.Metrics->InstanceSampleSlotLookups;
					Context.Metrics->LastInstanceSampleSlotNanoseconds += SampleElapsed(SlotStart);
				}
				if (!Slot) { Flush(); return; }
				const auto BatchStart = Sampled ? Clock::now() : Clock::time_point{};
				const auto First = Projected.Primitives ? Projected.Primitives->front().FirstIndex : 0;
				const auto Count = Projected.Primitives ? Projected.Primitives->front().IndexCount : Mesh->IndexCount;
				std::optional<OpaqueCoverage> Coverage;
				if (!ContiguousOrder) {
					Coverage = Projector->Project(Item.ModelMatrix, *Mesh->ImmutablePositionBounds);
					if (!Coverage || Coverage->CellCount() > MAX_CELLS_PER_OBJECT) {
						Flush();
						if (Sampled) {
							++Context.Metrics->InstanceSampleBatchAdmissions;
							Context.Metrics->LastInstanceSampleBatchNanoseconds += SampleElapsed(BatchStart);
						}
						return;
					}
				}
				if (!Pending.empty() && (Mesh != PendingMesh || First != PendingFirst || Count != PendingCount ||
					Material.DoubleSided != PendingDoubleSided || Material.ShadingModel != PendingShading)) Flush();
				if (Pending.size() == MAX_OPAQUE_BATCH_OBJECTS) Flush();
				if (!ContiguousOrder && !Pending.empty() &&
					InstanceCoverageGrid.Check(*Coverage, nullptr) != OpaqueCoverageGrid::Admission::Safe)
					Flush();
				if (Pending.empty()) {
					PendingMesh = Mesh;
					PendingFirst = First;
					PendingCount = Count;
					PendingDoubleSided = Material.DoubleSided;
					PendingShading = Material.ShadingModel;
				}
				if (!ContiguousOrder) InstanceCoverageGrid.Add(*Coverage, static_cast<std::uint16_t>(Pending.size()));
				Pending.emplace_back(Position, *Slot);
				if (Sampled) {
					++Context.Metrics->InstanceSampleBatchAdmissions;
					Context.Metrics->LastInstanceSampleBatchNanoseconds += SampleElapsed(BatchStart);
				}
			};
			if (Ordered) {
				for (std::size_t Position = 0; Position < Ordered->size(); ++Position) {
					const auto &Record = (*Ordered)[Position];
					const bool Sampled = SampleDetailed && (Position & 127u) == SampleOffset;
					VisitCandidate(Position, Record.Object, *Record.Projected, Record.Pose, Record.MeshBounds, Sampled);
				}
			} else {
				std::size_t Position = 0;
				for (const auto &[Object, Projected] : Context.Projection.GetObjects()) {
					const bool Sampled = SampleDetailed && (Position & 127u) == SampleOffset;
					const auto SourceStart = Sampled ? Clock::now() : Clock::time_point{};
					const auto *Pose = Context.Projection.GetAnimationPose(Object);
					const auto *MeshBounds = Projected.Mesh
						? Context.Projection.GetMeshBounds(*Projected.Mesh) : nullptr;
					if (Sampled) Context.Metrics->LastInstanceSampleSourceNanoseconds += SampleElapsed(SourceStart);
					VisitCandidate(Position++, Object, Projected, Pose, MeshBounds, Sampled);
				}
			}
			Flush();
			const auto UploadStart = Clock::now();
			if (Context.Metrics) Context.Metrics->LastInstanceCandidateNanoseconds =
				std::chrono::duration_cast<std::chrono::nanoseconds>(UploadStart - CandidateStart).count();
			if (!InstanceBatches.empty()) VisibleInstances.Upload(Context.Commands, Context.Metrics);
			if (Context.Metrics) Context.Metrics->LastInstanceVisibleUploadNanoseconds =
				std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - UploadStart).count();
		}

		void DrawWorld(SDL_GPUDevice *Gpu, SDL_GPURenderPass *Pass, SDLFrameContext &Context, SDL_GPUTextureSamplerBinding ShadowBinding,
			SDL_GPUTexture *White) {
			using Clock = std::chrono::steady_clock;
			const bool Detailed = Context.ProfileDetailed && Context.Metrics;
			const auto ElapsedNanoseconds = [](Clock::time_point Start) {
				return static_cast<std::uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - Start).count());
			};
			const auto &Frame = Context.Projection.GetFrame();
			const auto &Environment = Frame.Environment;
			const auto EnvironmentAngle = glm::radians(Environment.EnvironmentRotation);
			WorldUniforms Uniforms{
				.ViewMatrix = Frame.Camera.ViewMatrix,
				.ProjectionMatrix = Frame.Camera.ProjectionMatrix,
				.ShadowBiasMatrix = SHADOW_BIAS_MATRIX * Context.ShadowMatrix,
				.SunDirectionIntensity = glm::vec4(Environment.SunDirection, Environment.SunIntensity),
				.AmbientExposure = glm::vec4(Environment.AmbientColor, Environment.ExposureMultiplier),
				.SunColorFogEnabled = glm::vec4(Environment.SunColor, Environment.Fog.Enabled ? 1.0f : 0.0f),
				.FogColorStart = glm::vec4(Environment.Fog.Color, Environment.Fog.Start),
				.CameraPositionFogEnd = glm::vec4(Frame.Camera.Position, Environment.Fog.End),
				.EnvironmentLighting = glm::vec4(Context.EnvironmentLighting.Enabled ? 1.0f : 0.0f,
					Context.EnvironmentLighting.ReflectionMaxLod, Environment.IblIntensity,
					(Context.CascadedShadows ? Context.CascadeHasCasters : Context.LegacyHasCasters) ? 1.0f : 0.0f),
				.EnvironmentRotationTone = glm::vec4(std::sin(EnvironmentAngle), std::cos(EnvironmentAngle),
					Environment.ToneMapping == RenderToneMapping::Reinhard ? 1.0f : 0.0f, 0.0f),
				.LocalLighting = LocalLights.GetUniforms(),
			};
			PopulateSDLWorldShadows(Uniforms, Context);
			Uniforms.ProbeLighting = {static_cast<float>(Context.ReflectionProbes.Count), 6.0f, 0, 0};
			SDL_PushGPUVertexUniformData(Context.Commands, 0, &Uniforms, sizeof(Uniforms));
			SDL_PushGPUFragmentUniformData(Context.Commands, 0, &Uniforms, sizeof(Uniforms));

			const auto Planes = GetClipPlanes(Frame.Camera);
			SDL_GPUGraphicsPipeline *BoundPipeline = nullptr;
			const SDLGpuMesh *BoundMesh = nullptr;
			const RenderProjectedObject *BoundObject = nullptr;
			SDL_GPUBuffer *BoundPalette = nullptr;
			std::array<SDL_GPUTexture *, 5> BoundTextures{};
			std::array<SDL_GPUSampler *, 5> BoundSamplers{};
			const RenderMaterialState *ResolvedMaterial = nullptr;
			bool ResolvedTextured = false;
			std::array<bool, 5> ResolvedNativeSrgb{};
			auto *DefaultSampler = Context.MaterialSamplers->Get(Gpu, {}, false);
			TransparentDraws.clear();

			const auto WrapMode = [](const RenderSamplerState &State) {
				return static_cast<float>(static_cast<unsigned>(State.WrapU) + 3u * static_cast<unsigned>(State.WrapV));
			};
			const auto FilterMode = [](const RenderSamplerState &State) {
				return static_cast<float>(SDLMaterialSamplers::UsesNearestMinFilter(State.MinFilter) +
					2u * (State.MagFilter == RenderTextureMagFilter::Nearest));
			};
			auto DrawPrimitive = [&](const WorldDraw &Draw, std::uint32_t FirstIndex, std::uint32_t IndexCount,
				const RenderMaterialState &Material) {
				const auto RecordingStart = Detailed ? Clock::now() : Clock::time_point{};
				const auto &Item = Draw.Object->Item;
				const bool Transparent = Material.OpacityMode == RenderOpacityMode::Transparent;
				auto *MaterialPipeline = Transparent
					? (Material.DoubleSided ? BlendedDoubleSidedPipeline : BlendedPipeline)
					: (Material.DoubleSided ? DoubleSidedPipeline : Pipeline);
				if (MaterialPipeline != BoundPipeline) {
					SDL_BindGPUGraphicsPipeline(Pass, MaterialPipeline);
					const auto LightBuffers = LocalLights.GetBuffers();
					const std::array<SDL_GPUBuffer *, 3> FragmentBuffers{
						LightBuffers[0], LightBuffers[1],
						ProbeArrayShader ? Context.ReflectionProbes.Records : LightBuffers[0]};
					SDL_BindGPUFragmentStorageBuffers(Pass, 0, FragmentBuffers.data(), FragmentBuffers.size());
					BoundPipeline = MaterialPipeline;
					BoundMesh = nullptr;
					BoundObject = nullptr;
					BoundPalette = nullptr;
					BoundTextures = {};
					BoundSamplers = {};
					ResolvedMaterial = nullptr;
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
					if (Palette.GpuSkinned) {
						const auto ShadowRevision = Context.ShadowPoseRevisions.find(Item.Object);
						if (ShadowRevision != Context.ShadowPoseRevisions.end()) {
							if (Context.Metrics) ++Context.Metrics->ShadowPoseChecks;
							if (ShadowRevision->second != Palette.PoseRevision) {
								if (Context.Metrics) ++Context.Metrics->MainShadowPoseMismatches;
								throw std::logic_error("[Renderer:Material] World and shadow passes resolved different animation poses");
							}
						}
					}
					BoundObject = Draw.Object;
				}
				// Grouped ranges often share the same immutable material. Its native
				// views and sampler state stay fixed for the duration of this pass.
				if (!ResolvedMaterial || (&Material != ResolvedMaterial && (ResolvedTextured ||
					Material.BaseColorTexture || Material.NormalTexture || Material.MetallicRoughnessTexture ||
					Material.EmissiveTexture || Material.OcclusionTexture))) {
					const std::array References{
						Material.BaseColorTexture, Material.NormalTexture, Material.MetallicRoughnessTexture,
						Material.EmissiveTexture, Material.OcclusionTexture,
					};
					std::array<SDL_GPUTexture *, 5> Textures{White, White, White, White, White};
					std::array<SDL_GPUSampler *, 5> Samplers{
						DefaultSampler, DefaultSampler, DefaultSampler, DefaultSampler, DefaultSampler,
					};
					ResolvedNativeSrgb = {};
					ResolvedTextured = false;
					const bool Physical = Material.ShadingModel == RenderShadingModel::MetallicRoughness;
					for (std::size_t Slot = 0; Slot < References.size(); ++Slot) {
						if (!References[Slot]) continue;
						ResolvedTextured = true;
						const bool Srgb = Physical && (Slot == 0 || Slot == 3);
						const auto View = Context.TextureResources->FindMaterialTexture(*References[Slot], Srgb);
						const bool Mask = Slot == 0 && Material.OpacityMode == RenderOpacityMode::Masked;
						Textures[Slot] = View.Texture;
						ResolvedNativeSrgb[Slot] = View.NativeSrgb;
						Samplers[Slot] = Context.MaterialSamplers->Get(Gpu, Material.TextureSamplers[Slot],
							View.HasMipmaps && !Mask);
					}
					if (std::ranges::find(Textures, nullptr) != Textures.end())
						throw std::logic_error("[Renderer:Material] A material texture has no SDL residency");
					if (Textures != BoundTextures || Samplers != BoundSamplers) {
						const std::array<SDL_GPUTextureSamplerBinding, 13> Bindings{{
							ShadowBinding, {Textures[0], Samplers[0]}, {Textures[1], Samplers[1]}, {Textures[2], Samplers[2]},
							{Textures[3], Samplers[3]}, {Textures[4], Samplers[4]}, Context.EnvironmentLighting.Irradiance,
							Context.EnvironmentLighting.Reflections, Context.EnvironmentLighting.BrdfLut,
							{Context.CascadeShadowTexture, Context.ShadowSampler}, {Context.LocalShadowTexture, Context.ShadowSampler},
							ProbeArrayShader ? Context.ReflectionProbes.Irradiance : Context.EnvironmentLighting.Irradiance,
							ProbeArrayShader ? Context.ReflectionProbes.Reflections : Context.EnvironmentLighting.Reflections,
						}};
						SDL_BindGPUFragmentSamplers(Pass, 0, Bindings.data(), Bindings.size());
						BoundTextures = Textures;
						BoundSamplers = Samplers;
						if (Context.Metrics) ++Context.Metrics->WorldMaterialBindings;
					}
					ResolvedMaterial = &Material;
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
						Material.NormalTexture ? 1.0f : 0.0f, OutputMode},
					.EmissiveOcclusion = glm::vec4(Material.EmissiveFactor, Material.OcclusionTexture ? Material.OcclusionStrength : 0.0f),
					.MaterialControls = {Material.NormalScale, Material.EmissiveTexture ? 1.0f : 0.0f,
						ResolvedNativeSrgb[0] ? 1.0f : 0.0f, ResolvedNativeSrgb[3] ? 1.0f : 0.0f},
					.ColorSamplerModes = {WrapMode(Material.TextureSamplers[0]), WrapMode(Material.TextureSamplers[3]),
						FilterMode(Material.TextureSamplers[0]), FilterMode(Material.TextureSamplers[3])},
				};
				SDL_PushGPUVertexUniformData(Context.Commands, 1, &Part, sizeof(PartUniforms));
				SDL_DrawGPUIndexedPrimitives(Pass, IndexCount, 1, FirstIndex, 0, 0);
				if (Context.Metrics) {
					++Context.Metrics->DrawCalls;
					if (Detailed) {
						++Context.Metrics->WorldPrimitiveDraws;
						Context.Metrics->LastWorldDrawRecordingNanoseconds += ElapsedNanoseconds(RecordingStart);
					}
				}
			};
			const auto DrawInstanceBatch = [&](const InstanceBatch &Batch) {
				auto *Selected = Batch.DoubleSided ? InstancedDoubleSidedPipeline : InstancedPipeline;
				SDL_BindGPUGraphicsPipeline(Pass, Selected);
				const auto LightBuffers = LocalLights.GetBuffers();
				const std::array<SDL_GPUBuffer *, 3> FragmentBuffers{
					LightBuffers[0], LightBuffers[1],
					ProbeArrayShader ? Context.ReflectionProbes.Records : LightBuffers[0]};
				SDL_BindGPUFragmentStorageBuffers(Pass, 0, FragmentBuffers.data(), FragmentBuffers.size());
				BoundPipeline = Selected;
				BoundMesh = nullptr;
				BoundObject = nullptr;
				BoundPalette = nullptr;
				BoundTextures = {};
				BoundSamplers = {};
				ResolvedMaterial = nullptr;
				if (Context.Metrics) ++Context.Metrics->PipelineSwitches;
				const SDL_GPUBufferBinding VertexBinding{.buffer = Batch.Mesh->VertexBuffer};
				const SDL_GPUBufferBinding IndexBinding{.buffer = Batch.Mesh->IndexBuffer};
				SDL_BindGPUVertexBuffers(Pass, 0, &VertexBinding, 1);
				SDL_BindGPUIndexBuffer(Pass, &IndexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
				if (Context.Metrics) ++Context.Metrics->WorldGeometryBindings;
				const auto IdentityPalette = Context.SkinPalettes.Find({}, nullptr);
				const std::array<SDL_GPUBuffer *, 3> Storage{
					IdentityPalette.Buffer, Context.InstanceResources->GetBuffer(), VisibleInstances.GetBuffer()};
				SDL_BindGPUVertexStorageBuffers(Pass, 0, Storage.data(), Storage.size());
				const std::array<SDL_GPUTextureSamplerBinding, 13> Bindings{{
					ShadowBinding, {White, DefaultSampler}, {White, DefaultSampler}, {White, DefaultSampler},
					{White, DefaultSampler}, {White, DefaultSampler}, Context.EnvironmentLighting.Irradiance,
					Context.EnvironmentLighting.Reflections, Context.EnvironmentLighting.BrdfLut,
					{Context.CascadeShadowTexture, Context.ShadowSampler},
					{Context.LocalShadowTexture, Context.ShadowSampler},
					ProbeArrayShader ? Context.ReflectionProbes.Irradiance : Context.EnvironmentLighting.Irradiance,
					ProbeArrayShader ? Context.ReflectionProbes.Reflections : Context.EnvironmentLighting.Reflections,
				}};
				SDL_BindGPUFragmentSamplers(Pass, 0, Bindings.data(), Bindings.size());
				if (Context.Metrics) ++Context.Metrics->WorldMaterialBindings;
				SDL_DrawGPUIndexedPrimitives(Pass, Batch.IndexCount, Batch.Count, Batch.FirstIndex, 0,
					Batch.FirstInstance);
				if (Context.Metrics) {
					++Context.Metrics->DrawCalls;
					++Context.Metrics->InstancedDrawCalls;
					if (Context.UseContiguousInstancing) ++Context.Metrics->ContiguousInstancedDrawCalls;
					Context.Metrics->MaximumInstancedBatchSize =
						std::max(Context.Metrics->MaximumInstancedBatchSize, static_cast<std::uint64_t>(Batch.Count));
					Context.Metrics->InstancedObjects += Batch.Count;
					if (Detailed) ++Context.Metrics->WorldPrimitiveDraws;
				}
			};
			auto ForPrimitives = [](const WorldDraw &Draw, auto &&Visit) {
				if (Draw.Object->Primitives) {
					for (const auto &Primitive : *Draw.Object->Primitives)
						Visit(Primitive.FirstIndex, Primitive.IndexCount, Primitive.Material);
				} else Visit(0, Draw.Mesh->IndexCount, Draw.Object->Material);
			};
			std::optional<CoverageProjector> Projector;
			const bool GroupingEnabled = GroupingSupported && Context.EnableOpaqueGrouping;
			if (GroupingEnabled && Context.Width && Context.Height) {
				const glm::dmat4 Projection(Frame.Camera.ProjectionMatrix), View(Frame.Camera.ViewMatrix);
				if (BoundedMatrix(Projection) && BoundedMatrix(View)) {
					Projector = CoverageProjector{Projection * View, Absolute(Projection) * Absolute(View),
						{0.25 / Context.Width, 0.25 / Context.Height}};
				}
			}
			const auto FlushOpaque = [&] {
				if (OpaqueBatch.empty()) return;
				if (Context.Metrics && OpaqueBatch.size() > 1)
					Context.Metrics->OpaqueGroupedObjects += OpaqueBatch.size();
				// A batch only contains pairwise independent objects. Keep the
				// original primitive order inside each object and its admission order
				// within each range, so an exact tie never gains a new priority.
				for (std::size_t Range = 0; Range < OpaqueBatch.front().Object->Primitives->size(); ++Range) {
					for (const auto &Draw : OpaqueBatch) {
						const auto &Primitive = (*Draw.Object->Primitives)[Range];
						DrawPrimitive(Draw, Primitive.FirstIndex, Primitive.IndexCount, Primitive.Material);
					}
				}
				OpaqueBatch.clear();
				CoverageGrid.Clear();
			};
			const auto TryBatchOpaque = [&](const WorldDraw &Draw) {
				const auto &Ranges = Draw.Object->Primitives;
				if (!GroupingEnabled || !Draw.Object->Mesh || Draw.Pose || !Ranges || Ranges->size() < 2 ||
					Ranges->size() > MAX_OPAQUE_BATCH_RANGES) return false;
				const auto MatchesPending = [&] {
					if (OpaqueBatch.empty() || OpaqueBatch.front().Mesh != Draw.Mesh) return false;
					const auto &Pending = OpaqueBatch.front().Object->Primitives;
					return Pending == Ranges || std::ranges::equal(*Pending, *Ranges, {}, BindingLayout, BindingLayout);
				};
				if (!MatchesPending()) {
					FlushOpaque();
					const auto FirstBinding = BindingLayout(Ranges->front());
					bool ChangesBinding = false;
					for (const auto &Primitive : *Ranges) {
						if (Primitive.Material.OpacityMode != RenderOpacityMode::Opaque) return false;
						ChangesBinding |= BindingLayout(Primitive) != FirstBinding;
					}
					if (!ChangesBinding) return false;
				}
				if (Context.Metrics) ++Context.Metrics->OpaqueGroupingCandidates;
				const auto &Bounds = Draw.Mesh->ImmutablePositionBounds;
				const auto Candidate = Projector && Bounds ? Projector->Project(Draw.Object->Item.ModelMatrix, *Bounds)
					: std::nullopt;
				if (!Candidate) {
					if (Context.Metrics) ++Context.Metrics->OpaqueGroupingUnprovenBounds;
					return false;
				}
				if (Candidate->CellCount() > MAX_CELLS_PER_OBJECT) {
					if (Context.Metrics) ++Context.Metrics->OpaqueGroupingBudgetBarriers;
					return false;
				}
				if (OpaqueBatch.size() == MAX_OPAQUE_BATCH_OBJECTS) FlushOpaque();
				if (!OpaqueBatch.empty()) {
					const auto Admission = CoverageGrid.Check(*Candidate, Context.Metrics);
					if (Admission != OpaqueCoverageGrid::Admission::Safe) {
						if (Context.Metrics) {
							if (Admission == OpaqueCoverageGrid::Admission::Conflict) ++Context.Metrics->OpaqueGroupingConflictBarriers;
							else ++Context.Metrics->OpaqueGroupingBudgetBarriers;
						}
						FlushOpaque();
					}
				}
				OpaqueBatch.reserve(MAX_OPAQUE_BATCH_OBJECTS);
				CoverageGrid.Add(*Candidate, static_cast<std::uint16_t>(OpaqueBatch.size()));
				OpaqueBatch.push_back(Draw);
				return true;
			};

			const auto TraversalStart = Detailed ? Clock::now() : Clock::time_point{};
			const auto VisitObject = [&](std::size_t Position, ObjectId Object, const RenderProjectedObject &Projected,
				const RenderAnimationPoseUpdate *CachedPose, const RenderBounds *CachedBounds, bool Cached) {
				if (Detailed) ++Context.Metrics->CameraVisitedObjects;
				if (Position < InstanceActions.size() && InstanceActions[Position] != -1) {
					if (InstanceActions[Position] == CulledInstanceAction) {
						if (Detailed) ++Context.Metrics->CameraFrustumTests;
						if (Context.Metrics) ++Context.Metrics->CameraCulledObjects;
						return;
					}
					if (InstanceActions[Position] >= 0) {
						FlushOpaque();
						DrawInstanceBatch(InstanceBatches[static_cast<std::size_t>(InstanceActions[Position])]);
					}
					return;
				}
				if (!Projected.Visible) {
					if (Detailed) ++Context.Metrics->CameraHiddenObjects;
					return;
				}
				const auto CullStart = Detailed ? Clock::now() : Clock::time_point{};
				const auto &Item = Projected.Item;
				const auto *Pose = Cached ? CachedPose : Context.Projection.GetAnimationPose(Object);
				const auto *Bounds = Projected.Mesh
					? (Cached ? CachedBounds : Context.Projection.GetMeshBounds(*Projected.Mesh)) : &PrimitiveBounds;
				const bool TestFrustum = Bounds && (!Pose || Pose->Mode != RenderAnimationSkinningMode::GpuPalette);
				if (TestFrustum && Detailed) ++Context.Metrics->CameraFrustumTests;
				const bool Culled = TestFrustum && OutsideView(Planes, Item.ModelMatrix, *Bounds);
				if (Detailed) Context.Metrics->LastCameraCullNanoseconds += ElapsedNanoseconds(CullStart);
				if (Culled) {
					if (Context.Metrics) ++Context.Metrics->CameraCulledObjects;
					return;
				}
				const auto ResidencyStart = Detailed ? Clock::now() : Clock::time_point{};
				if (Detailed) ++Context.Metrics->CameraMeshLookups;
				const auto *Mesh = Projected.Mesh
					? Context.MeshResources.Find(*Projected.Mesh) : Context.MeshResources.Find(Item.Geometry);
				if (Detailed) Context.Metrics->LastCameraResidencyNanoseconds += ElapsedNanoseconds(ResidencyStart);
				if (!Mesh || !Mesh->VertexBuffer || !Mesh->IndexBuffer) {
					LOG_WARN(App, "[Renderer:Material] Publication %llu skipped ObjectId %u:%u with unavailable GPU geometry",
						static_cast<unsigned long long>(Context.Projection.GetLastPublicationId()), Object.Slot, Object.Generation);
					return;
				}

				WorldDraw Draw{&Projected, Mesh, Pose, 0.0f};
				const auto GroupingStart = Detailed ? Clock::now() : Clock::time_point{};
				const bool Batched = TryBatchOpaque(Draw);
				if (Detailed) Context.Metrics->LastOpaqueGroupingNanoseconds += ElapsedNanoseconds(GroupingStart);
				if (Batched) return;
				FlushOpaque();
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
			};
			if (RejectSceneEnvelope) {
				// No accepted camera draw can be present; preserve the ordinary traversal for all other frames.
			} else if (Context.UseCompactTraversal) {
				std::size_t Position = 0;
				for (const auto &Record : Context.Projection.GetCompactTraversal())
					VisitObject(Position++, Record.Object, *Record.Projected, Record.Pose, Record.MeshBounds, true);
			} else {
				std::size_t Position = 0;
				for (const auto &[Object, Projected] : Context.Projection.GetObjects())
					VisitObject(Position++, Object, Projected, nullptr, nullptr, false);
			}
			FlushOpaque();
			if (Detailed) Context.Metrics->LastCameraTraversalNanoseconds = ElapsedNanoseconds(TraversalStart);

			const auto TransparentStart = Detailed ? Clock::now() : Clock::time_point{};
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
			if (Detailed) Context.Metrics->LastTransparentNanoseconds = ElapsedNanoseconds(TransparentStart);
		}

		SDL_GPUGraphicsPipeline *DoubleSidedPipeline = nullptr;
		SDL_GPUGraphicsPipeline *BlendedPipeline = nullptr;
		SDL_GPUGraphicsPipeline *BlendedDoubleSidedPipeline = nullptr;
		SDLFileShader InstancedShader;
		SDL_GPUGraphicsPipeline *InstancedPipeline = nullptr;
		SDL_GPUGraphicsPipeline *InstancedDoubleSidedPipeline = nullptr;
		bool InstancedPipelineFailed = false;
		bool InstancingDisabled = false;
		bool RejectSceneEnvelope = false;
		std::vector<std::int32_t> InstanceActions;
		std::vector<InstanceBatch> InstanceBatches;
		OpaqueCoverageGrid InstanceCoverageGrid;
		SDLVisibleInstanceBuffer VisibleInstances;
		std::vector<WorldDraw> OpaqueBatch;
		OpaqueCoverageGrid CoverageGrid;
		std::vector<WorldDraw> TransparentDraws;
		float OutputMode = 1.0f;
		bool ProbeArrayShader = false;
		SDL_GPUTextureFormat InstanceColorFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
		bool GroupingSupported = false;
		SDLLocalLightGrid LocalLights;
	};

	std::unique_ptr<SDLRenderPass> CreateOpaquePass(
		SDL_GPUDevice *gpu, SDL_GPUTextureFormat swapchainFormat, SDLRendererMetrics *Metrics
	) {
		return std::make_unique<OpaquePass>(gpu, swapchainFormat, Metrics);
	}
} // namespace gargantuan
