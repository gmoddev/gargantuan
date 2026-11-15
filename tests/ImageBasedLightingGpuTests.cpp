// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, you can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/editor/EditorViewport.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "render/sdl/SDLImageBasedLighting.hpp"
#include "render/sdl/SDLTextureCache.hpp"

#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
	using namespace gargantuan;
	constexpr std::uint32_t FACE_SIZE = 8;
	constexpr std::uint32_t VIEWPORT_SIZE = 128;
	constexpr auto SHADER_FORMATS = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB;

	void Require(bool Condition, const char *Message) {
		if (!Condition) throw std::runtime_error(Message);
	}

	class VideoGuard final {
	  public:
		VideoGuard() { Require(SDL_InitSubSystem(SDL_INIT_VIDEO), SDL_GetError()); }
		~VideoGuard() { SDL_QuitSubSystem(SDL_INIT_VIDEO); }
	};

	struct CacheFixture {
		SDLRendererMetrics Metrics;
		SDLTextureCache Cache;
		SDLImageBasedLighting Lighting;
		explicit CacheFixture(SDL_GPUDevice *Gpu, std::uint64_t MaterialBudget = 128ull * 1024 * 1024,
			std::uint64_t SourceBudget = 128ull * 1024 * 1024)
			: Cache(Gpu, &Metrics, MaterialBudget, SourceBudget), Lighting(Gpu) {}
		~CacheFixture() {
			Lighting.Destroy();
			Cache.Destroy();
		}
	};

	std::shared_ptr<const std::vector<std::uint8_t>> FacePixels(std::size_t Face, std::size_t HighlightComponent) {
		auto Pixels = std::make_shared<std::vector<std::uint8_t>>(FACE_SIZE * FACE_SIZE * 4);
		for (std::uint32_t Y = 0; Y < FACE_SIZE; ++Y) {
			for (std::uint32_t X = 0; X < FACE_SIZE; ++X) {
				const auto Offset = (Y * FACE_SIZE + X) * 4;
				(*Pixels)[Offset + 3] = 255;
				if (Face == 4 && X >= 1 && X <= 6 && Y >= 1 && Y <= 4) (*Pixels)[Offset + HighlightComponent] = 255;
				if (Face == 5) (*Pixels)[Offset + 2] = 128;
			}
		}
		return Pixels;
	}

	std::shared_ptr<const std::vector<std::uint8_t>> HdrFacePixels(float Radiance) {
		auto Pixels = std::make_shared<std::vector<std::uint8_t>>();
		Pixels->reserve(FACE_SIZE * FACE_SIZE * 16);
		for (std::size_t Pixel = 0; Pixel < FACE_SIZE * FACE_SIZE; ++Pixel)
			for (const float Channel : {Radiance, Radiance * 0.5f, Radiance * 0.25f, 1.0f}) {
				const auto Bits = std::bit_cast<std::uint32_t>(Channel);
				for (unsigned Byte = 0; Byte < 4; ++Byte)
					Pixels->push_back(static_cast<std::uint8_t>(Bits >> (Byte * 8)));
			}
		return Pixels;
	}

	RenderSkyState PublishSky(RenderPublication &Publication, std::size_t HighlightComponent, std::uint64_t Revision = 1) {
		RenderSkyState Sky{.FaceDimension = FACE_SIZE};
		for (std::size_t Face = 0; Face < Sky.Faces.size(); ++Face) {
			const RenderTextureIdentity Identity{Face + 100, 1};
			const auto FaceRevision = Face == 4 ? Revision : 1u;
			Publication.TextureCreates.push_back({
				Identity, FaceRevision, FACE_SIZE, FACE_SIZE, RenderTextureFormat::Rgba8Unorm,
				FacePixels(Face, HighlightComponent),
			});
			Sky.Faces[Face] = {Identity, FaceRevision};
		}
		return Sky;
	}

	glm::vec3 ReadTexel(SDL_GPUDevice *Gpu, SDL_GPUTexture *Texture, std::uint32_t Layer,
		std::uint32_t Mip, std::uint32_t X, std::uint32_t Y) {
		const SDL_GPUTransferBufferCreateInfo BufferInfo{.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = 8};
		const auto ReleaseBuffer = [Gpu](SDL_GPUTransferBuffer *Buffer) { SDL_ReleaseGPUTransferBuffer(Gpu, Buffer); };
		std::unique_ptr<SDL_GPUTransferBuffer, decltype(ReleaseBuffer)> Buffer(SDL_CreateGPUTransferBuffer(Gpu, &BufferInfo), ReleaseBuffer);
		Require(Buffer != nullptr, SDL_GetError());
		auto *Commands = SDL_AcquireGPUCommandBuffer(Gpu);
		Require(Commands != nullptr, SDL_GetError());
		auto *Copy = SDL_BeginGPUCopyPass(Commands);
		if (!Copy) {
			SDL_CancelGPUCommandBuffer(Commands);
			throw std::runtime_error("Could not begin environment test readback");
		}
		const SDL_GPUTextureRegion Source{
			.texture = Texture, .mip_level = Mip, .layer = Layer, .x = X, .y = Y, .w = 1, .h = 1, .d = 1,
		};
		const SDL_GPUTextureTransferInfo Destination{.transfer_buffer = Buffer.get(), .pixels_per_row = 1, .rows_per_layer = 1};
		SDL_DownloadFromGPUTexture(Copy, &Source, &Destination);
		SDL_EndGPUCopyPass(Copy);
		auto *Fence = SDL_SubmitGPUCommandBufferAndAcquireFence(Commands);
		Require(Fence != nullptr, SDL_GetError());
		const bool Completed = SDL_WaitForGPUFences(Gpu, true, &Fence, 1);
		SDL_ReleaseGPUFence(Gpu, Fence);
		Require(Completed, SDL_GetError());
		const auto *Mapped = SDL_MapGPUTransferBuffer(Gpu, Buffer.get(), false);
		Require(Mapped != nullptr, SDL_GetError());
		std::array<std::uint16_t, 4> Pixel;
		std::memcpy(Pixel.data(), Mapped, sizeof(Pixel));
		SDL_UnmapGPUTransferBuffer(Gpu, Buffer.get());
		return {glm::unpackHalf1x16(Pixel[0]), glm::unpackHalf1x16(Pixel[1]), glm::unpackHalf1x16(Pixel[2])};
	}

	void CheckLargestSource(SDL_GPUDevice *Gpu, CacheFixture &Fixture, SDLImageBasedLighting &Lighting) {
		constexpr std::uint32_t Dimension = 1024;
		constexpr RenderTextureIdentity Image{400, 1};
		auto Pixels = std::make_shared<std::vector<std::uint8_t>>(Dimension * Dimension * 4);
		for (std::uint32_t Y = 0; Y < Dimension; ++Y) {
			for (std::uint32_t X = 0; X < Dimension; ++X) {
				const auto Offset = (Y * Dimension + X) * 4;
				(*Pixels)[Offset] = (X ^ Y) & 1u ? 255 : 0;
				(*Pixels)[Offset + 3] = 255;
			}
		}
		RenderPublication Publication{.FullResync = true};
		Publication.TextureCreates.push_back({Image, 1, Dimension, Dimension, RenderTextureFormat::Rgba8Unorm, Pixels});
		RenderSkyState Sky{.FaceDimension = Dimension};
		Sky.Faces.fill({Image, 1});
		Fixture.Cache.ApplyPublication(Publication);
		Require(SDL_WaitForGPUIdle(Gpu), SDL_GetError());
		const auto Start = std::chrono::steady_clock::now();
		const auto Prepared = Lighting.Prepare(Sky, Fixture.Cache, &Fixture.Metrics);
		Require(Prepared.Enabled && SDL_WaitForGPUIdle(Gpu), "largest supported Sky prepares and drains");
		const auto Wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
		const auto Cpu = static_cast<double>(Fixture.Metrics.IblLastPrepareNanoseconds) / 1'000'000.0;
		const auto Mirror = ReadTexel(Gpu, Prepared.Reflections.texture, 4, 0, 32, 32);
		const auto Diffuse = ReadTexel(Gpu, Prepared.Irradiance.texture, 5, 0, 8, 8);
		Require(std::abs(Mirror.r - 0.5f) < 0.005f && std::abs(Diffuse.r - 0.5f) < 0.005f,
			"maximum-resolution black/white checkerboard preserves half linear energy after downsampling");
		constexpr int WarmFrames = 4096;
		const auto WarmStart = std::chrono::steady_clock::now();
		const auto Builds = Fixture.Metrics.IblBuilds;
		const auto CacheHits = Fixture.Metrics.IblCacheHits;
		for (int Frame = 0; Frame < WarmFrames; ++Frame) {
			const auto Warm = Lighting.Prepare(Sky, Fixture.Cache, &Fixture.Metrics);
			Require(Warm.Reflections.texture == Prepared.Reflections.texture, "warm Sky preserves its reflection texture");
		}
		const auto Warm = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - WarmStart).count() / WarmFrames;
		Require(Fixture.Metrics.IblBuilds == Builds && Fixture.Metrics.IblCacheHits == CacheHits + WarmFrames,
			"warm cache never resubmits environment preprocessing");
		std::cout << "[Render:IBLGpu] 1024px preprocess CPU ms=" << Cpu << " completion wall ms=" << Wall
			<< " warm cache CPU ns/frame=" << Warm << " mirror red=" << Mirror.r << " diffuse red=" << Diffuse.r << '\n';
	}

	void CheckGpuResources() {
		const std::unique_ptr<SDL_GPUDevice, decltype(&SDL_DestroyGPUDevice)> Gpu(
			SDL_CreateGPUDevice(SHADER_FORMATS, false, nullptr), SDL_DestroyGPUDevice);
		Require(Gpu != nullptr, SDL_GetError());
		CacheFixture Fixture(Gpu.get());
		RenderPublication Initial{.FullResync = true};
		auto Sky = PublishSky(Initial, 0);
		Fixture.Cache.ApplyPublication(Initial);
		const auto First = Fixture.Lighting.Prepare(Sky, Fixture.Cache, &Fixture.Metrics);
		Require(First.Enabled && First.ReflectionMaxLod == 6.0f && Fixture.Metrics.IblBuilds == 1,
			"first valid Sky builds a complete IBL set");
		const auto CheckSharp = [&](const SDLImageBasedLightingBindings &Bindings, std::size_t Channel) {
			const auto Pixel = ReadTexel(Gpu.get(), Bindings.Reflections.texture, 4, 0, 20, 20);
			Require(Pixel[static_cast<glm::length_t>(Channel)] > 0.85f, "sharp +Z reflection keeps the expected highlight orientation");
		};
		CheckSharp(First, 0);
		const auto Flipped = ReadTexel(Gpu.get(), First.Reflections.texture, 4, 0, 20, 44);
		Require(Flipped.r < 0.05f, "cubemap source and output keep top-left texture rows");
		const auto Gray = ReadTexel(Gpu.get(), First.Reflections.texture, 5, 0, 32, 32);
		Require(std::abs(Gray.b - 0.21586f) < 0.01f && Gray.r < 0.01f, "mirror reflection samples decoded linear radiance");
		const auto Rough = ReadTexel(Gpu.get(), First.Reflections.texture, 4, 6, 0, 0);
		Require(Rough.r > 0.03f && Rough.r < 0.75f, "roughness prefilter broadens the bright spot");
		const auto Positive = ReadTexel(Gpu.get(), First.Irradiance.texture, 4, 0, 8, 8);
		const auto Negative = ReadTexel(Gpu.get(), First.Irradiance.texture, 5, 0, 8, 8);
		Require(Positive.r > Negative.r + 0.12f, "diffuse irradiance responds to hemisphere orientation");
		const auto Brdf = ReadTexel(Gpu.get(), First.BrdfLut.texture, 0, 0, 63, 63);
		Require(std::abs((Brdf.r + Brdf.g) - (1.0f - std::log(2.0f))) < 0.08f,
			"GPU split sum agrees with rough normal-view white-furnace reference");
		const auto Again = Fixture.Lighting.Prepare(Sky, Fixture.Cache, &Fixture.Metrics);
		Require(Again.Reflections.texture == First.Reflections.texture && Fixture.Metrics.IblCacheHits == 1 &&
			Fixture.Metrics.IblBuilds == 1 && Fixture.Metrics.IblLastPrepareNanoseconds == 0,
			"unchanged Sky reuses GPU resources without preprocessing");
		RenderPublication Reimport;
		++Sky.Faces[4].ContentRevision;
		Reimport.TextureUpdates.push_back({Sky.Faces[4].Texture, 2, 0, 0, FACE_SIZE, FACE_SIZE, FacePixels(4, 1)});
		Fixture.Cache.ApplyPublication(Reimport);
		const auto Updated = Fixture.Lighting.Prepare(Sky, Fixture.Cache, &Fixture.Metrics);
		Require(Updated.Enabled && Updated.BrdfLut.texture == First.BrdfLut.texture && Fixture.Metrics.IblBuilds == 2 &&
			Fixture.Metrics.IblPreprocessPasses == 109, "new Sky texels rebuild lighting but reuse the device BRDF lookup");
		CheckSharp(Updated, 1);
		RenderPublication Reconstruction{.FullResync = true};
		const auto ReusedIds = PublishSky(Reconstruction, 2, 2);
		Fixture.Cache.ApplyPublication(Reconstruction);
		const auto Rebuilt = Fixture.Lighting.Prepare(ReusedIds, Fixture.Cache, &Fixture.Metrics);
		Require(Rebuilt.Enabled && Fixture.Metrics.IblBuilds == 3, "full residency resync invalidates even an identical logical key");
		CheckSharp(Rebuilt, 2);
		const auto Fallback = Fixture.Lighting.Prepare(std::nullopt, Fixture.Cache, &Fixture.Metrics);
		Require(!Fallback.Enabled && Fallback.Irradiance.texture && Fallback.Reflections.texture && Fallback.BrdfLut.texture,
			"absent Sky disables IBL and supplies valid fallback bindings");
		Fixture.Lighting.Destroy();
		SDLImageBasedLighting Recreated(Gpu.get());
		const auto AfterRestart = Recreated.Prepare(ReusedIds, Fixture.Cache, &Fixture.Metrics);
		Require(AfterRestart.Enabled && Fixture.Metrics.IblBuilds == 4, "renderer-owned IBL resources can be recreated from residency");
		CheckSharp(AfterRestart, 2);
		CheckLargestSource(Gpu.get(), Fixture, Recreated);
		if (SDL_GPUTextureSupportsFormat(Gpu.get(), SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,
			SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
			RenderPublication HdrPublication{.FullResync = true};
			RenderSkyState HdrSky{.FaceDimension = FACE_SIZE};
			for (std::size_t Face = 0; Face < 6; ++Face) {
				const RenderTextureIdentity Identity{Face + 500, 1};
				HdrPublication.TextureCreates.push_back({Identity, 1, FACE_SIZE, FACE_SIZE,
					RenderTextureFormat::Rgba32Float, HdrFacePixels(10000.0f)});
				HdrSky.Faces[Face] = {Identity, 1};
			}
			Fixture.Cache.ApplyPublication(HdrPublication);
			const auto HdrLighting = Recreated.Prepare(HdrSky, Fixture.Cache, &Fixture.Metrics);
			Require(HdrLighting.Enabled, "HDR faces prepare image-based lighting");
			for (std::uint32_t Level = 0; Level <= 6; ++Level) {
				const auto Texel = ReadTexel(Gpu.get(), HdrLighting.Reflections.texture, 4, Level,
					std::max(0u, (64u >> Level) / 2), std::max(0u, (64u >> Level) / 2));
				Require(std::isfinite(Texel.r) && std::isfinite(Texel.g) && std::isfinite(Texel.b) &&
					std::abs(Texel.r - 10000.0f) < 25.0f && std::abs(Texel.g - 5000.0f) < 15.0f,
					"HDR reflection mip retains finite linear radiance above SDR white");
			}
			const auto HdrDiffuse = ReadTexel(Gpu.get(), HdrLighting.Irradiance.texture, 4, 0, 8, 8);
			Require(std::isfinite(HdrDiffuse.r) && std::abs(HdrDiffuse.r - 10000.0f) < 25.0f,
				"HDR diffuse irradiance remains finite at high radiance");
			RenderPublication ExtremePublication{.FullResync = true};
			RenderSkyState ExtremeSky{.FaceDimension = FACE_SIZE};
			for (std::size_t Face = 0; Face < 6; ++Face) {
				const RenderTextureIdentity Identity{Face + 550, 1};
				ExtremePublication.TextureCreates.push_back({Identity, 1, FACE_SIZE, FACE_SIZE,
					RenderTextureFormat::Rgba32Float, HdrFacePixels(1.0e20f)});
				ExtremeSky.Faces[Face] = {Identity, 1};
			}
			Fixture.Cache.ApplyPublication(ExtremePublication);
			const auto ExtremeLighting = Recreated.Prepare(ExtremeSky, Fixture.Cache, &Fixture.Metrics);
			Require(ExtremeLighting.Enabled, "finite float32 radiance above half-float range prepares IBL");
			for (std::uint32_t Level = 0; Level <= 6; ++Level) {
				const auto Texel = ReadTexel(Gpu.get(), ExtremeLighting.Reflections.texture, 4, Level,
					std::max(0u, (64u >> Level) / 2), std::max(0u, (64u >> Level) / 2));
				Require(std::isfinite(Texel.r) && std::isfinite(Texel.g) && std::isfinite(Texel.b) &&
					std::abs(Texel.r - 65504.0f) < 64.0f && std::abs(Texel.g - 65504.0f) < 64.0f,
					"extreme canonical radiance clamps before every half-float reflection level");
			}
			const auto ExtremeDiffuse = ReadTexel(Gpu.get(), ExtremeLighting.Irradiance.texture, 4, 0, 8, 8);
			Require(std::isfinite(ExtremeDiffuse.r) && std::abs(ExtremeDiffuse.r - 65504.0f) < 64.0f,
				"extreme canonical radiance clamps before half-float diffuse convolution");
		}
		{
			constexpr std::uint32_t Dimension = 1024;
			auto Pixels = std::make_shared<const std::vector<std::uint8_t>>(
				static_cast<std::size_t>(Dimension) * Dimension * 4, 128);
			RenderPublication LargeTextures{.FullResync = true};
			for (std::uint64_t Index = 0; Index < 5; ++Index)
				LargeTextures.TextureCreates.push_back({{600 + Index, 1}, 1, Dimension, Dimension,
					RenderTextureFormat::Rgba8Unorm, Pixels});
			Fixture.Cache.ApplyPublication(LargeTextures);
			Require(Fixture.Cache.GetBaseUploadStagingBytes() <= 16ull * 1024 * 1024 &&
				Fixture.Cache.GetBaseUploadStagingEvictions() > 0,
				"completed bulk upload trims long-lived base staging to the residency budget");
			auto *Resident = Fixture.Cache.Find({600, 1});
			RenderPublication ReimportTexture;
			ReimportTexture.TextureUpdates.push_back({{600, 1}, 2, 0, 0, Dimension, Dimension, Pixels});
			Fixture.Cache.ApplyPublication(ReimportTexture);
			Require(Resident && Fixture.Cache.Find({600, 1}) == Resident &&
				Fixture.Cache.GetBaseUploadStagingBytes() <= 16ull * 1024 * 1024,
				"evicted upload staging is rebuilt without evicting the resident GPU texture");
		}
		if (SDL_GPUTextureSupportsFormat(Gpu.get(), SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB,
			SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
			constexpr std::uint32_t SourceDimension = 256;
			constexpr std::uint64_t SourceBytes = SourceDimension * SourceDimension * 4ull;
			constexpr RenderTextureIdentity FirstSource{900, 1}, LateSource{932, 1};
			CacheFixture SourcePressure(Gpu.get(), 128ull * 1024 * 1024, 32 * SourceBytes);
			RenderPublication BulkSources{.FullResync = true};
			for (std::uint64_t Index = 0; Index < 33; ++Index)
				BulkSources.TextureCreates.push_back({{900 + Index, 1}, 1, SourceDimension, SourceDimension,
					RenderTextureFormat::Rgba8Unorm,
					std::make_shared<const std::vector<std::uint8_t>>(SourceBytes, static_cast<std::uint8_t>(Index))});
			SourcePressure.Cache.ApplyPublication(BulkSources);
			Require(SourcePressure.Cache.GetMaterialSourceBytes() == SourcePressure.Cache.GetMaterialSourceBudget() &&
				SourcePressure.Cache.GetMaterialSourceRehydrates() == 0,
				"thirty-three full images initially retain exactly thirty-two sources under the scaled source budget");
			auto *LateBase = SourcePressure.Cache.Find(LateSource);
			Require(SourcePressure.Cache.FindMaterialTexture(LateSource, true).Texture == LateBase,
				"late material demand samples its resident base until source promotion and mip upload");
			SourcePressure.Cache.ApplyPublication(RenderPublication{});
			const auto LateView = SourcePressure.Cache.FindMaterialTexture(LateSource, true);
			Require(LateView.Texture != LateBase && LateView.NativeSrgb && LateView.HasMipmaps &&
				SourcePressure.Cache.GetMaterialSourceEvictions() == 1 &&
				SourcePressure.Cache.GetMaterialSourceRehydrates() == 1 &&
				SourcePressure.Cache.GetMaterialSourceBytes() == SourcePressure.Cache.GetMaterialSourceBudget(),
				"late visible image displaces an undemanded source and gains a bounded native mip view");
			BulkSources.TextureCreates.clear(); // The evicted first backing now expires.
			const auto RehydratesBeforePartial = SourcePressure.Cache.GetMaterialSourceRehydrates();
			RenderPublication PartialSource;
			PartialSource.TextureUpdates.push_back({FirstSource, 2, 0, 0, 1, 1,
				std::make_shared<const std::vector<std::uint8_t>>(
					std::initializer_list<std::uint8_t>{0, 0, 255, 255})});
			SourcePressure.Cache.ApplyPublication(PartialSource);
			Require(SourcePressure.Cache.FindMaterialTexture(FirstSource, true).Texture ==
				SourcePressure.Cache.Find(FirstSource) &&
				SourcePressure.Cache.GetMaterialSourceRehydrates() == RehydratesBeforePartial,
				"partial update never reconstructs a new revision from expired old source bytes");
			RenderPublication FullSource;
			FullSource.TextureUpdates.push_back({FirstSource, 3, 0, 0, SourceDimension, SourceDimension,
				std::make_shared<const std::vector<std::uint8_t>>(SourceBytes, 192)});
			SourcePressure.Cache.ApplyPublication(FullSource);
			(void)SourcePressure.Cache.FindMaterialTexture(FirstSource, true);
			SourcePressure.Cache.ApplyPublication(RenderPublication{});
			Require(SourcePressure.Cache.FindMaterialTexture(FirstSource, true).Texture !=
				SourcePressure.Cache.Find(FirstSource) &&
				SourcePressure.Cache.GetMaterialSourceBytes() <= SourcePressure.Cache.GetMaterialSourceBudget(),
				"complete source replacement restores mips after an unrehydratable partial update");
			CacheFixture PartialPressure(Gpu.get(), 128ull * 1024 * 1024, SourceBytes);
			constexpr RenderTextureIdentity PartialA{940, 1}, PartialB{941, 1};
			RenderPublication RetainedBacking{.FullResync = true};
			RetainedBacking.TextureCreates.push_back({PartialA, 1, SourceDimension, SourceDimension,
				RenderTextureFormat::Rgba8Unorm,
				std::make_shared<const std::vector<std::uint8_t>>(SourceBytes, 64)});
			RetainedBacking.TextureCreates.push_back({PartialB, 1, SourceDimension, SourceDimension,
				RenderTextureFormat::Rgba8Unorm,
				std::make_shared<const std::vector<std::uint8_t>>(SourceBytes, 128)});
			PartialPressure.Cache.ApplyPublication(RetainedBacking);
			(void)PartialPressure.Cache.FindMaterialTexture(PartialB, true);
			PartialPressure.Cache.ApplyPublication(RenderPublication{});
			PartialPressure.Cache.ApplyPublication(RenderPublication{});
			RenderPublication PatchRetained;
			PatchRetained.TextureUpdates.push_back({PartialA, 2, 0, 0, 1, 1,
				std::make_shared<const std::vector<std::uint8_t>>(
					std::initializer_list<std::uint8_t>{255, 0, 0, 255})});
			PartialPressure.Cache.ApplyPublication(PatchRetained);
			(void)PartialPressure.Cache.FindMaterialTexture(PartialA, true);
			PartialPressure.Cache.ApplyPublication(RenderPublication{});
			Require(PartialPressure.Cache.FindMaterialTexture(PartialA, true).Texture !=
				PartialPressure.Cache.Find(PartialA) &&
				PartialPressure.Cache.GetMaterialSourceRehydrates() >= 2 &&
				PartialPressure.Cache.GetMaterialSourceBytes() <= PartialPressure.Cache.GetMaterialSourceBudget(),
				"partial update rebuilds from a live exact-revision backing without crossing the source budget");
			RenderPublication SourceResync{.FullResync = true};
			SourceResync.TextureCreates.push_back({{942, 1}, 1, SourceDimension, SourceDimension,
				RenderTextureFormat::Rgba8Unorm,
				std::make_shared<const std::vector<std::uint8_t>>(SourceBytes, 192)});
			PartialPressure.Cache.ApplyPublication(SourceResync);
			Require(!PartialPressure.Cache.Find(PartialA) && !PartialPressure.Cache.Find(PartialB) &&
				PartialPressure.Cache.GetMaterialSourceBytes() == SourceBytes,
				"full resync releases old source handles and resets exact source byte accounting");
			constexpr std::uint32_t Dimension = 256;
			constexpr RenderTextureIdentity A{700, 1}, B{701, 1};
			CacheFixture Pressure(Gpu.get(), 400000);
			auto Pixels = std::make_shared<const std::vector<std::uint8_t>>(
				static_cast<std::size_t>(Dimension) * Dimension * 4, 255);
			RenderPublication Sources{.FullResync = true};
			Sources.TextureCreates.push_back({A, 1, Dimension, Dimension, RenderTextureFormat::Rgba8Unorm, Pixels});
			Sources.TextureCreates.push_back({B, 1, Dimension, Dimension, RenderTextureFormat::Rgba8Unorm, Pixels});
			Pressure.Cache.ApplyPublication(Sources);
			auto *BaseA = Pressure.Cache.Find(A);
			Require(Pressure.Cache.FindMaterialTexture(A, true).Texture == BaseA,
				"first material demand uses the resident base texture until upload admission");
			(void)Pressure.Cache.FindMaterialTexture(B, true);
			Pressure.Cache.ApplyPublication(RenderPublication{});
			const auto FirstView = Pressure.Cache.FindMaterialTexture(A, true);
			Require(FirstView.Texture != BaseA && FirstView.NativeSrgb && FirstView.HasMipmaps,
				"first demanded color view gains native sRGB mips");
			(void)Pressure.Cache.FindMaterialTexture(B, true);
			Pressure.Cache.ApplyPublication(RenderPublication{});
			(void)Pressure.Cache.FindMaterialTexture(B, true);
			Pressure.Cache.ApplyPublication(RenderPublication{});
			const auto Evicted = Pressure.Cache.FindMaterialTexture(A, true);
			const auto SecondView = Pressure.Cache.FindMaterialTexture(B, true);
			Require(Evicted.Texture == BaseA && !Evicted.NativeSrgb && SecondView.Texture != Pressure.Cache.Find(B) &&
				SecondView.NativeSrgb && Pressure.Cache.GetMaterialViewEvictions() >= 1 &&
				Pressure.Cache.GetMaterialViewBytes() <= Pressure.Cache.GetMaterialViewBudget(),
				"cold view eviction preserves a defined base fallback and admits recent demand");
			CacheFixture Scheduled(Gpu.get());
			RenderPublication QueueSources{.FullResync = true};
			auto SmallPixels = std::make_shared<const std::vector<std::uint8_t>>(4 * 4 * 4, 192);
			for (std::uint64_t Index = 0; Index < 34; ++Index)
				QueueSources.TextureCreates.push_back({{800 + Index, 1}, 1, 4, 4,
					RenderTextureFormat::Rgba8Unorm, SmallPixels});
			Scheduled.Cache.ApplyPublication(QueueSources);
			for (std::uint64_t Index = 0; Index < 34; ++Index)
				(void)Scheduled.Cache.FindMaterialTexture({800 + Index, 1}, true);
			Scheduled.Cache.ApplyPublication(RenderPublication{});
			Require(Scheduled.Cache.GetDeferredMaterialViewUploads() == 2,
				"one publication admits at most 32 material mip views");
			Scheduled.Cache.ApplyPublication(RenderPublication{});
			for (std::uint64_t Index = 0; Index < 34; ++Index)
				Require(Scheduled.Cache.FindMaterialTexture({800 + Index, 1}, true).NativeSrgb,
					"deferred material views finish on a later publication without losing their base images");
		}
		std::cout << "[Render:IBLGpu] " << SDL_GetGPUDeviceDriver(Gpu.get()) << " rough red=" << Rough.r
			<< " diffuse red=" << Positive.r << '/' << Negative.r << " BRDF=" << Brdf.r + Brdf.g << '\n';
	}

	std::shared_ptr<RenderPublication> WorldChart(RenderPublicationId Id, bool WithSky) {
		auto Publication = std::make_shared<RenderPublication>();
		Publication->Id = Id;
		Publication->FullResync = true;
		Publication->Frame.ViewportWidth = VIEWPORT_SIZE;
		Publication->Frame.ViewportHeight = VIEWPORT_SIZE;
		auto &Camera = Publication->Frame.Camera;
		Camera.VerticalFieldOfView = 45.0f;
		Camera.ProjectionMatrix = glm::perspectiveRH_ZO(glm::radians(45.0f), 1.0f, Camera.NearPlane, Camera.FarPlane);
		Camera.ViewProjectionMatrix = Camera.ProjectionMatrix;
		auto &Environment = Publication->Frame.Environment;
		Environment.EnvironmentColor = glm::vec3(0.0f);
		Environment.AmbientColor = glm::vec3(0.0f);
		Environment.SunIntensity = 0.0f;
		if (WithSky) Environment.Sky = PublishSky(*Publication, 0);
		for (std::uint32_t Index = 0; Index < 3; ++Index) {
			RenderItem Item;
			Item.Object = {Index + 1u, 1};
			Item.CastShadow = false;
			Item.ModelMatrix = glm::translate(glm::mat4(1.0f), {0.95f * (static_cast<float>(Index) - 1.0f), 0.0f, -4.0f});
			Item.ModelMatrix = glm::scale(Item.ModelMatrix, {0.65f, 1.2f, 0.15f});
			Item.InverseModelMatrix = glm::inverse(Item.ModelMatrix);
			RenderMaterialState Material;
			Material.ShadingModel = RenderShadingModel::MetallicRoughness;
			Material.Metallic = Index < 2 ? 1.0f : 0.0f;
			Material.Roughness = Index == 0 ? 0.05f : 1.0f;
			Publication->Creates.push_back({.Item = Item, .Material = Material});
		}
		return Publication;
	}

	void CheckWorldChart() {
		EditorViewportRenderer Viewport(VIEWPORT_SIZE, VIEWPORT_SIZE);
		const auto Dark = Viewport.Capture(WorldChart(1, false));
		const auto Lit = Viewport.Capture(WorldChart(2, true));
		const auto RedAt = [](const EditorViewportFrame &Image, std::size_t X) {
			return Image.RgbPixels.at((VIEWPORT_SIZE / 2 * VIEWPORT_SIZE + X) * 3);
		};
		const auto Smooth = RedAt(Lit, 27);
		const auto Rough = RedAt(Lit, 64);
		const auto Dielectric = RedAt(Lit, 101);
		Require(RedAt(Dark, 27) <= 1 && RedAt(Dark, 64) <= 1 && RedAt(Dark, 101) <= 1,
			"metallic and dielectric probes stay dark without any light source");
		Require(Smooth > Rough + 20 && Rough > 8 && Dielectric > 12,
			"world shading distinguishes smooth metal, rough metal and diffuse dielectric without a sun or authored ambient");
		std::cout << "[Render:IBLGpu] World chart red channels smooth=" << static_cast<int>(Smooth)
			<< " rough=" << static_cast<int>(Rough) << " dielectric=" << static_cast<int>(Dielectric) << '\n';
	}
}

int main() try {
#ifdef _WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
	VideoGuard Video;
	CheckGpuResources();
	CheckWorldChart();
	return 0;
} catch (const std::exception &Error) {
	std::cerr << "[Render:IBLGpu] " << Error.what() << '\n';
	return 1;
}
