// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#pragma once

#include "gargantuan/gui/GuiTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gargantuan {
	class AssetService;
	struct GuiTextureChanges {
		std::vector<RenderTextureCreate> Creates;
		std::vector<RenderTextureUpdate> Updates;
		std::vector<RenderTextureRemove> Removes;
		std::size_t UploadBytes = 0;
	};

	struct GuiTextRequest {
		std::string Text;
		std::string FontFace;
		float LogicalSize = 16.0f;
		float DpiScale = 1.0f;
		float LogicalWrapWidth = 0.0f;
		bool Wrapped = false;
		int HorizontalAlignment = 1;
		bool EditableMetrics = false;
	};

	class GuiTextSystem final {
	  public:
		explicit GuiTextSystem(std::shared_ptr<AssetService> Assets);
		~GuiTextSystem();
		GuiTextSystem(const GuiTextSystem &) = delete;
		GuiTextSystem &operator=(const GuiTextSystem &) = delete;

		[[nodiscard]] std::shared_ptr<const GuiShapedText> Shape(const GuiTextRequest &Request);
		[[nodiscard]] GuiTextureChanges DrainTextureChanges();
		[[nodiscard]] GuiRuntimeProfile ConsumeProfile();
		void ResetFrameBudget(std::size_t ReservedUploadBytes = 0);
		[[nodiscard]] bool HasFont() const;

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};

}
