#include "render/sdl/SDLLocalLightGrid.hpp"
#include "render/sdl/SDLSpotShadowAtlas.hpp"
#include "gargantuan/render/SDLRenderer.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>
#include <stdexcept>

namespace gargantuan {
	namespace {
		constexpr std::uint32_t MaximumTileAxis = 64;
		constexpr std::uint32_t TileLightLimit = 24;
		constexpr std::uint32_t TileStride = TileLightLimit + 1;

		struct TileRect {
			std::uint32_t Left;
			std::uint32_t Top;
			std::uint32_t Right;
			std::uint32_t Bottom;
		};

		struct VisibleLight {
			const RenderLocalLightState *Light;
			TileRect Rect;
			double Score;
		};

		std::optional<TileRect> GetTileRect(const RenderLocalLightState &Light, const glm::dmat4 &Clip,
			const std::array<glm::dvec4, 6> &Planes, std::uint32_t Width, std::uint32_t Height,
			std::uint32_t TilePixels, std::uint32_t Columns, std::uint32_t Rows) {
			const glm::dvec3 Center(Light.Position);
			for (const auto &Plane : Planes) {
				const glm::dvec3 Normal(Plane);
				const auto Radius = glm::length(Normal) * Light.Range;
				const auto Distance = glm::dot(Normal, Center) + Plane.w;
				const auto Margin = 1e-5 * (glm::dot(glm::abs(Normal), glm::abs(Center)) + std::abs(Plane.w) + Radius + 1.0);
				if (Distance + Radius < -Margin) return std::nullopt;
			}

			glm::dvec2 Minimum(1.0), Maximum(-1.0);
			for (unsigned Corner = 0; Corner < 8; ++Corner) {
				const glm::dvec3 Offset((Corner & 1) ? Light.Range : -Light.Range,
					(Corner & 2) ? Light.Range : -Light.Range, (Corner & 4) ? Light.Range : -Light.Range);
				const auto Projected = Clip * glm::dvec4(Center + Offset, 1.0);
				if (Projected.w <= 1e-6) return TileRect{0, 0, Columns - 1, Rows - 1};
				const glm::dvec2 Point(Projected.x / Projected.w, -Projected.y / Projected.w);
				Minimum = glm::min(Minimum, Point);
				Maximum = glm::max(Maximum, Point);
			}
			const glm::dvec2 Extent(Width, Height);
			Minimum = glm::clamp((Minimum * 0.5 + 0.5) * Extent - 1.0, glm::dvec2(0.0), Extent - 1.0);
			Maximum = glm::clamp((Maximum * 0.5 + 0.5) * Extent + 1.0, glm::dvec2(0.0), Extent - 1.0);
			return TileRect{static_cast<std::uint32_t>(Minimum.x) / TilePixels,
				static_cast<std::uint32_t>(Minimum.y) / TilePixels, static_cast<std::uint32_t>(Maximum.x) / TilePixels,
				static_cast<std::uint32_t>(Maximum.y) / TilePixels};
		}
	}

	void SDLLocalLightGrid::Release(Buffer &Value) {
		if (Value.Transfer) SDL_ReleaseGPUTransferBuffer(Gpu, Value.Transfer);
		if (Value.GpuBuffer) SDL_ReleaseGPUBuffer(Gpu, Value.GpuBuffer);
		Value = {};
	}

	void SDLLocalLightGrid::Destroy() {
		Release(Lights);
		Release(Tiles);
		LastLights.reset();
		LastShadowBindings = {};
		Valid = false;
	}

	void SDLLocalLightGrid::Upload(Buffer &Value, SDL_GPUCopyPass *Pass, const void *Data, std::uint32_t Size,
		SDLRendererMetrics *Metrics) {
		const bool Grow = Size > Value.Capacity;
		if (Grow) {
			const auto Capacity = std::bit_ceil(Size);
			const SDL_GPUBufferCreateInfo BufferInfo{.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, .size = Capacity};
			const SDL_GPUTransferBufferCreateInfo TransferInfo{.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = Capacity};
			Buffer Replacement{SDL_CreateGPUBuffer(Gpu, &BufferInfo), SDL_CreateGPUTransferBuffer(Gpu, &TransferInfo), Capacity};
			if (!Replacement.GpuBuffer || !Replacement.Transfer) {
				Release(Replacement);
				throw std::runtime_error(std::format("[Renderer:LocalLights] Could not allocate light buffers: {}", SDL_GetError()));
			}
			Release(Value);
			Value = Replacement;
			if (Metrics) {
				++Metrics->TransferBufferCreations;
				++Metrics->BufferReallocations;
			}
		}
		auto *Mapped = SDL_MapGPUTransferBuffer(Gpu, Value.Transfer, !Grow);
		if (!Mapped) throw std::runtime_error(std::format("[Renderer:LocalLights] Could not map light buffer: {}", SDL_GetError()));
		std::memcpy(Mapped, Data, Size);
		SDL_UnmapGPUTransferBuffer(Gpu, Value.Transfer);
		const SDL_GPUTransferBufferLocation Source{.transfer_buffer = Value.Transfer};
		const SDL_GPUBufferRegion Destination{.buffer = Value.GpuBuffer, .size = Size};
		SDL_UploadToGPUBuffer(Pass, &Source, &Destination, !Grow);
		if (Metrics) {
			++Metrics->UploadOperations;
			Metrics->UploadedBytes += Size;
			Metrics->LocalLightUploadBytes += Size;
		}
	}

	void SDLLocalLightGrid::Prepare(const RenderFrameState &Frame, SDLRendererMetrics *Metrics,
		const SDLSpotShadowAtlas *Atlas) {
		const auto Start = std::chrono::steady_clock::now();
		std::array<ShadowBinding, MaximumShadowedSpotlights> ShadowBindings{};
		if (Atlas) {
			std::size_t Count = 0;
			for (const auto &Assignment : Atlas->GetAssignments()) {
				if (!Assignment.Sampleable) continue;
				ShadowBindings[Count++] = {Assignment.Light, Assignment.Layer + 1};
			}
		}
		const auto &Source = Frame.Environment.LocalLights;
		const bool HasLights = Source && !Source->empty();
		const auto ClipMatrix = HasLights
			? glm::dmat4(Frame.Camera.ProjectionMatrix) * glm::dmat4(Frame.Camera.ViewMatrix) : glm::dmat4(1.0);
		if (Valid && Source == LastLights && ShadowBindings == LastShadowBindings &&
			(!HasLights || (LastClip == ClipMatrix && LastCamera == Frame.Camera.Position &&
			Width == Frame.ViewportWidth && Height == Frame.ViewportHeight))) {
			if (Metrics) Metrics->LastLocalLightGridNanoseconds = 0;
			return;
		}
		Valid = false;
		Width = Frame.ViewportWidth;
		Height = Frame.ViewportHeight;
		TilePixels = std::max({32u, (Width - 1) / MaximumTileAxis + 1, (Height - 1) / MaximumTileAxis + 1});
		Columns = (Width - 1) / TilePixels + 1;
		const auto Rows = (Height - 1) / TilePixels + 1;
		std::vector<VisibleLight> Visible;
		if (HasLights) {
			Visible.reserve(Source->size());
			const auto MatrixRows = glm::transpose(ClipMatrix);
			const std::array Planes{MatrixRows[3] + MatrixRows[0], MatrixRows[3] - MatrixRows[0],
				MatrixRows[3] + MatrixRows[1], MatrixRows[3] - MatrixRows[1], MatrixRows[2], MatrixRows[3] - MatrixRows[2]};
			for (const auto &Light : *Source) {
				if (Light.Intensity <= 0.0f) continue;
				const auto Rect = GetTileRect(Light, ClipMatrix, Planes, Width, Height, TilePixels, Columns, Rows);
				if (!Rect) continue;
				const auto Near = std::max(1.0, glm::length(glm::dvec3(Light.Position) -
					glm::dvec3(Frame.Camera.Position)) - Light.Range);
				const auto Score = Light.Intensity * std::max({Light.Color.r, Light.Color.g, Light.Color.b}) / (Near * Near);
				Visible.push_back({&Light, *Rect, Score});
			}
			std::ranges::sort(Visible, [](const VisibleLight &Left, const VisibleLight &Right) {
				if (Left.Score != Right.Score) return Left.Score > Right.Score;
				return Left.Light->Object < Right.Light->Object;
			});
		}
		VisibleCount = static_cast<std::uint32_t>(Visible.size());
		OverBudgetTiles = 0;
		DroppedIndices = 0;
		PackedLights.clear();
		TileWords.assign(Visible.empty() ? 1u : Rows * Columns * TileStride, 0);
		for (const auto &Entry : Visible) {
			const auto &Light = *Entry.Light;
			std::uint32_t LayerPlusOne = 0;
			for (const auto &Binding : ShadowBindings) if (Binding.Light == Light.Object) {
				LayerPlusOne = Binding.LayerPlusOne;
				break;
			}
			const auto Index = static_cast<std::uint32_t>(PackedLights.size());
			PackedLights.push_back({
				{Light.Position, Light.Range}, {Light.Direction, Light.OuterConeCosine}, {Light.Color, Light.Intensity},
				{Light.InnerConeCosine, Light.Kind == RenderLocalLightKind::Spot ? 1.0f : 0.0f,
					static_cast<float>(LayerPlusOne), 0.0f},
			});
			for (auto Y = Entry.Rect.Top; Y <= Entry.Rect.Bottom; ++Y) for (auto X = Entry.Rect.Left; X <= Entry.Rect.Right; ++X) {
				const auto Offset = (Y * Columns + X) * TileStride;
				auto &Count = TileWords[Offset];
				if (Count < TileLightLimit) TileWords[Offset + 1 + Count++] = Index;
				else {
					if (Count == TileLightLimit) {
						++OverBudgetTiles;
						++Count;
					}
					++DroppedIndices;
				}
			}
		}
		if (PackedLights.empty()) PackedLights.emplace_back();
		auto *Commands = SDL_AcquireGPUCommandBuffer(Gpu);
		if (!Commands) throw std::runtime_error("[Renderer:LocalLights] Could not acquire upload commands");
		auto *Pass = SDL_BeginGPUCopyPass(Commands);
		try {
			if (!Pass) throw std::runtime_error("[Renderer:LocalLights] Could not begin uploads");
			Upload(Lights, Pass, PackedLights.data(), static_cast<std::uint32_t>(PackedLights.size() * sizeof(GpuLight)), Metrics);
			Upload(Tiles, Pass, TileWords.data(), static_cast<std::uint32_t>(TileWords.size() * sizeof(std::uint32_t)), Metrics);
		} catch (...) {
			if (Pass) SDL_EndGPUCopyPass(Pass);
			SDL_CancelGPUCommandBuffer(Commands);
			throw;
		}
		SDL_EndGPUCopyPass(Pass);
		if (!SDL_SubmitGPUCommandBuffer(Commands)) throw std::runtime_error("[Renderer:LocalLights] Could not submit light uploads");
		LastLights = Source;
		LastShadowBindings = ShadowBindings;
		LastClip = ClipMatrix;
		LastCamera = Frame.Camera.Position;
		Valid = true;
		if (Metrics) {
			Metrics->VisibleLocalLights = VisibleCount;
			Metrics->OverBudgetLightTiles = OverBudgetTiles;
			Metrics->DroppedLightTileIndices = DroppedIndices;
			Metrics->LastLocalLightGridNanoseconds = static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Start).count());
		}
	}
}
