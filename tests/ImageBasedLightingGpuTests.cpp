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

#include <array>
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
		explicit CacheFixture(SDL_GPUDevice *Gpu) : Cache(Gpu, &Metrics), Lighting(Gpu) {}
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
