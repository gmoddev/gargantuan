// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/render/RenderPublication.hpp"
#include "gargantuan/render/RenderSnapshot.hpp"
#include "gargantuan/runtime/ObjectId.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace gargantuan {
	struct EditorViewportFrame {
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
		std::vector<std::uint8_t> RgbPixels;
	};

	struct EditorViewportPick {
		ObjectId Object;
		float Distance = 0.0f;
	};

	[[nodiscard]] std::optional<EditorViewportPick> PickEditorViewport(
		const RenderSnapshot &Snapshot,
		float X,
		float Y
	);
	[[nodiscard]] std::optional<EditorViewportPick> PickEditorViewport(
		const RenderProjection &Projection,
		float X,
		float Y
	);

	class EditorViewportRenderer final {
	  public:
		EditorViewportRenderer(std::uint32_t Width, std::uint32_t Height);
		~EditorViewportRenderer();

		EditorViewportRenderer(const EditorViewportRenderer &) = delete;
		EditorViewportRenderer &operator=(const EditorViewportRenderer &) = delete;

		void Resize(std::uint32_t Width, std::uint32_t Height);
		void ApplyPublication(RenderPublicationPtr Publication);
		[[nodiscard]] EditorViewportFrame Capture(RenderPublicationPtr Publication);
		[[nodiscard]] std::optional<EditorViewportPick> Pick(float X, float Y) const;

	  private:
		struct Backend;
		std::unique_ptr<Backend> State;
	};
}
