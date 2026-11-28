// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace gargantuan {
	class DataModel;
	class Script;
	namespace application_template {
		inline constexpr std::string_view Path = "content/application.json";
		inline constexpr std::size_t MaximumBytes = 2 * 1024 * 1024;
		struct Graph {
			std::shared_ptr<DataModel> World;
			std::shared_ptr<Script> Entry;
		};
		[[nodiscard]] std::string Extract(std::string &Project, std::string_view SourceId);
		void Validate(std::string_view Template);
		[[nodiscard]] Graph Load(std::string_view Template);
	}
}
