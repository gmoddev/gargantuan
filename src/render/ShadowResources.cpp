#include "render/sdl/SDLShadowResources.hpp"
#include "render/sdl/SDLRenderPass.hpp"
#include "render/sdl/SDLSpotShadowAtlas.hpp"
#include "render/sdl/SDLDirectionalShadowCache.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include <algorithm>
#include <stdexcept>

namespace gargantuan {
	void SDLShadowResources::PrepareFrame(SDL_GPUDevice *Gpu, SDLFrameContext &Frame, SDLSpotShadowAtlas &Atlas) {
		if (!Fallback) {
			const SDL_GPUTextureCreateInfo Info{
				.type = SDL_GPU_TEXTURETYPE_2D_ARRAY, .format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
				.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
				.width = 1, .height = 1, .layer_count_or_depth = 1, .num_levels = 1,
			};
			Fallback = SDL_CreateGPUTexture(Gpu, &Info);
			if (!Fallback) throw std::runtime_error("[Renderer:Shadow] Could not create the array fallback");
		}
		if (FallbackNeedsInitialization) {
			const SDL_GPUDepthStencilTargetInfo Target{
				.texture = Fallback, .clear_depth = 1, .load_op = SDL_GPU_LOADOP_CLEAR,
				.store_op = SDL_GPU_STOREOP_STORE, .stencil_load_op = SDL_GPU_LOADOP_DONT_CARE,
				.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
			};
			auto *Pass = SDL_BeginGPURenderPass(Frame.Commands, nullptr, 0, &Target);
			if (!Pass) throw std::runtime_error("[Renderer:Shadow] Could not clear the array fallback");
			SDL_EndGPURenderPass(Pass);
		}
		Frame.CascadeShadowTexture = Fallback;
		Frame.LocalShadowTexture = Fallback;
		Frame.CascadedShadows = Frame.Projection.GetFrame().Environment.DirectionalShadows.CascadeCount > 0;
		Frame.SpotShadowAtlas = nullptr;
		const auto &State = Frame.Projection.GetFrame();
		const auto &Settings = State.Environment.DirectionalShadows;
		if (Settings.CascadeCount) {
			const auto Plan = MakeSDLDirectionalCascadePlan(State.Camera, State.Environment.SunDirection,
				static_cast<float>(Frame.Width) / Frame.Height, {
					.Count = Settings.CascadeCount, .Resolution = Settings.Resolution,
					.MaximumDistance = Settings.Distance, .SplitLambda = Settings.SplitLambda,
					.TransitionFraction = Settings.TransitionFraction,
				});
			if (Plan && EnsureDirectional(Gpu, Settings.Resolution)) {
				Frame.CascadePlan = *Plan;
				Frame.CascadeShadowTexture = Directional;
			}
		} else if (Directional) {
			SDL_ReleaseGPUTexture(Gpu, Directional);
			Directional = nullptr; DirectionalResolution = 0;
			Frame.ShadowCache.Invalidate();
		}
		const auto &Lights = State.Environment.LocalLights;
		const bool WantsLocal = Lights && std::ranges::any_of(*Lights, [](const auto &Light) {
			return Light.Kind == RenderLocalLightKind::Spot && Light.ShadowEnabled && Light.Intensity > 0;
		});
		if (WantsLocal && EnsureLocal(Gpu)) {
			Frame.LocalShadowTexture = Local;
			Frame.SpotShadowAtlas = &Atlas;
		} else {
			Atlas.Invalidate();
			if (Local) { SDL_ReleaseGPUTexture(Gpu, Local); Local = nullptr; }
		}
	}
}
