#pragma once

#include "gargantuan/datatypes/Vector2.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/render/RenderPublication.hpp"
#include "gargantuan/render/RenderSnapshot.hpp"

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace gargantuan {
	class BaseRenderer {
	  public:
		BaseRenderer() = default;
		explicit BaseRenderer(const Vector2 &ViewportSize) { (void)ViewportSize; }
		virtual ~BaseRenderer() = default;

		BaseRenderer(const BaseRenderer &) = delete;
		BaseRenderer &operator=(const BaseRenderer &) = delete;

		virtual void Draw(RenderPublicationPtr Publication) = 0;
		void Draw(RenderSnapshotPtr Snapshot);
		virtual void Resize(int WidthValue, int HeightValue) = 0;
		virtual void Destroy() = 0;
		[[nodiscard]] virtual std::pair<std::uint32_t, std::uint32_t> GetViewportSize() const = 0;
	};

	class HeadlessRenderer final : public BaseRenderer {
	  public:
		explicit HeadlessRenderer(const Vector2 &ViewportSize)
			: BaseRenderer(ViewportSize), Width(static_cast<std::uint32_t>(ViewportSize.GetX())),
			  Height(static_cast<std::uint32_t>(ViewportSize.GetY())) {};

		using BaseRenderer::Draw;
		void Draw(RenderPublicationPtr Publication) override {
			if (!Publication) throw std::invalid_argument("HeadlessRenderer requires an immutable RenderPublication");
			LastChanges = Projection.Apply(*Publication);
			LastPublication = std::move(Publication);
		};
		void Resize(int WidthValue, int HeightValue) override {
			if (WidthValue < 1 || HeightValue < 1) return;
			Width = static_cast<std::uint32_t>(WidthValue);
			Height = static_cast<std::uint32_t>(HeightValue);
		};
		void Destroy() override {};
		[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> GetViewportSize() const override {
			return {Width, Height};
		}
		[[nodiscard]] const RenderProjection &GetProjection() const { return Projection; }
		[[nodiscard]] RenderProjectionChanges GetLastChanges() const { return LastChanges; }
		[[nodiscard]] RenderPublicationPtr TakeLastPublication() { return std::move(LastPublication); }

	  private:
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		RenderProjection Projection;
		RenderProjectionChanges LastChanges;
		RenderPublicationPtr LastPublication;
	};
} // namespace gargantuan
