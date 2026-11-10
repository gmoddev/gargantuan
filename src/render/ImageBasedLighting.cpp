// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "render/sdl/SDLImageBasedLighting.hpp"

#include "gargantuan/render/SDLRenderer.hpp"
#include "render/sdl/SDLTextureCache.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <format>
#include <stdexcept>

namespace gargantuan {
	namespace {
		constexpr auto IBL_FORMAT = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
		constexpr auto IBL_USAGE = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
		constexpr std::uint32_t MAX_SOURCE_SIZE = 128;
		constexpr std::uint32_t IRRADIANCE_SIZE = 16;
		constexpr std::uint32_t REFLECTION_SIZE = 64;
		constexpr std::uint32_t REFLECTION_LEVELS = std::bit_width(REFLECTION_SIZE);
		constexpr std::uint32_t BRDF_SIZE = 64;

		SDL_GPUTexture *CreateTexture(
			SDL_GPUDevice *Gpu, SDL_GPUTextureType Type, SDL_GPUTextureFormat Format,
			std::uint32_t Size, std::uint32_t Levels, SDLRendererMetrics *Metrics
		) {
			const SDL_GPUTextureCreateInfo Info{
				.type = Type, .format = Format, .usage = IBL_USAGE, .width = Size, .height = Size,
				.layer_count_or_depth = Type == SDL_GPU_TEXTURETYPE_CUBE ? 6u : 1u,
				.num_levels = Levels, .sample_count = SDL_GPU_SAMPLECOUNT_1,
			};
			auto *Texture = SDL_CreateGPUTexture(Gpu, &Info);
			if (!Texture) throw std::runtime_error(std::format("Could not create IBL texture: {}", SDL_GetError()));
			if (Metrics) ++Metrics->TextureCreations;
			return Texture;
		}

		SDL_GPURenderPass *BeginTarget(
			SDL_GPUCommandBuffer *Commands, SDL_GPUTexture *Texture, std::uint32_t Layer, std::uint32_t Mip,
			SDL_GPULoadOp Load
		) {
			const SDL_GPUColorTargetInfo Target{
				.texture = Texture, .mip_level = Mip, .layer_or_depth_plane = Layer,
				.clear_color = {0.0f, 0.0f, 0.0f, 1.0f}, .load_op = Load, .store_op = SDL_GPU_STOREOP_STORE,
			};
			auto *Pass = SDL_BeginGPURenderPass(Commands, &Target, 1, nullptr);
			if (!Pass) throw std::runtime_error(std::format("Could not begin IBL render pass: {}", SDL_GetError()));
			return Pass;
		}

		void DrawTarget(
			SDL_GPUCommandBuffer *Commands, SDL_GPUTexture *Texture, std::uint32_t Layer, std::uint32_t Mip,
			std::uint32_t Size, SDL_GPUGraphicsPipeline *Pipeline,
			const SDL_GPUTextureSamplerBinding *Input, const void *Uniforms, std::uint32_t UniformBytes
		) {
			auto *Pass = BeginTarget(Commands, Texture, Layer, Mip, SDL_GPU_LOADOP_DONT_CARE);
			const SDL_GPUViewport Viewport{0.0f, 0.0f, static_cast<float>(Size), static_cast<float>(Size), 0.0f, 1.0f};
			SDL_SetGPUViewport(Pass, &Viewport);
			SDL_BindGPUGraphicsPipeline(Pass, Pipeline);
			if (Input) SDL_BindGPUFragmentSamplers(Pass, 0, Input, 1);
			SDL_PushGPUFragmentUniformData(Commands, 0, Uniforms, UniformBytes);
			SDL_DrawGPUPrimitives(Pass, 3, 1, 0, 0);
			SDL_EndGPURenderPass(Pass);
		}

		SDL_GPUCommandBuffer *Acquire(SDL_GPUDevice *Gpu) {
			auto *Commands = SDL_AcquireGPUCommandBuffer(Gpu);
			if (!Commands) throw std::runtime_error(std::format("Could not acquire IBL commands: {}", SDL_GetError()));
			return Commands;
		}

		void Submit(SDL_GPUCommandBuffer *&Commands) {
			if (!SDL_SubmitGPUCommandBuffer(std::exchange(Commands, nullptr)))
				throw std::runtime_error(std::format("Could not submit IBL commands: {}", SDL_GetError()));
		}
	}

	SDLImageBasedLighting::~SDLImageBasedLighting() { Destroy(); }

	void SDLImageBasedLighting::Release(SDL_GPUTexture *&Texture, SDLRendererMetrics *Metrics) {
		if (!Texture) return;
		SDL_ReleaseGPUTexture(Gpu, std::exchange(Texture, nullptr));
		if (Metrics) ++Metrics->TextureReleases;
	}

	void SDLImageBasedLighting::ReleaseSky(SDLRendererMetrics *Metrics) {
		Release(Irradiance, Metrics);
		Release(Reflections, Metrics);
		ActiveKey.reset();
	}

	void SDLImageBasedLighting::Destroy() {
		ReleaseSky(nullptr);
		Release(BrdfLut, nullptr);
		Release(FallbackCube, nullptr);
		if (PreparationFence) SDL_ReleaseGPUFence(Gpu, std::exchange(PreparationFence, nullptr));
		if (Sampler) SDL_ReleaseGPUSampler(Gpu, std::exchange(Sampler, nullptr));
		for (std::size_t Index = 0; Index < Pipelines.size(); ++Index) {
			if (Pipelines[Index]) SDL_ReleaseGPUGraphicsPipeline(Gpu, std::exchange(Pipelines[Index], nullptr));
			Shaders[Index].Destroy(Gpu);
		}
	}

	void SDLImageBasedLighting::EnsureFallback(SDLRendererMetrics *Metrics) {
		if (FallbackCube) return;
		if (!Sampler) {
			const SDL_GPUSamplerCreateInfo Info{
				.min_filter = SDL_GPU_FILTER_LINEAR, .mag_filter = SDL_GPU_FILTER_LINEAR,
				.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR,
				.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
				.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
				.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
				.max_lod = static_cast<float>(std::bit_width(MAX_SOURCE_SIZE) - 1),
			};
			Sampler = SDL_CreateGPUSampler(Gpu, &Info);
			if (!Sampler) throw std::runtime_error(std::format("Could not create IBL sampler: {}", SDL_GetError()));
		}
		SDL_GPUCommandBuffer *Commands = nullptr;
		SDL_GPUTexture *Cube = nullptr;
		try {
			Cube = CreateTexture(Gpu, SDL_GPU_TEXTURETYPE_CUBE, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1, 1, Metrics);
			Commands = Acquire(Gpu);
			for (std::uint32_t Face = 0; Face < 6; ++Face)
				SDL_EndGPURenderPass(BeginTarget(Commands, Cube, Face, 0, SDL_GPU_LOADOP_CLEAR));
			Submit(Commands);
		} catch (...) {
			if (Commands) SDL_CancelGPUCommandBuffer(Commands);
			Release(Cube, Metrics);
			throw;
		}
		FallbackCube = Cube;
	}

	bool SDLImageBasedLighting::EnsurePipelines(SDLRendererMetrics *Metrics) {
		if (PreprocessingUnavailable) return false;
		if (Pipelines.back()) return true;
		if (!SDL_GPUTextureSupportsFormat(Gpu, IBL_FORMAT, SDL_GPU_TEXTURETYPE_CUBE, IBL_USAGE) ||
			!SDL_GPUTextureSupportsFormat(Gpu, IBL_FORMAT, SDL_GPU_TEXTURETYPE_2D, IBL_USAGE)) {
			PreprocessingUnavailable = true;
			SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "[Render:IBL] Float environment targets unavailable. Using authored ambient.");
			return false;
		}
		constexpr std::array FragmentFiles{"ibl_source.frag", "ibl_convolve.frag", "ibl_brdf.frag"};
		const SDL_GPUColorTargetDescription Color{.format = IBL_FORMAT};
		try {
			for (std::size_t Index = 0; Index < Pipelines.size(); ++Index) {
				auto &Shader = Shaders[Index];
				Shader.VertexFilepath = GetSDLShaderPath("ibl_fullscreen.vert");
				Shader.VertexUniformBufferCount = 0;
				Shader.FragmentFilepath = GetSDLShaderPath(FragmentFiles[Index]);
				Shader.FragmentUniformBufferCount = 1;
				Shader.FragmentSamplerCount = Index == 2 ? 0u : 1u;
				Shader.Init(Gpu, Metrics);
				const SDL_GPUGraphicsPipelineCreateInfo Info{
					.vertex_shader = Shader.VertexShader, .fragment_shader = Shader.FragmentShader,
					.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
					.rasterizer_state = {.cull_mode = SDL_GPU_CULLMODE_NONE},
					.multisample_state = {.sample_count = SDL_GPU_SAMPLECOUNT_1},
					.target_info = {.color_target_descriptions = &Color, .num_color_targets = 1},
				};
				Pipelines[Index] = SDL_CreateGPUGraphicsPipeline(Gpu, &Info);
				if (!Pipelines[Index])
					throw std::runtime_error(std::format("Could not create IBL pipeline: {}", SDL_GetError()));
				if (Metrics) ++Metrics->PipelineCreations;
			}
		} catch (const std::exception &Error) {
			for (std::size_t Index = 0; Index < Pipelines.size(); ++Index) {
				if (Pipelines[Index]) SDL_ReleaseGPUGraphicsPipeline(Gpu, std::exchange(Pipelines[Index], nullptr));
				Shaders[Index].Destroy(Gpu);
			}
			PreprocessingUnavailable = true;
			SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "[Render:IBL] %s. Using authored ambient.", Error.what());
			return false;
		}
		return true;
	}

	bool SDLImageBasedLighting::CanBuildSky() {
		if (!PreparationFence) return true;
		if (!SDL_QueryGPUFence(Gpu, PreparationFence)) return false;
		SDL_ReleaseGPUFence(Gpu, std::exchange(PreparationFence, nullptr));
		return true;
	}

	void SDLImageBasedLighting::BuildSky(const SkyKey &Key, SDLTextureCache &Textures, SDLRendererMetrics *Metrics) {
		const auto SourceSize = std::min(std::bit_ceil(Key.Dimension), MAX_SOURCE_SIZE);
		const auto SourceLevels = std::bit_width(SourceSize);
		SDL_GPUTexture *SourceCube = nullptr;
		SDL_GPUTexture *NextIrradiance = nullptr;
		SDL_GPUTexture *NextReflections = nullptr;
		SDL_GPUTexture *NextBrdf = nullptr;
		SDL_GPUCommandBuffer *Commands = nullptr;
		try {
			SourceCube = CreateTexture(Gpu, SDL_GPU_TEXTURETYPE_CUBE, IBL_FORMAT, SourceSize, SourceLevels, Metrics);
			NextIrradiance = CreateTexture(Gpu, SDL_GPU_TEXTURETYPE_CUBE, IBL_FORMAT, IRRADIANCE_SIZE, 1, Metrics);
			NextReflections = CreateTexture(Gpu, SDL_GPU_TEXTURETYPE_CUBE, IBL_FORMAT, REFLECTION_SIZE, REFLECTION_LEVELS, Metrics);
			if (!BrdfLut) NextBrdf = CreateTexture(Gpu, SDL_GPU_TEXTURETYPE_2D, IBL_FORMAT, BRDF_SIZE, 1, Metrics);
			Commands = Acquire(Gpu);
			const glm::vec4 SourceDimensions(static_cast<float>(SourceSize), 0.0f, 0.0f, 0.0f);
			for (std::uint32_t Face = 0; Face < 6; ++Face) {
				auto *Texture = Textures.Find(Key.Faces[Face].first);
				if (!Texture) throw std::logic_error("Sky source texture lost SDL residency");
				const SDL_GPUTextureSamplerBinding Binding{.texture = Texture, .sampler = Sampler};
				DrawTarget(Commands, SourceCube, Face, 0, SourceSize, Pipelines[0], &Binding, &SourceDimensions, sizeof(SourceDimensions));
			}
			if (SourceLevels > 1) SDL_GenerateMipmapsForGPUTexture(Commands, SourceCube);
			const SDL_GPUTextureSamplerBinding Source{.texture = SourceCube, .sampler = Sampler};
			const glm::vec4 SourceInfo(static_cast<float>(SourceSize), static_cast<float>(SourceLevels - 1), 0.0f, 0.0f);
			for (std::uint32_t Face = 0; Face < 6; ++Face) {
				const std::array Diffuse{glm::vec4(Face, IRRADIANCE_SIZE, 0.0f, 1.0f), SourceInfo};
				DrawTarget(Commands, NextIrradiance, Face, 0, IRRADIANCE_SIZE, Pipelines[1], &Source, &Diffuse, sizeof(Diffuse));
				for (std::uint32_t Level = 0; Level < REFLECTION_LEVELS; ++Level) {
					const auto Size = REFLECTION_SIZE >> Level;
					const auto Roughness = static_cast<float>(Level) / static_cast<float>(REFLECTION_LEVELS - 1);
					const std::array Specular{glm::vec4(Face, Size, Roughness, 0.0f), SourceInfo};
					DrawTarget(Commands, NextReflections, Face, Level, Size, Pipelines[1], &Source, &Specular, sizeof(Specular));
				}
			}
			if (NextBrdf) {
				const glm::vec4 BrdfDimensions(static_cast<float>(BRDF_SIZE), 0.0f, 0.0f, 0.0f);
				DrawTarget(Commands, NextBrdf, 0, 0, BRDF_SIZE, Pipelines[2], nullptr, &BrdfDimensions, sizeof(BrdfDimensions));
			}
			PreparationFence = SDL_SubmitGPUCommandBufferAndAcquireFence(std::exchange(Commands, nullptr));
			if (!PreparationFence)
				throw std::runtime_error(std::format("Could not submit IBL preparation: {}", SDL_GetError()));
		} catch (...) {
			if (Commands) SDL_CancelGPUCommandBuffer(Commands);
			Release(SourceCube, Metrics);
			Release(NextIrradiance, Metrics);
			Release(NextReflections, Metrics);
			Release(NextBrdf, Metrics);
			throw;
		}
		Release(SourceCube, Metrics);
		ReleaseSky(Metrics);
		Irradiance = NextIrradiance;
		Reflections = NextReflections;
		if (NextBrdf) BrdfLut = NextBrdf;
		ActiveKey = Key;
		if (Metrics) {
			++Metrics->IblBuilds;
			Metrics->IblPreprocessPasses += 6 * (2 + REFLECTION_LEVELS) + (NextBrdf ? 1 : 0);
		}
	}

	SDLImageBasedLightingBindings SDLImageBasedLighting::Prepare(
		const std::optional<RenderSkyState> &Sky, SDLTextureCache &Textures, SDLRendererMetrics *Metrics
	) {
		if (Metrics) Metrics->IblLastPrepareNanoseconds = 0;
		EnsureFallback(Metrics);
		bool Ready = false;
		if (!Sky) {
			ReleaseSky(Metrics);
			RejectedKey.reset();
		} else {
			SkyKey Key{.ResidencyGeneration = Textures.GetResidencyGeneration(), .Dimension = Sky->FaceDimension};
			for (std::size_t Face = 0; Face < Key.Faces.size(); ++Face)
				Key.Faces[Face] = {Sky->Faces[Face].Texture, Sky->Faces[Face].ContentRevision};
			if (ActiveKey == Key) {
				if (Metrics) ++Metrics->IblCacheHits;
			} else if (RejectedKey != Key && !PreprocessingUnavailable && CanBuildSky()) {
				const auto Start = std::chrono::steady_clock::now();
				try {
					if (EnsurePipelines(Metrics)) {
						BuildSky(Key, Textures, Metrics);
						RejectedKey.reset();
					}
				} catch (const std::exception &Error) {
					ReleaseSky(Metrics);
					RejectedKey = Key;
					SDL_LogWarn(SDL_LOG_CATEGORY_GPU, "[Render:IBL] %s. Using authored ambient.", Error.what());
				}
				if (Metrics) Metrics->IblLastPrepareNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - Start).count();
			}
			// When a newer Sky arrives during preparation, keep the previous
			// resource alive but never light the new Sky with stale radiance.
			Ready = ActiveKey == Key;
		}
		return {
			.Irradiance = {Ready ? Irradiance : FallbackCube, Sampler},
			.Reflections = {Ready ? Reflections : FallbackCube, Sampler},
			.BrdfLut = {BrdfLut ? BrdfLut : Textures.GetWhiteTexture(), Sampler},
			.Enabled = Ready,
			.ReflectionMaxLod = Ready ? static_cast<float>(REFLECTION_LEVELS - 1) : 0.0f,
		};
	}
}
