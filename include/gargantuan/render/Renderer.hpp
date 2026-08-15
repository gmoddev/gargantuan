#pragma once

#include "gargantuan/datatypes/Vector2.hpp"
#include "gargantuan/render/RenderSnapshot.hpp"

#include <cstdint>
#include <utility>

namespace gargantuan {
	class BaseRenderer {
	  public:
		BaseRenderer() = default;
		explicit BaseRenderer(const Vector2 &ViewportSize) { (void)ViewportSize; }
		virtual ~BaseRenderer() = default;

		BaseRenderer(const BaseRenderer &) = delete;
		BaseRenderer &operator=(const BaseRenderer &) = delete;

		virtual void Draw(RenderSnapshotPtr Snapshot) = 0;
		virtual void Resize(int WidthValue, int HeightValue) = 0;
		virtual void Destroy() = 0;
		[[nodiscard]] virtual std::pair<std::uint32_t, std::uint32_t> GetViewportSize() const = 0;
	};

	class HeadlessRenderer final : public BaseRenderer {
	  public:
		explicit HeadlessRenderer(const Vector2 &ViewportSize)
			: BaseRenderer(ViewportSize), Width(static_cast<std::uint32_t>(ViewportSize.GetX())),
			  Height(static_cast<std::uint32_t>(ViewportSize.GetY())) {};

		void Draw(RenderSnapshotPtr Snapshot) override { (void)Snapshot; };
		void Resize(int WidthValue, int HeightValue) override {
			if (WidthValue < 1 || HeightValue < 1) return;
			Width = static_cast<std::uint32_t>(WidthValue);
			Height = static_cast<std::uint32_t>(HeightValue);
		};
		void Destroy() override {};
		[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> GetViewportSize() const override {
			return {Width, Height};
		}

	  private:
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
	};
} // namespace gargantuan
