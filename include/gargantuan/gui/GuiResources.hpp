// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#pragma once

#include "gargantuan/gui/GuiTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gargantuan {
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
	};

	class GuiTextSystem final {
	  public:
		explicit GuiTextSystem(std::filesystem::path DefaultFontPath);
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

	struct GuiImageResource {
		RenderTextureIdentity Texture;
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
	};

	class GuiImageStore final {
	  public:
		GuiImageStore();
		~GuiImageStore();
		GuiImageStore(const GuiImageStore &) = delete;
		GuiImageStore &operator=(const GuiImageStore &) = delete;

		void Register(std::string LogicalId, std::uint32_t Width, std::uint32_t Height, std::span<const std::uint8_t> Rgba8);
		[[nodiscard]] std::optional<GuiImageResource> Find(const std::string &LogicalId) const;
		[[nodiscard]] std::size_t PendingUploadBytes() const;
		[[nodiscard]] GuiTextureChanges DrainTextureChanges();

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
