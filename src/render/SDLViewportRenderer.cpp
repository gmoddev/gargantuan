#include "render/sdl/SDLViewportRenderer.hpp"

#include "gargantuan/Log.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "render/sdl/SDLSceneDepth.hpp"
#include "runtime/RuntimeWorkDiagnostics.hpp"

#include <algorithm>
#include <chrono>

namespace gargantuan {
	namespace {
		constexpr auto ColorFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		constexpr std::size_t MaximumRedrawsPerFrame = 2;

		std::uint64_t TargetBytes(std::uint32_t Width, std::uint32_t Height) {
			return std::uint64_t{Width} * Height * 8;
		}
	}

	void SDLViewportRenderer::Release(SDL_GPUDevice *Gpu, Target &Value, SDLRendererMetrics &Metrics) {
		if (Value.Palettes) Value.Palettes->Destroy();
		if (Value.Color) SDL_ReleaseGPUTexture(Gpu, Value.Color);
		if (Value.Depth) SDL_ReleaseGPUTexture(Gpu, Value.Depth);
		if (Value.Color) {
			Metrics.ViewportTargetBytes -= TargetBytes(Value.Width, Value.Height);
			++Metrics.ViewportTargetReleases;
		}
		Value = {};
	}

	bool SDLViewportRenderer::Resize(SDL_GPUDevice *Gpu, Target &Value, std::uint32_t Width, std::uint32_t Height,
		SDLRendererMetrics &Metrics
	) {
		if (Value.Color && Width == Value.Width && Height == Value.Height) return true;
		const SDL_GPUTextureCreateInfo ColorInfo{
			.type = SDL_GPU_TEXTURETYPE_2D, .format = ColorFormat,
			.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
			.width = Width, .height = Height, .layer_count_or_depth = 1, .num_levels = 1,
		};
		const auto DepthInfo = GetSDLSceneDepthTargetInfo(Gpu, Width, Height);
		auto *Color = SDL_CreateGPUTexture(Gpu, &ColorInfo);
		if (!Color) return false;
		auto *Depth = SDL_CreateGPUTexture(Gpu, &DepthInfo);
		if (!Depth) {
			SDL_ReleaseGPUTexture(Gpu, Color);
			return false;
		}
		if (Value.Color) {
			SDL_ReleaseGPUTexture(Gpu, Value.Color);
			SDL_ReleaseGPUTexture(Gpu, Value.Depth);
			Metrics.ViewportTargetBytes -= TargetBytes(Value.Width, Value.Height);
			++Metrics.ViewportTargetReleases;
		}
		Value.Color = Color;
		Value.Depth = Depth;
		Value.Width = Width;
		Value.Height = Height;
		Value.Rendered.reset();
		Metrics.ViewportTargetBytes += TargetBytes(Width, Height);
		++Metrics.ViewportTargetAllocations;
		return true;
	}

	void SDLViewportRenderer::ApplyPalettes(SDL_GPUDevice *Gpu, Target &Value, const RenderProjection &Scene,
		SDLRendererMetrics &Metrics
	) {
		const auto &Poses = Scene.GetAnimationPoses();
		if (!Value.Palettes && Poses.empty()) return;
		if (!Value.Palettes) Value.Palettes = std::make_unique<SDLSkinPaletteCache>(Gpu, &Metrics);
		const auto Previous = std::span(Value.UploadedPoses).first(Value.UploadedCount);
		RenderPublication Changes;
		for (const auto &Pose : Previous) {
			const auto Existing = Poses.find(Pose.Object);
			if (Existing == Poses.end() || Existing->second.Palette.Skeleton != Pose.Palette.Skeleton ||
				Existing->second.Palette.Entries->size() != Pose.Palette.Entries->size())
				Changes.AnimationPoseRemoves.push_back({Pose.Object});
		}
		for (const auto &[Object, Pose] : Poses) {
			const auto Before = std::ranges::find(Previous, Object, &RenderAnimationPoseUpdate::Object);
			if (Before == Previous.end() || Before->PoseRevision != Pose.PoseRevision ||
				Before->Palette.Entries != Pose.Palette.Entries)
				Changes.AnimationPoseUpdates.push_back(Pose);
		}
		Value.Palettes->ApplyPublication(Changes);
		Value.UploadedCount = 0;
		for (const auto &[Object, Pose] : Poses) {
			(void)Object;
			Value.UploadedPoses[Value.UploadedCount++] = Pose;
		}
		std::fill(Value.UploadedPoses.begin() + Value.UploadedCount, Value.UploadedPoses.end(),
			RenderAnimationPoseUpdate{});
	}

	void SDLViewportRenderer::Draw(SDL_GPUDevice *Gpu, SDLFrameContext &Frame) {
		const auto &Scenes = Frame.Projection.GetViewports();
		if (Scenes.empty() && TargetCount == 0) return;
		runtime_detail::WorkScope Work(runtime_detail::WorkPhase::ViewportDraw);
		auto &Metrics = *Frame.Metrics;
		const auto Start = std::chrono::steady_clock::now();
		if (UiPublicationId != Frame.Projection.GetUiPublicationId()) {
			UiPublicationId = Frame.Projection.GetUiPublicationId();
			ReferenceCount = 0;
			for (const auto &Batch : Frame.Projection.GetUi().Batches) {
				if (!Batch.Viewport.IsValid() || Batch.Opacity <= 0.0f ||
					(Batch.Clip && (Batch.Clip->Width <= 0.0f || Batch.Clip->Height <= 0.0f))) continue;
				const auto End = ReferencedViewports.begin() + ReferenceCount;
				if (std::find(ReferencedViewports.begin(), End, Batch.Viewport) != End) continue;
				ReferencedViewports[ReferenceCount++] = Batch.Viewport;
				if (ReferenceCount == ReferencedViewports.size()) break;
			}
		}
		const auto Referenced = [&](ObjectId Identity) {
			const auto End = ReferencedViewports.begin() + ReferenceCount;
			return std::find(ReferencedViewports.begin(), End, Identity) != End;
		};
		const auto FindScene = [&](ObjectId Identity) {
			return std::ranges::find_if(Scenes, [&](const auto &Scene) { return Scene->Viewport == Identity; });
		};
		for (auto &Value : Targets) {
			if (Value.Viewport.IsValid() && (FindScene(Value.Viewport) == Scenes.end() || !Referenced(Value.Viewport))) {
				Release(Gpu, Value, Metrics);
				--TargetCount;
			}
		}
		for (const auto &Scene : Scenes) {
			if (std::ranges::find(Targets, Scene->Viewport, &Target::Viewport) != Targets.end() ||
				!Referenced(Scene->Viewport)) continue;
			const auto Slot = std::ranges::find(Targets, ObjectId{}, &Target::Viewport);
			if (Slot != Targets.end()) {
				Slot->Viewport = Scene->Viewport;
				++TargetCount;
			}
		}
		std::size_t Drawn = 0;
		const auto First = Cursor;
		for (std::size_t Index = 0; Index < Targets.size() && Drawn < MaximumRedrawsPerFrame; ++Index) {
			const auto TargetIndex = (First + Index) % Targets.size();
			auto &Value = Targets[TargetIndex];
			if (!Value.Viewport.IsValid()) continue;
			const auto &Scene = *FindScene(Value.Viewport);
			if (Value.Rendered == Scene || ResourcesFailed) continue;
			Cursor = (TargetIndex + 1) % Targets.size();
			++Drawn;
			try {
				if (!Opaque) Opaque = CreateOpaquePass(Gpu, ColorFormat, &Metrics);
				if (!Resize(Gpu, Value, Scene->Width, Scene->Height, Metrics)) {
					++Metrics.ViewportFailures;
					continue;
				}
				Projection.ApplyViewportScene(*Scene, Frame.Projection);
				ApplyPalettes(Gpu, Value, Projection, Metrics);
				auto &Palettes = Value.Palettes ? *Value.Palettes : Frame.SkinPalettes;
				SDLFrameContext View(Projection, Frame.MeshResources, Palettes, Frame.ShadowCache, Frame.ShadowPoseRevisions);
				View.Commands = Frame.Commands;
				View.Width = Scene->Width;
				View.Height = Scene->Height;
				View.SwapchainTexture = Value.Color;
				View.DepthTexture = Value.Depth;
				View.ClearPresentationColor = true;
				View.EnableOpaqueGrouping = false;
				View.TextureResources = Frame.TextureResources;
				View.MaterialSamplers = Frame.MaterialSamplers;
				View.EnvironmentLighting = Frame.EnvironmentLighting;
				View.EnvironmentLighting.Enabled = false;
				View.ReflectionProbes = Frame.ReflectionProbes;
				View.ReflectionProbes.Count = 0;
				View.ShadowMapTexture = Frame.ShadowMapTexture;
				View.ShadowSampler = Frame.ShadowSampler;
				View.CascadeShadowTexture = Frame.CascadeShadowTexture;
				View.LocalShadowTexture = Frame.LocalShadowTexture;
				if (auto *Pass = Opaque->Draw(Gpu, View)) SDL_EndGPURenderPass(Pass);
				Value.Pending = Scene;
				++Metrics.ViewportRenders;
				runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::ViewportDraw);
			} catch (const std::exception &Error) {
				++Metrics.ViewportFailures;
				if (!Opaque) ResourcesFailed = true;
				Release(Gpu, Value, Metrics);
				--TargetCount;
				LOG_WARN(App, "[Render:ViewportFrame] Secondary view skipped: %s", Error.what());
			}
		}
		Metrics.CpuViewportNanoseconds += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - Start).count());
		Frame.Viewports = this;
	}

	void SDLViewportRenderer::OnFrameSubmitted() {
		for (auto &Value : Targets) if (Value.Pending) Value.Rendered = std::move(Value.Pending);
		if (Opaque) Opaque->OnFrameSubmitted();
	}

	void SDLViewportRenderer::OnFrameAborted() {
		for (auto &Value : Targets) Value.Pending.reset();
		if (Opaque) Opaque->OnFrameAborted();
	}

	void SDLViewportRenderer::Destroy(SDL_GPUDevice *Gpu, SDLRendererMetrics &Metrics) {
		for (auto &Value : Targets) Release(Gpu, Value, Metrics);
		if (Opaque) Opaque->Destroy(Gpu);
		Opaque.reset();
		Projection.Clear();
		ResourcesFailed = false;
		Cursor = 0;
		ReferenceCount = 0;
		TargetCount = 0;
		UiPublicationId = InvalidRenderPublicationId;
	}

	SDL_GPUTexture *SDLViewportRenderer::Find(ObjectId Viewport) const {
		const auto Value = std::ranges::find(Targets, Viewport, &Target::Viewport);
		return Value != Targets.end() && (Value->Rendered || Value->Pending) ? Value->Color : nullptr;
	}
}
