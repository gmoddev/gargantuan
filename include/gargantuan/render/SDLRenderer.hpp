// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/render/Renderer.hpp"

#include <memory>
#include <string>

namespace gargantuan {
	class SDLRenderer final : public BaseRenderer {
	  public:
		explicit SDLRenderer(const Vector2 &ViewportSize);
		~SDLRenderer() override;

		void Draw(RenderSnapshotPtr Snapshot) override;
		void Resize(int WidthValue, int HeightValue) override;
		void Destroy() override;
		[[nodiscard]] std::string GetDriverName() const;
		[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> GetViewportSize() const override;

	  private:
		struct Backend;
		std::unique_ptr<Backend> State;
	};
}
