// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/render/Renderer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gargantuan {
	struct FilamentFrameMetrics {
		double ProjectionScanMilliseconds = 0.0;
		double ChangedObjectApplyMilliseconds = 0.0;
		double RendererSubmissionMilliseconds = 0.0;
		std::optional<double> GpuFrameMilliseconds;
		RenderProjectionChanges Changes;
		std::size_t ProjectedObjects = 0;
	};

	class FilamentRenderer final : public BaseRenderer {
	  public:
		explicit FilamentRenderer(
			const Vector2 &ViewportSize,
			bool Headless = false,
			bool ShadowsEnabled = false
		);
		~FilamentRenderer() override;

		void Draw(RenderSnapshotPtr Snapshot) override;
		void Resize(int WidthValue, int HeightValue) override;
		void Destroy() override;
		[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> GetViewportSize() const override;

		[[nodiscard]] FilamentFrameMetrics GetLastMetrics() const;
		[[nodiscard]] std::string GetBackendName() const;
		[[nodiscard]] std::size_t GetMaxAutomaticInstances() const;
		[[nodiscard]] bool IsAutomaticInstancingEnabled() const;
		[[nodiscard]] std::optional<std::size_t> GetRendererOwnedBytes() const;
		[[nodiscard]] std::vector<double> GetGpuFrameHistoryMilliseconds(std::size_t MaximumFrames);
		void FlushAndWait();

	  private:
		struct Impl;
		std::unique_ptr<Impl> Implementation;
	};
} // namespace gargantuan
